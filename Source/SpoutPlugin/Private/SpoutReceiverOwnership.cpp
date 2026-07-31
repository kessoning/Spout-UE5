/**
 * Implements plugin-side ownership of receiver-created UObjects.
 */
#include "SpoutReceiverOwnership.h"

#include "SpoutModule.h"

#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/UObjectGlobals.h"

FSpoutReceiverOwnership* FSpoutReceiverOwnership::Instance = nullptr;
FCriticalSection FSpoutReceiverOwnership::InstanceMutex;

FSpoutReceiverOwnership& FSpoutReceiverOwnership::Get()
{
	FScopeLock Lock(&InstanceMutex);
	if (!Instance)
	{
		// Heap-allocated and destroyed explicitly in TearDown rather than being a function-local
		// static: an FGCObject registers itself with the global referencer, and a static destructor
		// would run at process exit, after the UObject system has gone.
		Instance = new FSpoutReceiverOwnership();
	}
	return *Instance;
}

FSpoutReceiverOwnership* FSpoutReceiverOwnership::Peek()
{
	FScopeLock Lock(&InstanceMutex);
	return Instance;
}

void FSpoutReceiverOwnership::TearDown()
{
	FSpoutReceiverOwnership* Doomed = nullptr;
	{
		FScopeLock Lock(&InstanceMutex);
		Doomed = Instance;
		Instance = nullptr;
	}

	if (Doomed)
	{
		// Give the tracked objects back before unregistering from the collector, so nothing is
		// left referenced by an object that is about to stop reporting its references.
		Doomed->ReleaseAll();
		delete Doomed;
	}
}

UTexture2D* FSpoutReceiverOwnership::ResolveDetachFallbackTexture()
{
	// The engine's default texture. There is no plugin-owned neutral texture to use instead: the
	// plugin creates only received textures, which are exactly what a detach has to stop
	// referencing. Creating one here purely to detach into would be a new allocation with a
	// lifetime problem of its own, so the engine's always-loaded default is the better answer.
	return GEngine ? ToRawPtr(GEngine->DefaultTexture) : nullptr;
}

void FSpoutReceiverOwnership::DetachBinding(const FMaterialBinding& Binding, UTexture2D* FallbackTexture)
{
	// Resolved through the weak pointer, so a consumer-supplied material that has already been
	// collected is simply skipped rather than dereferenced.
	UMaterialInstanceDynamic* Material = Binding.Material.Get();
	if (!Material || Binding.TextureParameter.IsNone())
	{
		return;
	}

	// Clearing the parameter is the part that actually affects other holders of the material.
	// Nulling a caller's local variable does not: the MID keeps sampling the texture, and every
	// other reference to that MID keeps the binding -- and, because a material parameter is a strong
	// reference, keeps the texture alive too, which would defeat releasing ownership here.
	//
	// Rebound to a real texture rather than to null. UMaterialInstance's texture setter ignores a
	// null value outright -- it only assigns inside `if (Value && ensureMsgf(...))` -- so passing
	// null leaves the received texture bound and referenced.
	if (FallbackTexture)
	{
		Material->SetTextureParameterValue(Binding.TextureParameter, FallbackTexture);
		return;
	}

	// No default texture to fall back on (very early startup, or a stripped configuration). Say so
	// rather than leave a silently still-bound material.
	UE_LOG(LogSpoutPlugin, Warning,
		TEXT("Spout: could not detach texture parameter '%s' from material '%s' because no engine default texture is available. ")
		TEXT("The material still references the received texture, which will keep it alive until the material is released."),
		*Binding.TextureParameter.ToString(),
		*Material->GetName());
}

void FSpoutReceiverOwnership::PruneDeadBindings(FEntry& Entry)
{
	// A consumer-supplied material that has been collected leaves a dead weak reference behind.
	// Dropping those keeps the collection proportional to the live consumers rather than to every
	// material that has ever been bound over the session's life.
	Entry.MaterialBindings.RemoveAll([](const FMaterialBinding& Binding)
	{
		return !Binding.Material.IsValid();
	});
}

void FSpoutReceiverOwnership::CloseEntry(FEntry& Entry, UTexture2D* FallbackTexture, int32& OutDetachedCount)
{
	OutDetachedCount = 0;

	// 1. Detach EVERY live binding, not just the most recent one. This is the whole point of the
	//    collection: any binding left attached keeps the released texture referenced and alive.
	for (const FMaterialBinding& Binding : Entry.MaterialBindings)
	{
		if (Binding.Material.IsValid())
		{
			DetachBinding(Binding, FallbackTexture);
			++OutDetachedCount;
		}
	}

	// 2. Release plugin ownership of the materials this plugin created, before the records go, so
	//    the ordering is explicit rather than a side effect of destroying the array.
	for (FMaterialBinding& Binding : Entry.MaterialBindings)
	{
		Binding.OwnedMaterial = nullptr;
	}

	// 3. Clear the binding records. A reopened session starts with none.
	Entry.MaterialBindings.Empty();

	// The received texture is released by the caller dropping the whole entry, which is the last
	// step of the teardown ordering.
}

void FSpoutReceiverOwnership::AdoptTexture(FName ReceiverName, UTexture2D* Texture)
{
	FScopeLock Lock(&Mutex);

	FEntry& Entry = Entries.FindOrAdd(ReceiverName);

	// Assigning over the previous texture is the whole release: the old object loses this
	// reference and nothing else here touches it. No unrooting, no BeginReleaseResource, no
	// MarkAsGarbage -- the engine's own UTexture teardown does that correctly once it is collected,
	// and it is the only thing that knows when the render thread has finished with it.
	Entry.Texture = Texture;
}

UTexture2D* FSpoutReceiverOwnership::FindTexture(FName ReceiverName) const
{
	FScopeLock Lock(&Mutex);
	const FEntry* Entry = Entries.Find(ReceiverName);
	return Entry ? Entry->Texture : nullptr;
}

bool FSpoutReceiverOwnership::OwnsTexture(FName ReceiverName, const UTexture2D* Texture) const
{
	if (!Texture)
	{
		return false;
	}

	FScopeLock Lock(&Mutex);
	const FEntry* Entry = Entries.Find(ReceiverName);
	return Entry != nullptr && Entry->Texture == Texture;
}

void FSpoutReceiverOwnership::RegisterMaterialBinding(FName ReceiverName, UMaterialInstanceDynamic* Material, FName ParameterName, bool bPluginCreated)
{
	if (!Material || ParameterName.IsNone())
	{
		return;
	}

	FScopeLock Lock(&Mutex);

	FEntry& Entry = Entries.FindOrAdd(ReceiverName);
	PruneDeadBindings(Entry);

	// De-duplicated on (material, parameter), so this can be called every tick by every consumer
	// without the collection growing. The same material through a different parameter name is a
	// genuinely different binding and gets its own entry.
	for (FMaterialBinding& Existing : Entry.MaterialBindings)
	{
		if (Existing.Material.Get() == Material && Existing.TextureParameter == ParameterName)
		{
			if (bPluginCreated)
			{
				// Idempotent: re-asserting plugin ownership is fine. The reverse is deliberately not
				// done -- a later call that does not claim creation must never drop ownership of a
				// material this plugin made.
				Existing.OwnedMaterial = Material;
			}
			return;
		}
	}

	FMaterialBinding Binding;
	Binding.Material = Material;
	Binding.TextureParameter = ParameterName;
	// Weak always; strong only when the plugin created it. A consumer-supplied material is tracked
	// so close can detach the parameter this plugin set, never so that this plugin keeps it alive.
	Binding.OwnedMaterial = bPluginCreated ? Material : nullptr;
	Entry.MaterialBindings.Add(MoveTemp(Binding));
}

bool FSpoutReceiverOwnership::OwnsMaterial(FName ReceiverName, const UMaterialInstanceDynamic* Material) const
{
	if (!Material)
	{
		return false;
	}

	FScopeLock Lock(&Mutex);
	const FEntry* Entry = Entries.Find(ReceiverName);
	if (!Entry)
	{
		return false;
	}

	for (const FMaterialBinding& Binding : Entry->MaterialBindings)
	{
		if (Binding.OwnedMaterial == Material)
		{
			return true;
		}
	}
	return false;
}

int32 FSpoutReceiverOwnership::NumMaterialBindings(FName ReceiverName) const
{
	FScopeLock Lock(&Mutex);
	const FEntry* Entry = Entries.Find(ReceiverName);
	if (!Entry)
	{
		return 0;
	}

	int32 LiveCount = 0;
	for (const FMaterialBinding& Binding : Entry->MaterialBindings)
	{
		if (Binding.Material.IsValid())
		{
			++LiveCount;
		}
	}
	return LiveCount;
}

bool FSpoutReceiverOwnership::HasMaterialBinding(FName ReceiverName, const UMaterialInstanceDynamic* Material, FName ParameterName) const
{
	if (!Material)
	{
		return false;
	}

	FScopeLock Lock(&Mutex);
	const FEntry* Entry = Entries.Find(ReceiverName);
	if (!Entry)
	{
		return false;
	}

	for (const FMaterialBinding& Binding : Entry->MaterialBindings)
	{
		if (Binding.Material.Get() == Material && Binding.TextureParameter == ParameterName)
		{
			return true;
		}
	}
	return false;
}

bool FSpoutReceiverOwnership::Release(FName ReceiverName)
{
	UTexture2D* FallbackTexture = ResolveDetachFallbackTexture();

	FScopeLock Lock(&Mutex);

	FEntry* Entry = Entries.Find(ReceiverName);
	if (!Entry)
	{
		// Idempotent: closing a session that is not open is not an error.
		return false;
	}

	int32 DetachedCount = 0;
	CloseEntry(*Entry, FallbackTexture, DetachedCount);

	// Last: dropping the entry releases the received texture itself, after every material that
	// referenced it has been detached.
	Entries.Remove(ReceiverName);

	if (DetachedCount > 0)
	{
		UE_LOG(LogSpoutPlugin, Verbose,
			TEXT("Spout receiver '%s': detached %d material binding(s) and released the received texture."),
			*ReceiverName.ToString(),
			DetachedCount);
	}

	return true;
}

void FSpoutReceiverOwnership::ReleaseAll()
{
	UTexture2D* FallbackTexture = ResolveDetachFallbackTexture();

	FScopeLock Lock(&Mutex);

	int32 TotalDetached = 0;
	for (TPair<FName, FEntry>& Pair : Entries)
	{
		int32 DetachedCount = 0;
		CloseEntry(Pair.Value, FallbackTexture, DetachedCount);
		TotalDetached += DetachedCount;
	}

	const int32 ReleasedCount = Entries.Num();
	Entries.Reset();

	if (ReleasedCount > 0)
	{
		UE_LOG(LogSpoutPlugin, Log,
			TEXT("Spout: released plugin ownership of %d receiver texture(s) and detached %d material binding(s); they are now collectable."),
			ReleasedCount,
			TotalDetached);
	}
}

int32 FSpoutReceiverOwnership::Num() const
{
	FScopeLock Lock(&Mutex);
	return Entries.Num();
}

bool FSpoutReceiverOwnership::Contains(FName ReceiverName) const
{
	FScopeLock Lock(&Mutex);
	return Entries.Contains(ReceiverName);
}

void FSpoutReceiverOwnership::AddReferencedObjects(FReferenceCollector& Collector)
{
	// This is the whole of the "keep it alive" mechanism. It reports the references without rooting
	// anything, so an entry that is dropped becomes collectable on the next pass instead of
	// surviving until someone remembers to unroot it.
	FScopeLock Lock(&Mutex);

	for (TPair<FName, FEntry>& Pair : Entries)
	{
		Collector.AddReferencedObject(Pair.Value.Texture);

		for (FMaterialBinding& Binding : Pair.Value.MaterialBindings)
		{
			// Only the materials this plugin created. A consumer's material is held weakly and is
			// theirs to keep alive.
			if (Binding.OwnedMaterial)
			{
				Collector.AddReferencedObject(Binding.OwnedMaterial);
			}
		}
	}
}

FString FSpoutReceiverOwnership::GetReferencerName() const
{
	return TEXT("FSpoutReceiverOwnership");
}
