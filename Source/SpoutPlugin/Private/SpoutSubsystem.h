// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Records which world uses each Spout stream, so that the end of a Play In Editor session closes that session's
 * streams and leaves the others running (an editor tool's stream, or another Play world's).
 *
 * Streams are opened through static functions that take no world, so the world is worked out at each call: the
 * context object's world when there is one (the placeable actors pass themselves), else the innermost Blueprint frame
 * whose object lives in a world, else the world being ticked. A stream whose world cannot be told is treated as part
 * of the running Play session, which is how every stream was treated before.
 *
 * Exists in the editor only. A packaged game never closes streams when a world is cleaned up, because a level travel
 * must not stop a sender, so it has nothing to record.
 */
#pragma once

#include "CoreMinimal.h"
#include "SpoutBPFunctionLibrary.h"
#include "Subsystems/EngineSubsystem.h"
#include "UObject/ObjectKey.h"
#include "SpoutSubsystem.generated.h"

class UWorld;

UCLASS()
class USpoutSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Records that a stream was used, from Context's world or, without one, from the calling world. Game thread. */
	static void NoteStreamUse(FName Name, ESpoutType Type, const UObject* Context = nullptr);

	/** Records that a stream was used from World; null when no world can be told. Game thread. */
	static void RecordStream(FName Name, ESpoutType Type, const UWorld* World);

	/** Forgets a closed stream. */
	static void NoteStreamClosed(FName Name, ESpoutType Type);

	/** Forgets every stream (Close All Streams, shutdown). */
	static void NoteAllStreamsClosed();

	/**
	 * The live worlds recorded for a stream, for tests and diagnostics. Empty for a stream used from no known world.
	 * Returns false when the stream is not recorded at all.
	 */
	static bool GetStreamWorlds(FName Name, ESpoutType Type, TArray<const UWorld*>& OutWorlds);

private:
#if WITH_EDITOR
	void OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);

	struct FStreamUse
	{
		/** The worlds that used the stream. Empty when no world could be told. */
		TSet<FObjectKey> Worlds;
	};

	TMap<TPair<FName, ESpoutType>, FStreamUse> Streams;
	FDelegateHandle WorldCleanupHandle;
#endif

	static USpoutSubsystem* Instance;
};
