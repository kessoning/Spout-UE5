// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// Which streams the end of a Play In Editor world closes: the streams that world used, and those whose world is not
// known, while other worlds' streams stay open; with no other world using a stream, everything closes as before.
// Worlds are created and destroyed directly (destroying one runs the real world-cleanup event) and streams are set up
// in the real registry, so no external sender is needed.
#include "Misc/AutomationTest.h"
#if SPOUT_WITH_TESTS && PLATFORM_WINDOWS && WITH_EDITOR
#include "SpoutBPFunctionLibrary.h"
#include "SpoutSenderRegistry.h"
#include "SpoutSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "RenderingThread.h"

#define WorldTestFlags (EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	/** A world of one type; ending it destroys it, which runs the world-cleanup event. */
	struct FTestWorld
	{
		UWorld* World = nullptr;

		explicit FTestWorld(EWorldType::Type Type) : World(UWorld::CreateWorld(Type, false)) {}
		~FTestWorld() { End(); }

		void End()
		{
			if (World)
			{
				World->DestroyWorld(false);
				World = nullptr;
			}
			FlushRenderingCommands();
		}
	};

	void Open(FName Name, ESpoutType Type, const UWorld* World)
	{
		FSpoutSenderRegistry::Get().FindOrAdd(Name, Type);
		USpoutSubsystem::RecordStream(Name, Type, World);
	}

	bool IsOpen(FName Name, ESpoutType Type)
	{
		return FSpoutSenderRegistry::Get().Find(Name, Type).IsValid();
	}

	void Close(FName Name, ESpoutType Type)
	{
		if (Type == ESpoutType::Sender)
		{
			USpoutBPFunctionLibrary::CloseSender(Name);
		}
		else
		{
			UTexture2D* Texture = nullptr;
			UMaterialInstanceDynamic* Material = nullptr;
			USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material);
		}
		FlushRenderingCommands();
	}

	/** The editor's own world, which a Play session's end must leave alone. */
	UWorld* EditorWorld()
	{
		UWorld* World = GWorld;
		return World && World->WorldType == EWorldType::Editor ? World : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutWorldsSubsystemExists, "SpoutPlugin.Worlds.SubsystemExistsInTheEditor", WorldTestFlags)
bool FSpoutWorldsSubsystemExists::RunTest(const FString&)
{
	TestNotNull(TEXT("the subsystem that records stream worlds exists in the editor"),
		GEngine ? GEngine->GetEngineSubsystem<USpoutSubsystem>() : nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutWorldsEndingOneKeepsAnother, "SpoutPlugin.Worlds.EndingOnePlayWorldKeepsAnothersStreams", WorldTestFlags)
bool FSpoutWorldsEndingOneKeepsAnother::RunTest(const FString&)
{
	const FName SenderA(TEXT("SpoutWorldsTest_SenderA_5e3d"));
	const FName ReceiverB(TEXT("SpoutWorldsTest_ReceiverB_5e3d"));
	const FName Shared(TEXT("SpoutWorldsTest_Shared_5e3d"));
	FTestWorld A(EWorldType::PIE), B(EWorldType::PIE);
	if (!TestNotNull(TEXT("world A"), A.World) || !TestNotNull(TEXT("world B"), B.World)) return false;

	Open(SenderA, ESpoutType::Sender, A.World);
	Open(ReceiverB, ESpoutType::Receiver, B.World);
	Open(Shared, ESpoutType::Receiver, A.World);
	Open(Shared, ESpoutType::Receiver, B.World);

	A.End();
	TestFalse(TEXT("the ending world's sender is closed"), IsOpen(SenderA, ESpoutType::Sender));
	TestTrue(TEXT("another Play world's receiver stays open"), IsOpen(ReceiverB, ESpoutType::Receiver));
	TestTrue(TEXT("a stream both worlds used stays open for the one still running"), IsOpen(Shared, ESpoutType::Receiver));
	TArray<const UWorld*> Worlds;
	TestTrue(TEXT("the shared stream is still recorded"), USpoutSubsystem::GetStreamWorlds(Shared, ESpoutType::Receiver, Worlds));
	TestTrue(TEXT("now for the remaining world only"), Worlds.Num() == 1 && Worlds[0] == B.World);

	B.End();
	TestFalse(TEXT("the last world's end closes its receiver"), IsOpen(ReceiverB, ESpoutType::Receiver));
	TestFalse(TEXT("and the shared stream"), IsOpen(Shared, ESpoutType::Receiver));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutWorldsEditorStreamsSurvivePlay, "SpoutPlugin.Worlds.EndingPlayKeepsEditorWorldStreams", WorldTestFlags)
bool FSpoutWorldsEditorStreamsSurvivePlay::RunTest(const FString&)
{
	UWorld* Editor = EditorWorld();
	if (!Editor)
	{
		AddInfo(TEXT("No editor world in this session; nothing to check."));
		return true;
	}
	const FName EditorStream(TEXT("SpoutWorldsTest_Editor_5e3d"));
	const FName PlayStream(TEXT("SpoutWorldsTest_Play_5e3d"));
	FTestWorld Play(EWorldType::PIE);
	Open(EditorStream, ESpoutType::Sender, Editor);
	Open(PlayStream, ESpoutType::Sender, Play.World);

	Play.End();
	TestFalse(TEXT("the Play world's sender is closed"), IsOpen(PlayStream, ESpoutType::Sender));
	TestTrue(TEXT("the editor world's sender keeps running"), IsOpen(EditorStream, ESpoutType::Sender));

	Close(EditorStream, ESpoutType::Sender);
	TArray<const UWorld*> Worlds;
	TestFalse(TEXT("a closed stream is forgotten"), USpoutSubsystem::GetStreamWorlds(EditorStream, ESpoutType::Sender, Worlds));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutWorldsUnknownWorldClosesWithPlay, "SpoutPlugin.Worlds.StreamWithNoKnownWorldClosesWithPlay", WorldTestFlags)
bool FSpoutWorldsUnknownWorldClosesWithPlay::RunTest(const FString&)
{
	UWorld* Editor = EditorWorld();
	if (!Editor)
	{
		AddInfo(TEXT("No editor world in this session; nothing to check."));
		return true;
	}
	const FName Unknown(TEXT("SpoutWorldsTest_Unknown_5e3d"));
	const FName EditorStream(TEXT("SpoutWorldsTest_EditorKeep_5e3d"));
	FTestWorld Play(EWorldType::PIE);
	Open(Unknown, ESpoutType::Receiver, nullptr);
	Open(EditorStream, ESpoutType::Receiver, Editor);

	Play.End();
	TestFalse(TEXT("a stream whose world is not known closes with Play, as every stream did before"), IsOpen(Unknown, ESpoutType::Receiver));
	TestTrue(TEXT("the editor world's receiver stays open"), IsOpen(EditorStream, ESpoutType::Receiver));
	Close(EditorStream, ESpoutType::Receiver);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutWorldsGameWorldEndClosesNothing, "SpoutPlugin.Worlds.EndOfAGameWorldClosesNothing", WorldTestFlags)
bool FSpoutWorldsGameWorldEndClosesNothing::RunTest(const FString&)
{
	// A game world ends at every level travel; its senders are meant to survive it.
	const FName Name(TEXT("SpoutWorldsTest_Game_5e3d"));
	FTestWorld Game(EWorldType::Game);
	Open(Name, ESpoutType::Sender, Game.World);
	Game.End();
	TestTrue(TEXT("the end of a game world closes nothing"), IsOpen(Name, ESpoutType::Sender));
	Close(Name, ESpoutType::Sender);
	TestFalse(TEXT("Close Sender still closes it"), IsOpen(Name, ESpoutType::Sender));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutWorldsCloseAllUnchanged, "SpoutPlugin.Worlds.CloseAllStreamsClosesEveryWorld", WorldTestFlags)
bool FSpoutWorldsCloseAllUnchanged::RunTest(const FString&)
{
	const FName InPlay(TEXT("SpoutWorldsTest_AllPlay_5e3d"));
	const FName InOther(TEXT("SpoutWorldsTest_AllOther_5e3d"));
	FTestWorld Play(EWorldType::PIE), Other(EWorldType::PIE);
	Open(InPlay, ESpoutType::Sender, Play.World);
	Open(InOther, ESpoutType::Receiver, Other.World);

	USpoutBPFunctionLibrary::CloseAllStreams();
	FlushRenderingCommands();
	TestFalse(TEXT("Close All Streams closes a Play world's stream"), IsOpen(InPlay, ESpoutType::Sender));
	TestFalse(TEXT("and every other world's"), IsOpen(InOther, ESpoutType::Receiver));
	TArray<const UWorld*> Worlds;
	TestFalse(TEXT("and forgets them"), USpoutSubsystem::GetStreamWorlds(InPlay, ESpoutType::Sender, Worlds)
		|| USpoutSubsystem::GetStreamWorlds(InOther, ESpoutType::Receiver, Worlds));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutWorldsCallingWorldIsRecorded, "SpoutPlugin.Worlds.CallingWorldIsRecorded", WorldTestFlags)
bool FSpoutWorldsCallingWorldIsRecorded::RunTest(const FString&)
{
	const FName Name(TEXT("SpoutWorldsTest_Calling_5e3d"));
	FTestWorld Play(EWorldType::PIE);
	TArray<const UWorld*> Worlds;

	// The placeable actors pass themselves; any object in a world gives that world.
	USpoutSubsystem::NoteStreamUse(Name, ESpoutType::Receiver, Play.World->PersistentLevel);
	TestTrue(TEXT("recorded"), USpoutSubsystem::GetStreamWorlds(Name, ESpoutType::Receiver, Worlds));
	TestTrue(TEXT("for the context object's world"), Worlds.Num() == 1 && Worlds[0] == Play.World);

	// Native code with no context and no Blueprint frame: the world being ticked (here the editor's, no Play running).
	USpoutSubsystem::NoteStreamUse(Name, ESpoutType::Receiver);
	USpoutSubsystem::GetStreamWorlds(Name, ESpoutType::Receiver, Worlds);
	if (UWorld* Editor = EditorWorld())
	{
		TestTrue(TEXT("a native call outside Play is recorded for the ticking world"), Worlds.Contains(Editor));
	}

	USpoutSubsystem::NoteStreamClosed(Name, ESpoutType::Receiver);
	TestFalse(TEXT("a closed stream is forgotten"), USpoutSubsystem::GetStreamWorlds(Name, ESpoutType::Receiver, Worlds));
	return true;
}
#endif
