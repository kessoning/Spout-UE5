// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#include "SpoutHostTestPerf.h"

#include "SpoutHostTestDiagnostics.h"
#include "SpoutHostTestModule.h"
#include "SpoutHostTestReport.h"
#include "SpoutHostTestPixels.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"

#include "SpoutBPFunctionLibrary.h"

#include "DynamicRHI.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RenderTimer.h"
#include "Runtime/Launch/Resources/Version.h"

// GPU frame time moved twice across the supported range. Rather than report it as unavailable on
// four of the six engines, each location is used where it exists:
//   5.3, 5.4  - GGPUFrameTime, ENGINE_API, EngineGlobals.h
//   5.5       - GGPUFrameTime, RHI_API, RHIGlobals.h
//   5.6+      - GRHIGPUFrameTimeHistory, RHI_API, GPUProfiler.h
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	#include "GPUProfiler.h"
	#define SPOUTHOST_GPU_TIME_HISTORY 1
#elif ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 5
	#include "RHIGlobals.h"
	#define SPOUTHOST_GPU_TIME_GLOBAL 1
#else
	#include "EngineGlobals.h"
	#define SPOUTHOST_GPU_TIME_GLOBAL 1
#endif

namespace SpoutHostTest
{
	namespace
	{
		/** Milliseconds of GPU time for the most recent completed frame, or -1 when unavailable. */
		double ReadGpuFrameMilliseconds()
		{
#if defined(SPOUTHOST_GPU_TIME_HISTORY)
			static FRHIGPUFrameTimeHistory::FState State;
			uint64 Cycles = 0;
			double Latest = -1.0;
			while (State.PopFrameCycles(Cycles) != FRHIGPUFrameTimeHistory::EResult::Empty)
			{
				Latest = FPlatformTime::ToMilliseconds64(Cycles);
			}
			return Latest;
#else
			return GGPUFrameTime > 0 ? static_cast<double>(FPlatformTime::ToMilliseconds(GGPUFrameTime)) : -1.0;
#endif
		}

		double Percentile(const TArray<double>& SortedValues, double Fraction)
		{
			if (SortedValues.Num() == 0)
			{
				return -1.0;
			}
			const int32 Index = FMath::Clamp(
				FMath::RoundToInt(Fraction * (SortedValues.Num() - 1)), 0, SortedValues.Num() - 1);
			return SortedValues[Index];
		}

		double Mean(const TArray<double>& Values)
		{
			if (Values.Num() == 0)
			{
				return -1.0;
			}
			double Sum = 0.0;
			for (double Value : Values)
			{
				Sum += Value;
			}
			return Sum / Values.Num();
		}

		FString JsonNumber(double Value)
		{
			return Value < 0.0 ? FString(TEXT("null")) : FString::Printf(TEXT("%.4f"), Value);
		}
	}

	FPerfHarness* FPerfHarness::Instance = nullptr;

	EPerfScenario FPerfHarness::GetRequestedScenario()
	{
		FString Name;
		if (!FParse::Value(FCommandLine::Get(), TEXT("-SpoutPerf="), Name) || Name.IsEmpty())
		{
			return EPerfScenario::None;
		}

		Name = Name.ToLower();
		if (Name == TEXT("baseline")) return EPerfScenario::Baseline;
		if (Name == TEXT("sender1080p"))     return EPerfScenario::Sender1080p;
		if (Name == TEXT("receiver1080p"))   return EPerfScenario::Receiver1080p;
		if (Name == TEXT("sender4k"))        return EPerfScenario::Sender4K;
		if (Name == TEXT("receiver4k"))      return EPerfScenario::Receiver4K;
		if (Name == TEXT("multistream"))     return EPerfScenario::MultiStream;
		if (Name == TEXT("cpufallback"))     return EPerfScenario::CpuFallback;
		if (Name == TEXT("idlereceiver"))    return EPerfScenario::IdleReceiver;
		if (Name == TEXT("resizechurn"))     return EPerfScenario::ResizeChurn;
		if (Name == TEXT("startupshutdown")) return EPerfScenario::StartupShutdown;
		if (Name == TEXT("restartpaced"))    return EPerfScenario::RestartPaced;

		// Deliberately not None: an unrecognised name must fail the run rather than quietly turn
		// into "no performance test requested" and let the matrix report a pass.
		return EPerfScenario::None;
	}

	bool FPerfHarness::IsRequested()
	{
		FString Name;
		return FParse::Value(FCommandLine::Get(), TEXT("-SpoutPerf="), Name) && !Name.IsEmpty();
	}

	bool FPerfHarness::WasStarted()  { return Instance != nullptr; }
	bool FPerfHarness::HasPassed()   { return Instance != nullptr && Instance->bPassed; }

	void FPerfHarness::Start()
	{
		if (Instance != nullptr || !IsRequested())
		{
			return;
		}

		Instance = new FPerfHarness();
		Instance->Scenario = GetRequestedScenario();

		FParse::Value(FCommandLine::Get(), TEXT("-SpoutPerf="), Instance->ScenarioName);

		int32 Seconds = 60;
		if (FParse::Value(FCommandLine::Get(), TEXT("-SpoutPerfSeconds="), Seconds))
		{
			Instance->MeasureSeconds = FMath::Max(1, Seconds);
		}
		int32 Warmup = 5;
		if (FParse::Value(FCommandLine::Get(), TEXT("-SpoutPerfWarmupSeconds="), Warmup))
		{
			Instance->WarmupSeconds = FMath::Max(0, Warmup);
		}
		int32 Streams = 4;
		if (FParse::Value(FCommandLine::Get(), TEXT("-SpoutPerfStreams="), Streams))
		{
			Instance->StreamCount = FMath::Clamp(Streams, 1, 32);
		}

		Instance->StartTimeSeconds = FPlatformTime::Seconds();

		Instance->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(Instance, &FPerfHarness::Tick), 0.0f);
	}

	void FPerfHarness::Stop()
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

		Instance->SourceRenderTargets.Empty();
		Instance->ReceivedTextures.Empty();
		Instance->ReceivedMaterials.Empty();
		Instance->PixelProbes.Empty();
	}

	UTextureRenderTarget2D* FPerfHarness::MakeRenderTarget(int32 InWidth, int32 InHeight) const
	{
		return USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(
			InWidth, InHeight, PF_B8G8R8A8, /*bForceLinearGamma*/ true);
	}

	void FPerfHarness::FailAndFinish(const FString& Reason)
	{
		bFinished = true;
		bPassed = false;
		FailureReason = Reason;

		if (FReport* Report = FReport::Get())
		{
			Report->Mark(FString::Printf(TEXT("%s%s"), Markers::PerfFailPrefix, *Reason));
			Report->Fail(Reason);
		}

		WriteResults();
		// Older Windows launchers can return zero after a non-forced exit request.
		// Preserve the failure report, drain/release the plugin streams, then guarantee the status.
		USpoutBPFunctionLibrary::CloseAllStreams();
		FPlatformMisc::RequestExitWithStatus(true, 5);
	}

	bool FPerfHarness::SetUpScenario()
	{
		switch (Scenario)
		{
		case EPerfScenario::Baseline:
			Width=Height=StreamCount=0; return true;
		case EPerfScenario::Sender1080p:
			Width = 1920; Height = 1080; StreamCount = 1; bUsesSender = true;  bUsesReceiver = false; break;
		case EPerfScenario::Receiver1080p:
			Width = 1920; Height = 1080; StreamCount = 1; bUsesSender = true;  bUsesReceiver = true;  break;
		case EPerfScenario::Sender4K:
			Width = 3840; Height = 2160; StreamCount = 1; bUsesSender = true;  bUsesReceiver = false; break;
		case EPerfScenario::Receiver4K:
			Width = 3840; Height = 2160; StreamCount = 1; bUsesSender = true;  bUsesReceiver = true;  break;
		case EPerfScenario::MultiStream:
			Width = 1920; Height = 1080; bUsesSender = true; bUsesReceiver = true; break;
		case EPerfScenario::CpuFallback:
			Width = 1920; Height = 1080; StreamCount = 1; bUsesSender = true; bUsesReceiver = true;
			bGpuReceiverPath = false; break;
		case EPerfScenario::IdleReceiver:
			Width = 1920; Height = 1080; StreamCount = 1; bUsesSender = true; bUsesReceiver = true;  break;
		case EPerfScenario::ResizeChurn:
			Width = 1920; Height = 1080; StreamCount = 1; bUsesSender = true; bUsesReceiver = true;  break;
		case EPerfScenario::StartupShutdown:
		case EPerfScenario::RestartPaced:
			Width = 1920; Height = 1080; StreamCount = 1; bUsesSender = true; bUsesReceiver = true;  break;
		default:
			return false;
		}

		if (!bGpuReceiverPath)
		{
			// The CPU fallback has no public switch other than this CVar, so the scenario sets it
			// rather than depending on the caller remembering to pass -ExecCmds. Which path was
			// actually used is recorded in the result file.
			if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.GPUReceiver")))
			{
				CVar->Set(0, ECVF_SetByCode);
			}
			else
			{
				FailAndFinish(TEXT("r.Spout.GPUReceiver does not exist, so the CPU fallback cannot be forced"));
				return false;
			}
		}

		const uint32 ProcessId = FPlatformProcess::GetCurrentProcessId();
		for (int32 Index = 0; Index < StreamCount; ++Index)
		{
			UTextureRenderTarget2D* RenderTarget = MakeRenderTarget(Width, Height);
			if (RenderTarget == nullptr)
			{
				FailAndFinish(FString::Printf(
					TEXT("could not create a %dx%d render target for stream %d"), Width, Height, Index));
				return false;
			}
			SourceRenderTargets.Emplace(RenderTarget);
			SenderNames.Add(FName(*FString::Printf(TEXT("SpoutPerf_%u_%d"), ProcessId, Index)));
			ReceivedTextures.Emplace();
			ReceivedMaterials.Emplace();
			PublishedSequences.Add(0); ObservedSequences.Add(0);
			PreviousReceiverSubmissions.Add(0);
			LastProgressTimes.Add(FPlatformTime::Seconds());
			if (!bUsesReceiver)
			{
				auto Probe=MakeUnique<FViewportSenderProbe>(); FString Error;
				if (!Probe->Initialize(Error)) { FailAndFinish(Error); return false; }
				PixelProbes.Add(MoveTemp(Probe));
			}
		}

		return true;
	}

	void FPerfHarness::DriveScenario(int32 InFrameIndex)
	{
		// Resize churn: every 60 frames the source is recreated at a different resolution and the
		// sender restarted, so the resize/reconnect cost shows up as a measurable spike.
		if (Scenario == EPerfScenario::ResizeChurn && (InFrameIndex % 60) == 0 && InFrameIndex > 0)
		{
			const bool bLarge = ((InFrameIndex / 60) % 2) == 0;
			const int32 NewWidth  = bLarge ? 1920 : 1280;
			const int32 NewHeight = bLarge ? 1080 : 720;

			USpoutBPFunctionLibrary::CloseSender(SenderNames[0]);
			if (UTextureRenderTarget2D* Replacement = MakeRenderTarget(NewWidth, NewHeight))
			{
				SourceRenderTargets[0].Reset(Replacement);
				++ResizeEvents;
			}
		}

		// Startup/shutdown churn: close and reopen every stream every 30 frames. That rate is a stress
		// case: closed outputs stay reserved until garbage collection, so it exhausts the default
		// retention budget by contract. The paced variant is the same close/reopen sequence every
		// 120 frames (2 s at the 60 Hz work rate), the qualified close/reopen workload.
		const int32 RestartIntervalFrames =
			Scenario == EPerfScenario::StartupShutdown ? 30 : (Scenario == EPerfScenario::RestartPaced ? 120 : 0);
		if (RestartIntervalFrames > 0 && (InFrameIndex % RestartIntervalFrames) == 0 && InFrameIndex > 0)
		{
			for (int32 Index = 0; Index < StreamCount; ++Index)
			{
				UTexture2D* Texture = ReceivedTextures[Index].Get();
				UMaterialInstanceDynamic* Material = ReceivedMaterials[Index].Get();
				const double CloseReceiverStart = FPlatformTime::Seconds();
				USpoutBPFunctionLibrary::CloseReceiver(SenderNames[Index], Texture, Material);
				if (bMeasuring) { const double Elapsed = FPlatformTime::Seconds()-CloseReceiverStart; CloseReceiverSeconds += Elapsed; PeakCloseReceiverMs=FMath::Max(PeakCloseReceiverMs,Elapsed*1000.0); }

				ReceivedTextures[Index].Reset();
				ReceivedMaterials[Index].Reset();
				const double CloseSenderStart = FPlatformTime::Seconds();
				USpoutBPFunctionLibrary::CloseSender(SenderNames[Index]);
				if (bMeasuring) { const double Elapsed=FPlatformTime::Seconds()-CloseSenderStart; CloseSenderSeconds+=Elapsed; PeakCloseSenderMs=FMath::Max(PeakCloseSenderMs,Elapsed*1000.0); }

			}
			++StreamRestartEvents;
		}

		if (bUsesSender)
		{
			// The idle-receiver scenario publishes once a second so the receiver spends ~59 of every
			// 60 frames with no new frame to copy. That gap is the cost being measured.
			const bool bPublishThisFrame =
				(Scenario != EPerfScenario::IdleReceiver) || ((InFrameIndex % 60) == 0);

			if (bPublishThisFrame)
			{
				if (bMeasuring) ExpectedDeliveries += StreamCount;
				const bool Suppress=bMeasuring && FParse::Param(FCommandLine::Get(),TEXT("SpoutPerfSuppressTransfers"));
				if (!Suppress) PublishTimes.Add(InFrameIndex,FPlatformTime::Seconds());
				// Bound diagnostic history even during arbitrarily long runs.
				for (auto It=PublishTimes.CreateIterator(); It; ++It) if (It.Key()<InFrameIndex-600) It.RemoveCurrent();
				for (int32 Index = 0; Index < StreamCount; ++Index)
				{
					if (Suppress) continue;
					if (!SourceRenderTargets[Index].IsValid())
					{
						continue;
					}
					PublishedSequences[Index]=InFrameIndex;
					const FColor Pixel(InFrameIndex&255,(InFrameIndex>>8)&255,(InFrameIndex>>16)&255,0xa7);
					auto* Resource=SourceRenderTargets[Index]->GameThread_GetRenderTargetResource();
					ENQUEUE_RENDER_COMMAND(SpoutPerfSequence)([Resource,Pixel](FRHICommandListImmediate& Cmd)
					{ Cmd.UpdateTexture2D(Resource->GetTextureRHI(),0,FUpdateTextureRegion2D(0,0,0,0,1,1),4,reinterpret_cast<const uint8*>(&Pixel)); });
					const double SendCallStart=FPlatformTime::Seconds();
					const bool bAccepted = USpoutBPFunctionLibrary::SpoutSender(
						SenderNames[Index], ESpoutSendTextureFrom::TextureRenderTarget2D,
						SourceRenderTargets[Index].Get());
					if(bMeasuring) PeakSendCallMs=FMath::Max(PeakSendCallMs,(FPlatformTime::Seconds()-SendCallStart)*1000.0);
					bAccepted ? ++AcceptedSends : ++RejectedSends;
				}
			}
		}

		if (bUsesReceiver)
		{
			UMaterial* ParentMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
			for (int32 Index = 0; Index < StreamCount; ++Index)
			{
				UMaterialInstanceDynamic* Material = ReceivedMaterials[Index].Get();
				UTexture2D* Texture = ReceivedTextures[Index].Get();

				const double ReceiveCallStart=FPlatformTime::Seconds();
				const bool bAccepted = USpoutBPFunctionLibrary::SpoutReceiver(
					SenderNames[Index], ParentMaterial, FName(TEXT("SpoutTexture")),
					Material, Texture, nullptr, ESpoutColorSpace::Auto);

				if(bMeasuring) PeakReceiveCallMs=FMath::Max(PeakReceiveCallMs,(FPlatformTime::Seconds()-ReceiveCallStart)*1000.0);
				if (bAccepted)
				{
					++AcceptedReceives;
					ReceivedMaterials[Index].Reset(Material);
					ReceivedTextures[Index].Reset(Texture);
				}
				else
				{
					++RejectedReceives;
				}
			}
		}
	}

	void FPerfHarness::ObserveDelivery()
	{
		if (Scenario==EPerfScenario::Baseline) return;
		const double Start=FPlatformTime::Seconds();
		// This deliberately drains the queue: timings include a serialized pixel observer.
		// It is a delivery-qualified baseline, not a claim about unobserved asynchronous throughput.
		FlushRenderingCommands();
		for (int32 Index=0; Index<StreamCount; ++Index)
		{
			FColor Pixel(0,0,0,0); bool Have=false;
			if (bUsesReceiver && ReceivedTextures[Index].IsValid())
			{
				// Status is cumulative for the session, including outputs replaced on resize.
				// Validate only an actual new submission; a cleared replacement awaiting its
				// first copy is a missing opportunity and still counts against the delivery gate.
				const int64 Submitted = USpoutBPFunctionLibrary::GetSpoutStreamStatus(SenderNames[Index], ESpoutType::Receiver).FramesSubmitted;
				if (Submitted <= PreviousReceiverSubmissions[Index]) continue;
				const auto Pixels=ReadPixels(ReceivedTextures[Index]->GetResource(),1,1);
				if (Pixels.Num()==1) { Pixel=Pixels[0]; Have=true; }
			}
			else if (!bUsesReceiver) Have=PixelProbes[Index]->ReadFirstPixel(SenderNames[Index].ToString(),Pixel);
			if (!Have) continue;
			const int32 Sequence=Pixel.R | (Pixel.G<<8) | (Pixel.B<<16);
			if (Pixel.A!=0xa7 || Sequence>PublishedSequences[Index]) { if(bMeasuring) ++PixelMismatches; continue; }
			if (Sequence<=ObservedSequences[Index]) { if(bMeasuring) ++StaleObservations; continue; }
			ObservedSequences[Index]=Sequence;
			LastProgressTimes[Index]=FPlatformTime::Seconds();
			if (bMeasuring)
			{
				const double* Published=PublishTimes.Find(Sequence);
				if (Published && *Published>=MeasureStartTimeSeconds)
				{ ++ObservedDeliveries; DeliveryLatenciesMs.Add((FPlatformTime::Seconds()-*Published)*1000.0); }
			}
		}
		if(bMeasuring) { const double Elapsed=FPlatformTime::Seconds()-Start; ValidationSeconds+=Elapsed; PeakObserverMs=FMath::Max(PeakObserverMs,Elapsed*1000.0); }
	}

	void FPerfHarness::RecordSample(float DeltaSeconds)
	{
		FFrameSample Sample;
		Sample.TotalMs = static_cast<double>(DeltaSeconds) * 1000.0;
		Sample.GameThreadMs = static_cast<double>(FPlatformTime::ToMilliseconds(GGameThreadTime));
		Sample.RenderThreadMs = static_cast<double>(FPlatformTime::ToMilliseconds(GRenderThreadTime));
		Sample.GpuMs = ReadGpuFrameMilliseconds();
		Samples.Add(Sample);

		const FCPUTime CpuTime = FPlatformTime::GetCPUTime();
		CpuPercentAccumulator += CpuTime.CPUTimePct;
		++CpuPercentSamples;

		const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
		PeakWorkingSetBytes = FMath::Max(PeakWorkingSetBytes, static_cast<uint64>(MemoryStats.UsedPhysical));

		++MeasuredFrames;
	}

	bool FPerfHarness::Tick(float DeltaSeconds)
	{
		if (bFinished)
		{
			return true;
		}

		if (FrameIndex == 0)
		{
			if (Scenario == EPerfScenario::None)
			{
				FString Requested;
				FParse::Value(FCommandLine::Get(), TEXT("-SpoutPerf="), Requested);
				FailAndFinish(FString::Printf(TEXT("unknown performance scenario '%s'"), *Requested));
				return true;
			}

			if (FReport* Report = FReport::Get())
			{
				Report->MarkValue(Markers::PerfBegin, ScenarioName);
			}

			if (!SetUpScenario())
			{
				return true;
			}
			if (Scenario!=EPerfScenario::Baseline)
			{
				FString Error;
				if (!QualifyExternalPixels(Error)) { FailAndFinish(Error); return true; }
			}
			StartTimeSeconds=FPlatformTime::Seconds();
		}

		const double WorkNow=FPlatformTime::Seconds();
		if (WorkNow<NextWorkSeconds) return true;
		const float WorkDelta=LastWorkSeconds>0.0 ? static_cast<float>(WorkNow-LastWorkSeconds) : DeltaSeconds;
		LastWorkSeconds=WorkNow;
		NextWorkSeconds=FMath::Max(NextWorkSeconds+1.0/60.0,WorkNow);
		++FrameIndex;
		for (int32 Index = 0; bUsesReceiver && Index < StreamCount; ++Index)
			PreviousReceiverSubmissions[Index] = USpoutBPFunctionLibrary::GetSpoutStreamStatus(SenderNames[Index], ESpoutType::Receiver).FramesSubmitted;
		DriveScenario(FrameIndex);
		ObserveDelivery();

		const double Now = FPlatformTime::Seconds();
		const double Elapsed = Now - StartTimeSeconds;

		if (!bMeasuring)
		{
			if (Elapsed < WarmupSeconds)
			{
				return true;
			}

			// A scenario that never once did the thing it is measuring has nothing to report.
			// Publishing zeros would be a fabricated baseline, so it fails instead.
			//
			// The first few frames of every run legitimately reject: a render target's RHI resource
			// is created on the render thread, so it is not publishable on the frame it is made.
			// That is what the warmup absorbs - but only the warmup. Zero acceptances by the end of
			// it is a real failure.
			if (bUsesSender && AcceptedSends == 0)
			{
				FailAndFinish(FString::Printf(
					TEXT("no send was accepted during %.0f s of warmup (%d rejected); there is nothing to measure"),
					WarmupSeconds, RejectedSends));
				return true;
			}
			if (bUsesReceiver && AcceptedReceives == 0)
			{
				FailAndFinish(FString::Printf(
					TEXT("no receive was accepted during %.0f s of warmup (%d rejected); there is nothing to measure"),
					WarmupSeconds, RejectedReceives));
				return true;
			}

			bMeasuring = true;
			MeasureStartTimeSeconds = Now;

			// Warm-up counters are not part of the baseline.
			AcceptedSends = RejectedSends = AcceptedReceives = RejectedReceives = 0;
			return true;
		}

		RecordSample(WorkDelta);

		if ((Now - MeasureStartTimeSeconds) >= MeasureSeconds)
		{
			const double TargetDeliveries=MeasureSeconds*(Scenario==EPerfScenario::IdleReceiver?1.0:60.0)*StreamCount;
			const double MaxSilence=Scenario==EPerfScenario::IdleReceiver ? 1.5 : 0.5;
			for (double LastProgress : LastProgressTimes)
			{
				if (Now-LastProgress>MaxSilence) { FailAndFinish(TEXT("measured stream stopped making pixel progress before the run ended")); return true; }
			}
			if (Scenario!=EPerfScenario::Baseline && (ExpectedDeliveries<=0 || ObservedDeliveries<ExpectedDeliveries*0.9 ||
				ObservedDeliveries<TargetDeliveries*0.9 || PixelMismatches>0))
			{
				FailAndFinish(FString::Printf(TEXT("measured delivery gate failed: %d observed / %d published opportunities, target %.0f, %d corrupt observations"),ObservedDeliveries,ExpectedDeliveries,TargetDeliveries,PixelMismatches));
				return true;
			}
			bFinished = true;
			bPassed = true;
			WriteResults();

			if (FReport* Report = FReport::Get())
			{
				Report->Mark(Markers::PerfPass);
			}
			FPlatformMisc::RequestExitWithStatus(false, 0);
		}

		return true;
	}

	void FPerfHarness::WriteResults()
	{
		FReport* Report = FReport::Get();
		if (Report == nullptr)
		{
			return;
		}

		TArray<double> Totals, GameTimes, RenderTimes, GpuTimes;
		Totals.Reserve(Samples.Num());
		GameTimes.Reserve(Samples.Num());
		RenderTimes.Reserve(Samples.Num());
		GpuTimes.Reserve(Samples.Num());

		for (const FFrameSample& Sample : Samples)
		{
			Totals.Add(Sample.TotalMs);
			GameTimes.Add(Sample.GameThreadMs);
			RenderTimes.Add(Sample.RenderThreadMs);
			if (Sample.GpuMs >= 0.0)
			{
				GpuTimes.Add(Sample.GpuMs);
			}
		}

		TArray<double> SortedTotals = Totals;
		SortedTotals.Sort();

		double PeakTotal = -1.0;
		for (double Value : Totals)
		{
			PeakTotal = FMath::Max(PeakTotal, Value);
		}

		FTextureMemoryStats TextureStats;
		bool bHaveTextureStats = false;
		if (GDynamicRHI != nullptr)
		{
			RHIGetTextureMemoryStats(TextureStats);
			bHaveTextureStats = true;
		}

		const double MeasuredSeconds = bMeasuring
			? (FPlatformTime::Seconds() - MeasureStartTimeSeconds)
			: 0.0;

		FString Json;
		Json += TEXT("{\n");
		TArray<double> SortedLatency=DeliveryLatenciesMs; SortedLatency.Sort();
		double MaxNoProgressSeconds=0.0;
		for(double LastProgress : LastProgressTimes) MaxNoProgressSeconds=FMath::Max(MaxNoProgressSeconds,FPlatformTime::Seconds()-LastProgress);
		Json += FString::Printf(TEXT("  \"maxNoProgressSeconds\":%.6f,\n"),MaxNoProgressSeconds);
		Json += FString::Printf(TEXT("  \"delivery\": {\"targetHz\":60,\"expected\":%d,\"observed\":%d,\"ratio\":%.6f,\"drops\":%d,\"corrupt\":%d,\"staleObservations\":%d,\"maxObserverQueueDepth\":1,\"observerSeconds\":%.6f,\"latencyMeanMs\":%s,\"latencyP95Ms\":%s,\"timingScope\":\"host including serialized pixel observer; baseline has no streams\"},\n"),
			ExpectedDeliveries,ObservedDeliveries,ExpectedDeliveries?double(ObservedDeliveries)/ExpectedDeliveries:1.0,FMath::Max(0,ExpectedDeliveries-ObservedDeliveries),PixelMismatches,StaleObservations,ValidationSeconds,*JsonNumber(Mean(DeliveryLatenciesMs)),*JsonNumber(Percentile(SortedLatency,0.95)));
		Json += TEXT("  \"schema\": \"spouthosttest-performance/1\",\n");
		Json += FString::Printf(TEXT("  \"scenario\": \"%s\",\n"), *EscapeJson(ScenarioName));
		Json += FString::Printf(TEXT("  \"passed\": %s,\n"), bPassed ? TEXT("true") : TEXT("false"));
		Json += FString::Printf(TEXT("  \"failureReason\": %s,\n"),
			FailureReason.IsEmpty() ? TEXT("null") : *FString::Printf(TEXT("\"%s\""), *EscapeJson(FailureReason)));
		Json += FString::Printf(TEXT("  \"engineVersion\": \"%s\",\n"), *Diagnostics::GetEngineVersionString());
		Json += FString::Printf(TEXT("  \"buildConfiguration\": \"%s\",\n"), *Diagnostics::GetBuildConfigurationName());
		Json += FString::Printf(TEXT("  \"commandLine\": \"%s\",\n"), *EscapeJson(FCommandLine::Get()));
		Json += FString::Printf(TEXT("  \"runtimeDllPath\": \"%s\",\n"),
			*EscapeJson(Diagnostics::QueryRuntimeDll().ResolvedPath));

		Json += TEXT("  \"configuration\": {\n");
		Json += FString::Printf(TEXT("    \"width\": %d,\n"), Width);
		Json += FString::Printf(TEXT("    \"height\": %d,\n"), Height);
		Json += TEXT("    \"pixelFormat\": \"PF_B8G8R8A8\",\n");
		Json += FString::Printf(TEXT("    \"streamCount\": %d,\n"), StreamCount);
		Json += FString::Printf(TEXT("    \"usesSender\": %s,\n"), bUsesSender ? TEXT("true") : TEXT("false"));
		Json += FString::Printf(TEXT("    \"usesReceiver\": %s,\n"), bUsesReceiver ? TEXT("true") : TEXT("false"));
		Json += FString::Printf(TEXT("    \"receiverMode\": \"%s\",\n"),
			bGpuReceiverPath ? TEXT("gpu-direct") : TEXT("cpu-fallback"));
		Json += TEXT("    \"cvars\": {\n");
		Json += FString::Printf(TEXT("      \"r.Spout.GPUReceiver\": %d,\n"), bGpuReceiverPath ? 1 : 0);
		Json += FString::Printf(TEXT("      \"r.Spout.MaxRetainedMiB\": %d\n"), IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.MaxRetainedMiB"))->GetInt());
		Json += TEXT("    },\n");
		Json += FString::Printf(TEXT("    \"warmupSeconds\": %.1f,\n"), WarmupSeconds);
		Json += FString::Printf(TEXT("    \"requestedMeasureSeconds\": %.1f\n"), MeasureSeconds);
		Json += TEXT("  },\n");

		Json += TEXT("  \"results\": {\n");
		const auto RuntimeStatus = USpoutBPFunctionLibrary::GetSpoutRuntimeStatus();
		Json += FString::Printf(TEXT("    \"peakEstimatedRetainedBytes\": %lld, \"rejectedAllocations\": %lld, \"peakPendingCommands\": %d, \"queueRejections\": %lld,\n"), RuntimeStatus.PeakEstimatedRetainedBytes, RuntimeStatus.RejectedAllocations, RuntimeStatus.PeakPendingCommands, RuntimeStatus.RejectedRequests);
		Json += FString::Printf(TEXT("    \"frameCount\": %d,\n"), MeasuredFrames);
		Json += FString::Printf(TEXT("    \"measuredSeconds\": %.3f,\n"), MeasuredSeconds);
		Json += FString::Printf(TEXT("    \"averageFps\": %s,\n"),
			*JsonNumber(MeasuredSeconds > 0.0 ? (MeasuredFrames / MeasuredSeconds) : -1.0));
		Json += FString::Printf(TEXT("    \"frameTimeMeanMs\": %s,\n"), *JsonNumber(Mean(Totals)));
		Json += FString::Printf(TEXT("    \"frameTimeP95Ms\": %s,\n"), *JsonNumber(Percentile(SortedTotals, 0.95)));
		Json += FString::Printf(TEXT("    \"frameTimeP99Ms\": %s,\n"), *JsonNumber(Percentile(SortedTotals, 0.99)));
		Json += FString::Printf(TEXT("    \"frameTimePeakMs\": %s,\n"), *JsonNumber(PeakTotal));
		Json += FString::Printf(TEXT("    \"gameThreadMeanMs\": %s,\n"), *JsonNumber(Mean(GameTimes)));
		Json += FString::Printf(TEXT("    \"renderThreadMeanMs\": %s,\n"), *JsonNumber(Mean(RenderTimes)));
		Json += FString::Printf(TEXT("    \"gpuMeanMs\": %s,\n"), *JsonNumber(Mean(GpuTimes)));
		Json += FString::Printf(TEXT("    \"gpuSampleCount\": %d,\n"), GpuTimes.Num());
		Json += FString::Printf(TEXT("    \"processCpuPercentMean\": %s,\n"),
			*JsonNumber(CpuPercentSamples > 0 ? (CpuPercentAccumulator / CpuPercentSamples) : -1.0));
		Json += FString::Printf(TEXT("    \"peakWorkingSetBytes\": %llu,\n"), PeakWorkingSetBytes);
		Json += FString::Printf(TEXT("    \"acceptedSends\": %d,\n"), AcceptedSends);
		Json += FString::Printf(TEXT("    \"rejectedSends\": %d,\n"), RejectedSends);
		Json += FString::Printf(TEXT("    \"acceptedReceives\": %d,\n"), AcceptedReceives);
		Json += FString::Printf(TEXT("    \"rejectedReceives\": %d,\n"), RejectedReceives);
		Json += FString::Printf(TEXT("    \"resizeEvents\": %d,\n"), ResizeEvents);
		Json += FString::Printf(TEXT("    \"peakCloseReceiverMs\": %.6f, \"peakCloseSenderMs\": %.6f, \"peakSendCallMs\": %.6f, \"peakReceiveCallMs\": %.6f, \"peakObserverMs\": %.6f,\n"),PeakCloseReceiverMs,PeakCloseSenderMs,PeakSendCallMs,PeakReceiveCallMs,PeakObserverMs);
		Json += FString::Printf(TEXT("    \"closeReceiverSeconds\": %.6f, \"closeSenderSeconds\": %.6f,\n"),CloseReceiverSeconds,CloseSenderSeconds);
		Json += FString::Printf(TEXT("    \"streamRestartEvents\": %d\n"), StreamRestartEvents);
		Json += TEXT("  },\n");

		Json += TEXT("  \"rhiMemory\": ");
		if (bHaveTextureStats)
		{
			Json += TEXT("{\n");
			Json += FString::Printf(TEXT("    \"dedicatedVideoMemoryBytes\": %lld,\n"), TextureStats.DedicatedVideoMemory);
			Json += FString::Printf(TEXT("    \"totalGraphicsMemoryBytes\": %lld,\n"), TextureStats.TotalGraphicsMemory);
			Json += FString::Printf(TEXT("    \"streamingMemoryBytes\": %llu,\n"), TextureStats.StreamingMemorySize);
			Json += FString::Printf(TEXT("    \"nonStreamingMemoryBytes\": %llu\n"), TextureStats.NonStreamingMemorySize);
			Json += TEXT("  }\n");
		}
		else
		{
			Json += TEXT("null\n");
		}
		Json += TEXT("}\n");

		const FString OutputPath = FPaths::Combine(
			Report->GetResultDir(), FString::Printf(TEXT("perf-%s.json"), *ScenarioName.ToLower()));
		FFileHelper::SaveStringToFile(Json, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

		Report->SetField(TEXT("performanceResultPath"), OutputPath);
	}
}
