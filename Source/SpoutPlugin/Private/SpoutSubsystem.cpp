// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#include "SpoutSubsystem.h"

#include "SpoutModule.h"
#include "SpoutReceiver.h"
#include "SpoutReceiverOwnership.h"
#include "SpoutSender.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/Script.h"
#include "UObject/Stack.h"

USpoutSubsystem* USpoutSubsystem::Instance = nullptr;

#if WITH_EDITOR
namespace
{
	const UWorld* WorldOf(const UObject* Object)
	{
		// Class defaults and archetypes belong to no world.
		if (!Object || Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			return nullptr;
		}
		return Object->GetWorld();
	}

	bool IsPlaySessionRunning()
	{
		if (!GEngine)
		{
			return false;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World())
			{
				return true;
			}
		}
		return false;
	}

	/** The world a stream call comes from, or null when it cannot be told (see SpoutSubsystem.h). */
	const UWorld* ResolveCallingWorld(const UObject* Context)
	{
		if (const UWorld* World = WorldOf(Context))
		{
			return World;
		}

#if DO_BLUEPRINT_GUARD
		// Innermost first. A Blueprint function library's own frame runs on its class default object, which has no
		// world, so the search carries on to the graph that called it.
		const TArrayView<const FFrame* const> Stack = FBlueprintContextTracker::Get().GetCurrentScriptStack();
		for (int32 Index = Stack.Num() - 1; Index >= 0; --Index)
		{
			if (const UWorld* World = Stack[Index] ? WorldOf(Stack[Index]->Object) : nullptr)
			{
				return World;
			}
		}
#endif

		// Native code: the world being ticked. While a Play session runs, the editor's world here means a caller outside
		// any world's tick, which cannot be placed.
		UWorld* Ticking = GWorld;
		if (Ticking && Ticking->WorldType != EWorldType::PIE && IsPlaySessionRunning())
		{
			return nullptr;
		}
		return Ticking;
	}
}
#endif

bool USpoutSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if WITH_EDITOR
	return GIsEditor && Super::ShouldCreateSubsystem(Outer);
#else
	return false;
#endif
}

void USpoutSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Instance = this;
#if WITH_EDITOR
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(this, &USpoutSubsystem::OnWorldCleanup);
#endif
}

void USpoutSubsystem::Deinitialize()
{
#if WITH_EDITOR
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
	WorldCleanupHandle.Reset();
	Streams.Reset();
#endif
	if (Instance == this)
	{
		Instance = nullptr;
	}
	Super::Deinitialize();
}

void USpoutSubsystem::NoteStreamUse(FName Name, ESpoutType Type, const UObject* Context)
{
#if WITH_EDITOR
	if (Instance)
	{
		RecordStream(Name, Type, ResolveCallingWorld(Context));
	}
#endif
}

void USpoutSubsystem::RecordStream(FName Name, ESpoutType Type, const UWorld* World)
{
#if WITH_EDITOR
	check(IsInGameThread());
	if (!Instance || Name.IsNone())
	{
		return;
	}
	FStreamUse& Use = Instance->Streams.FindOrAdd(TPair<FName, ESpoutType>(Name, Type));
	if (World)
	{
		Use.Worlds.Add(FObjectKey(World));
	}
#endif
}

void USpoutSubsystem::NoteStreamClosed(FName Name, ESpoutType Type)
{
#if WITH_EDITOR
	check(IsInGameThread());
	if (Instance)
	{
		Instance->Streams.Remove(TPair<FName, ESpoutType>(Name, Type));
	}
#endif
}

void USpoutSubsystem::NoteAllStreamsClosed()
{
#if WITH_EDITOR
	check(IsInGameThread());
	if (Instance)
	{
		Instance->Streams.Reset();
	}
#endif
}

bool USpoutSubsystem::GetStreamWorlds(FName Name, ESpoutType Type, TArray<const UWorld*>& OutWorlds)
{
	OutWorlds.Reset();
#if WITH_EDITOR
	const FStreamUse* Use = Instance ? Instance->Streams.Find(TPair<FName, ESpoutType>(Name, Type)) : nullptr;
	if (!Use)
	{
		return false;
	}
	for (const FObjectKey& Key : Use->Worlds)
	{
		if (const UWorld* World = Cast<UWorld>(Key.ResolveObjectPtr()))
		{
			OutWorlds.Add(World);
		}
	}
	return true;
#else
	return false;
#endif
}

#if WITH_EDITOR
void USpoutSubsystem::OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	// Play In Editor worlds only. A game world is cleaned up at every level travel, and closing streams there would
	// stop senders that are meant to survive it.
	if (!World || World->WorldType != EWorldType::PIE)
	{
		return;
	}

	const FObjectKey Ending(World);
	TArray<TPair<FName, ESpoutType>> ToClose;
	bool bOtherWorldsKeepStreams = false;
	for (TPair<TPair<FName, ESpoutType>, FStreamUse>& Stream : Streams)
	{
		TSet<FObjectKey>& Worlds = Stream.Value.Worlds;
		Worlds.Remove(Ending);
		for (auto It = Worlds.CreateIterator(); It; ++It)
		{
			// A world that no longer exists holds nothing open.
			if (!It->ResolveObjectPtr())
			{
				It.RemoveCurrent();
			}
		}
		if (Worlds.Num() == 0)
		{
			ToClose.Add(Stream.Key);
		}
		else
		{
			bOtherWorldsKeepStreams = true;
		}
	}

	if (!bOtherWorldsKeepStreams)
	{
		// No other world uses a stream: release everything, exactly as the end of Play always did. Only this full close
		// also frees the viewport capture textures; the interop device is kept for the next session.
		USpoutBPFunctionLibrary::CloseAllStreams();
		return;
	}

	for (const TPair<FName, ESpoutType>& Stream : ToClose)
	{
		if (Stream.Value == ESpoutType::Sender)
		{
			FSpoutSender::Close(Stream.Key);
		}
		else
		{
			// What Close Receiver does, without a caller's variables to clear.
			FSpoutReceiver::Close(Stream.Key);
			if (FSpoutReceiverOwnership* Ownership = FSpoutReceiverOwnership::Peek())
			{
				Ownership->Release(Stream.Key);
			}
		}
		Streams.Remove(Stream);
	}
	UE_LOG(LogSpoutPlugin, Log,
		TEXT("End of Play in '%s': closed %d stream(s) this world used; %d stream(s) of other worlds stay open."),
		*World->GetName(), ToClose.Num(), Streams.Num());
}
#endif
