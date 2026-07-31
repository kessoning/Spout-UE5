#include "SpoutHostTestSmoke.h"

#include "SpoutHostTestDiagnostics.h"
#include "SpoutHostTestModule.h"
#include "SpoutHostTestReport.h"

#include "SpoutBPFunctionLibrary.h"

#include "Engine/Engine.h"
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
#include "Misc/Parse.h"
#include "Misc/Paths.h"

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
		}
		Advance(EStep::Finished);

		// Non-zero exit status so a caller that only has the process result still sees a failure.
		FPlatformMisc::RequestExitWithStatus(/*Force*/ false, /*ReturnCode*/ 3);
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
			case EStep::StaleSessionCheck:    Advance(EStep::ShutdownWithActivity); break;
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

	bool FSmokeRunner::StepShutdownWithActivity()
	{
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
