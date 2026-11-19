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
#include "Math/RandomStream.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RenderTimer.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UObject/UObjectGlobals.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include "Windows/HideWindowsPlatformTypes.h"

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

		/**
		 * The viewport scenario's GPU load when -SpoutPerfGpuLoadCopies= is not given. Calibrated on an RTX 4090 at 1920x1080:
		 * about 16 ms of GPU time per frame. The GPU time does not follow the number of copies in proportion (below about
		 * 80 copies it barely shows), so another GPU needs its own calibration.
		 */
		constexpr int32 DefaultViewportGpuLoadCopies = 105;
		/** The release scenario samples memory this long after its senders close, for one second. */
		constexpr double ReleaseSettleSeconds = 3.0;

		/** Dedicated GPU memory this process uses on Unreal's adapter, as DXGI reports it, or -1 when it cannot be read. */
		int64 QueryLocalVideoMemoryBytes()
		{
			static TRefCountPtr<IDXGIAdapter3> Adapter;
			static bool bLookedUp = false;
			if (!bLookedUp)
			{
				bLookedUp = true;
				ID3D12Device* Device = GDynamicRHI && RHIGetInterfaceType() == ERHIInterfaceType::D3D12
					? static_cast<ID3D12Device*>(GDynamicRHI->RHIGetNativeDevice()) : nullptr;
				TRefCountPtr<IDXGIFactory4> Factory;
				if (Device && SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(Factory.GetInitReference()))))
				{
					Factory->EnumAdapterByLuid(Device->GetAdapterLuid(), __uuidof(IDXGIAdapter3), reinterpret_cast<void**>(Adapter.GetInitReference()));
				}
			}
			DXGI_QUERY_VIDEO_MEMORY_INFO Info = {};
			return Adapter.IsValid() && SUCCEEDED(Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Info))
				? static_cast<int64>(Info.CurrentUsage) : -1;
		}

		int64 Median(TArray<int64> Values)
		{
			if (Values.Num() == 0) return -1;
			Values.Sort();
			return Values[Values.Num() / 2];
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
		if (Name == TEXT("multistream8"))    return EPerfScenario::MultiStream8;
		if (Name == TEXT("receiver1080p10")) return EPerfScenario::Receiver1080p10;
		if (Name == TEXT("receiver4k10"))    return EPerfScenario::Receiver4K10;
		if (Name == TEXT("consumers8"))      return EPerfScenario::Consumers8;
		if (Name == TEXT("consumers32"))     return EPerfScenario::Consumers32;
		if (Name == TEXT("unconsumed"))      return EPerfScenario::Unconsumed;
		if (Name == TEXT("viewportwindow"))  return EPerfScenario::ViewportWindow;
		if (Name == TEXT("viewportrelease")) return EPerfScenario::ViewportRelease;

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

		Instance->GpuLoadCopies = Instance->Scenario == EPerfScenario::ViewportWindow ? DefaultViewportGpuLoadCopies : 0;
		FParse::Value(FCommandLine::Get(), TEXT("-SpoutPerfGpuLoadCopies="), Instance->GpuLoadCopies);
		Instance->GpuLoadCopies = FMath::Clamp(Instance->GpuLoadCopies, 0, 2000);

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
		Instance->GpuLoadSource.Reset();
		Instance->GpuLoadTarget.Reset();
		Instance->ReceivedTextures.Empty();
		Instance->ReceivedMaterials.Empty();
		Instance->ConsumerTextures.Empty();
		Instance->ConsumerMaterials.Empty();
		Instance->PixelProbes.Empty();
	}

	UTextureRenderTarget2D* FPerfHarness::MakeRenderTarget(int32 InWidth, int32 InHeight) const
	{
		return USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(
			InWidth, InHeight, SourceFormat, /*bForceLinearGamma*/ true);
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
		case EPerfScenario::MultiStream8:
			Width = 1920; Height = 1080; StreamCount = 8; bUsesSender = true; bUsesReceiver = true;  break;
		case EPerfScenario::Receiver1080p10:
			Width = 1920; Height = 1080; StreamCount = 1; bUsesSender = true; bUsesReceiver = true;
			SourceFormat = PF_A2B10G10R10; break;
		case EPerfScenario::Receiver4K10:
			Width = 3840; Height = 2160; StreamCount = 1; bUsesSender = true; bUsesReceiver = true;
			SourceFormat = PF_A2B10G10R10; break;
		case EPerfScenario::Consumers8:
			Width = 1920; Height = 1080; StreamCount = 1; bUsesSender = true; bUsesReceiver = true;
			ConsumerCount = 8; break;
		case EPerfScenario::Consumers32:
			Width = 1920; Height = 1080; StreamCount = 1; bUsesSender = true; bUsesReceiver = true;
			ConsumerCount = 32; break;
		case EPerfScenario::Unconsumed:
			Width = 1920; Height = 1080; StreamCount = 1; bUsesSender = true; bUsesReceiver = true;
			bUnconsumed = true; break;
		// The viewport scenarios ask the harness for a 1920x1080 window. What is sent is the window's size; it is
		// recorded from the stream itself.
		case EPerfScenario::ViewportWindow:
			Width = 1920; Height = 1080; StreamCount = 1; bUsesSender = true; bUsesReceiver = false;
			bViewportSend = true; SourceFormat = PF_Unknown; break;
		case EPerfScenario::ViewportRelease:
			// Stream 0 sends the whole window, stream 1 Game Viewport Only: both kinds of viewport texture.
			Width = 1920; Height = 1080; StreamCount = 2; bUsesSender = true; bUsesReceiver = false;
			bViewportSend = true; bViewportRelease = true; SourceFormat = PF_Unknown; break;
		default:
			return false;
		}

		if (GpuLoadCopies > 0)
		{
			// The GPU load: render targets of this harness only, copied on the GPU every frame (DriveScenario).
			for (TStrongObjectPtr<UTextureRenderTarget2D>* Slot : { &GpuLoadSource, &GpuLoadTarget })
			{
				UTextureRenderTarget2D* Load = NewObject<UTextureRenderTarget2D>();
				Load->InitCustomFormat(3840, 2160, PF_FloatRGBA, true);
				Load->UpdateResourceImmediate(true);
				Slot->Reset(Load);
			}
			// Random bytes (fixed seed) in the source: a cleared texture is copied almost free by the GPU's compression, so
			// the cost would not follow the number of copies.
			TArray<uint8> Noise;
			Noise.SetNumUninitialized(3840 * 2160 * 8);
			FRandomStream Random(0x5b07);
			for (int32 Index = 0; Index < Noise.Num(); Index += 4)
			{
				const uint32 Value = static_cast<uint32>(Random.GetUnsignedInt());
				FMemory::Memcpy(&Noise[Index], &Value, 4);
			}
			FTextureRenderTargetResource* NoiseTarget = GpuLoadSource->GameThread_GetRenderTargetResource();
			ENQUEUE_RENDER_COMMAND(SpoutPerfGpuLoadNoise)([NoiseTarget, Noise = MoveTemp(Noise)](FRHICommandListImmediate& Cmd)
			{
				if (FRHITexture* Texture = NoiseTarget ? NoiseTarget->GetTextureRHI() : nullptr)
				{
					Cmd.UpdateTexture2D(Texture, 0, FUpdateTextureRegion2D(0, 0, 0, 0, 3840, 2160), 3840 * 8, Noise.GetData());
				}
			});
			FlushRenderingCommands();
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

		if (ConsumerCount > 1)
		{
			// Each receive call queues one command for its stream, and r.Spout.MaxPendingPerStream (default 4) refuses
			// the calls beyond that limit within a frame. These scenarios measure the per-call cost of many consumers,
			// so they raise the limit; the value used is written to the result file.
			if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.MaxPendingPerStream")))
			{
				CVar->Set(64, ECVF_SetByCode);
			}
			else
			{
				FailAndFinish(TEXT("r.Spout.MaxPendingPerStream does not exist, so the consumer scenarios cannot run"));
				return false;
			}
		}

		const uint32 ProcessId = FPlatformProcess::GetCurrentProcessId();
		if (bViewportSend)
		{
			for (int32 Index = 0; Index < StreamCount; ++Index)
			{
				SenderNames.Add(FName(*FString::Printf(TEXT("SpoutPerf_%u_%d"), ProcessId, Index)));
				LastProgressTimes.Add(FPlatformTime::Seconds());
				LastSenderFrames.Add(-1);
				auto Probe=MakeUnique<FViewportSenderProbe>(); FString Error;
				if (!Probe->Initialize(Error)) { FailAndFinish(Error); return false; }
				if (!Probe->IsFrameCountingAvailable())
				{
					FailAndFinish(TEXT("Spout frame counting is disabled on this machine, so viewport frames cannot be counted externally"));
					return false;
				}
				PixelProbes.Add(MoveTemp(Probe));
			}
			return true;
		}
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
			if (!bUsesReceiver || bUnconsumed)
			{
				auto Probe=MakeUnique<FViewportSenderProbe>(); FString Error;
				if (!Probe->Initialize(Error)) { FailAndFinish(Error); return false; }
				PixelProbes.Add(MoveTemp(Probe));
			}
		}

		for (int32 Consumer = 1; Consumer < ConsumerCount; ++Consumer)
		{
			ConsumerTextures.Emplace();
			ConsumerMaterials.Emplace();
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

		if (GpuLoadCopies > 0 && GpuLoadSource.IsValid() && GpuLoadTarget.IsValid())
		{
			// Queued before this frame's scene and its viewport send, so the send's copy waits behind it on the GPU. The
			// copies go back and forth between the two textures, each reading what the one before wrote, so none of them
			// is redundant work a driver could leave out.
			FTextureRenderTargetResource* LoadSource = GpuLoadSource->GameThread_GetRenderTargetResource();
			FTextureRenderTargetResource* LoadTarget = GpuLoadTarget->GameThread_GetRenderTargetResource();
			const int32 Copies = GpuLoadCopies;
			ENQUEUE_RENDER_COMMAND(SpoutPerfGpuLoad)([LoadSource, LoadTarget, Copies](FRHICommandListImmediate& Cmd)
			{
				FRHITexture* From = LoadSource ? LoadSource->GetTextureRHI() : nullptr;
				FRHITexture* To = LoadTarget ? LoadTarget->GetTextureRHI() : nullptr;
				if (!From || !To) return;
				Cmd.Transition(FRHITransitionInfo(From, ERHIAccess::Unknown, ERHIAccess::CopySrc));
				Cmd.Transition(FRHITransitionInfo(To, ERHIAccess::Unknown, ERHIAccess::CopyDest));
				for (int32 Copy = 0; Copy < Copies; ++Copy)
				{
					if (Copy > 0)
					{
						Swap(From, To);
						Cmd.Transition(FRHITransitionInfo(From, ERHIAccess::CopyDest, ERHIAccess::CopySrc));
						Cmd.Transition(FRHITransitionInfo(To, ERHIAccess::CopySrc, ERHIAccess::CopyDest));
					}
					Cmd.CopyTexture(From, To, FRHICopyTextureInfo());
				}
				Cmd.Transition(FRHITransitionInfo(From, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
				Cmd.Transition(FRHITransitionInfo(To, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
			});
		}

		if (bViewportSend)
		{
			// One send per frame and name; each is published when the frame is presented. The release scenario sends
			// nothing during warm-up (memory before) and nothing once its senders closed (memory after).
			const bool bSendNow = !bViewportRelease || (bMeasuring && !bViewportSendersClosed);
			for (int32 Index = 0; bSendNow && Index < StreamCount; ++Index)
			{
				const ESpoutSendTextureFrom Source = Index == 0 ? ESpoutSendTextureFrom::GameViewport : ESpoutSendTextureFrom::GameViewportOnly;
				const double SendCallStart=FPlatformTime::Seconds();
				const bool bAccepted = USpoutBPFunctionLibrary::SpoutSender(SenderNames[Index], Source, nullptr);
				if(bMeasuring) PeakSendCallMs=FMath::Max(PeakSendCallMs,(FPlatformTime::Seconds()-SendCallStart)*1000.0);
				bAccepted ? ++AcceptedSends : ++RejectedSends;
				if (bMeasuring && bAccepted) ++ExpectedDeliveries;
			}
		}
		else if (bUsesSender)
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
					uint32 Raw = 0;
					if (SourceFormat == PF_A2B10G10R10)
					{
						// R10G10B10A2_UNORM: each sequence byte maps to the nearest 10-bit code, which reads
						// back as the same byte after the engine's 10-to-8-bit requantize; alpha 3 reads as 255.
						const auto To10 = [](uint32 Byte) { return (Byte * 1023u + 127u) / 255u; };
						Raw = To10(InFrameIndex & 255) | (To10((InFrameIndex >> 8) & 255) << 10)
							| (To10((InFrameIndex >> 16) & 255) << 20) | (3u << 30);
					}
					else
					{
						const FColor Pixel(InFrameIndex&255,(InFrameIndex>>8)&255,(InFrameIndex>>16)&255,0xa7);
						FMemory::Memcpy(&Raw, &Pixel, sizeof(Raw));
					}
					auto* Resource=SourceRenderTargets[Index]->GameThread_GetRenderTargetResource();
					ENQUEUE_RENDER_COMMAND(SpoutPerfSequence)([Resource,Raw](FRHICommandListImmediate& Cmd)
					{ Cmd.UpdateTexture2D(Resource->GetTextureRHI(),0,FUpdateTextureRegion2D(0,0,0,0,1,1),4,reinterpret_cast<const uint8*>(&Raw)); });
					const double SendCallStart=FPlatformTime::Seconds();
					const bool bAccepted = USpoutBPFunctionLibrary::SpoutSender(
						SenderNames[Index], ESpoutSendTextureFrom::TextureRenderTarget2D,
						SourceRenderTargets[Index].Get());
					if(bMeasuring) PeakSendCallMs=FMath::Max(PeakSendCallMs,(FPlatformTime::Seconds()-SendCallStart)*1000.0);
					bAccepted ? ++AcceptedSends : ++RejectedSends;
				}
			}
		}

		if (bUsesReceiver && !(bUnconsumed && bMeasuring))
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

				// Further consumers of the same stream, each with its own material instance.
				for (int32 Consumer = 1; Consumer < ConsumerCount; ++Consumer)
				{
					UMaterialInstanceDynamic* ConsumerMaterial = ConsumerMaterials[Consumer - 1].Get();
					UTexture2D* ConsumerTexture = ConsumerTextures[Consumer - 1].Get();
					if (USpoutBPFunctionLibrary::SpoutReceiver(
						SenderNames[Index], ParentMaterial, FName(TEXT("SpoutTexture")),
						ConsumerMaterial, ConsumerTexture, nullptr, ESpoutColorSpace::Auto))
					{
						++ConsumerAcceptedReceives;
						ConsumerMaterials[Consumer - 1].Reset(ConsumerMaterial);
						ConsumerTextures[Consumer - 1].Reset(ConsumerTexture);
					}
					else
					{
						++ConsumerRejectedReceives;
					}
				}
			}
		}
	}

	void FPerfHarness::ObserveDelivery()
	{
		if (Scenario==EPerfScenario::Baseline) return;
		const double Start=FPlatformTime::Seconds();
		if (bViewportSend)
		{
			// Frames are counted outside the plugin: the frame-count semaphore an external receiver reads.
			for (int32 Index=0; Index<StreamCount; ++Index)
			{
				FViewportSenderProbe& Probe=*PixelProbes[Index];
				const FString Name=SenderNames[Index].ToString();
				if (LastSenderFrames[Index] < 0)
				{
					if (!Probe.IsSenderAdvertised(Name)) continue;
					Probe.BeginFrameObservation(Name);
					LastSenderFrames[Index]=Probe.GetSenderFrameCount();
					if (Index == 0) Probe.GetSenderDescription(Name, ViewportStreamWidth, ViewportStreamHeight, ViewportStreamDxgiFormat);
					continue;
				}
				if (!Probe.PollNewFrame()) continue;
				const int64 Count=Probe.GetSenderFrameCount();
				const int64 Advance=Count-LastSenderFrames[Index];
				LastSenderFrames[Index]=Count;
				if (Advance<=0) continue;
				LastProgressTimes[Index]=FPlatformTime::Seconds();
				if (bMeasuring && !bViewportSendersClosed) ObservedDeliveries+=static_cast<int32>(Advance);
			}
			if(bMeasuring) { const double Elapsed=FPlatformTime::Seconds()-Start; ValidationSeconds+=Elapsed; PeakObserverMs=FMath::Max(PeakObserverMs,Elapsed*1000.0); }
			return;
		}
		// This deliberately drains the queue: timings include a serialized pixel observer.
		// It is a delivery-qualified baseline, not a claim about unobserved asynchronous throughput.
		FlushRenderingCommands();
		for (int32 Index=0; Index<StreamCount; ++Index)
		{
			FColor Pixel(0,0,0,0); bool Have=false;
			if (bUsesReceiver && !bUnconsumed && ReceivedTextures[Index].IsValid())
			{
				// Status is cumulative for the session, including outputs replaced on resize.
				// Validate only an actual new submission; a cleared replacement awaiting its
				// first copy is a missing opportunity and still counts against the delivery gate.
				const int64 Submitted = USpoutBPFunctionLibrary::GetSpoutStreamStatus(SenderNames[Index], ESpoutType::Receiver).FramesSubmitted;
				if (Submitted <= PreviousReceiverSubmissions[Index]) continue;
				const auto Pixels=ReadPixels(ReceivedTextures[Index]->GetResource(),1,1);
				if (Pixels.Num()==1) { Pixel=Pixels[0]; Have=true; }
			}
			else if (!bUsesReceiver || bUnconsumed) Have=PixelProbes[Index]->ReadFirstPixel(SenderNames[Index].ToString(),Pixel);
			if (!Have) continue;
			const int32 Sequence=Pixel.R | (Pixel.G<<8) | (Pixel.B<<16);
			const uint8 ExpectedAlpha = SourceFormat == PF_A2B10G10R10 ? 0xff : 0xa7;
			if (Pixel.A!=ExpectedAlpha || Sequence>PublishedSequences[Index]) { if(bMeasuring) ++PixelMismatches; continue; }
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
		const auto SampleMemory = [](TArray<int64>& Local, TArray<int64>& WorkingSet)
		{
			Local.Add(QueryLocalVideoMemoryBytes());
			WorkingSet.Add(static_cast<int64>(FPlatformMemory::GetStats().UsedPhysical));
		};

		if (!bMeasuring)
		{
			if (Elapsed < WarmupSeconds)
			{
				// The release scenario's memory before any viewport sender opened: the last 1.5 s of warm-up.
				if (bViewportRelease && Elapsed >= WarmupSeconds - 1.5) SampleMemory(LocalMemoryBefore, WorkingSetBefore);
				return true;
			}

			// A scenario that never once did the thing it is measuring has nothing to report.
			// Publishing zeros would be a fabricated baseline, so it fails instead.
			//
			// The first few frames of every run legitimately reject: a render target's RHI resource
			// is created on the render thread, so it is not publishable on the frame it is made.
			// That is what the warmup absorbs - but only the warmup. Zero acceptances by the end of
			// it is a real failure.
			if (bUsesSender && AcceptedSends == 0 && !bViewportRelease)
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
			ConsumerAcceptedReceives = ConsumerRejectedReceives = 0;
			return true;
		}

		if (bViewportSendersClosed)
		{
			// The release scenario after its senders closed: memory once the settle time has passed, then the result.
			const double SinceClose = Now - ViewportSendersClosedSeconds;
			if (SinceClose >= ReleaseSettleSeconds) SampleMemory(LocalMemoryAfter, WorkingSetAfter);
			if (SinceClose >= ReleaseSettleSeconds + 1.0)
			{
				bFinished = true;
				bPassed = true;
				WriteResults();
				if (FReport* Report = FReport::Get()) Report->Mark(Markers::PerfPass);
				FPlatformMisc::RequestExitWithStatus(false, 0);
			}
			return true;
		}

		RecordSample(WorkDelta);
		if (bViewportRelease && (Now - MeasureStartTimeSeconds) >= MeasureSeconds - 1.0) SampleMemory(LocalMemoryWhileSending, WorkingSetWhileSending);

		if ((Now - MeasureStartTimeSeconds) >= MeasureSeconds)
		{
			// A viewport stream is published once per rendered frame, so its target follows the frames, not 60 Hz.
			const double TargetDeliveries=bViewportSend ? static_cast<double>(MeasuredFrames)*StreamCount
				: MeasureSeconds*(Scenario==EPerfScenario::IdleReceiver?1.0:60.0)*StreamCount;
			const double MaxSilence=Scenario==EPerfScenario::IdleReceiver ? 1.5 : 0.5;
			for (double LastProgress : LastProgressTimes)
			{
				if (Now-LastProgress>MaxSilence) { FailAndFinish(TEXT("measured stream stopped making pixel progress before the run ended")); return true; }
			}
			if (ConsumerRejectedReceives > 0)
			{
				FailAndFinish(FString::Printf(TEXT("%d receive calls of additional consumers were refused during the measured window"), ConsumerRejectedReceives));
				return true;
			}
			if (Scenario!=EPerfScenario::Baseline && (ExpectedDeliveries<=0 || ObservedDeliveries<ExpectedDeliveries*0.9 ||
				ObservedDeliveries<TargetDeliveries*0.9 || PixelMismatches>0))
			{
				FailAndFinish(FString::Printf(TEXT("measured delivery gate failed: %d observed / %d published opportunities, target %.0f, %d corrupt observations"),ObservedDeliveries,ExpectedDeliveries,TargetDeliveries,PixelMismatches));
				return true;
			}
			if (bViewportRelease)
			{
				// Close the viewport senders, collect garbage, and keep running: memory after is sampled once it settled.
				for (double LastProgress : LastProgressTimes) MaxNoProgressWhileSending=FMath::Max(MaxNoProgressWhileSending,Now-LastProgress);
				for (const FName& Name : SenderNames) USpoutBPFunctionLibrary::CloseSender(Name);
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
				FlushRenderingCommands();
				bViewportSendersClosed = true;
				ViewportSendersClosedSeconds = FPlatformTime::Seconds();
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
		// A frame published by the last warm-up send is counted when it arrives: never report more than was sent.
		if (bViewportSend) ObservedDeliveries=FMath::Min(ObservedDeliveries,ExpectedDeliveries);
		double MaxNoProgressSeconds=0.0;
		for(double LastProgress : LastProgressTimes) MaxNoProgressSeconds=FMath::Max(MaxNoProgressSeconds,FPlatformTime::Seconds()-LastProgress);
		if (bViewportSendersClosed) MaxNoProgressSeconds=MaxNoProgressWhileSending;
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
		Json += FString::Printf(TEXT("    \"pixelFormat\": \"%s\",\n"), GetPixelFormatString(SourceFormat));
		Json += FString::Printf(TEXT("    \"consumersPerStream\": %d,\n"), ConsumerCount);
		Json += FString::Printf(TEXT("    \"receiverCalledWhileMeasuring\": %s,\n"), (bUsesReceiver && !bUnconsumed) ? TEXT("true") : TEXT("false"));
		Json += FString::Printf(TEXT("    \"streamCount\": %d,\n"), StreamCount);
		Json += FString::Printf(TEXT("    \"usesSender\": %s,\n"), bUsesSender ? TEXT("true") : TEXT("false"));
		Json += FString::Printf(TEXT("    \"usesReceiver\": %s,\n"), bUsesReceiver ? TEXT("true") : TEXT("false"));
		Json += FString::Printf(TEXT("    \"receiverMode\": \"%s\",\n"),
			bGpuReceiverPath ? TEXT("gpu-direct") : TEXT("cpu-fallback"));
		// Which receive mode each stream actually ran when the measurement ended (Async Receive is the default; the
		// CPU fallback setting makes it step aside), so results of different modes are never mixed.
		Json += TEXT("    \"receiveModes\": [");
		if (bUsesReceiver)
		{
			const UEnum* ModeEnum = StaticEnum<ESpoutReceiveMode>();
			for (int32 Index = 0; Index < SenderNames.Num(); ++Index)
			{
				const FSpoutStreamStatus S = USpoutBPFunctionLibrary::GetSpoutStreamStatus(SenderNames[Index], ESpoutType::Receiver);
				Json += FString::Printf(TEXT("%s{\"requested\": \"%s\", \"effective\": \"%s\", \"note\": \"%s\"}"), Index ? TEXT(", ") : TEXT(""),
					*ModeEnum->GetNameStringByValue(static_cast<int64>(S.RequestedReceiveMode)), *ModeEnum->GetNameStringByValue(static_cast<int64>(S.EffectiveReceiveMode)),
					*EscapeJson(S.ReceiveModeNote));
			}
		}
		Json += TEXT("],\n");
		Json += TEXT("    \"cvars\": {\n");
		Json += FString::Printf(TEXT("      \"r.Spout.GPUReceiver\": %d,\n"), bGpuReceiverPath ? 1 : 0);
		Json += FString::Printf(TEXT("      \"r.Spout.MaxPendingPerStream\": %d,\n"), IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.MaxPendingPerStream"))->GetInt());
		Json += FString::Printf(TEXT("      \"r.Spout.MaxRetainedMiB\": %d\n"), IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.MaxRetainedMiB"))->GetInt());
		Json += TEXT("    },\n");
		Json += FString::Printf(TEXT("    \"warmupSeconds\": %.1f,\n"), WarmupSeconds);
		Json += FString::Printf(TEXT("    \"requestedMeasureSeconds\": %.1f\n"), MeasureSeconds);
		Json += TEXT("  },\n");

		if (bViewportSend)
		{
			Json += TEXT("  \"viewport\": {\n");
			Json += FString::Printf(TEXT("    \"sources\": \"%s\",\n"), bViewportRelease ? TEXT("GameViewport (whole window) and GameViewportOnly") : TEXT("GameViewport (whole window)"));
			Json += FString::Printf(TEXT("    \"gpuLoadCopiesPerFrame\": %d, \"gpuLoadTexture\": \"3840x2160 PF_FloatRGBA\",\n"), GpuLoadCopies);
			Json += FString::Printf(TEXT("    \"streamWidth\": %u, \"streamHeight\": %u, \"streamDxgiFormat\": %u, \"frameBytes\": %lld,\n"),
				ViewportStreamWidth, ViewportStreamHeight, ViewportStreamDxgiFormat, static_cast<int64>(ViewportStreamWidth) * ViewportStreamHeight * 4);
			Json += FString::Printf(TEXT("    \"releaseSettleSeconds\": %.1f, \"sendersClosed\": %s,\n"), ReleaseSettleSeconds, bViewportSendersClosed ? TEXT("true") : TEXT("false"));
			Json += FString::Printf(TEXT("    \"localMemoryBeforeBytes\": %lld, \"localMemoryWhileSendingBytes\": %lld, \"localMemoryAfterBytes\": %lld,\n"),
				Median(LocalMemoryBefore), Median(LocalMemoryWhileSending), Median(LocalMemoryAfter));
			Json += FString::Printf(TEXT("    \"workingSetBeforeBytes\": %lld, \"workingSetWhileSendingBytes\": %lld, \"workingSetAfterBytes\": %lld,\n"),
				Median(WorkingSetBefore), Median(WorkingSetWhileSending), Median(WorkingSetAfter));
			Json += FString::Printf(TEXT("    \"memorySamples\": [%d, %d, %d]\n"), LocalMemoryBefore.Num(), LocalMemoryWhileSending.Num(), LocalMemoryAfter.Num());
			Json += TEXT("  },\n");
		}
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
		Json += FString::Printf(TEXT("    \"consumerAcceptedReceives\": %d, \"consumerRejectedReceives\": %d,\n"), ConsumerAcceptedReceives, ConsumerRejectedReceives);
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
