// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the sender/receiver registry, handling shared resource lifetimes and Spout
 * name registration used by both sender and receiver paths.
 */
#include "SpoutSenderRegistry.h"
#include "SpoutD3DContext.h"
#include "SpoutModule.h"
#include "SpoutValidation.h"

bool FSpoutSdkSenderNameBackend::CreateSender(const ANSICHAR* Utf8Name, uint32 Width, uint32 Height, void* SharedHandle, uint32 Format)
{
	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	spoutSenderNames* SenderNames = Context.GetSenderNames();
	if (!SenderNames)
	{
		return false;
	}

	// spoutSenderNames::CreateSender takes a mutable char*, so hand it a local copy rather than
	// casting away const on the caller's buffer.
	TArray<ANSICHAR> MutableName(Utf8Name, FCStringAnsi::Strlen(Utf8Name) + 1);

	FScopeLock SpoutLock(&Context.GetSpoutMutex());
	return SenderNames->CreateSender(
		MutableName.GetData(),
		Width,
		Height,
		static_cast<HANDLE>(SharedHandle),
		static_cast<DWORD>(Format));
}

bool FSpoutSdkSenderNameBackend::UpdateSender(const ANSICHAR* Utf8Name, uint32 Width, uint32 Height, void* SharedHandle, uint32 Format)
{
	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	spoutSenderNames* SenderNames = Context.GetSenderNames();
	if (!SenderNames)
	{
		return false;
	}

	FScopeLock SpoutLock(&Context.GetSpoutMutex());
	return SenderNames->UpdateSender(
		Utf8Name,
		Width,
		Height,
		static_cast<HANDLE>(SharedHandle),
		static_cast<DWORD>(Format));
}

void FSpoutSdkSenderNameBackend::ReleaseSenderName(const ANSICHAR* Utf8Name)
{
	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	spoutSenderNames* SenderNames = Context.GetSenderNames();
	if (!SenderNames)
	{
		return;
	}

	FScopeLock SpoutLock(&Context.GetSpoutMutex());
	SenderNames->ReleaseSenderName(Utf8Name);
}

FSpoutSharedSender::FSpoutSharedSender(FName InName, ESpoutType InType)
	: Name(InName)
	, Type(InType)
	, FrameSync(MakeSpoutSdkFrameSyncBackend())
{
	// Encode once. A name that fails validation leaves Utf8Name empty, so HasValidName() is false
	// and no SDK call is ever made with it.
	const SpoutValidation::EResult NameResult = SpoutValidation::ValidateName(InName, &Utf8Name);
	if (NameResult != SpoutValidation::EResult::Ok)
	{
		UE_LOG(LogSpoutPlugin, Warning,
			TEXT("Spout stream '%s' has an unusable name (%s); it cannot be registered or queried."),
			*InName.ToString(),
			SpoutValidation::ToString(NameResult));
	}
}

FSpoutSharedSender::~FSpoutSharedSender()
{
	// Ensure shared handles are released before destruction.
	ReleaseResources();
}

void FSpoutSharedSender::ReleaseResources()
{
	// Release shared textures and cached wrappers under lock.
	FScopeLock Lock(&ResourceMutex);

	// Close the cross-process protocol first, while the texture it names still exists. By this
	// point the ordered teardown has already flushed rendering commands, so no access scope can be
	// live; FSpoutFrameSync::Close says so out loud if one somehow is.
	FrameSync.Close();

	SharedTexture.Reset();
	StagingTexture.Reset();
	CachedWrappedResource.Reset();
	CachedRTWrappedResource.Reset();

	SharedHandle = nullptr;
	SharedFormat = DXGI_FORMAT_UNKNOWN;
	CachedNativeResource = nullptr;
	CachedRTNativeResource = nullptr;

	if (Type == ESpoutType::Sender)
	{
		// Release the Spout name only when the registration state says this plugin owns it, and
		// only through the name the registration itself recorded at CreateSender time. Releasing
		// unconditionally would remove a name that either never registered or, worse, belongs to
		// a different process that has taken it.
		FSpoutSdkSenderNameBackend Backend;
		Registration.Release(Backend);
	}
}

FSpoutSenderRegistry& FSpoutSenderRegistry::Get()
{
	static FSpoutSenderRegistry Instance;
	return Instance;
}

TSharedPtr<FSpoutSharedSender> FSpoutSenderRegistry::FindOrAdd(const FName& Name, ESpoutType Type)
{
	// Registry access is shared across game/render threads.
	FScopeLock Lock(&Mutex);

	const TPair<FName, ESpoutType> Key(Name, Type);
	if (TSharedPtr<FSpoutSharedSender>* Existing = Senders.Find(Key))
	{
		return *Existing;
	}

	TSharedPtr<FSpoutSharedSender> NewSender = MakeShared<FSpoutSharedSender>(Name, Type);
	Senders.Add(Key, NewSender);
	return NewSender;
}

TSharedPtr<FSpoutSharedSender> FSpoutSenderRegistry::Find(const FName& Name, ESpoutType Type)
{
	FScopeLock Lock(&Mutex);
	const TPair<FName, ESpoutType> Key(Name, Type);
	if (TSharedPtr<FSpoutSharedSender>* Existing = Senders.Find(Key))
	{
		return *Existing;
	}
	return nullptr;
}

void FSpoutSenderRegistry::Remove(const FName& Name, ESpoutType Type)
{
	// Move the entry out under the lock and let it destruct after the lock is dropped. Destroying it
	// in place would run ~FSpoutSharedSender -- which takes the entry's ResourceMutex and then the
	// process-local Spout mutex -- while still holding the registry mutex, nesting three locks for
	// no reason. Keeping registry removal to a plain map operation avoids that widened lock scope.
	TSharedPtr<FSpoutSharedSender> Removed;
	{
		FScopeLock Lock(&Mutex);
		Senders.RemoveAndCopyValue(TPair<FName, ESpoutType>(Name, Type), Removed);
	}
}

void FSpoutSenderRegistry::Clear()
{
	// Same reasoning as Remove, for every entry at once.
	TArray<TSharedPtr<FSpoutSharedSender>> Removed;
	{
		FScopeLock Lock(&Mutex);
		Senders.GenerateValueArray(Removed);
		Senders.Empty();
	}
}

int32 FSpoutSenderRegistry::Num() const
{
	FScopeLock Lock(&Mutex);
	return Senders.Num();
}
