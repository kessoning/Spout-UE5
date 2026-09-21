// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#include "SpoutHostTestSmoke.h"

#include "SpoutHostTestDiagnostics.h"
#include "SpoutHostTestModule.h"
#include "SpoutHostTestReport.h"
#include "SpoutHostTestViewportProbe.h"
#include "SpoutHostTestPixels.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "TextureResource.h"

#include "SpoutBPFunctionLibrary.h"

#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
// MaterialDomain.h and not just Material.h: on UE 5.3 the latter only forward-declares
// EMaterialDomain, so MD_Surface is undeclared without it.
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
// Same macro the plugin's SpoutSender.cpp keys its delegate-signature branch on. Evaluating the
// identical expression against the identical engine headers is what lets this harness state which
// viewport access path the plugin compiled (see ViewportAccessPathName below).
#include "Misc/EngineVersionComparison.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
// EWindowMode for the controlled resolution change below.
#include "GenericPlatform/GenericWindow.h"
// FSystemResolution, used for the controlled mid-run resolution change.
#include "UnrealEngine.h"

namespace SpoutHostTest
{
	namespace
	{
		// Frames allowed to elapse between a queued plugin call and the assertion that depends on
		// it. Three is enough for a render command to be enqueued, executed and retired; the
		// registration and travel steps below use their own, much larger, budgets.
		constexpr int32 SettleFrames = 3;

		// Sender registration is a round trip through the render thread and Spout's shared memory.
		// 600 frames is generous by design: this budget exists to turn a hang into a diagnosable
		// failure, not to be a tight assertion about how fast registration should be.
		constexpr int32 RegistrationBudgetFrames = 600;

		// Level travel tears down and rebuilds a world; on a cold package the first load can be slow.
		constexpr int32 TravelBudgetFrames = 1800;

		constexpr double DefaultDeadlineSeconds = 600.0;

		// GameViewportSenderSmoke thresholds. Conservative on purpose: the point is "frames are
		// genuinely advancing across real presents", not a performance assertion. Three distinct
		// new-frame observations and a sender frame count that moved by at least two cannot be
		// produced by a single accidental publish.
		constexpr int32 ViewportMinAdvanceObservations = 3;
		constexpr long ViewportMinFrameDelta = 2;

		// The controlled resize target. Different from the launch resolution (640x360) in both
		// dimensions, so a stale registry entry cannot satisfy the check by accident.
		constexpr int32 ViewportResizeWidth = 800;
		constexpr int32 ViewportResizeHeight = 450;

		// The second resize target: a third size, distinct from both the launch resolution and
		// the first resize, so each transition is provably a fresh swap-chain resize.
		constexpr int32 ViewportResize2Width = 960;
		constexpr int32 ViewportResize2Height = 540;

		// How many frames the host must stay alive after the post-close resize request. The
		// resize itself is processed within a frame or two; the margin is for slow machines.
		constexpr int32 PostCloseResizeSurviveFrames = 60;

		// Full detach/re-attach cycles driven through CloseAllStreams. Each one removes and
		// re-adds the back-buffer delegate, an operation the plugin performs on the rendering
		// thread on UE 5.3; three is enough to catch a wait that only sometimes completes.
		constexpr int32 ViewportRestartCycleCount = 3;

		/**
		 * Which back-buffer access path the plugin compiled on this engine.
		 *
		 * This is decided by the same UE_VERSION_OLDER_THAN(5, 8, 0) expression, against the same
		 * engine headers, as SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER in the plugin's
		 * SpoutSender.cpp. The two cannot disagree: the branches change the delegate lambda's
		 * parameter list, so a plugin built with the wrong branch for this engine would not
		 * compile at all, let alone publish. Frames advancing in this run therefore prove the
		 * named path executed, not merely that common code compiled.
		 */
		const TCHAR* ViewportAccessPathName()
		{
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
			return TEXT("SlateViewportProvider");
#else
			return TEXT("LegacyBackBuffer");
#endif
		}

		/**
		 * Which thread the plugin mutates the back-buffer delegate's invocation list on.
		 *
		 * Derived from the same UE_VERSION_OLDER_THAN(5, 4, 0) expression, against the same engine
		 * headers, as SPOUT_BACKBUFFER_DELEGATE_IS_THREAD_SAFE in the plugin's SpoutSender.cpp:
		 * UE 5.3 declares FOnBackBufferReadyToPresent non-thread-safe and so mutates it on the
		 * rendering thread, while 5.4+ use the thread-safe declaration and mutate from the game
		 * thread. Recording it here lets the matrix assert the right branch was compiled for the
		 * engine under test; the plugin additionally logs the ACTUAL thread it mutated on, which is
		 * what proves the branch executed rather than merely compiled.
		 */
		const TCHAR* ViewportDelegateMutationThreadName()
		{
#if UE_VERSION_OLDER_THAN(5, 4, 0)
			return TEXT("RenderThread");
#else
			return TEXT("GameThread");
#endif
		}

		int32 ParseIntSwitch(const TCHAR* Switch, int32 Default, int32 MinValue)
		{
			int32 Value = Default;
			if (FParse::Value(FCommandLine::Get(), Switch, Value))
			{
				return FMath::Max(Value, MinValue);
			}
			return Default;
		}

		FString ParseStringSwitch(const TCHAR* Switch, const TCHAR* Default)
		{
			FString Value;
			if (FParse::Value(FCommandLine::Get(), Switch, Value) && !Value.IsEmpty())
			{
				return Value;
			}
			return FString(Default);
		}
	}

	FSmokeRunner* FSmokeRunner::Instance = nullptr;

	bool FSmokeRunner::IsRequested()
	{
		return FParse::Param(FCommandLine::Get(), TEXT("SpoutSmoke"));
	}

	bool FSmokeRunner::WasStarted()
	{
		return Instance != nullptr;
	}

	bool FSmokeRunner::HasPassed()
	{
		return Instance != nullptr && Instance->bPassed && !Instance->bFailed;
	}

	void FSmokeRunner::Start()
	{
		if (Instance != nullptr || !IsRequested())
		{
			return;
		}

		Instance = new FSmokeRunner();

		Instance->SenderCycleCount   = ParseIntSwitch(TEXT("-SpoutSenderCycles="), 10, 1);
		Instance->ReceiverCycleCount = ParseIntSwitch(TEXT("-SpoutReceiverCycles="), 10, 1);
		Instance->TravelCycleCount   = ParseIntSwitch(TEXT("-SpoutTravelCycles="), 5, 1);
		Instance->MapA = ParseStringSwitch(TEXT("-SpoutSmokeMapA="), TEXT("/Engine/Maps/Entry"));
		Instance->MapB = ParseStringSwitch(TEXT("-SpoutSmokeMapB="), TEXT("/Engine/Maps/Templates/Template_Default"));

		const int32 DeadlineOverride = ParseIntSwitch(TEXT("-SpoutSmokeTimeoutSeconds="), 0, 0);
		Instance->DeadlineSeconds = DeadlineOverride > 0 ? static_cast<double>(DeadlineOverride) : DefaultDeadlineSeconds;
		Instance->StartTimeSeconds = FPlatformTime::Seconds();

		// Names are unique per process so two matrix rows running back to back, or an unrelated Spout
		// application on the same machine, cannot be mistaken for this run's sender.
		const uint32 ProcessId = FPlatformProcess::GetCurrentProcessId();
		Instance->LifecycleSenderName = FName(*FString::Printf(TEXT("SpoutHostTest_Life_%u"), ProcessId));
		Instance->ShutdownSenderName  = FName(*FString::Printf(TEXT("SpoutHostTest_Exit_%u"), ProcessId));
		Instance->WarmupSenderName    = FName(*FString::Printf(TEXT("SpoutHostTest_Warm_%u"), ProcessId));
		Instance->ViewportSenderName  = FName(*FString::Printf(TEXT("SpoutHostTest_View_%u"), ProcessId));

		Instance->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(Instance, &FSmokeRunner::Tick), 0.0f);
	}

	void FSmokeRunner::Stop()
	{
		if (Instance == nullptr)
		{
			return;
		}

		if (Instance->TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Instance->TickerHandle);
			Instance->TickerHandle.Reset();
		}

		Instance->SourceRenderTarget.Reset();
		Instance->ReceivedTexture.Reset();
		Instance->ReceivedMaterial.Reset();
		Instance->ViewportProbe.Reset();

		// Deliberately not deleted: HasPassed() is read after Stop() during module shutdown, and the
		// process is exiting anyway. Leaking one small object at exit beats reading freed memory to
		// decide whether the run passed.
	}

	UWorld* FSmokeRunner::GetGameWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::Game && Context.World() != nullptr)
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	FString FSmokeRunner::GetCurrentMapShortName()
	{
		const UWorld* World = GetGameWorld();
		return World != nullptr ? World->GetMapName() : FString();
	}

	void FSmokeRunner::Advance(EStep Next)
	{
		Step = Next;
		StepFrames = 0;
	}

	void FSmokeRunner::FailAndFinish(const FString& Reason)
	{
		bFailed = true;
		if (FReport* Report = FReport::Get())
		{
			Report->Fail(Reason);
			Report->SetField(TEXT("failedAtStep"), FString::FromInt(static_cast<int32>(Step)));
			Report->WriteResultJson(TEXT("smoke"), false);
		}
		Advance(EStep::Finished);

		// Older engine launch loops can turn a graceful requested error into process exit zero.
		// Preserve the failure evidence and close plugin streams before forcing the error status.
		USpoutBPFunctionLibrary::CloseAllStreams();
		FPlatformMisc::RequestExitWithStatus(/*Force*/ true, /*ReturnCode*/ 3);
	}

	bool FSmokeRunner::Tick(float /*DeltaSeconds*/)
	{
		if (Step == EStep::Finished)
		{
			return true;
		}

		if ((FPlatformTime::Seconds() - StartTimeSeconds) > DeadlineSeconds)
		{
			FailAndFinish(FString::Printf(
				TEXT("timeout after %.0f s at step %d"), DeadlineSeconds, static_cast<int32>(Step)));
			return true;
		}

		++StepFrames;

		bool bStepComplete = false;
		switch (Step)
		{
		case EStep::WaitForWorld:         bStepComplete = StepWaitForWorld();         break;
		case EStep::Begin:                bStepComplete = StepBegin();                break;
		case EStep::CheckPluginModule:    bStepComplete = StepCheckPluginModule();    break;
		case EStep::CheckRuntimeDll:      bStepComplete = StepCheckRuntimeDll();      break;
		case EStep::CheckApiInit:         bStepComplete = StepCheckApiInit();         break;
		case EStep::PrepareSource:        bStepComplete = StepPrepareSource();        break;
		case EStep::SenderCycles:         bStepComplete = StepSenderCycles();         break;
		case EStep::SenderRegistration:   bStepComplete = StepSenderRegistration();   break;
		case EStep::SameNameRestart:      bStepComplete = StepSameNameRestart();      break;
		case EStep::ReceiverCycles:       bStepComplete = StepReceiverCycles();       break;
		case EStep::LevelTravel:          bStepComplete = StepLevelTravel();          break;
		case EStep::StaleSessionCheck:    bStepComplete = StepStaleSessionCheck();    break;
		case EStep::ViewportSenderStart:     bStepComplete = StepViewportSenderStart();     break;
		case EStep::ViewportSenderAdvertise: bStepComplete = StepViewportSenderAdvertise(); break;
		case EStep::ViewportSenderFrames:    bStepComplete = StepViewportSenderFrames();    break;
		case EStep::ViewportSenderResize:    bStepComplete = StepViewportSenderResize();    break;
		case EStep::ViewportSenderResize2:   bStepComplete = StepViewportSenderResize2();   break;
		case EStep::ViewportSenderClose:     bStepComplete = StepViewportSenderClose();     break;
		case EStep::ViewportAttachDetachStress: bStepComplete = StepViewportAttachDetachStress(); break;
		case EStep::ViewportPostCloseResize: bStepComplete = StepViewportPostCloseResize(); break;
		case EStep::ShutdownWithActivity: bStepComplete = StepShutdownWithActivity(); break;
		default: break;
		}

		if (bStepComplete && !bFailed)
		{
			switch (Step)
			{
			case EStep::WaitForWorld:         Advance(EStep::Begin);                break;
			case EStep::Begin:                Advance(EStep::CheckPluginModule);    break;
			case EStep::CheckPluginModule:    Advance(EStep::CheckRuntimeDll);      break;
			case EStep::CheckRuntimeDll:      Advance(EStep::CheckApiInit);         break;
			case EStep::CheckApiInit:         Advance(EStep::PrepareSource);        break;
			case EStep::PrepareSource:        Advance(EStep::SenderCycles);         break;
			case EStep::SenderCycles:         Advance(EStep::SenderRegistration);   break;
			case EStep::SenderRegistration:   Advance(EStep::SameNameRestart);      break;
			case EStep::SameNameRestart:      Advance(EStep::ReceiverCycles);       break;
			case EStep::ReceiverCycles:       Advance(EStep::LevelTravel);          break;
			case EStep::LevelTravel:          Advance(EStep::StaleSessionCheck);    break;
			case EStep::StaleSessionCheck:    Advance(EStep::ViewportSenderStart);  break;
			case EStep::ViewportSenderStart:     Advance(EStep::ViewportSenderAdvertise); break;
			case EStep::ViewportSenderAdvertise: Advance(EStep::ViewportSenderFrames);    break;
			case EStep::ViewportSenderFrames:    Advance(EStep::ViewportSenderResize);    break;
			case EStep::ViewportSenderResize:    Advance(EStep::ViewportSenderResize2);   break;
			case EStep::ViewportSenderResize2:   Advance(EStep::ViewportSenderClose);     break;
			case EStep::ViewportSenderClose:     Advance(EStep::ViewportAttachDetachStress); break;
			case EStep::ViewportAttachDetachStress: Advance(EStep::ViewportPostCloseResize); break;
			case EStep::ViewportPostCloseResize: Advance(EStep::ShutdownWithActivity);    break;
			case EStep::ShutdownWithActivity: Advance(EStep::Finished);             break;
			default: break;
			}
		}

		return true;
	}

	// ---------------------------------------------------------------------------------------------
	// Steps
	// ---------------------------------------------------------------------------------------------

	bool FSmokeRunner::StepWaitForWorld()
	{
		const UWorld* World = GetGameWorld();
		if (World == nullptr || !World->HasBegunPlay())
		{
			if (StepFrames > TravelBudgetFrames)
			{
				FailAndFinish(TEXT("no Game world began play within the budget"));
			}
			return false;
		}
		return true;
	}

	bool FSmokeRunner::StepBegin()
	{
		FReport* Report = FReport::Get();
		if (Report == nullptr)
		{
			// Without the report there is no evidence channel, so there is no way to report a pass
			// honestly. Fail loudly rather than run a test nobody can read the result of.
			bFailed = true;
			FPlatformMisc::RequestExitWithStatus(false, 4);
			Advance(EStep::Finished);
			return false;
		}

		Report->Mark(Markers::SmokeBegin);
		Report->SetField(TEXT("engineVersion"), Diagnostics::GetEngineVersionString());
		Report->SetField(TEXT("buildConfiguration"), Diagnostics::GetBuildConfigurationName());
		Report->SetField(TEXT("executable"), Diagnostics::GetExecutablePath());
		Report->SetField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		Report->SetField(TEXT("pluginBaseDir"), Diagnostics::GetPluginBaseDir());
		Report->SetField(TEXT("startupMap"), GetCurrentMapShortName());
		Report->SetField(TEXT("senderCyclesRequested"), FString::FromInt(SenderCycleCount));
		Report->SetField(TEXT("receiverCyclesRequested"), FString::FromInt(ReceiverCycleCount));
		Report->SetField(TEXT("travelCyclesRequested"), FString::FromInt(TravelCycleCount));
		Report->SetField(TEXT("mapA"), MapA);
		Report->SetField(TEXT("mapB"), MapB);
		Report->SetField(TEXT("commandLine"), FCommandLine::Get());
		return true;
	}

	bool FSmokeRunner::StepCheckPluginModule()
	{
		if (!Diagnostics::IsPluginModuleLoaded())
		{
			FailAndFinish(TEXT("SpoutPlugin module is not loaded"));
			return false;
		}

		FReport::Get()->Mark(Markers::PluginModuleLoaded);
		return true;
	}

	bool FSmokeRunner::StepCheckRuntimeDll()
	{
		FReport* Report = FReport::Get();

		const FRuntimeDllInfo Info = Diagnostics::QueryRuntimeDll();

		Report->SetField(TEXT("loaderReportsDllLoaded"), Info.bLoaded ? TEXT("true") : TEXT("false"));
		Report->SetField(TEXT("runtimeDllPath"), Info.ResolvedPath);
		Report->SetField(TEXT("runtimeDllSizeBytes"), FString::Printf(TEXT("%lld"), Info.SizeBytes));
		if (!Info.Error.IsEmpty())
		{
			Report->SetField(TEXT("runtimeDllQueryError"), Info.Error);
		}

		if (!Info.bLoaded)
		{
			FailAndFinish(TEXT("Spout.dll is not mapped into the process"));
			return false;
		}

		if (Info.ResolvedPath.IsEmpty())
		{
			FailAndFinish(TEXT("Spout.dll is loaded but its path could not be resolved"));
			return false;
		}

		Report->Mark(Markers::DllLoaded);
		Report->MarkValue(TEXT("SPOUT_DLL_PATH"), Info.ResolvedPath);
		Report->MarkValue(TEXT("SPOUT_DLL_SIZE"), FString::Printf(TEXT("%lld"), Info.SizeBytes));
		return true;
	}

	bool FSmokeRunner::StepCheckApiInit()
	{
		FReport* Report = FReport::Get();

		// This is where "the plugin can actually USE the DLL" is established, as opposed to the
		// previous step's "the DLL is mapped into the process". GetMaxSenders sets its output to 0
		// whenever Spout is unavailable, so a non-zero limit is evidence the DLL answered - which no
		// amount of file-presence or handle checking can give.
		int32 MaxSenders = -1;
		USpoutBPFunctionLibrary::GetMaxSenders(MaxSenders);
		Report->SetField(TEXT("maxSenders"), FString::FromInt(MaxSenders));

		TArray<FSenderStruct> Senders;
		const bool bEnumerated = USpoutBPFunctionLibrary::SpoutInfo(Senders);
		Report->SetField(TEXT("senderEnumerationSucceeded"), bEnumerated ? TEXT("true") : TEXT("false"));
		Report->SetField(TEXT("senderCountAtStartup"), FString::FromInt(Senders.Num()));

		if (MaxSenders <= 0)
		{
			FailAndFinish(TEXT("GetMaxSenders returned 0, so Spout is unavailable in this host"));
			return false;
		}

		Report->Mark(Markers::ApiInitPass);
		return true;
	}

	bool FSmokeRunner::EnsureSourceRenderTarget(int32 Width, int32 Height)
	{
		if (SourceRenderTarget.IsValid()
			&& SourceRenderTarget->SizeX == Width
			&& SourceRenderTarget->SizeY == Height)
		{
			return true;
		}

		UTextureRenderTarget2D* RenderTarget = USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(
			Width, Height, PF_B8G8R8A8, /*bForceLinearGamma*/ true);

		if (RenderTarget == nullptr)
		{
			return false;
		}

		// The factory deliberately does not root its result, so the caller has to keep it alive.
		SourceRenderTarget.Reset(RenderTarget);
		return true;
	}

	bool FSmokeRunner::OpenSender(FName Name)
	{
		if (!EnsureSourceRenderTarget(256, 256))
		{
			return false;
		}

		return USpoutBPFunctionLibrary::SpoutSender(
			Name, ESpoutSendTextureFrom::TextureRenderTarget2D, SourceRenderTarget.Get());
	}

	bool FSmokeRunner::IsSenderAdvertised(FName Name)
	{
		FSenderStruct Info;
		return USpoutBPFunctionLibrary::GetSenderInfo(Name, Info);
	}

	bool FSmokeRunner::StepPrepareSource()
	{
		if (!EnsureSourceRenderTarget(256, 256))
		{
			FailAndFinish(TEXT("CreateTextureRenderTarget2D returned null for a 256x256 B8G8R8A8 target"));
			return false;
		}

		// Poll rather than sleep a fixed number of frames: how long the render thread takes to
		// create the RHI resource is not something to guess at, and the moment the plugin accepts
		// the target is the moment it is genuinely ready.
		if (USpoutBPFunctionLibrary::SpoutSender(
				WarmupSenderName, ESpoutSendTextureFrom::TextureRenderTarget2D, SourceRenderTarget.Get()))
		{
			USpoutBPFunctionLibrary::CloseSender(WarmupSenderName);
			FReport::Get()->SetField(TEXT("sourceReadyAfterFrames"), FString::FromInt(StepFrames));
			return true;
		}

		if (StepFrames > RegistrationBudgetFrames)
		{
			FailAndFinish(FString::Printf(
				TEXT("a freshly created render target was still not publishable after %d frames"),
				RegistrationBudgetFrames));
		}
		return false;
	}

	bool FSmokeRunner::StepSenderCycles()
	{
		// One cycle per SettleFrames frames: open, let the render thread act on it, close.
		if ((StepFrames % SettleFrames) != 1)
		{
			return false;
		}

		const FName CycleName(*FString::Printf(TEXT("%s_c%d"), *LifecycleSenderName.ToString(), CompletedSenderCycles));

		if (!OpenSender(CycleName))
		{
			// The source was proven publishable by StepPrepareSource, so a rejection here is a real
			// failure rather than engine warm-up.
			FailAndFinish(FString::Printf(
				TEXT("sender cycle %d: SpoutSender rejected the request for a source already proven publishable"),
				CompletedSenderCycles));
			return false;
		}

		USpoutBPFunctionLibrary::CloseSender(CycleName);
		++CompletedSenderCycles;

		if (CompletedSenderCycles >= SenderCycleCount)
		{
			FReport* Report = FReport::Get();
			Report->SetField(TEXT("senderCyclesCompleted"), FString::FromInt(CompletedSenderCycles));
			Report->MarkValue(TEXT("SPOUT_SENDER_CYCLES"), FString::FromInt(CompletedSenderCycles));
			return true;
		}

		return false;
	}

	bool FSmokeRunner::StepSenderRegistration()
	{
		// The cycles above prove the API accepts and cancels work. This proves a sender actually
		// reaches Spout's sender list, which the queueing return value says nothing about.
		if (StepFrames == 1)
		{
			if (!OpenSender(LifecycleSenderName))
			{
				FailAndFinish(TEXT("registration: SpoutSender rejected the request"));
				return false;
			}
			return false;
		}

		// Re-queue every frame. A sender publishes per frame in real use, and the registration
		// retry path is only exercised by repeated sends.
		USpoutBPFunctionLibrary::SpoutSender(
			LifecycleSenderName, ESpoutSendTextureFrom::TextureRenderTarget2D, SourceRenderTarget.Get());

		if (IsSenderAdvertised(LifecycleSenderName))
		{
			FReport* Report = FReport::Get();
			Report->SetField(TEXT("registrationFrames"), FString::FromInt(StepFrames));
			Report->Mark(Markers::SenderCreatePass);
			return true;
		}

		if (StepFrames > RegistrationBudgetFrames)
		{
			FailAndFinish(FString::Printf(
				TEXT("sender '%s' never appeared in the Spout sender list within %d frames"),
				*LifecycleSenderName.ToString(), RegistrationBudgetFrames));
		}
		return false;
	}

	bool FSmokeRunner::StepSameNameRestart()
	{
		// Close, then immediately reopen the SAME name in the same frame. Token-based cancellation
		// is what makes this deterministic; without it the close can be overtaken by queued work.
		if (StepFrames == 1)
		{
			USpoutBPFunctionLibrary::CloseSender(LifecycleSenderName);
			if (!OpenSender(LifecycleSenderName))
			{
				FailAndFinish(TEXT("same-name restart: SpoutSender rejected the reopen"));
			}
			return false;
		}

		USpoutBPFunctionLibrary::SpoutSender(
			LifecycleSenderName, ESpoutSendTextureFrom::TextureRenderTarget2D, SourceRenderTarget.Get());

		if (IsSenderAdvertised(LifecycleSenderName))
		{
			FReport::Get()->Mark(Markers::SameNameRestartPass);
			return true;
		}

		if (StepFrames > RegistrationBudgetFrames)
		{
			FailAndFinish(FString::Printf(
				TEXT("sender '%s' did not come back after a close/reopen within %d frames"),
				*LifecycleSenderName.ToString(), RegistrationBudgetFrames));
		}
		return false;
	}

	bool FSmokeRunner::StepReceiverCycles()
	{
		auto* PixelResource=SourceRenderTarget->GameThread_GetRenderTargetResource();
		const FColor ExpectedPixel(17,83,149,211);
		ENQUEUE_RENDER_COMMAND(SpoutSmokeCyclePixel)([PixelResource,ExpectedPixel](FRHICommandListImmediate& Cmd)
		{ Cmd.UpdateTexture2D(PixelResource->GetTextureRHI(),0,FUpdateTextureRegion2D(0,0,0,0,1,1),4,reinterpret_cast<const uint8*>(&ExpectedPixel)); });
		// Keep the sender publishing; the receiver below consumes it. Sender and receiver in one
		// process is a common real-world setup and is covered explicitly here.
		USpoutBPFunctionLibrary::SpoutSender(
			LifecycleSenderName, ESpoutSendTextureFrom::TextureRenderTarget2D, SourceRenderTarget.Get());

		if ((StepFrames % SettleFrames) != 1)
		{
			return false;
		}

		UMaterialInstanceDynamic* OutMaterial = ReceivedMaterial.Get();
		UTexture2D* OutTexture = ReceivedTexture.Get();

		UMaterial* ParentMaterial = UMaterial::GetDefaultMaterial(MD_Surface);

		const bool bAccepted = USpoutBPFunctionLibrary::SpoutReceiver(
			LifecycleSenderName,
			ParentMaterial,
			FName(TEXT("SpoutTexture")),
			OutMaterial,
			OutTexture,
			/*OptionalOutputRenderTarget*/ nullptr,
			ESpoutColorSpace::Auto);

		if (!bAccepted)
		{
			if (StepFrames > RegistrationBudgetFrames)
			{
				FailAndFinish(FString::Printf(
					TEXT("receiver cycle %d: SpoutReceiver never accepted '%s' within %d frames"),
					CompletedReceiverCycles, *LifecycleSenderName.ToString(), RegistrationBudgetFrames));
			}
			// Not yet a failure: the sender's shared texture may not have been published yet.
			return false;
		}

		ReceivedMaterial.Reset(OutMaterial);
		ReceivedTexture.Reset(OutTexture);

		if (OutTexture == nullptr)
		{
			FailAndFinish(FString::Printf(
				TEXT("receiver cycle %d: SpoutReceiver succeeded but returned no texture"),
				CompletedReceiverCycles));
			return false;
		}

		const auto ActualPixels = ReadPixels(OutTexture->GetResource(), OutTexture->GetSizeX(), OutTexture->GetSizeY());
		const auto SourcePixels = ReadPixels(SourceRenderTarget->GetResource(), SourceRenderTarget->SizeX, SourceRenderTarget->SizeY);
		if (ActualPixels.IsEmpty() || ActualPixels != SourcePixels || ActualPixels[0]!=ExpectedPixel)
		{
			FailAndFinish(TEXT("receiver cycle: completed pixels differ from source before close"));
			return false;
		}
		// Close by name. The pointers are handed back exactly as documented, so both are expected to
		// be cleared - a receiver that leaves them set would mean the close did not own them.
		USpoutBPFunctionLibrary::CloseReceiver(LifecycleSenderName, OutTexture, OutMaterial);

		if (OutTexture != nullptr || OutMaterial != nullptr)
		{
			FailAndFinish(FString::Printf(
				TEXT("receiver cycle %d: CloseReceiver left the caller's texture or material set"),
				CompletedReceiverCycles));
			return false;
		}

		ReceivedTexture.Reset();
		ReceivedMaterial.Reset();

		++CompletedReceiverCycles;

		if (CompletedReceiverCycles >= ReceiverCycleCount)
		{
			FReport* Report = FReport::Get();
			Report->SetField(TEXT("receiverCyclesCompleted"), FString::FromInt(CompletedReceiverCycles));
			Report->MarkValue(TEXT("SPOUT_RECEIVER_CYCLES"), FString::FromInt(CompletedReceiverCycles));
			Report->Mark(Markers::ReceiverCreatePass);
			return true;
		}

		return false;
	}

	bool FSmokeRunner::StepLevelTravel()
	{
		// The sender stays open across every travel. That is the point: a packaged game's world tears
		// down on each transition, and whether a stream survives it (or crashes it) is exactly what
		// this step checks.
		if (SourceRenderTarget.IsValid())
		{
			USpoutBPFunctionLibrary::SpoutSender(
				LifecycleSenderName, ESpoutSendTextureFrom::TextureRenderTarget2D, SourceRenderTarget.Get());
		}

		UWorld* World = GetGameWorld();
		if (World == nullptr)
		{
			// Mid-transition; the world is being swapped.
			if (StepFrames > TravelBudgetFrames)
			{
				FailAndFinish(TEXT("level travel: no Game world reappeared within the budget"));
			}
			return false;
		}

		const FString TargetPath = (TravelPhase == 0) ? MapB : MapA;
		const FString TargetShortName = FPaths::GetCleanFilename(TargetPath);
		const FString CurrentShortName = World->GetMapName();

		if (CurrentShortName == TargetShortName)
		{
			// Arrived. Let the new world settle before issuing the next travel, so the test is not
			// racing the engine's own post-load work.
			if (!World->HasBegunPlay())
			{
				if (StepFrames > TravelBudgetFrames)
				{
					FailAndFinish(FString::Printf(
						TEXT("level travel: '%s' loaded but never began play"), *TargetShortName));
				}
				return false;
			}

			TravelPhase = 1 - TravelPhase;
			if (TravelPhase == 0)
			{
				// Back on the startup map: one full out-and-back cycle done.
				++CompletedTravelCycles;
			}

			StepFrames = 0;

			if (CompletedTravelCycles >= TravelCycleCount)
			{
				FReport* Report = FReport::Get();
				Report->SetField(TEXT("travelCyclesCompleted"), FString::FromInt(CompletedTravelCycles));
				Report->MarkValue(TEXT("SPOUT_TRAVEL_CYCLES"), FString::FromInt(CompletedTravelCycles));
				Report->Mark(Markers::LevelTravelPass);
				return true;
			}

			return false;
		}

		// Issue the travel once per arrival, on the frame right after the previous one completed.
		if (StepFrames == 1)
		{
			GEngine->SetClientTravel(World, *TargetPath, TRAVEL_Absolute);
			return false;
		}

		if (StepFrames > TravelBudgetFrames)
		{
			FailAndFinish(FString::Printf(
				TEXT("level travel to '%s' did not complete within %d frames (still on '%s')"),
				*TargetPath, TravelBudgetFrames, *CurrentShortName));
		}
		return false;
	}

	bool FSmokeRunner::StepStaleSessionCheck()
	{
		if (StepFrames == 1)
		{
			USpoutBPFunctionLibrary::CloseSender(LifecycleSenderName);
			return false;
		}

		// Give the close a few frames to reach Spout's sender list before asserting it is gone.
		if (StepFrames < (SettleFrames * 4))
		{
			return false;
		}

		if (IsSenderAdvertised(LifecycleSenderName))
		{
			FailAndFinish(FString::Printf(
				TEXT("sender '%s' is still advertised after CloseSender"), *LifecycleSenderName.ToString()));
			return false;
		}

		FReport::Get()->Mark(Markers::NoStaleSessionPass);
		return true;
	}

	// ---------------------------------------------------------------------------------------------
	// GameViewportSenderSmoke
	// ---------------------------------------------------------------------------------------------

	bool FSmokeRunner::QueueViewportSend()
	{
		// The source selector is the point of this scenario: GameViewport, never a render target.
		// The third argument is ignored on this path and passed null so no render target of ours
		// can possibly be involved.
		if (!USpoutBPFunctionLibrary::SpoutSender(
				ViewportSenderName, ESpoutSendTextureFrom::GameViewport, nullptr))
		{
			FailAndFinish(FString::Printf(
				TEXT("viewport sender: SpoutSender rejected a GameViewport request at step %d frame %d"),
				static_cast<int32>(Step), StepFrames));
			return false;
		}
		return true;
	}

	bool FSmokeRunner::StepViewportSenderStart()
	{
		FReport* Report = FReport::Get();

		if (StepFrames == 1)
		{
			Report->Mark(Markers::ViewportBegin);
			Report->SetField(TEXT("viewportScenario"), TEXT("GameViewportSenderSmoke"));
			Report->SetField(TEXT("viewportSenderName"), ViewportSenderName.ToString());
			Report->SetField(TEXT("viewportAdvertiseBudgetFrames"), FString::FromInt(RegistrationBudgetFrames));
			Report->SetField(TEXT("viewportFrameBudgetFrames"), FString::FromInt(RegistrationBudgetFrames));
			Report->SetField(TEXT("viewportMinAdvanceObservations"), FString::FromInt(ViewportMinAdvanceObservations));
			Report->SetField(TEXT("viewportMinFrameDelta"), FString::FromInt(static_cast<int32>(ViewportMinFrameDelta)));

			Report->SetField(TEXT("viewportAccessPath"), ViewportAccessPathName());
			Report->MarkValue(Markers::ViewportPathValue, ViewportAccessPathName());

			Report->SetField(TEXT("viewportDelegateMutationThread"), ViewportDelegateMutationThreadName());
			Report->MarkValue(Markers::ViewportDelegateThreadValue, ViewportDelegateMutationThreadName());

			const FString RhiName = GDynamicRHI ? FString(GDynamicRHI->GetName()) : FString(TEXT("none"));
			Report->SetField(TEXT("viewportRhi"), RhiName);
			if (!RhiName.Contains(TEXT("D3D12")))
			{
				FailAndFinish(FString::Printf(
					TEXT("viewport sender: the host is running RHI '%s', not the required D3D12"), *RhiName));
				return false;
			}
		}

		// A real viewport and a real window are prerequisites, not things to wait out forever:
		// in this windowed host both exist from startup, so the budget only absorbs frame noise.
		const bool bViewportFound = GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport;
		const bool bWindowFound = bViewportFound && GEngine->GameViewport->GetWindow().IsValid();
		Report->SetField(TEXT("viewportGameViewportFound"), bViewportFound ? TEXT("true") : TEXT("false"));
		Report->SetField(TEXT("viewportGameWindowFound"), bWindowFound ? TEXT("true") : TEXT("false"));

		if (!bViewportFound || !bWindowFound)
		{
			if (StepFrames > RegistrationBudgetFrames)
			{
				FailAndFinish(TEXT("viewport sender: no game viewport/window appeared within the budget"));
			}
			return false;
		}

		// The external observer. Its failure is the scenario's failure: without it there is no
		// evidence, and no evidence is a fail, not a skip.
		ViewportProbe = MakeUnique<FViewportSenderProbe>();
		FString ProbeError;
		if (!ViewportProbe->Initialize(ProbeError))
		{
			FailAndFinish(FString::Printf(TEXT("viewport sender: %s"), *ProbeError));
			return false;
		}

		const bool bFrameCounting = ViewportProbe->IsFrameCountingAvailable();
		Report->SetField(TEXT("viewportFrameCountEnabled"), bFrameCounting ? TEXT("true") : TEXT("false"));
		if (!bFrameCounting)
		{
			// Frame counting can be disabled machine-wide in SpoutSettings. Without the semaphore
			// there is no external frame evidence to record, so this machine cannot produce the
			// validation this scenario exists for.
			FailAndFinish(TEXT("viewport sender: Spout frame counting is disabled on this machine, so frame advancement cannot be observed externally"));
			return false;
		}

		ViewportProbe->BeginFrameObservation(ViewportSenderName.ToString());
		ViewportInitialSenderFrame = ViewportProbe->GetSenderFrameCount();
		Report->SetField(TEXT("viewportInitialSenderFrame"), FString::FromInt(static_cast<int32>(ViewportInitialSenderFrame)));

		return QueueViewportSend();
	}

	bool FSmokeRunner::StepViewportSenderAdvertise()
	{
		// One publish per present, so real per-frame use means re-queueing every tick.
		if (!QueueViewportSend())
		{
			return false;
		}

		FReport* Report = FReport::Get();
		const FString SenderName = ViewportSenderName.ToString();

		uint32 Width = 0, Height = 0, DxgiFormat = 0;
		if (ViewportProbe->IsSenderAdvertised(SenderName)
			&& ViewportProbe->GetSenderDescription(SenderName, Width, Height, DxgiFormat))
		{
			if (Width == 0 || Height == 0)
			{
				FailAndFinish(FString::Printf(
					TEXT("viewport sender: advertised with a degenerate size %ux%u"), Width, Height));
				return false;
			}

			Report->SetField(TEXT("viewportSenderAdvertised"), TEXT("true"));
			Report->SetField(TEXT("viewportRegisteredAfterFrames"), FString::FromInt(StepFrames));
			Report->SetField(TEXT("viewportSenderWidth"), FString::FromInt(static_cast<int32>(Width)));
			Report->SetField(TEXT("viewportSenderHeight"), FString::FromInt(static_cast<int32>(Height)));
			Report->SetField(TEXT("viewportSenderDxgiFormat"), FString::FromInt(static_cast<int32>(DxgiFormat)));
			Report->Mark(Markers::ViewportAdvertisedPass);

			// Open the shared texture on the probe's own device, from the share handle in the
			// registry - the exact call an external receiver makes.
			uint32 TexWidth = 0, TexHeight = 0, TexFormat = 0;
			FString OpenError;
			if (!ViewportProbe->OpenSharedTexture(SenderName, TexWidth, TexHeight, TexFormat, OpenError))
			{
				FailAndFinish(FString::Printf(TEXT("viewport sender: %s"), *OpenError));
				return false;
			}
			if (TexWidth != Width || TexHeight != Height)
			{
				FailAndFinish(FString::Printf(
					TEXT("viewport sender: the opened shared texture is %ux%u but the registry advertises %ux%u"),
					TexWidth, TexHeight, Width, Height));
				return false;
			}

			Report->SetField(TEXT("viewportSharedTextureOpened"), TEXT("true"));
			Report->SetField(TEXT("viewportSharedTextureWidth"), FString::FromInt(static_cast<int32>(TexWidth)));
			Report->SetField(TEXT("viewportSharedTextureHeight"), FString::FromInt(static_cast<int32>(TexHeight)));
			Report->SetField(TEXT("viewportSharedTextureDxgiFormat"), FString::FromInt(static_cast<int32>(TexFormat)));
			Report->Mark(Markers::ViewportTextureOpenPass);
			return true;
		}

		if (StepFrames > RegistrationBudgetFrames)
		{
			FailAndFinish(FString::Printf(
				TEXT("viewport sender '%s' never appeared in the Spout registry within %d frames"),
				*SenderName, RegistrationBudgetFrames));
		}
		return false;
	}

	bool FSmokeRunner::StepViewportSenderFrames()
	{
		if (!QueueViewportSend())
		{
			return false;
		}

		FReport* Report = FReport::Get();

		// The receiver side of the cross-process frame semaphore. Each true is at least one new
		// frame published since the previous poll; each publish is one completed present-callback
		// -> render-command -> copy -> announce chain, which is why this doubles as the external
		// observation of callback executions.
		if (ViewportProbe->PollNewFrame())
		{
			const double NowSeconds = FPlatformTime::Seconds() - StartTimeSeconds;
			if (ViewportObservedAdvances == 0)
			{
				ViewportFirstAdvanceSeconds = NowSeconds;
			}
			ViewportLastAdvanceSeconds = NowSeconds;
			++ViewportObservedAdvances;
			ViewportLastSenderFrame = ViewportProbe->GetSenderFrameCount();
		}

		const long FrameDelta = ViewportLastSenderFrame - ViewportInitialSenderFrame;
		if (ViewportObservedAdvances >= ViewportMinAdvanceObservations && FrameDelta >= ViewportMinFrameDelta)
		{
			Report->SetField(TEXT("viewportObservedFrameAdvances"), FString::FromInt(ViewportObservedAdvances));
			Report->SetField(TEXT("viewportCallbackObservations"), FString::FromInt(ViewportObservedAdvances));
			Report->SetField(TEXT("viewportFinalSenderFrame"), FString::FromInt(static_cast<int32>(ViewportLastSenderFrame)));
			Report->SetField(TEXT("viewportFrameAdvanceDelta"), FString::FromInt(static_cast<int32>(FrameDelta)));
			Report->SetField(TEXT("viewportFirstAdvanceSeconds"), FString::Printf(TEXT("%.3f"), ViewportFirstAdvanceSeconds));
			Report->SetField(TEXT("viewportLastAdvanceSeconds"), FString::Printf(TEXT("%.3f"), ViewportLastAdvanceSeconds));
			Report->SetField(TEXT("viewportFramesObservedAfterFrames"), FString::FromInt(StepFrames));
			Report->Mark(Markers::ViewportFramesPass);
			return true;
		}

		if (StepFrames > RegistrationBudgetFrames)
		{
			FailAndFinish(FString::Printf(
				TEXT("viewport sender: only %d frame advance(s) (sender frame moved %d) were observed within %d frames; required %d observation(s) and a delta of %d"),
				ViewportObservedAdvances, static_cast<int32>(FrameDelta), RegistrationBudgetFrames,
				ViewportMinAdvanceObservations, static_cast<int32>(ViewportMinFrameDelta)));
		}
		return false;
	}

	bool FSmokeRunner::StepViewportSenderResize()
	{
		FReport* Report = FReport::Get();

		if (StepFrames == 1)
		{
			// One controlled resolution change while the sender is live. The back buffer is the
			// source on this path, so the sender must follow the swap chain to the new size.
			Report->SetField(TEXT("viewportResizeAttempted"), TEXT("true"));
			Report->SetField(TEXT("viewportResizeRequestedWidth"), FString::FromInt(ViewportResizeWidth));
			Report->SetField(TEXT("viewportResizeRequestedHeight"), FString::FromInt(ViewportResizeHeight));
			FSystemResolution::RequestResolutionChange(ViewportResizeWidth, ViewportResizeHeight, EWindowMode::Windowed);
		}

		if (!QueueViewportSend())
		{
			return false;
		}

		if (ViewportProbe->PollNewFrame())
		{
			ViewportLastSenderFrame = ViewportProbe->GetSenderFrameCount();
			if (bViewportResizeDimensionsSeen)
			{
				// A publish AFTER the registry showed the new size: the resized swap chain is
				// genuinely flowing.
				++ViewportResizeObservedAdvances;
			}
		}

		if (!bViewportResizeDimensionsSeen)
		{
			uint32 Width = 0, Height = 0, DxgiFormat = 0;
			if (ViewportProbe->GetSenderDescription(ViewportSenderName.ToString(), Width, Height, DxgiFormat)
				&& Width == static_cast<uint32>(ViewportResizeWidth)
				&& Height == static_cast<uint32>(ViewportResizeHeight))
			{
				bViewportResizeDimensionsSeen = true;
				Report->SetField(TEXT("viewportResizeObservedWidth"), FString::FromInt(static_cast<int32>(Width)));
				Report->SetField(TEXT("viewportResizeObservedHeight"), FString::FromInt(static_cast<int32>(Height)));
			}
		}

		if (bViewportResizeDimensionsSeen && ViewportResizeObservedAdvances >= 1)
		{
			// Re-open the shared texture: the resize recreated it, so the registry's share handle
			// changed, and the reopened resource must report the new geometry.
			uint32 TexWidth = 0, TexHeight = 0, TexFormat = 0;
			FString OpenError;
			if (!ViewportProbe->OpenSharedTexture(ViewportSenderName.ToString(), TexWidth, TexHeight, TexFormat, OpenError))
			{
				FailAndFinish(FString::Printf(TEXT("viewport sender: after resize, %s"), *OpenError));
				return false;
			}
			if (TexWidth != static_cast<uint32>(ViewportResizeWidth) || TexHeight != static_cast<uint32>(ViewportResizeHeight))
			{
				FailAndFinish(FString::Printf(
					TEXT("viewport sender: after resize the reopened shared texture is %ux%u, expected %dx%d"),
					TexWidth, TexHeight, ViewportResizeWidth, ViewportResizeHeight));
				return false;
			}

			Report->SetField(TEXT("viewportResizeObservedFrameAdvances"), FString::FromInt(ViewportResizeObservedAdvances));
			Report->SetField(TEXT("viewportResizeCompletedAfterFrames"), FString::FromInt(StepFrames));
			// The RESIZE_PASS marker is emitted after the SECOND resize completes: one marker
			// covering the whole resize block, so a host that survives one resize but not the
			// next cannot pass.
			return true;
		}

		if (StepFrames > TravelBudgetFrames)
		{
			FailAndFinish(FString::Printf(
				TEXT("viewport sender: the resize to %dx%d was not observed within %d frames (registry size seen: %s, post-resize advances: %d)"),
				ViewportResizeWidth, ViewportResizeHeight, TravelBudgetFrames,
				bViewportResizeDimensionsSeen ? TEXT("yes") : TEXT("no"),
				ViewportResizeObservedAdvances));
		}
		return false;
	}

	bool FSmokeRunner::StepViewportSenderResize2()
	{
		FReport* Report = FReport::Get();

		// A second full resize cycle to a third size, and deliberately a RAPID one: the request
		// is issued on two consecutive frames (the system coalesces them) to prove back-to-back
		// resize requests cannot wedge the sender or the swap chain.
		if (StepFrames == 1 || StepFrames == 2)
		{
			if (StepFrames == 1)
			{
				Report->SetField(TEXT("viewportResize2RequestedWidth"), FString::FromInt(ViewportResize2Width));
				Report->SetField(TEXT("viewportResize2RequestedHeight"), FString::FromInt(ViewportResize2Height));
			}
			FSystemResolution::RequestResolutionChange(ViewportResize2Width, ViewportResize2Height, EWindowMode::Windowed);
		}

		if (!QueueViewportSend())
		{
			return false;
		}

		if (ViewportProbe->PollNewFrame())
		{
			ViewportLastSenderFrame = ViewportProbe->GetSenderFrameCount();
			if (bViewportResize2DimensionsSeen)
			{
				++ViewportResize2ObservedAdvances;
			}
		}

		if (!bViewportResize2DimensionsSeen)
		{
			uint32 Width = 0, Height = 0, DxgiFormat = 0;
			if (ViewportProbe->GetSenderDescription(ViewportSenderName.ToString(), Width, Height, DxgiFormat)
				&& Width == static_cast<uint32>(ViewportResize2Width)
				&& Height == static_cast<uint32>(ViewportResize2Height))
			{
				bViewportResize2DimensionsSeen = true;
				Report->SetField(TEXT("viewportResize2ObservedWidth"), FString::FromInt(static_cast<int32>(Width)));
				Report->SetField(TEXT("viewportResize2ObservedHeight"), FString::FromInt(static_cast<int32>(Height)));
			}
		}

		if (bViewportResize2DimensionsSeen && ViewportResize2ObservedAdvances >= 1)
		{
			uint32 TexWidth = 0, TexHeight = 0, TexFormat = 0;
			FString OpenError;
			if (!ViewportProbe->OpenSharedTexture(ViewportSenderName.ToString(), TexWidth, TexHeight, TexFormat, OpenError))
			{
				FailAndFinish(FString::Printf(TEXT("viewport sender: after the second resize, %s"), *OpenError));
				return false;
			}
			if (TexWidth != static_cast<uint32>(ViewportResize2Width) || TexHeight != static_cast<uint32>(ViewportResize2Height))
			{
				FailAndFinish(FString::Printf(
					TEXT("viewport sender: after the second resize the reopened shared texture is %ux%u, expected %dx%d"),
					TexWidth, TexHeight, ViewportResize2Width, ViewportResize2Height));
				return false;
			}

			Report->SetField(TEXT("viewportResize2ObservedFrameAdvances"), FString::FromInt(ViewportResize2ObservedAdvances));
			Report->SetField(TEXT("viewportResize2CompletedAfterFrames"), FString::FromInt(StepFrames));
			Report->Mark(Markers::ViewportResizePass);
			return true;
		}

		if (StepFrames > TravelBudgetFrames)
		{
			FailAndFinish(FString::Printf(
				TEXT("viewport sender: the second resize to %dx%d was not observed within %d frames (registry size seen: %s, post-resize advances: %d)"),
				ViewportResize2Width, ViewportResize2Height, TravelBudgetFrames,
				bViewportResize2DimensionsSeen ? TEXT("yes") : TEXT("no"),
				ViewportResize2ObservedAdvances));
		}
		return false;
	}

	bool FSmokeRunner::StepViewportSenderClose()
	{
		FReport* Report = FReport::Get();

		if (StepFrames == 1)
		{
			// The real public lifecycle path, and no more queueing after it: a re-queued send
			// would legitimately re-open the stream and make this step test nothing.
			USpoutBPFunctionLibrary::CloseSender(ViewportSenderName);
			Report->SetField(TEXT("viewportCloseRequested"), TEXT("true"));
			return false;
		}

		// Give the close a few frames to reach the shared registry before asserting, mirroring
		// StepStaleSessionCheck.
		if (StepFrames < (SettleFrames * 4))
		{
			return false;
		}

		if (ViewportProbe->IsSenderAdvertised(ViewportSenderName.ToString()))
		{
			if (StepFrames > RegistrationBudgetFrames)
			{
				FailAndFinish(FString::Printf(
					TEXT("viewport sender '%s' is still advertised %d frames after CloseSender"),
					*ViewportSenderName.ToString(), StepFrames));
			}
			return false;
		}

		Report->SetField(TEXT("viewportAdvertisedAfterClose"), TEXT("false"));
		Report->Mark(Markers::ViewportClosePass);

		ViewportProbe->Close();
		ViewportProbe.Reset();

		// The scenario's own pass marker comes after the post-close resize below.
		return true;
	}

	bool FSmokeRunner::StepViewportAttachDetachStress()
	{
		FReport* Report = FReport::Get();

		// One cycle per invocation of the "start" phase: CloseAllStreams performs the ordered
		// shutdown, which detaches the back-buffer delegate (on UE 5.3 that is a render-thread
		// removal with a completion wait); the following sends re-attach it. A deadlock or an
		// unbounded wait in either direction shows up here as the run's overall timeout rather
		// than as a silent pass, and the per-cycle wall time is recorded.
		if (StepFrames == 1)
		{
			ViewportRestartCycleStartSeconds = FPlatformTime::Seconds();
			USpoutBPFunctionLibrary::CloseAllStreams();
			return false;
		}

		// Re-attach: queue the viewport sender again under a per-cycle name.
		const FName CycleName(*FString::Printf(TEXT("%s_r%d"), *ViewportSenderName.ToString(), CompletedViewportRestartCycles));
		if (!USpoutBPFunctionLibrary::SpoutSender(CycleName, ESpoutSendTextureFrom::GameViewport, nullptr))
		{
			// A rejection right after a full teardown is legitimate for a frame or two while the
			// session leaves the stopping state and the viewport is revalidated.
			if (StepFrames > RegistrationBudgetFrames)
			{
				FailAndFinish(FString::Printf(
					TEXT("viewport restart cycle %d: SpoutSender never re-accepted a GameViewport request within %d frames"),
					CompletedViewportRestartCycles, RegistrationBudgetFrames));
			}
			return false;
		}

		// Give the re-registered delegate a few presents, then close this cycle's name and move on.
		if ((StepFrames % SettleFrames) != 1)
		{
			return false;
		}

		USpoutBPFunctionLibrary::CloseSender(CycleName);

		const double CycleSeconds = FPlatformTime::Seconds() - ViewportRestartCycleStartSeconds;
		ViewportRestartWorstCycleSeconds = FMath::Max(ViewportRestartWorstCycleSeconds, CycleSeconds);
		++CompletedViewportRestartCycles;

		if (CompletedViewportRestartCycles >= ViewportRestartCycleCount)
		{
			Report->SetField(TEXT("viewportRestartCyclesCompleted"), FString::FromInt(CompletedViewportRestartCycles));
			Report->SetField(TEXT("viewportRestartWorstCycleSeconds"),
				FString::Printf(TEXT("%.3f"), ViewportRestartWorstCycleSeconds));
			Report->Mark(Markers::ViewportRestartPass);
			return true;
		}

		// Next cycle starts with another full teardown.
		StepFrames = 0;
		return false;
	}

	bool FSmokeRunner::StepViewportPostCloseResize()
	{
		FReport* Report = FReport::Get();

		// Close followed by resize, back to the original resolution. The sender is gone, but on
		// UE 5.3-5.7 the pre-resize hook remains registered until the ordered teardown and must
		// clear a (now empty) cache without incident; on UE 5.8 nothing was retained. Surviving
		// this proves a resize after the last sender closed cannot crash either.
		if (StepFrames == 1)
		{
			FSystemResolution::RequestResolutionChange(640, 360, EWindowMode::Windowed);
			return false;
		}

		if (StepFrames < PostCloseResizeSurviveFrames)
		{
			return false;
		}

		// Still ticking N frames after the request: the resize was processed (or coalesced away)
		// without taking the process down.
		Report->SetField(TEXT("viewportPostCloseResizeSurvived"), TEXT("true"));
		Report->Mark(Markers::ViewportSenderPass);
		return true;
	}

	bool FSmokeRunner::StepShutdownWithActivity()
	{
		if (StepFrames == 1)
		{
			FString Error;
			if (!QualifyExternalPixels(Error)) { FailAndFinish(Error); return false; }
			FReport::Get()->Mark(TEXT("SPOUT_EXTERNAL_PIXELS_PASS"));
			if (!QualifyShippedExamples(GetGameWorld(),Error)) { FailAndFinish(Error); return false; }
			FReport::Get()->Mark(TEXT("SPOUT_SHIPPED_EXAMPLES_PASS"));
		}
		// Deliberately leave a sender publishing across process exit. This is the packaged-host
		// equivalent of quitting the editor with an active viewport sender: if teardown ordering
		// is wrong, this is where it crashes.
		if (StepFrames == 1)
		{
			if (!OpenSender(ShutdownSenderName))
			{
				FailAndFinish(TEXT("shutdown: SpoutSender rejected the final sender"));
				return false;
			}
			return false;
		}

		USpoutBPFunctionLibrary::SpoutSender(
			ShutdownSenderName, ESpoutSendTextureFrom::TextureRenderTarget2D, SourceRenderTarget.Get());

		if (StepFrames < (SettleFrames * 4))
		{
			return false;
		}

		FReport* Report = FReport::Get();
		Report->SetField(TEXT("shutdownSenderAdvertised"),
			IsSenderAdvertised(ShutdownSenderName) ? TEXT("true") : TEXT("false"));
		Report->Mark(Markers::ShutdownBegin);

		// Every step passed. SPOUT_SMOKE_PASS is NOT emitted here - the module's shutdown emits it,
		// so the pass marker can only exist if the process also survived teardown.
		bPassed = true;

		FPlatformMisc::RequestExitWithStatus(/*Force*/ false, /*ReturnCode*/ 0);
		return true;
	}
}
