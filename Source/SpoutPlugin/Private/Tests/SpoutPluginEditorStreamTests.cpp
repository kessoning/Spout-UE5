// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// Sending and receiving while the level is edited (Send In Editor, Receive In Editor, the Startup Policy), with the
// Spout Sender and Spout Receiver actors in a level-editor world and real transfers through this process. Covered:
// the defaults, the policy, a render target sent and received in the editor, the paused Status shown while no level
// viewport is Realtime, the hand-over to Play's copy of a sender and back, the viewport sources waiting without a level
// viewport, and closing a level. The level viewport's image itself, and Realtime itself, are checked by hand.
#include "Misc/AutomationTest.h"
#if SPOUT_WITH_TESTS && PLATFORM_WINDOWS && WITH_EDITOR
#include "SpoutBPFunctionLibrary.h"
#include "SpoutModule.h"
#include "SpoutReceiverActor.h"
#include "SpoutSenderActor.h"
#include "SpoutSettings.h"
#include "SpoutSubsystem.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SWidget.h"

#define EditorStreamTestFlags (EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace SpoutModuleTestHooks
{
	// SpoutModule.cpp.
	TFunction<TSharedPtr<SWidget>(bool&)> ExchangeLevelViewportProvider(TFunction<TSharedPtr<SWidget>(bool&)> Provider);
}

namespace
{
	/** Sets the project's Startup Policy for the test and puts it back afterwards. */
	struct FScopedStartupPolicy
	{
		ESpoutStartupPolicy Before;
		explicit FScopedStartupPolicy(ESpoutStartupPolicy Policy) : Before(GetDefault<USpoutSettings>()->StartupPolicy)
		{
			GetMutableDefault<USpoutSettings>()->StartupPolicy = Policy;
		}
		~FScopedStartupPolicy() { GetMutableDefault<USpoutSettings>()->StartupPolicy = Before; }
		void Set(ESpoutStartupPolicy Policy) { GetMutableDefault<USpoutSettings>()->StartupPolicy = Policy; }
	};

	/** A world of one type, destroyed (and so cleaned up) at the end of the scope. */
	struct FScopedWorld
	{
		UWorld* World;
		explicit FScopedWorld(EWorldType::Type Type) : World(UWorld::CreateWorld(Type, false)) {}
		~FScopedWorld()
		{
			if (World)
			{
				World->DestroyWorld(false);
			}
			FlushRenderingCommands();
		}
	};

	ASpoutSenderActor* SpawnRenderTargetSender(UWorld* World, FName Name, UTextureRenderTarget2D* Target)
	{
		ASpoutSenderActor* Sender = World->SpawnActor<ASpoutSenderActor>();
		if (Sender)
		{
			Sender->SpoutName = Name;
			Sender->SendFrom = ESpoutSendTextureFrom::TextureRenderTarget2D;
			Sender->TextureRenderTarget = Target;
			Sender->bSendInEditor = true;
		}
		return Sender;
	}

	bool IsSending(FName Name)
	{
		return USpoutBPFunctionLibrary::GetSpoutStreamStatus(Name, ESpoutType::Sender).bActive;
	}

	/** Deleting an actor in a world made for the test (it has no engine world context) makes Unreal warn about that. */
	void ExpectContextWarning(FAutomationTestBase& Test)
	{
		Test.AddExpectedMessage(TEXT("World has no context"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	}

	void Step(AActor* A, AActor* B = nullptr)
	{
		if (A) A->Tick(1.f / 60.f);
		FlushRenderingCommands();
		if (B) B->Tick(1.f / 60.f);
		FlushRenderingCommands();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutEditorStreamsDefaults, "SpoutPlugin.EditorStreams.OffByDefault", EditorStreamTestFlags)
bool FSpoutEditorStreamsDefaults::RunTest(const FString&)
{
	TestFalse(TEXT("Send In Editor is off by default"), GetDefault<ASpoutSenderActor>()->bSendInEditor);
	TestFalse(TEXT("Receive In Editor is off by default"), GetDefault<ASpoutReceiverActor>()->bReceiveInEditor);
	TestFalse(TEXT("a sender does not tick in the level editor by default"), GetDefault<ASpoutSenderActor>()->ShouldTickIfViewportsOnly());
	TestFalse(TEXT("a receiver does not tick in the level editor by default"), GetDefault<ASpoutReceiverActor>()->ShouldTickIfViewportsOnly());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutEditorStreamsPolicy, "SpoutPlugin.EditorStreams.PlayOnlyPolicyKeepsTheEditorQuiet", EditorStreamTestFlags)
bool FSpoutEditorStreamsPolicy::RunTest(const FString&)
{
	ExpectContextWarning(*this);
	FScopedStartupPolicy Policy(ESpoutStartupPolicy::PlayOnly);
	FScopedWorld Editor(EWorldType::Editor);
	TStrongObjectPtr<UTextureRenderTarget2D> Target(USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(64, 32, PF_B8G8R8A8, true));
	const FName Name(TEXT("SpoutEditorTest_Policy_3b8e"));
	ASpoutSenderActor* Sender = SpawnRenderTargetSender(Editor.World, Name, Target.Get());
	if (!TestNotNull(TEXT("sender placed"), Sender)) return false;

	TestTrue(TEXT("with Send In Editor on, it ticks in the level editor"), Sender->ShouldTickIfViewportsOnly());
	Step(Sender);
	TestFalse(TEXT("with the Startup Policy at Play only, nothing is sent"), IsSending(Name));
	TestTrue(TEXT("and Status says why"), Sender->Status.Contains(TEXT("Startup Policy is Play only")));
	Sender->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutEditorStreamsLoopback, "SpoutPlugin.EditorStreams.SendAndReceiveInTheEditor", EditorStreamTestFlags)
bool FSpoutEditorStreamsLoopback::RunTest(const FString&)
{
	ExpectContextWarning(*this);
	FScopedStartupPolicy Policy(ESpoutStartupPolicy::EditorAndPlay);
	FScopedWorld Editor(EWorldType::Editor);
	TStrongObjectPtr<UTextureRenderTarget2D> Target(USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(64, 32, PF_B8G8R8A8, true));
	const FName Name(TEXT("SpoutEditorTest_Loopback_3b8e"));
	ASpoutSenderActor* Sender = SpawnRenderTargetSender(Editor.World, Name, Target.Get());
	ASpoutReceiverActor* Receiver = Editor.World->SpawnActor<ASpoutReceiverActor>();
	if (!TestNotNull(TEXT("sender placed"), Sender) || !TestNotNull(TEXT("receiver placed"), Receiver)) return false;
	Receiver->SenderName = Name;
	Receiver->bReceiveInEditor = true;

	// Frames go through the real path: the sender's render-thread copy, then the receive worker. Bounded.
	for (int32 Attempt = 0; Attempt < 300 && Receiver->State != ESpoutReceiverActorState::Receiving; ++Attempt)
	{
		Step(Sender, Receiver);
		FPlatformProcess::Sleep(0.005f);
	}
	TestTrue(TEXT("the sender sends in the level editor"), IsSending(Name));
	TestTrue(*FString::Printf(TEXT("the receiver receives in the level editor (status: %s)"), *Receiver->Status), Receiver->State == ESpoutReceiverActorState::Receiving);
	TestEqual(TEXT("at the render target's size"), Receiver->ReceivedSize, FIntPoint(64, 32));
	TArray<const UWorld*> Worlds;
	TestTrue(TEXT("the stream belongs to the editor's world"), USpoutSubsystem::GetStreamWorlds(Name, ESpoutType::Sender, Worlds) && Worlds.Contains(Editor.World));
	if (UMaterialInstanceDynamic* Display = Receiver->GetDisplayMaterialInstance())
	{
		TestTrue(TEXT("the screen's material instance is never saved with the level"), Display->HasAnyFlags(RF_Transient));
	}

	// Back to Play only: both stop at their next tick.
	Policy.Set(ESpoutStartupPolicy::PlayOnly);
	Step(Sender, Receiver);
	TestFalse(TEXT("the sender stops"), IsSending(Name));
	TestTrue(TEXT("the receiver stops"), Receiver->State == ESpoutReceiverActorState::Disabled);
	Receiver->Destroy();
	Sender->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutEditorStreamsPaused, "SpoutPlugin.EditorStreams.PausedWithoutRealtimeSaysSo", EditorStreamTestFlags)
bool FSpoutEditorStreamsPaused::RunTest(const FString&)
{
	// Without a Realtime level viewport the editor calls ShowPausedInEditor on each Spout actor (the actors themselves do
	// not tick then); their next tick shows their own state again.
	ExpectContextWarning(*this);
	FScopedStartupPolicy Policy(ESpoutStartupPolicy::EditorAndPlay);
	FScopedWorld Editor(EWorldType::Editor);
	TStrongObjectPtr<UTextureRenderTarget2D> Target(USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(64, 32, PF_B8G8R8A8, true));
	const FName Name(TEXT("SpoutEditorTest_Paused_3b8e"));
	ASpoutSenderActor* Sender = SpawnRenderTargetSender(Editor.World, Name, Target.Get());
	ASpoutReceiverActor* Receiver = Editor.World->SpawnActor<ASpoutReceiverActor>();
	ASpoutSenderActor* Idle = Editor.World->SpawnActor<ASpoutSenderActor>();
	if (!TestNotNull(TEXT("sender placed"), Sender) || !TestNotNull(TEXT("receiver placed"), Receiver) || !TestNotNull(TEXT("second sender placed"), Idle)) return false;
	Receiver->SenderName = Name;
	Receiver->bReceiveInEditor = true;
	for (int32 Attempt = 0; Attempt < 300 && Receiver->State != ESpoutReceiverActorState::Receiving; ++Attempt)
	{
		Step(Sender, Receiver);
		FPlatformProcess::Sleep(0.005f);
	}
	if (!TestTrue(*FString::Printf(TEXT("streaming in the level editor first (status: %s)"), *Receiver->Status), Receiver->State == ESpoutReceiverActorState::Receiving)) return false;

	TestTrue(TEXT("the sender takes the pause"), Sender->ShowPausedInEditor());
	TestTrue(TEXT("the receiver takes the pause"), Receiver->ShowPausedInEditor());
	TestTrue(*FString::Printf(TEXT("the sender's Status says why (%s)"), *Sender->Status), Sender->Status.StartsWith(TEXT("Paused")) && Sender->Status.Contains(TEXT("Realtime")));
	TestFalse(TEXT("and it is not publishing"), Sender->bPublishing);
	TestTrue(*FString::Printf(TEXT("the receiver's Status says why (%s)"), *Receiver->Status), Receiver->Status.StartsWith(TEXT("Paused")) && Receiver->Status.Contains(TEXT("Realtime")));
	TestTrue(TEXT("a second call keeps it"), Sender->ShowPausedInEditor() && Sender->Status.StartsWith(TEXT("Paused")));
	const FString IdleBefore = Idle->Status;
	TestFalse(TEXT("a sender that does not send in the editor is left alone"), Idle->ShowPausedInEditor());
	TestEqual(TEXT("its Status unchanged"), Idle->Status, IdleBefore);

	// The level ticks again: both show their own state.
	for (int32 Attempt = 0; Attempt < 60 && (!Sender->bPublishing || Receiver->Status.StartsWith(TEXT("Paused"))); ++Attempt)
	{
		Step(Sender, Receiver);
		FPlatformProcess::Sleep(0.005f);
	}
	TestFalse(*FString::Printf(TEXT("the sender's Status is its own again (%s)"), *Sender->Status), Sender->Status.StartsWith(TEXT("Paused")));
	TestFalse(*FString::Printf(TEXT("the receiver's Status is its own again (%s)"), *Receiver->Status), Receiver->Status.StartsWith(TEXT("Paused")));

	// With the Startup Policy at Play only nothing works in the editor, so nothing is paused either.
	Policy.Set(ESpoutStartupPolicy::PlayOnly);
	Step(Sender, Receiver);
	TestFalse(TEXT("a stopped sender is not shown as paused"), Sender->ShowPausedInEditor());
	TestFalse(TEXT("a stopped receiver is not shown as paused"), Receiver->ShowPausedInEditor());
	Receiver->Destroy();
	Idle->Destroy();
	Sender->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutEditorStreamsHandOver, "SpoutPlugin.EditorStreams.PlayTakesTheNameAndGivesItBack", EditorStreamTestFlags)
bool FSpoutEditorStreamsHandOver::RunTest(const FString&)
{
	ExpectContextWarning(*this);
	FScopedStartupPolicy Policy(ESpoutStartupPolicy::EditorAndPlay);
	FScopedWorld Editor(EWorldType::Editor);
	FScopedWorld Play(EWorldType::PIE);
	TStrongObjectPtr<UTextureRenderTarget2D> Target(USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(64, 32, PF_B8G8R8A8, true));
	const FName Name(TEXT("SpoutEditorTest_HandOver_3b8e"));
	ASpoutSenderActor* InEditor = SpawnRenderTargetSender(Editor.World, Name, Target.Get());
	if (!TestNotNull(TEXT("editor sender placed"), InEditor)) return false;
	Step(InEditor);
	TestTrue(TEXT("the editor's sender sends"), IsSending(Name));

	// Play's copy of the sender, with the same name.
	ASpoutSenderActor* InPlay = SpawnRenderTargetSender(Play.World, Name, Target.Get());
	if (!TestNotNull(TEXT("Play sender spawned"), InPlay)) return false;
	Step(InPlay);
	TestTrue(*FString::Printf(TEXT("Play's sender takes the name (status: %s)"), *InPlay->Status), InPlay->Status.StartsWith(TEXT("Request accepted")) || InPlay->Status.StartsWith(TEXT("Sending")));
	TestTrue(TEXT("the editor's sender pauses"), InEditor->Status.StartsWith(TEXT("Paused while Play runs")));
	TestTrue(TEXT("the stream is now Play's"), IsSending(Name));
	Step(InEditor);
	TestTrue(TEXT("the editor's sender stays paused while Play's sends"), InEditor->Status.StartsWith(TEXT("Paused while Play runs")));

	// Play ends: its sender goes, the editor's resumes.
	InPlay->Destroy();
	FlushRenderingCommands();
	Step(InEditor);
	TestTrue(*FString::Printf(TEXT("the editor's sender resumes (status: %s)"), *InEditor->Status), IsSending(Name) && !InEditor->Status.StartsWith(TEXT("Paused")));
	InEditor->Destroy();
	FlushRenderingCommands();
	TestFalse(TEXT("deleting the editor's sender closes its stream"), IsSending(Name));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutEditorStreamsNoViewport, "SpoutPlugin.EditorStreams.ViewportSourcesWaitForALevelViewport", EditorStreamTestFlags)
bool FSpoutEditorStreamsNoViewport::RunTest(const FString&)
{
	ExpectContextWarning(*this);
	// The editor running the tests may have a level viewport: it is taken away for this test and put back after it.
	TFunction<TSharedPtr<SWidget>(bool&)> Previous = SpoutModuleTestHooks::ExchangeLevelViewportProvider(nullptr);
	ON_SCOPE_EXIT { SpoutModuleTestHooks::ExchangeLevelViewportProvider(MoveTemp(Previous)); };
	bool bRealtime = false;
	TestFalse(TEXT("no level viewport"), FSpoutModule::GetLevelViewport(bRealtime).IsValid());
	FScopedStartupPolicy Policy(ESpoutStartupPolicy::EditorAndPlay);
	FScopedWorld Editor(EWorldType::Editor);
	const FName Name(TEXT("SpoutEditorTest_NoViewport_3b8e"));
	ASpoutSenderActor* Sender = Editor.World->SpawnActor<ASpoutSenderActor>();
	if (!TestNotNull(TEXT("sender placed"), Sender)) return false;
	Sender->SpoutName = Name;
	Sender->bSendInEditor = true;
	for (const ESpoutSendTextureFrom From : { ESpoutSendTextureFrom::GameViewportOnly, ESpoutSendTextureFrom::GameViewport })
	{
		Sender->SendFrom = From;
		Step(Sender);
		TestEqual(TEXT("without a level viewport it waits for one"), Sender->Status, FString(TEXT("Waiting for a level viewport")));
		TestFalse(TEXT("and sends nothing"), Sender->bPublishing);
	}
	Sender->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutEditorStreamsLevelClosed, "SpoutPlugin.EditorStreams.ClosingALevelClosesItsStreams", EditorStreamTestFlags)
bool FSpoutEditorStreamsLevelClosed::RunTest(const FString&)
{
	FScopedStartupPolicy Policy(ESpoutStartupPolicy::EditorAndPlay);
	TStrongObjectPtr<UTextureRenderTarget2D> Target(USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(64, 32, PF_B8G8R8A8, true));
	const FName Name(TEXT("SpoutEditorTest_LevelClosed_3b8e"));
	{
		FScopedWorld Editor(EWorldType::Editor);
		ASpoutSenderActor* Sender = SpawnRenderTargetSender(Editor.World, Name, Target.Get());
		if (!TestNotNull(TEXT("sender placed"), Sender)) return false;
		Step(Sender);
		TestTrue(TEXT("sending in the level"), IsSending(Name));
		// The level closes without deleting its actors first, as when the editor opens another level.
	}
	TestFalse(TEXT("closing the level closes its stream"), IsSending(Name));
	return true;
}
#endif
