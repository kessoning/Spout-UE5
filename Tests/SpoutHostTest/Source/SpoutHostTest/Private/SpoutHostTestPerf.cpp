#include "SpoutHostTestPerf.h"

#include "SpoutHostTestDiagnostics.h"
#include "SpoutHostTestModule.h"
#include "SpoutHostTestReport.h"

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
		if (Name == TEXT("sender1080p"))     return EPerfScenario::Sender1080p;
		if (Name == TEXT("receiver1080p"))   return EPerfScenario::Receiver1080p;
		if (Name == TEXT("sender4k"))        return EPerfScenario::Sender4K;
		if (Name == TEXT("receiver4k"))      return EPerfScenario::Receiver4K;
		if (Name == TEXT("multistream"))     return EPerfScenario::MultiStream;
		if (Name == TEXT("cpufallback"))     return EPerfScenario::CpuFallback;
		if (Name == TEXT("idlereceiver"))    return EPerfScenario::IdleReceiver;
		if (Name == TEXT("resizechurn"))     return EPerfScenario::ResizeChurn;
		if (Name == TEXT("startupshutdown")) return EPerfScenario::StartupShutdown;

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
		FPlatformMisc::RequestExitWithStatus(false, 5);
	}

	bool FPerfHarness::SetUpScenario()
	{
		switch (Scenario)
		{
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

		// Startup/shutdown churn: close and reopen every stream every 30 frames.
		if (Scenario == EPerfScenario::StartupShutdown && (InFrameIndex % 30) == 0 && InFrameIndex > 0)
		{
			for (int32 Index = 0; Index < StreamCount; ++Index)
			{
				UTexture2D* Texture = ReceivedTextures[Index].Get();
				UMaterialInstanceDynamic* Material = ReceivedMaterials[Index].Get();
				USpoutBPFunctionLibrary::CloseReceiver(SenderNames[Index], Texture, Material);
				ReceivedTextures[Index].Reset();
				ReceivedMaterials[Index].Reset();
				USpoutBPFunctionLibrary::CloseSender(SenderNames[Index]);
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
				for (int32 Index = 0; Index < StreamCount; ++Index)
				{
					if (!SourceRenderTargets[Index].IsValid())
					{
						continue;
					}
					const bool bAccepted = USpoutBPFunctionLibrary::SpoutSender(
						SenderNames[Index], ESpoutSendTextureFrom::TextureRenderTarget2D,
						SourceRenderTargets[Index].Get());
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

				const bool bAccepted = USpoutBPFunctionLibrary::SpoutReceiver(
					SenderNames[Index], ParentMaterial, FName(TEXT("SpoutTexture")),
					Material, Texture, nullptr, ESpoutColorSpace::Auto);

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
		}

		++FrameIndex;
		DriveScenario(FrameIndex);

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

		RecordSample(DeltaSeconds);

		if ((Now - MeasureStartTimeSeconds) >= MeasureSeconds)
		{
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
		Json += FString::Printf(TEXT("      \"r.Spout.GPUReceiver\": %d\n"), bGpuReceiverPath ? 1 : 0);
		Json += TEXT("    },\n");
		Json += FString::Printf(TEXT("    \"warmupSeconds\": %.1f,\n"), WarmupSeconds);
		Json += FString::Printf(TEXT("    \"requestedMeasureSeconds\": %.1f\n"), MeasureSeconds);
		Json += TEXT("  },\n");

		Json += TEXT("  \"results\": {\n");
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
