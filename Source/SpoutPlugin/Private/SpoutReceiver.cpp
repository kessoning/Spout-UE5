// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the Spout receiver flow.
 *
 * Two paths are supported, controlled by CVar r.Spout.GPUReceiver:
 *   - GPU path (default, CVar=1): copies through native shared slots, or through
 *     D3D11-on-12 wrapping when the legacy wrap path is selected. No CPU readback.
 *   - CPU path (CVar=0): copies into a D3D11 STAGING texture, maps it and uploads
 *     through the RHI. It uses the native device in shared-slot mode and the
 *     D3D11-on-12 device on the legacy wrap path.
 * A receiver session may instead ask for Async Receive (Beta) (SpoutAsyncReceive.h): requested per session
 * through Receive's bAsyncReceive, fixed by the session's first accepted call, never by a process-wide setting.
 */
#include "SpoutReceiver.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "SpoutLimits.h"
#include "SpoutInstrument.h"
#include "SpoutFrameWait.h"
#include "SpoutAsyncReceive.h"
#include "SpoutDebugCapture.h"
#include "Materials/MaterialInterface.h"
#include "SpoutD3DContext.h"
#include "SpoutD3DUtils.h"
#include "SpoutFormatPolicy.h"
#include "SpoutFrameSync.h"
#include "SpoutLogThrottle.h"
#include "SpoutReceiverOwnership.h"
#include "SpoutRHIState.h"
#include "SpoutSenderRegistry.h"
#include "SpoutSession.h"
#include "SpoutTextureUtils.h"
#include "SpoutModule.h"
#include "SpoutValidation.h"

#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "HAL/IConsoleManager.h"
#include "PixelFormat.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "Engine/Texture.h"
#include "TextureResource.h"
#include "RenderResource.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTarget.h"
// UObject/Package.h is required for the full UPackage type so that
// GetTransientPackage() (UPackage*) implicitly converts to UObject* in
// non-Editor / non-unity builds where SharedPCH.UnrealEd doesn't apply.
#include "UObject/Package.h"
#include "Misc/ScopeExit.h"

using Microsoft::WRL::ComPtr;

namespace
{
#if WITH_DEV_AUTOMATION_TESTS
	TAtomic<bool> GFailNextCpuReadback{false};
#endif
	/**
	 * Render thread only. When the current receive command began and when its source update ended: written by the
	 * command, recorded in its measurement row (tWrap0/tWrap1), which the synchronous shared-slot path and Async
	 * Receive leave unused otherwise. 0 outside a receive command.
	 */
	uint64 GReceiveCommandStart = 0;
	uint64 GReceiveCommandUpdated = 0;

	/**
	 * Render thread only. The synchronous shared-slot receives of this render frame whose access was refused, waiting for
	 * their end-of-frame attempt (development switch r.Spout.Dev.Recv.SyncRetry; RunEndOfFrameRetries). Each keeps its
	 * destination until that attempt, which drains the list every frame; the teardown discards it first.
	 */
	struct FEndOfFrameRetry
	{
		TWeakPtr<FSpoutSharedSender> Receiver;
		TRefCountPtr<FRHITexture> Destination;
		FSpoutStreamToken Token;
	};
	TArray<FEndOfFrameRetry> GEndOfFrameRetries;

	/** The receive attempt: 0 the call's own (first) attempt, 1 its end-of-frame attempt. */
	enum class ESyncAttempt : uint8 { First = 0, EndOfFrame = 1 };

	void ArmEndOfFrameRetry(const TSharedPtr<FSpoutSharedSender>& Receiver, const TRefCountPtr<FRHITexture>& Destination, const FSpoutStreamToken& Token)
	{
		for (const FEndOfFrameRetry& E : GEndOfFrameRetries)
		{
			if (E.Receiver.HasSameObject(Receiver.Get())) return; // once per receiver and frame
		}
		GEndOfFrameRetries.Add({ Receiver, Destination, Token });
	}
	void CopyReceivedFrame(FRHICommandListImmediate& Cmd, FRHITexture* Source, FRHITexture* Destination, bool bAfterCpuUpload = false)
	{
		if (Source == Destination) return;
		// UE merges adjacent transitions of the same resource. An upload flushes prior
		// barriers, then leaves the source's CopyDest -> SRV restoration pending. Consume
		// that first; otherwise keep a fresh destination's RTV -> SRV restoration adjacent.
		if (bAfterCpuUpload)
		{
			SPOUT_D3D_DIAG_TRANSITION("receive.restore.uploaded-source.to-copysrc", Source, ERHIAccess::Unknown, ERHIAccess::CopySrc);
			Cmd.Transition(FRHITransitionInfo(Source, ERHIAccess::Unknown, ERHIAccess::CopySrc));
		}
		SPOUT_D3D_DIAG_TRANSITION("receive.restore.destination.to-copydest", Destination, ERHIAccess::Unknown, ERHIAccess::CopyDest);
		Cmd.Transition(FRHITransitionInfo(Destination, ERHIAccess::Unknown, ERHIAccess::CopyDest));
		if (!bAfterCpuUpload)
		{
			SPOUT_D3D_DIAG_TRANSITION("receive.restore.source.to-copysrc", Source, ERHIAccess::Unknown, ERHIAccess::CopySrc);
			Cmd.Transition(FRHITransitionInfo(Source, ERHIAccess::Unknown, ERHIAccess::CopySrc));
		}
		Cmd.CopyTexture(Source, Destination, FRHICopyTextureInfo());
		SPOUT_D3D_DIAG_TRANSITION("receive.restore.source.to-srv", Source, ERHIAccess::CopySrc, ERHIAccess::SRVMask);
		Cmd.Transition(FRHITransitionInfo(Source, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		SPOUT_D3D_DIAG_TRANSITION("receive.restore.destination.to-srv", Destination, ERHIAccess::CopyDest, ERHIAccess::SRVMask);
		Cmd.Transition(FRHITransitionInfo(Destination, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
	}

	ID3D11Device* ReceiverDevice(FSpoutD3DContext& Context)
	{
		// The CPU fallback of the shared-slot path shares the native device too. Creating a staging
		// texture through D3D11-on-12 otherwise creates a D3D12 readback buffer and emits
		// warning 1328. The legacy wrap path keeps its original device.
		return SpoutInterop::IsSharedSlot() ? FSpoutAsyncDevice::Get().GetDevice() : Context.GetD3D11Device();
	}
	static TAutoConsoleVariable<int32> CVarSpoutGPUReceiver(
		TEXT("r.Spout.GPUReceiver"),
		1,
		TEXT("Spout receiver path: 1 = GPU copy (default), 0 = CPU readback. Uses the latched interop mode."),
		ECVF_Default);

	bool QuerySenderInfo(const ANSICHAR* Utf8Name, unsigned int& OutWidth, unsigned int& OutHeight, HANDLE& OutHandle, unsigned long& OutFormat)
	{
		// Initialize every output before the call so a failed lookup cannot leave the caller
		// reading whatever the SDK did or did not write.
		OutWidth = 0;
		OutHeight = 0;
		OutHandle = nullptr;
		OutFormat = 0;

		if (Utf8Name == nullptr || Utf8Name[0] == '\0')
		{
			return false;
		}

		FSpoutD3DContext& Context = FSpoutD3DContext::Get();
		spoutSenderNames* SenderNames = Context.GetSenderNames();
		if (!SenderNames)
		{
			return false;
		}

		// Spout sender list is shared process-wide; guard with the mutex.
		FScopeLock SpoutLock(&Context.GetSpoutMutex());
		return SenderNames->GetSenderInfo(Utf8Name, OutWidth, OutHeight, OutHandle, OutFormat);
	}

	bool UpdateSharedReceiverTexture(FSpoutSharedSender& Receiver, unsigned int Width, unsigned int Height, HANDLE SharedHandle, DXGI_FORMAT Format, FSpoutD3DContext& Context)
	{
		check(IsInRenderingThread());
		FScopeLock ReceiverLock(&Receiver.ResourceMutex);
		if (!Receiver.CanRetry(Width, Height, Format, SharedHandle)) return false;
		bool bFrameSyncOpen = Receiver.FrameSync.IsOpen();
		if (!bFrameSyncOpen)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Spout_ReceiverFrameSyncOpen);
			bFrameSyncOpen = Receiver.FrameSync.Open(Receiver.GetUtf8Name());
		}
		if (!bFrameSyncOpen)
		{
			Receiver.NativeFailure(E_FAIL);
			return false;
		}
		if (Receiver.Width == Width && Receiver.Height == Height && Receiver.SharedHandle == SharedHandle
			&& Receiver.SharedFormat == Format && Receiver.SharedTexture)
		{
			return true;
		}

		int64 FrameBytes = 0;
		if (!SpoutLimits::FrameBytes(Width, Height, SpoutFormatPolicy::ResolveReceiverFormat(Format).PixelFormat, FrameBytes)) return false;
		// Shared source, staging/readback upload and completed-image overlap.
		const auto Reservation = FSpoutResourceReservation::TryReserve(FrameBytes * 4);
		if (!Reservation) return false;

		// Open the shared DX11 texture from the external sender's handle. OpenSharedResource is a
		// device-level (free-threaded) call; holding ResourceMutex across it is acceptable because
		// this only happens on connect/resize/handle-change, not per frame.
		ComPtr<ID3D11Texture2D> NewSharedTexture;
		HRESULT hr = E_FAIL;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Spout_OpenSharedResource);
			ID3D11Device* Device = ReceiverDevice(Context);
			if (!Device) return false;
			hr = Device->OpenSharedResource(
				SharedHandle,
				IID_PPV_ARGS(NewSharedTexture.GetAddressOf())
			);
		}

		if (FAILED(hr) || !NewSharedTexture)
		{
			Receiver.NativeFailure(FAILED(hr) ? hr : E_FAIL);
			// A peer may resize or exit between metadata lookup and opening the handle.
			if (Receiver.OpenLogThrottle.ShouldLog())
			{
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout receiver '%s': shared texture unavailable (hr=0x%08x); receive will retry."), *Receiver.Name.ToString(), hr);
			}
			return false;
		}

		D3D11_TEXTURE2D_DESC NativeDesc = {};
		NewSharedTexture->GetDesc(&NativeDesc);
		const auto NativeFormat = SpoutFormatPolicy::ResolveReceiverFormat(NativeDesc.Format);
		const auto AdvertisedFormat = SpoutFormatPolicy::ResolveReceiverFormat(Format);
		if (NativeDesc.Width != Width || NativeDesc.Height != Height || NativeDesc.MipLevels != 1
			|| NativeDesc.ArraySize != 1 || NativeDesc.SampleDesc.Count != 1 || NativeDesc.SampleDesc.Quality != 0
			|| !NativeFormat.IsUsable() || NativeFormat.PixelFormat != AdvertisedFormat.PixelFormat)
		{
			Receiver.NativeFailure(E_INVALIDARG);
			if (Receiver.FormatLogThrottle.ShouldLog())
			{
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout receiver '%s': opened texture does not match the advertised size/format or single-mip/sample layout; frame rejected."), *Receiver.Name.ToString());
			}
			return false;
		}
		Receiver.NativeSuccess();
		Receiver.SharedFormat = Format;

		Receiver.Width = Width;
		Receiver.Height = Height;
		Receiver.SharedHandle = SharedHandle;
		// A new sender identity is a new generation: the asynchronous receive of the old one ends
		// here, its worker stopped, before anything of the new one is installed.
		Receiver.Async.Reset();
		// Likewise the shared-slot receive of the old identity: retired with its outstanding GPU work, not dropped.
		Receiver.SlotReceive.Reset();
		// SharedTexture is owned by this receiver; the sender owns the underlying resource.
		Receiver.SharedTexture = MoveTemp(NewSharedTexture);
		// Invalidate caches tied to previous geometry/handle.
		Receiver.StagingTexture.Reset();
		Receiver.CachedWrappedResource.Reset();
		Receiver.CachedNativeResource = nullptr;
		Receiver.CachedRTWrappedResource.Reset();
		Receiver.CachedRTNativeResource = nullptr;
		Receiver.ReceivedFrame.SafeRelease();
		Receiver.DestinationDeliveries.Empty();
		Receiver.NativeReservation = Reservation;
		Receiver.FrameSync.RequestCurrentFrame();

		return true;
	}

	// GPU path: return just the shared texture + dimensions. No staging involvement.
	bool GetSharedForGPUPath(FSpoutSharedSender& Receiver, ComPtr<ID3D11Texture2D>& OutShared, uint32& OutWidth, uint32& OutHeight)
	{
		FScopeLock ReceiverLock(&Receiver.ResourceMutex);
		if (!Receiver.SharedTexture)
		{
			return false;
		}
		OutWidth = Receiver.Width;
		OutHeight = Receiver.Height;
		OutShared = Receiver.SharedTexture;
		return true;
	}

	// CPU path: return shared + staging (creating/resizing the staging texture on demand).
	bool GetSharedAndStaging(FSpoutSharedSender& Receiver, FSpoutD3DContext& Context, ComPtr<ID3D11Texture2D>& OutShared, ComPtr<ID3D11Texture2D>& OutStaging, uint32& OutWidth, uint32& OutHeight)
	{
		FScopeLock ReceiverLock(&Receiver.ResourceMutex);
		if (!Receiver.SharedTexture)
		{
			return false;
		}

		OutWidth = Receiver.Width;
		OutHeight = Receiver.Height;
		OutShared = Receiver.SharedTexture;

		// Staging texture is CPU-readable for readback into a UE UTexture2D.
		bool bNeedsResize = false;
		if (!Receiver.StagingTexture)
		{
			bNeedsResize = true;
		}
		else
		{
			D3D11_TEXTURE2D_DESC Desc;
			Receiver.StagingTexture->GetDesc(&Desc);
			if (Desc.Width != Receiver.Width || Desc.Height != Receiver.Height)
			{
				bNeedsResize = true;
			}
		}

		if (bNeedsResize)
		{
			D3D11_TEXTURE2D_DESC Desc;
			Receiver.SharedTexture->GetDesc(&Desc);

			Desc.Usage = D3D11_USAGE_STAGING;
			Desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			Desc.BindFlags = 0;
			Desc.MiscFlags = 0;
			Desc.SampleDesc.Count = 1;
			Desc.SampleDesc.Quality = 0;

			// Staging texture is owned by the receiver and recreated on size changes.
			ComPtr<ID3D11Texture2D> NewStaging;
			ID3D11Device* Device = ReceiverDevice(Context);
			if (!Device) return false;
			HRESULT hr = Device->CreateTexture2D(&Desc, nullptr, NewStaging.GetAddressOf());
			if (FAILED(hr))
			{
				Receiver.NativeFailure(hr);
				if (Receiver.OpenLogThrottle.ShouldLog()) UE_LOG(LogSpoutPlugin, Warning, TEXT("Failed to create staging texture (hr=0x%08x)"), hr);
				return false;
			}

			Receiver.StagingTexture = MoveTemp(NewStaging);
		}

		OutStaging = Receiver.StagingTexture;
		return OutStaging != nullptr;
	}

	// Cache a D3D11-on-12 wrap of the primary UE RHI destination (UTexture backing).
	// Keyed on native D3D12 pointer so UE texture recreation invalidates the wrap.
	ComPtr<ID3D11Texture2D> GetOrCreateReceiverWrap(FSpoutSharedSender& Receiver, ID3D12Resource* NativeResource, SpoutRHIState::EWrapState WrapState)
	{
		FScopeLock ReceiverLock(&Receiver.ResourceMutex);
		if (Receiver.CachedNativeResource != NativeResource || !Receiver.CachedWrappedResource)
		{
			Receiver.CachedWrappedResource = WrapD3D12Resource(NativeResource, WrapState);
			Receiver.CachedNativeResource = NativeResource;
		}
		return Receiver.CachedWrappedResource;
	}

	// Cache a D3D11-on-12 wrap of the optional secondary UE RHI destination (render target).
	ComPtr<ID3D11Texture2D> GetOrCreateReceiverRTWrap(FSpoutSharedSender& Receiver, ID3D12Resource* NativeResource, SpoutRHIState::EWrapState WrapState)
	{
		FScopeLock ReceiverLock(&Receiver.ResourceMutex);
		if (Receiver.CachedRTNativeResource != NativeResource || !Receiver.CachedRTWrappedResource)
		{
			Receiver.CachedRTWrappedResource = WrapD3D12Resource(NativeResource, WrapState);
			Receiver.CachedRTNativeResource = NativeResource;
		}
		return Receiver.CachedRTWrappedResource;
	}

	// Format classification for received frames comes from SpoutFormatPolicy, which is shared
	// with the sender path. A local switch that falls back to PF_B8G8R8A8 must be avoided here:
	// an external sender advertising a compressed, planar, depth or 32-bit float layout would
	// have its bytes copied into a BGRA8 destination and reinterpreted.

	/**
	 * The experimental new-frame wait (r.Spout.FrameWaitMs, off by default) for one receive call
	 * that found no new frame. Runs before any plugin lock or texture access is taken, so nothing the
	 * sender needs is held while it waits. Limited by r.Spout.FrameWaitMs per call and by
	 * r.Spout.FrameWaitBudgetMs per render frame across all streams; ended early when the stream is
	 * cancelled or the engine is exiting. Returns true when a frame is pending afterwards. With the
	 * setting at 0 it returns false at once.
	 */
	bool TryWaitForPublication(FSpoutSharedSender& Receiver, const FSpoutStreamToken& Token, FSpoutFrameWaitReport& OutReport, double& OutBudgetBeforeSeconds, double& OutBudgetAfterSeconds)
	{
		return SpoutFrameWait::RunBudgetedWait(SpoutLimits::FrameWaitSeconds(), GFrameCounterRenderThread,
			[&Receiver, &Token](double GrantedSeconds, FSpoutFrameWaitReport& Report)
			{
				return Receiver.FrameSync.WaitForPublication(GrantedSeconds, SpoutLimits::FrameWaitPollSeconds(),
					[&Token]() { return IsEngineExitRequested() || !FSpoutSession::Get().IsCurrent(Token); }, Report);
			},
			OutReport, OutBudgetBeforeSeconds, OutBudgetAfterSeconds);
	}

#if SPOUT_WITH_INSTRUMENT
	/** Measurement builds: starts one receive row. */
	void BeginReceiveRecord(SpoutInstrument::FRecord& Rec, FSpoutSharedSender& Receiver, TCHAR Kind)
	{
		if (Receiver.InstrumentId < 0) Receiver.InstrumentId = SpoutInstrument::RegisterStream(Receiver.Name, TEXT("R"));
		Rec.Kind = Kind;
		Rec.Stream = Receiver.InstrumentId;
		Rec.FrameRT = GFrameCounterRenderThread;
		Rec.Pending = Receiver.FrameSync.HasPendingFrame() ? 1 : 0;
		Rec.ReceivedBefore = Receiver.ReceivedFrameNumber;
		Rec.TDetect = SpoutInstrument::Now();
		Rec.TWrap0 = GReceiveCommandStart;
		Rec.TWrap1 = GReceiveCommandUpdated;
	}

	/** Measurement builds: what the check stage did. */
	void RecordReceiveCheck(SpoutInstrument::FRecord& Rec, const FSpoutReceiveCheck& Check)
	{
		if (Check.bTransferWithoutNotification) Rec.Flags |= 32;
		if (!Check.bWaitAsked || !(Rec.Flags & 1)) return;
		const FSpoutFrameWaitReport& Report = Check.WaitReport;
		Rec.WaitDecision = static_cast<int32>(Report.Decision);
		Rec.WaitOutcome = static_cast<int32>(Report.Outcome);
		Rec.TWait0 = Report.WaitBeginCycles;
		Rec.TWait1 = Report.WaitEndCycles;
		Rec.WaitPolls = Report.Polls;
		Rec.WaitBoundUs = Report.BoundSeconds * 1e6;
		if (Report.PredictedLatestSeconds > 0.0)
		{
			const double DetectSeconds = double(Rec.TDetect) * FPlatformTime::GetSecondsPerCycle64();
			Rec.PredLoUs = (Report.PredictedEarliestSeconds - DetectSeconds) * 1e6;
			Rec.PredHiUs = (Report.PredictedLatestSeconds - DetectSeconds) * 1e6;
		}
		Rec.PeriodUs = Report.PeriodSeconds * 1e6;
		Rec.CountAfterWait = Report.CountAfter;
		Rec.BudgetBeforeUs = Check.BudgetBeforeSeconds * 1e6;
		Rec.BudgetAfterUs = Check.BudgetAfterSeconds * 1e6;
		if (Check.bPendingAfterWait) Rec.Flags |= 8;
		// A probe is flagged only when it ended Published or TimedOut, as the predictor counts it.
		if (Report.bProbe && (Report.Outcome == SpoutFrameWait::EOutcome::Published || Report.Outcome == SpoutFrameWait::EOutcome::TimedOut)) Rec.Flags |= 16;
		Rec.PredSuspensions = Report.PredictorSuspensions;
		Rec.PredWastedWaits = Report.PredictorWastedWaits;
		Rec.PredJitterUs = Report.PredictorJitterSeconds >= 0.0 ? Report.PredictorJitterSeconds * 1e6 : -1.0;
	}

	/** Measurement builds: the read under access, taken from the frame sync's own record of it. */
	void RecordUnderAccess(SpoutInstrument::FRecord& Rec, const FSpoutSharedSender& Receiver)
	{
		const FSpoutFrameSync::FNotificationStats Stats = Receiver.FrameSync.GetNotificationStats();
		Rec.CountUnderAccess = Stats.UnderAccessCount;
		Rec.NewUnderAccess = Stats.bNewUnderAccess ? 1 : 0;
	}

	/** Measurement builds: what the call left for the downstream deliveries, then the row is queued. */
	void FinishReceiveRecord(SpoutInstrument::FRecord& Rec, const FSpoutSharedSender& Receiver)
	{
		const FSpoutFrameSync::FNotificationStats Stats = Receiver.FrameSync.GetNotificationStats();
		Rec.ReceivedAfter = Receiver.ReceivedFrameNumber;
		Rec.FrameRequests = Stats.CurrentFrameRequests;
		Rec.SyncOpens = Stats.Opens;
		SpoutInstrument::Push(Rec);
	}
#endif

	/**
	 * Async Receive (Beta), off by default. bAsyncRequested is the session's own request, decided by the first
	 * accepted receive of this entry and carried by every render command of it; no process-wide setting is read.
	 * Returns false when this call is to use the synchronous path: the session did not ask for it, a precondition
	 * failed, or the stream's worker failed. For a session that did not ask nothing here is created.
	 *
	 * On this path the render thread takes no Spout access and waits for no GPU work. It takes the
	 * newest image the worker completed, when there is one, and queues its copy into the same
	 * Unreal texture the synchronous path fills. That copy is the delivery.
	 *
	 * What the call ends up using is recorded on the entry (bAsyncReceiveActive, and the reason when a session
	 * that asked for it runs synchronously), so the status never reports a requested mode as the effective one.
	 */
	bool ReceiveOnRenderThread_Async(FSpoutSharedSender& Receiver, bool bAsyncRequested, FRHITexture* Destination, EPixelFormat PixelFormat, FRHICommandListImmediate& RHICmdList)
	{
		check(IsInRenderingThread());
		if (!bAsyncRequested)
		{
			if (Receiver.Async)
			{
				// Not reachable while the mode is fixed per entry; kept so that an entry that is not asking can
				// never keep a worker. Back to the synchronous path, from the current image.
				Receiver.Async.Reset();
				Receiver.FrameSync.RequestCurrentFrame();
			}
			Receiver.bAsyncReceiveActive.store(false);
			return false;
		}
		FSpoutAsyncStream::FIdentity Identity;
		{
			FScopeLock ReceiverLock(&Receiver.ResourceMutex);
			Identity.SharedHandle = Receiver.SharedHandle;
			Identity.Width = Receiver.Width;
			Identity.Height = Receiver.Height;
			Identity.Format = Receiver.SharedFormat;
		}
		if (Receiver.Async && (Receiver.Async->HasFailed() || Receiver.Async->GetIdentity() != Identity))
		{
			if (Receiver.Async->HasFailed())
			{
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout receiver '%s': Async Receive (Beta) stopped (%s); the stream uses the synchronous path."),
					*Receiver.Name.ToString(), *Receiver.Async->GetFailure());
				Receiver.SetAsyncReceiveNote(FString::Printf(TEXT("Async Receive (Beta) stopped: %s"), *Receiver.Async->GetFailure()));
				Receiver.AsyncRefusedFor = Identity;
			}
			Receiver.Async.Reset();
			Receiver.bAsyncReceiveActive.store(false);
			Receiver.FrameSync.RequestCurrentFrame();
		}
		if (!Receiver.Async)
		{
			if (Receiver.AsyncRefusedFor == Identity)
			{
				return false; // refused once for this identity: no retry on every frame, the reason is already recorded
			}
			// Measurement builds: the id the worker's rows carry. -1 otherwise, and then unused.
			int32 MeasurementStream = -1;
			SPOUT_INSTRUMENT(
				if (Receiver.InstrumentId < 0) Receiver.InstrumentId = SpoutInstrument::RegisterStream(Receiver.Name, TEXT("R"));
				MeasurementStream = Receiver.InstrumentId;
			)
			FString Why;
			Receiver.Async = FSpoutAsyncStream::Create(Receiver.Name, MeasurementStream, Receiver.FrameSync, Identity, PixelFormat, Why);
			if (!Receiver.Async)
			{
				Receiver.AsyncRefusedFor = Identity;
				Receiver.bAsyncReceiveActive.store(false);
				Receiver.SetAsyncReceiveNote(FString::Printf(TEXT("Async Receive (Beta) is not available for this sender: %s"), *Why));
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout receiver '%s': Async Receive (Beta) was requested but is not used (%s); the stream uses the synchronous path."),
					*Receiver.Name.ToString(), *Why);
				return false;
			}
			Receiver.SetAsyncReceiveNote(FString());
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout receiver '%s': Async Receive (Beta) started (worker generation %llu)."),
				*Receiver.Name.ToString(), Receiver.Async->GetGeneration());
		}
		Receiver.bAsyncReceiveActive.store(true);
		if (Receiver.Async->ConsumeOnRenderThread(RHICmdList, Destination, Receiver.ReceivedFrameNumber, GReceiveCommandStart, GReceiveCommandUpdated))
		{
			++Receiver.ReceivedFrameNumber;
			Receiver.AsyncDeliveredFrames.fetch_add(1); // the status's count only
		}
		return true;
	}

	/**
	 * The synchronous GPU receive on the shared-slot path (SpoutSharedSlot.h), used when the process latched
	 * it. Nothing of Unreal's is wrapped for D3D11 and the D3D11-on-12 device is not used.
	 *
	 * The same contract as the wrap path below: one check stage before any lock or access; the copy under the peer's
	 * access; the notification read again under access when the recheck is on; the frame completed only once the
	 * copy finished, while access is still held; the received-frame number moved only on a delivered completion.
	 * What differs is where the copy goes. The native device copies the peer's texture into a plugin-owned slot and
	 * the render thread waits, bounded, for that copy only. Then Unreal's queue is given the copy slot -> destination,
	 * ordered behind the slot's written fence, and signals the read fence after it. A slot is written again only
	 * once its read finished. A copy that timed out is never published; its frame stays pending.
	 *
	 * Only the primary destination is filled here: the caller passes no render target, and fills output render
	 * targets from the delivered frame with RHI copies.
	 */
	void ReceiveOnRenderThread_SharedSlot(
		TSharedPtr<FSpoutSharedSender> Receiver,
		TRefCountPtr<FRHITexture> DestTextureRHI,
		FRHICommandListImmediate& RHICmdList,
		const FSpoutStreamToken& Token,
		ESyncAttempt Attempt = ESyncAttempt::First,
		int32 AccessTimeoutMs = -1)
	{
		if (!DestTextureRHI.IsValid())
		{
			return;
		}
		FSpoutGpuRetirement::Get().Poll();

		FSpoutSlotReceive::FIdentity Identity;
		{
			FScopeLock ReceiverLock(&Receiver->ResourceMutex);
			if (!Receiver->SharedTexture)
			{
				return;
			}
			Identity.SharedHandle = Receiver->SharedHandle;
			Identity.Width = Receiver->Width;
			Identity.Height = Receiver->Height;
			Identity.Format = Receiver->SharedFormat;
		}
		if (Receiver->SlotReceive && Receiver->SlotReceive->GetIdentity() != Identity)
		{
			// Retired with its outstanding work, never dropped.
			Receiver->SlotReceive.Reset();
		}
		if (!Receiver->SlotReceive)
		{
			if (Receiver->SlotReceiveRefusedFor == Identity)
			{
				return;
			}
			FString Why;
			Receiver->SlotReceive = FSpoutSlotReceive::Create(Receiver->Name, Identity, DestTextureRHI->GetFormat(), Why);
			if (!Receiver->SlotReceive)
			{
				Receiver->SlotReceiveRefusedFor = Identity;
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout receiver '%s': the shared-slot receive could not start for this sender identity (%s). Nothing is received until the sender's size, format or handle changes."),
					*Receiver->Name.ToString(), *Why);
				return;
			}
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout receiver '%s': receiving through the shared-slot path (%ux%u, generation %llu)."),
				*Receiver->Name.ToString(), Identity.Width, Identity.Height, Receiver->SlotReceive->GetGeneration());
		}
		FSpoutSlotReceive& Slots = *Receiver->SlotReceive;
		if (Slots.HasFailed())
		{
			// Stopped for this identity (logged once when it stopped). Its objects are retired when the identity changes.
			return;
		}

		// Development switches (at their defaults, a single attempt): the end-of-frame attempt after a refused access (1),
		// and while the first attempt keeps failing, only that attempt (2); a first attempt that does not wait
		// (SyncFirstTryOnly).
		const int32 RetryMode = SpoutLimits::DevRecvSyncRetry();
		const bool bFirst = Attempt == ESyncAttempt::First;
		if (bFirst && AccessTimeoutMs < 0 && SpoutLimits::DevRecvSyncFirstTryOnly())
		{
			AccessTimeoutMs = 0;
		}
		bool bSkipFirst = false;
		if (bFirst && RetryMode == 2)
		{
			// Two refused first attempts in a row (a granted one clears the history): the first attempt is skipped, and made again every 30th frame as a probe.
			if (FPlatformMath::CountBits(Receiver->SyncFirstRefusedHistory & 0xFFu) >= 2 && Receiver->SyncFramesSinceProbe < 30)
			{
				bSkipFirst = true;
				++Receiver->SyncFramesSinceProbe;
			}
			else
			{
				Receiver->SyncFramesSinceProbe = 0;
			}
		}
		const bool bRecheckNotification = SpoutLimits::FrameCountRecheck();
		SPOUT_INSTRUMENT(
			const bool bInstrument = SpoutInstrument::IsEnabled();
			SpoutInstrument::FRecord Rec;
			if (bInstrument)
			{
				BeginReceiveRecord(Rec, *Receiver, TEXT('R'));
				Rec.CompletionPath = 1;
				Rec.CompletionStage = 1;
				Rec.Generation = static_cast<int64>(Slots.GetGeneration());
				Slots.ObserveDestination(Rec.Stream);
				// bit 6: an end-of-frame attempt; bit 7: a first attempt skipped for the end-of-frame one (development switches).
				Rec.Flags |= (bFirst ? 0 : 64) | (bSkipFirst ? 128 : 0);
			}
		)
		if (bSkipFirst)
		{
			ArmEndOfFrameRetry(Receiver, DestTextureRHI, Token);
			SPOUT_INSTRUMENT(if (bInstrument) { Rec.TRestore = SpoutInstrument::Now(); FinishReceiveRecord(Rec, *Receiver); })
			return;
		}
		FSpoutReceiveCheck Check;
		const bool bTransfer = SpoutFrameWait::CheckForNewFrame(Receiver->FrameSync,
			[&](bool bNewFrameAtRead)
			{
				SPOUT_INSTRUMENT(
					if (bInstrument)
					{
						Rec.NewFrame = bNewFrameAtRead ? 1 : 0;
						Rec.SenderFrame = Receiver->FrameSync.GetLastObservedSenderFrame();
						Rec.Flags |= (SpoutLimits::FrameWaitSeconds() > 0.0 ? 1 : 0) | (bRecheckNotification ? 2 : 0);
					}
				)
			},
			[&](FSpoutFrameWaitReport& Report, double& BudgetBeforeSeconds, double& BudgetAfterSeconds)
			{
				return TryWaitForPublication(*Receiver, Token, Report, BudgetBeforeSeconds, BudgetAfterSeconds);
			},
			Check);
		SPOUT_INSTRUMENT(if (bInstrument) RecordReceiveCheck(Rec, Check);)
		if (!bTransfer)
		{
			SPOUT_INSTRUMENT(if (bInstrument) { Rec.TRestore = SpoutInstrument::Now(); FinishReceiveRecord(Rec, *Receiver); })
			return;
		}
		SPOUT_INSTRUMENT(ON_SCOPE_EXIT { if (bInstrument) FinishReceiveRecord(Rec, *Receiver); };)

		// No slot whose last read finished: the frame stays pending and is taken on a later call.
		const int32 Slot = Slots.BeginWrite();
		if (Slot < 0)
		{
			SPOUT_INSTRUMENT(if (bInstrument) Rec.TRestore = SpoutInstrument::Now();)
			return;
		}

		const SpoutRHIState::FCopyBracket TextureBracket =
			SpoutRHIState::GetCopyBracket(SpoutRHIState::EResourceRole::ReceiverTexture);
		{
			// The peer's cross-process access, at most r.Spout.AccessWaitMs to take; a failed wait leaves the frame
			// pending. Once taken it is held until the copy is proven finished (or the native device is removed), even
			// when the copy is late and its frame discarded: the peer may not write while the read still runs.
			SPOUT_INSTRUMENT(if (bInstrument) Rec.TAccess0 = SpoutInstrument::Now();)
			FSpoutTextureAccessScope Access(Receiver->FrameSync, Slots.GetNativeSource(), AccessTimeoutMs);
			SPOUT_INSTRUMENT(if (bInstrument) { Rec.TAcquire = SpoutInstrument::Now(); Rec.Acquired = Access.IsAcquired() ? 1 : 0; })
			if (bFirst && RetryMode == 2)
			{
				Receiver->SyncFirstRefusedHistory = Access.IsAcquired() ? 0u : ((Receiver->SyncFirstRefusedHistory << 1) | 1u);
			}
			if (!Access.IsAcquired())
			{
				Slots.Abandon(Slot, SpoutAsync::EAbandon::AccessRefused);
				if (bFirst && RetryMode != 0)
				{
					// The frame stays pending: tried once more at the end of this render frame.
					ArmEndOfFrameRetry(Receiver, DestTextureRHI, Token);
				}
				if (Receiver->AccessLogThrottle.ShouldLog())
				{
					UE_LOG(LogSpoutPlugin, Warning,
						TEXT("Spout receiver '%s': shared-texture access was unavailable because a peer (the sender or another receiver) still held it after waiting up to %u ms (r.Spout.AccessWaitMs), ")
						TEXT("so this frame was not read yet. The frame stays pending and is read on a later frame."),
						*Receiver->Name.ToString(), SpoutLimits::AccessWaitMs());
				}
			}
			else
			{
				// Experimental: the notification read again while access is held (see the wrap path).
				if (bRecheckNotification)
				{
					Receiver->FrameSync.ObserveNotificationUnderAccess();
					SPOUT_INSTRUMENT(if (bInstrument) RecordUnderAccess(Rec, *Receiver);)
				}

				uint64 WrittenValue = 0;
				const ESpoutSlotCopy CopyResult = Slots.CopyAndWait(Slot, WrittenValue);
				SPOUT_INSTRUMENT(if (bInstrument) { Rec.TCopyRec = SpoutInstrument::Now(); Rec.TGpuDone = Rec.TCopyRec; Rec.Hr = static_cast<int32>(CopyResult); })
				if (CopyResult == ESpoutSlotCopy::Completed)
				{
					// The frame is complete in the slot and access is still held. Its copy into the destination is
					// queued now; only when that was queued does the frame stop being pending.
					if (Slots.PublishAndConsume(Slot, WrittenValue, RHICmdList, DestTextureRHI, TextureBracket.EntryBefore, TextureBracket.ExitAfter))
					{
						const bool bDelivered = Receiver->FrameSync.CompleteTransfer();
						SPOUT_INSTRUMENT(if (bInstrument) { Rec.Completed = 1; Rec.Delivered = bDelivered ? 1 : 0; Rec.ReadValue = static_cast<int64>(Slots.GetInstrumentReadValue()); Rec.WrittenValue = static_cast<int64>(WrittenValue); })
						if (bDelivered)
						{
							++Receiver->ReceivedFrameNumber;
							++Receiver->SlotTransfers;
						}
					}
				}
				else
				{
					// Late, failed or removed: never published, and the frame stays pending. CopyAndWait has already
					// waited until the copy was proven finished (or the device removed), so access is given back after it.
					Slots.Abandon(Slot, CopyResult == ESpoutSlotCopy::Late ? SpoutAsync::EAbandon::CompletionTimeout
						: (CopyResult == ESpoutSlotCopy::NotStarted ? SpoutAsync::EAbandon::Cancelled : SpoutAsync::EAbandon::CopyFailed));
				}
			}
		}
		// Access has been given back. A keyed texture's release reaches the sender when the native context is flushed.
		if (Slots.IsKeyedSource())
		{
			Slots.FlushKeyedRelease();
		}
		SPOUT_INSTRUMENT(if (bInstrument) { Rec.TRelease = SpoutInstrument::Now(); Rec.TRestore = Rec.TRelease; })
	}

	// GPU-direct path: wrap UE RHI destinations as D3D11 textures via D3D11-on-12 and
	// issue CopyResource straight from the external shared texture. Zero CPU readback.
	void ReceiveOnRenderThread_GPU(
		TSharedPtr<FSpoutSharedSender> Receiver,
		TRefCountPtr<FRHITexture> DestTextureRHI,
		TRefCountPtr<FRHITexture> DestRenderTargetRHI,
		FRHICommandListImmediate& RHICmdList,
		const FSpoutStreamToken& Token)
	{
		if (!DestTextureRHI.IsValid() && !DestRenderTargetRHI.IsValid())
		{
			return;
		}

		// Latched once per process (r.Spout.SharedSlotInterop): the shared-slot path wraps nothing of Unreal's.
		if (SpoutInterop::IsSharedSlot())
		{
			ReceiveOnRenderThread_SharedSlot(Receiver, DestTextureRHI, RHICmdList, Token);
			return;
		}

		FSpoutD3DContext& Context = FSpoutD3DContext::Get();

		ComPtr<ID3D11Texture2D> SharedTexture;
		uint32 LocalWidth = 0;
		uint32 LocalHeight = 0;
		if (!GetSharedForGPUPath(*Receiver, SharedTexture, LocalWidth, LocalHeight))
		{
			return;
		}

		// Ask before doing anything expensive. When the sender has published nothing since the
		// last frame this copied, the shared texture still holds that frame, so the transitions,
		// the RHI flush and the GPU copy are all skipped rather than repeated for no gain.
		//
		// The answer is latched inside FSpoutFrameSync rather than re-asked, because GetNewFrame()
		// decrements a semaphore: if the access wait below fails, the frame stays pending and the
		// next call copies it instead of losing it.
		//
		// Called outside the access mutex on purpose. The mutex protects the COPY, not the count:
		// a frame counted here and copied a moment later under the mutex is still a whole,
		// untorn frame -- at worst a newer one than the count referred to.
		//
		// The read is the first step of the check stage shared with the CPU path
		// (SpoutFrameWait::CheckForNewFrame). With the experimental settings at their defaults the
		// stage is this read and nothing else. With them on, a read that found nothing new may wait
		// for the sender's next publication, here, before the transitions, the D3D11 context lock
		// and the access scope, so that nothing the sender needs is held; and a read that found
		// nothing new still copies when the sender published since the previous read.
		const bool bRecheckNotification = SpoutLimits::FrameCountRecheck();
		SPOUT_INSTRUMENT(
			const bool bInstrument = SpoutInstrument::IsEnabled();
			SpoutInstrument::FRecord Rec;
			if (bInstrument)
			{
				SpoutInstrument::Calibrate(Context.GetD3D12CommandQueue());
				BeginReceiveRecord(Rec, *Receiver, TEXT('R'));
			}
		)
		FSpoutReceiveCheck Check;
		const bool bTransfer = SpoutFrameWait::CheckForNewFrame(Receiver->FrameSync,
			[&](bool bNewFrameAtRead)
			{
				SPOUT_INSTRUMENT(
					if (bInstrument)
					{
						Rec.NewFrame = bNewFrameAtRead ? 1 : 0;
						Rec.SenderFrame = Receiver->FrameSync.GetLastObservedSenderFrame();
						Rec.Flags |= (SpoutLimits::FrameWaitSeconds() > 0.0 ? 1 : 0) | (bRecheckNotification ? 2 : 0);
					}
				)
			},
			[&](FSpoutFrameWaitReport& Report, double& BudgetBeforeSeconds, double& BudgetAfterSeconds)
			{
				return TryWaitForPublication(*Receiver, Token, Report, BudgetBeforeSeconds, BudgetAfterSeconds);
			},
			Check);
		SPOUT_INSTRUMENT(if (bInstrument) RecordReceiveCheck(Rec, Check);)
		if (!bTransfer)
		{
			SPOUT_INSTRUMENT(if (bInstrument) { Rec.TRestore = SpoutInstrument::Now(); FinishReceiveRecord(Rec, *Receiver); })
			return;
		}
		// The row is queued when the function is left, whichever way.
		SPOUT_INSTRUMENT(ON_SCOPE_EXIT { if (bInstrument) FinishReceiveRecord(Rec, *Receiver); };)

		// Resolve native D3D12 resources up front. A null native means interop wrapping is
		// unavailable for that destination and it must be skipped (not fatal for the other).
		ID3D12Resource* DestNative = nullptr;
		ID3D12Resource* DestRTNative = nullptr;

		if (DestTextureRHI.IsValid())
		{
			DestNative = static_cast<ID3D12Resource*>(DestTextureRHI->GetNativeResource());
		}
		if (DestRenderTargetRHI.IsValid())
		{
			DestRTNative = static_cast<ID3D12Resource*>(DestRenderTargetRHI->GetNativeResource());
		}

		if (!DestNative && !DestRTNative)
		{
			return;
		}

		// Both destination brackets come from SpoutRHIState, which is also where the wrap
		// states below come from. The transition must precede the D3D11-on-12 Acquire, which
		// requires the resource to already be in the state declared at CreateWrappedResource time
		// -- that pairing is what makes the wrap's in/out states correct.
		//
		// The previous access is declared Unknown rather than SRVMask. Neither destination's prior
		// state is owned by this plugin: the caller-supplied render target may last have been an
		// RTV, a UAV or a copy destination, and the plugin's own received texture is created by
		// UTexture2D::UpdateResource, whose initial access is the RHI's choice rather than the plugin's.
		// Unknown resolves the barrier from the RHI's own whole-resource tracking, which is right
		// on the first frame after creation as well as on every frame after that.
		const SpoutRHIState::FCopyBracket TextureBracket =
			SpoutRHIState::GetCopyBracket(SpoutRHIState::EResourceRole::ReceiverTexture);
		const SpoutRHIState::FCopyBracket RenderTargetBracket =
			SpoutRHIState::GetCopyBracket(SpoutRHIState::EResourceRole::ReceiverRenderTarget);

		if (DestTextureRHI.IsValid() && DestNative)
		{
			TransitionSpoutInteropTexture(RHICmdList, DestTextureRHI, TextureBracket.EntryBefore, TextureBracket.CopyAccess);
		}
		if (DestRenderTargetRHI.IsValid() && DestRTNative)
		{
			TransitionSpoutInteropTexture(RHICmdList, DestRenderTargetRHI, RenderTargetBracket.EntryBefore, RenderTargetBracket.CopyAccess);
		}

		// Flush so D3D12 state transitions are submitted and visible to the D3D11-on-12 device
		// before Acquire/CopyResource runs. Mirrors the sender path.
		SPOUT_INSTRUMENT(if (bInstrument) Rec.TDrain0 = SpoutInstrument::Now();)
		SubmitSpoutInteropTransitions(RHICmdList);
		SPOUT_INSTRUMENT(if (bInstrument) Rec.TDrain1 = SpoutInstrument::Now();)

		{
			// Serialize D3D11 immediate-context access with the sender path.
			FScopeLock ContextLock(&Context.GetD3D11ContextMutex());

			ID3D11DeviceContext* ImmediateContext = Context.GetImmediateContext();
			ID3D11On12Device* D3D11On12 = Context.GetD3D11On12Device();
			if (ImmediateContext && D3D11On12)
			{
				ComPtr<ID3D11Texture2D> Wrapped;
				ComPtr<ID3D11Texture2D> WrappedRT;
				TArray<ID3D11Texture2D*, TInlineAllocator<2>> ToAcquire;

				SPOUT_INSTRUMENT(if (bInstrument) Rec.TWrap0 = SpoutInstrument::Now();)
				if (DestTextureRHI.IsValid() && DestNative)
				{
					Wrapped = GetOrCreateReceiverWrap(*Receiver, DestNative, TextureBracket.WrapState);
					if (Wrapped)
					{
						ToAcquire.Add(Wrapped.Get());
					}
				}
				if (DestRenderTargetRHI.IsValid() && DestRTNative)
				{
					WrappedRT = GetOrCreateReceiverRTWrap(*Receiver, DestRTNative, RenderTargetBracket.WrapState);
					if (WrappedRT)
					{
						ToAcquire.Add(WrappedRT.Get());
					}
				}
				SPOUT_INSTRUMENT(if (bInstrument) Rec.TWrap1 = SpoutInstrument::Now();)

				if (ToAcquire.Num() > 0)
				{
					// A keyed-mutex texture's release (ReleaseSync) is recorded on this D3D11-on-12
					// context and reaches the sender only when the context is flushed; the wrap
					// scope's flush below runs before it. Submit it as soon as access is given back,
					// not at this device's next flush, which could lock the sender out for a frame.
					D3D11_TEXTURE2D_DESC SharedDesc = {};
					SharedTexture->GetDesc(&SharedDesc);
					const bool bKeyedShared = (SharedDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0;
					ON_SCOPE_EXIT { if (bKeyedShared) ImmediateContext->Flush(); };

					// Take the sender's cross-process access mutex around the read, waiting at most
					// r.Spout.AccessWaitMs. A failed wait means the sender is mid-write, and
					// reading anyway would produce a torn frame. The scope owns nothing on failure,
					// so nothing is released, and the frame stays pending for the next call.
					SPOUT_INSTRUMENT(if (bInstrument) Rec.TAccess0 = SpoutInstrument::Now();)
					FSpoutTextureAccessScope Access(Receiver->FrameSync, SharedTexture.Get());
					SPOUT_INSTRUMENT(if (bInstrument) { Rec.TAcquire = SpoutInstrument::Now(); Rec.Acquired = Access.IsAcquired() ? 1 : 0; })
					if (!Access.IsAcquired())
					{
						if (Receiver->AccessLogThrottle.ShouldLog())
						{
							UE_LOG(LogSpoutPlugin, Warning,
								TEXT("Spout receiver '%s': shared-texture access was unavailable because a peer (the sender or another receiver) still held it after waiting up to %u ms (r.Spout.AccessWaitMs), ")
								TEXT("so this frame was not read yet. The frame stays pending and is read on a later frame."),
								*Receiver->Name.ToString(), SpoutLimits::AccessWaitMs());
						}
					}
					else
					{
						// Experimental: the notification read again while access is held. A sender
						// publishes before it releases access, so a frame published before this point
						// is seen now instead of being reported new again on the next call, after its
						// image was already copied. It makes a frame pending; it clears nothing.
						if (bRecheckNotification)
						{
							Receiver->FrameSync.ObserveNotificationUnderAccess();
							SPOUT_INSTRUMENT(if (bInstrument) RecordUnderAccess(Rec, *Receiver);)
						}

						// Acquire all destinations in their already-submitted COPY_DEST state.
						// Wait for the copies while shared access remains held; scope exit then
						// releases the wrappers and submits their outgoing transitions.
						FScopedD3D11On12Acquire Acquire(ToAcquire);
						if (Acquire.IsValid() && Context.PrepareGpuCompletion())
						{
							SPOUT_INSTRUMENT(if (bInstrument) SpoutInstrument::GetGpuBracket().Begin(ImmediateContext);)
							if (Wrapped)
							{
								TRACE_CPUPROFILER_EVENT_SCOPE(Spout_RecvCopyResource);
								ImmediateContext->CopyResource(Wrapped.Get(), SharedTexture.Get());
							}
							if (WrappedRT)
							{
								ImmediateContext->CopyResource(WrappedRT.Get(), SharedTexture.Get());
							}
							SPOUT_INSTRUMENT(if (bInstrument) { SpoutInstrument::GetGpuBracket().End(ImmediateContext); Rec.TCopyRec = SpoutInstrument::Now(); })

							// The completion point of the GPU path: the copy has finished on the GPU
							// and access is still held. Only here does the frame stop being pending;
							// a wrap failure or a failed completion wait leaves it pending instead.
							const HRESULT CompletionResult = Context.WaitForGpuCompletion();
							SPOUT_INSTRUMENT(if (bInstrument) { Rec.TGpuDone = SpoutInstrument::Now(); Rec.Hr = static_cast<int32>(CompletionResult); SpoutInstrument::GetGpuBracket().Resolve(ImmediateContext, Rec); })
							if (SUCCEEDED(CompletionResult))
							{
								const bool bDelivered = Receiver->FrameSync.CompleteTransfer();
								SPOUT_INSTRUMENT(if (bInstrument) { Rec.Completed = 1; Rec.Delivered = bDelivered ? 1 : 0; })
								if (bDelivered)
								{
									++Receiver->ReceivedFrameNumber;
								}
							}
						}
					}
				}
			}
		}
		// The access scope and the D3D11 context lock have ended.
		SPOUT_INSTRUMENT(if (bInstrument) Rec.TRelease = SpoutInstrument::Now();)

		// Always restore, even if the access wait, the wrap or the Acquire failed, so UE's state
		// tracking stays consistent with the earlier Transition. The declared previous state is the
		// copy access, never Unknown: the transition above is what set it.
		if (DestTextureRHI.IsValid() && DestNative)
		{
			TransitionSpoutInteropTexture(RHICmdList, DestTextureRHI, TextureBracket.CopyAccess, TextureBracket.ExitAfter);
		}
		if (DestRenderTargetRHI.IsValid() && DestRTNative)
		{
			TransitionSpoutInteropTexture(RHICmdList, DestRenderTargetRHI, RenderTargetBracket.CopyAccess, RenderTargetBracket.ExitAfter);
		}
		SPOUT_INSTRUMENT(if (bInstrument) Rec.TRestore = SpoutInstrument::Now();)
	}

	// CPU fallback path. Copy into a STAGING texture, Map, issue
	// RHIUpdateTexture2D straight from the mapped pointer (no intermediate TArray copy),
	// defer Unmap until after both updates return.
	void ReceiveOnRenderThread_CPU(
		TSharedPtr<FSpoutSharedSender> Receiver,
		TRefCountPtr<FRHITexture> DestTextureRHI,
		TRefCountPtr<FRHITexture> DestRenderTargetRHI,
		FRHICommandListImmediate& RHICmdList,
		const FSpoutStreamToken& Token,
		bool& bUploaded)
	{
		bUploaded = false;
		if (!DestTextureRHI.IsValid() && !DestRenderTargetRHI.IsValid())
		{
			return;
		}

		FSpoutD3DContext& Context = FSpoutD3DContext::Get();
		ComPtr<ID3D11Texture2D> SharedTexture;
		ComPtr<ID3D11Texture2D> StagingTexture;
		uint32 LocalWidth = 0;
		uint32 LocalHeight = 0;

		// Same rule as the GPU path: no new frame means the staging copy, the map and both
		// texture updates are all skipped. The same check stage as the GPU path, taken before any
		// lock or access.
		const bool bRecheckNotification = SpoutLimits::FrameCountRecheck();
		SPOUT_INSTRUMENT(
			const bool bInstrument = SpoutInstrument::IsEnabled();
			SpoutInstrument::FRecord Rec;
			if (bInstrument) BeginReceiveRecord(Rec, *Receiver, TEXT('C'));
			ON_SCOPE_EXIT
			{
				if (bInstrument)
				{
					Rec.TRestore = SpoutInstrument::Now();
					FinishReceiveRecord(Rec, *Receiver);
				}
			};
		)
		FSpoutReceiveCheck Check;
		const bool bTransfer = SpoutFrameWait::CheckForNewFrame(Receiver->FrameSync,
			[&](bool bNewFrameAtRead)
			{
				SPOUT_INSTRUMENT(
					if (bInstrument)
					{
						Rec.NewFrame = bNewFrameAtRead ? 1 : 0;
						Rec.SenderFrame = Receiver->FrameSync.GetLastObservedSenderFrame();
						Rec.Flags |= (SpoutLimits::FrameWaitSeconds() > 0.0 ? 1 : 0) | (bRecheckNotification ? 2 : 0);
					}
				)
			},
			[&](FSpoutFrameWaitReport& Report, double& BudgetBeforeSeconds, double& BudgetAfterSeconds)
			{
				return TryWaitForPublication(*Receiver, Token, Report, BudgetBeforeSeconds, BudgetAfterSeconds);
			},
			Check);
		SPOUT_INSTRUMENT(if (bInstrument) RecordReceiveCheck(Rec, Check);)
		if (!bTransfer)
		{
			return;
		}

		if (!GetSharedAndStaging(*Receiver, Context, SharedTexture, StagingTexture, LocalWidth, LocalHeight))
		{
			return;
		}

		const bool bNative = SpoutInterop::IsSharedSlot();
		FCriticalSection& ContextMutex = bNative ? FSpoutAsyncDevice::Get().GetContextMutex() : Context.GetD3D11ContextMutex();
		ID3D11DeviceContext* ImmediateContext = bNative ? FSpoutAsyncDevice::Get().GetContext() : Context.GetImmediateContext();
		if (!ImmediateContext || !SharedTexture || !StagingTexture)
		{
			return;
		}
		{
			FScopeLock Lock(&ContextMutex);
			if (!Receiver->CpuCompletion.Prepare(ImmediateContext)) return;
		}

		{
			// Hold access until the GPU has finished reading the shared texture into
			// staging. Mapping and UE upload happen afterwards using only local data. A keyed
			// release is submitted right after it is recorded, as on the GPU path.
			D3D11_TEXTURE2D_DESC SharedDesc = {};
			SharedTexture->GetDesc(&SharedDesc);
			const bool bKeyedShared = (SharedDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0;
			ON_SCOPE_EXIT { if (bKeyedShared) { FScopeLock Lock(&ContextMutex); ImmediateContext->Flush(); } };
			SPOUT_INSTRUMENT(if (bInstrument) Rec.TAccess0 = SpoutInstrument::Now();)
			FSpoutTextureAccessScope Access(Receiver->FrameSync, SharedTexture.Get());
			SPOUT_INSTRUMENT(if (bInstrument) { Rec.TAcquire = SpoutInstrument::Now(); Rec.Acquired = Access.IsAcquired() ? 1 : 0; })
			if (!Access.IsAcquired())
			{
				if (Receiver->AccessLogThrottle.ShouldLog())
				{
					UE_LOG(LogSpoutPlugin, Warning,
						TEXT("Spout receiver '%s': shared-texture access was unavailable on the CPU path because a peer (the sender or another receiver) still held it after waiting up to %u ms (r.Spout.AccessWaitMs), so this frame was not read yet."),
						*Receiver->Name.ToString(), SpoutLimits::AccessWaitMs());
				}
				return;
			}

			// Experimental: the notification read again while access is held, as on the GPU path.
			if (bRecheckNotification)
			{
				Receiver->FrameSync.ObserveNotificationUnderAccess();
				SPOUT_INSTRUMENT(if (bInstrument) RecordUnderAccess(Rec, *Receiver);)
			}

			// A Flush alone submits asynchronously and cannot hand ownership back safely.
			// Match the worker's lock order: peer access, then the native context. Never
			// hold that context while attempting to acquire access another user may hold.
			FScopeLock CopyLock(&ContextMutex);
			ImmediateContext->CopyResource(StagingTexture.Get(), SharedTexture.Get());
			SPOUT_INSTRUMENT(if (bInstrument) Rec.TCopyRec = SpoutInstrument::Now();)
			const HRESULT StagingResult = Receiver->CpuCompletion.Wait(ImmediateContext);
			SPOUT_INSTRUMENT(if (bInstrument) { Rec.TGpuDone = SpoutInstrument::Now(); Rec.Hr = static_cast<int32>(StagingResult); })
			if (FAILED(StagingResult))
			{
				return;
			}
		}
		SPOUT_INSTRUMENT(if (bInstrument) Rec.TRelease = SpoutInstrument::Now();)

		FScopeLock MapLock(&ContextMutex);
		D3D11_MAPPED_SUBRESOURCE Mapped = {};
		HRESULT MapResult;
#if WITH_DEV_AUTOMATION_TESTS
		if (GFailNextCpuReadback.Exchange(false)) MapResult = E_FAIL;
		else
#endif
		MapResult = ImmediateContext->Map(StagingTexture.Get(), 0, D3D11_MAP_READ, 0, &Mapped);
		if (SUCCEEDED(MapResult))
		{
			Receiver->LastNativeError.store(0);
			// FRHIComputeCommandList::UpdateTexture2D copies SourceData synchronously inside
			// the call, so Mapped.pData only needs to stay valid until both update calls return.
			const uint8* const SourceData = static_cast<const uint8*>(Mapped.pData);
			const FUpdateTextureRegion2D UpdateRegion(0, 0, 0, 0, LocalWidth, LocalHeight);

			// Use the command-list form: the global RHIUpdateTexture2D with an implied
			// immediate command list is deprecated in UE 5.8 and removed after it.
			if (DestTextureRHI.IsValid())
			{
				RHICmdList.UpdateTexture2D(
					DestTextureRHI,
					0,
					UpdateRegion,
					Mapped.RowPitch,
					SourceData
				);
			}

			if (DestRenderTargetRHI.IsValid())
			{
				RHICmdList.UpdateTexture2D(
					DestRenderTargetRHI,
					0,
					UpdateRegion,
					Mapped.RowPitch,
					SourceData
				);
			}

			ImmediateContext->Unmap(StagingTexture.Get(), 0);
			bUploaded = true;
			// The completion point of the CPU path: the staging copy finished under access, access
			// was released, and the map and the upload succeeded. A publication made after the
			// read under access is not consumed by this: the next read reports it.
			const bool bDelivered = Receiver->FrameSync.CompleteTransfer();
			SPOUT_INSTRUMENT(if (bInstrument) { Rec.Completed = 1; Rec.Delivered = bDelivered ? 1 : 0; })
			if (bDelivered)
			{
				++Receiver->ReceivedFrameNumber;
			}
		}
		else
		{
			++Receiver->NativeFailures; Receiver->LastNativeError.store(static_cast<int64>(MapResult));
			if (Receiver->OpenLogThrottle.ShouldLog()) UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout receiver '%s': CPU readback failed (hr=0x%08x); the frame remains pending for retry."), *Receiver->Name.ToString(), MapResult);
		}
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void SpoutReceiverInternal::FailNextCpuReadbackForTests() { GFailNextCpuReadback.Store(true); }
#endif

void SpoutReceiverInternal::RunEndOfFrameRetries()
{
	check(IsInRenderingThread());
	if (GEndOfFrameRetries.Num() == 0)
	{
		return;
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_EndOfFrameReceiveRetries);
	TArray<FEndOfFrameRetry> Retries = MoveTemp(GEndOfFrameRetries);
	GEndOfFrameRetries.Reset();
	if (SpoutLimits::DevRecvSyncRetry() == 0)
	{
		return; // switched off since they were armed: dropped
	}
	FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
	// One budget for the frame's end-of-frame attempts, all streams together: each attempt waits for access at most what is left.
	int64 BudgetLeftUs = SpoutLimits::DevRecvSyncRetryBudgetUs();
	for (FEndOfFrameRetry& Retry : Retries)
	{
		const TSharedPtr<FSpoutSharedSender> Receiver = Retry.Receiver.Pin();
		if (!Receiver || !Retry.Destination.IsValid() || !FSpoutSession::Get().IsCurrent(Retry.Token))
		{
			continue;
		}
		const int32 TimeoutMs = static_cast<int32>(FMath::Min<int64>(SpoutLimits::AccessWaitMs(), FMath::Max<int64>(0, BudgetLeftUs) / 1000));
		const uint64 Before = FPlatformTime::Cycles64();
		const uint64 PreviousFrame = Receiver->ReceivedFrameNumber;
		ReceiveOnRenderThread_SharedSlot(Receiver, Retry.Destination, RHICmdList, Retry.Token, ESyncAttempt::EndOfFrame, TimeoutMs);
		if (PreviousFrame != Receiver->ReceivedFrameNumber)
		{
			Receiver->ReceivedFrame = Retry.Destination;
		}
		BudgetLeftUs -= static_cast<int64>(FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - Before) * 1000.0);
	}
}

void SpoutReceiverInternal::DiscardEndOfFrameRetries()
{
	check(IsInRenderingThread());
	GEndOfFrameRetries.Reset();
}

#if WITH_DEV_AUTOMATION_TESTS
int32 SpoutReceiverInternal::GetArmedEndOfFrameRetriesForTests()
{
	check(IsInRenderingThread());
	return GEndOfFrameRetries.Num();
}
#endif

UMaterialInstanceDynamic* SpoutReceiverInternal::ResolveConsumerMaterial(
	FSpoutReceiverOwnership& Ownership,
	FName ReceiverName,
	UMaterialInstanceDynamic* CallerMaterial,
	UMaterialInterface* InputMaterial,
	bool& bOutPluginCreated)
{
	// A caller that already holds a material keeps it. There is deliberately NO fall back to "some
	// material already tracked for this receiver name": with two consumers of one name it would hand
	// the second consumer the FIRST consumer's instance and silently ignore its InputMaterial.
	UMaterialInstanceDynamic* Material = CallerMaterial;

	// Asked of the tracker rather than inferred here, so a material created by an earlier call is
	// still recognised as plugin-created on every later call.
	bOutPluginCreated = Ownership.OwnsMaterial(ReceiverName, Material);

	if (!Material && InputMaterial)
	{
		// Use the transient package as outer so the MID has a valid owning UObject graph and is not
		// at risk of being collected between creation and first sampling.
		Material = UMaterialInstanceDynamic::Create(InputMaterial, GetTransientPackage());
		bOutPluginCreated = Material != nullptr;
	}

	return Material;
}

bool FSpoutReceiver::Receive(const FName SpoutName, UMaterialInterface* InputMaterial, FName TextureParameterName, UMaterialInstanceDynamic*& OutMat, UTexture2D*& OutTexture, UTextureRenderTarget2D* OptionalOutputRenderTarget, ESpoutColorSpace ColorSpace, bool bAsyncReceive)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_Receive);
	// Validate the requested name before touching the interop context. An invalid name can never
	// resolve to a sender, so there is nothing to initialize for it.
	TArray<ANSICHAR> Utf8Name;
	const SpoutValidation::EResult NameResult = SpoutValidation::ValidateName(SpoutName, &Utf8Name);
	if (NameResult != SpoutValidation::EResult::Ok)
	{
		static FSpoutLogThrottle NameLogThrottle(5.0);
		if (NameLogThrottle.ShouldLog())
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("SpoutReceiver: rejected sender name '%s' (%s)."),
				*SpoutName.ToString(),
				SpoutValidation::ToString(NameResult));
		}
		return false;
	}

	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	if (!FSpoutSession::Get().IsRunning()) return false;
	Context.InitializeIfNeeded();

	if (!Context.GetSenderNames() || !Context.GetD3D11Device())
	{
		return false;
	}

	unsigned int W = 0;
	unsigned int H = 0;
	HANDLE SharedHandle = nullptr;
	unsigned long Format = 0;
	if (!FSpoutSession::Get().IsRunning() || !QuerySenderInfo(Utf8Name.GetData(), W, H, SharedHandle, Format))
	{
		return false;
	}

	// Everything above came from another process through shared memory. Validate the whole set
	// before opening a shared resource or allocating a destination texture for it: dimensions are
	// widened to int64 first so a 4-billion uint32 stays large rather than wrapping to a plausible
	// negative, and an unsupported or unknown format is refused instead of being reinterpreted.
	const SpoutValidation::EResult MetadataResult = SpoutValidation::ValidateExternalSenderMetadata(
		static_cast<int64>(W),
		static_cast<int64>(H),
		SharedHandle,
		static_cast<uint32>(Format));
	if (MetadataResult != SpoutValidation::EResult::Ok)
	{
		static FSpoutLogThrottle MetadataLogThrottle;
		if (MetadataLogThrottle.ShouldLog())
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("Spout receiver '%s': sender metadata rejected (%s). Reported %ux%u, format %s, handle %s. ")
				TEXT("Supported formats are RGBA8, BGRA8, RGB10A2 and RGBA16F (including their typeless variants)."),
				*SpoutName.ToString(),
				SpoutValidation::ToString(MetadataResult),
				W,
				H,
				*SpoutFormatPolicy::DescribeDxgiFormat(static_cast<uint32>(Format)),
				SharedHandle ? TEXT("present") : TEXT("null"));
		}
		return false;
	}

	const SpoutFormatPolicy::FReceiverFormat ReceiverFormat = SpoutFormatPolicy::ResolveReceiverFormat(static_cast<uint32>(Format));
	const EPixelFormat PixelFormat = ReceiverFormat.PixelFormat;
	int64 FrameBytes = 0;
	if (!SpoutLimits::FrameBytes(W, H, PixelFormat, FrameBytes)) return false;

	// One explicit colour-space contract, resolved here and nowhere else. Auto follows the
	// documented per-format convention; an explicit request is honoured where it is representable
	// and REFUSED where it is not, rather than being silently dropped. bSRGB is part of the
	// destination texture's identity in EnsureTransientTexture, so changing the setting recreates
	// the texture instead of reinterpreting the existing one.
	bool bSRGB = false;
	const SpoutFormatPolicy::EColorSpaceResult ColorSpaceResult =
		SpoutFormatPolicy::ResolveReceiverColorSpace(ReceiverFormat, ColorSpace, bSRGB);
	if (ColorSpaceResult != SpoutFormatPolicy::EColorSpaceResult::Ok)
	{
		static FSpoutLogThrottle ColorLogThrottle;
		if (ColorLogThrottle.ShouldLog())
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("Spout receiver '%s': colour space %s cannot be applied to %s (%s). ")
				TEXT("sRGB decoding only exists for the 8-bit formats; 10-bit and 16-bit float frames are linear. ")
				TEXT("Set Color Space to Auto or Linear."),
				*SpoutName.ToString(),
				SpoutFormatPolicy::ToString(ColorSpace),
				*SpoutFormatPolicy::DescribeDxgiFormat(static_cast<uint32>(Format)),
				SpoutFormatPolicy::ToString(ColorSpaceResult));
		}
		return false;
	}

	// Discovery failures above retain no stream/native records. Once metadata is valid,
	// the explicit close/session lifecycle owns this connected name.
	const FSpoutStreamToken Token = FSpoutSession::Get().OpenStream(SpoutName, ESpoutType::Receiver);
	if (!Token.IsSet()) return false;
	const auto QueuedWork = FSpoutSession::Get().TryQueue(Token);
	if (!QueuedWork) return false;
	TSharedPtr<FSpoutSharedSender> Receiver;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Spout_ReceiveFindOrAddEntry);
		Receiver = FSpoutSenderRegistry::Get().FindOrAdd(SpoutName, ESpoutType::Receiver);
	}
	if (!Receiver || !Receiver->HasValidName()) return false;

	// One session per name has one receive mode. The first accepted call fixed it; a call asking for the other mode
	// is refused here, before any reservation, output, material or queued work is touched, so the session and every
	// other consumer stay exactly as they were. It is neither switched behind them nor reopened, and a conflicting
	// caller does not win by calling last or more often.
	if (Receiver->bReceiveModeEstablished && Receiver->bAsyncReceiveRequested != bAsyncReceive)
	{
		if (Receiver->ReceiveModeLogThrottle.ShouldLog())
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("Spout receiver '%s': refused a receive asking for %s, because the open receiver for this name uses %s. ")
				TEXT("Every consumer of one Spout name shares one receiver and one receive mode, fixed when it opened. ")
				TEXT("Use the same Async Receive (Beta) setting for every consumer of the name, or Close Receiver and receive again to change it."),
				*SpoutName.ToString(),
				bAsyncReceive ? TEXT("Async Receive (Beta)") : TEXT("the synchronous receive"),
				Receiver->bAsyncReceiveRequested ? TEXT("Async Receive (Beta)") : TEXT("the synchronous receive"));
		}
		return false;
	}

	// The destination is whatever this plugin owns for this receiver name, not whatever the caller
	// happened to pass in. Ownership decides which object the receive path writes into and which
	// object the close path may release; a caller variable is an output rather than the authority.
	FSpoutReceiverOwnership& Ownership = FSpoutReceiverOwnership::Get();

	const FName ParamName = TextureParameterName.IsNone() ? FName("SpoutTexture") : TextureParameterName;
	if ((OutMat || InputMaterial) && !Ownership.HasMaterialBinding(SpoutName, OutMat, ParamName)
		&& Ownership.NumMaterialBindings(SpoutName) >= SpoutLimits::MaxConsumers()) return false;
	for (auto It = Receiver->OutputReservations.CreateIterator(); It; ++It)
		if (!It.Key().IsValid()) It.RemoveCurrent();
	TSharedPtr<FSpoutResourceReservation, ESPMode::ThreadSafe> PreviousOutputReservation;
	if (OptionalOutputRenderTarget && OptionalOutputRenderTarget->GetFormat() == PixelFormat && !OptionalOutputRenderTarget->bAutoGenerateMips)
	{
		const TWeakObjectPtr<UObject> Key(OptionalOutputRenderTarget);
		auto* Existing = Receiver->OutputReservations.Find(Key);
		if (!Existing && Receiver->OutputReservations.Num() >= SpoutLimits::MaxConsumers()) return false;
		if (!Existing || (*Existing)->GetBytes() != FrameBytes * 2)
		{
			const auto Reservation = FSpoutResourceReservation::TryReserve(FrameBytes * 2);
			if (!Reservation) return false;
			if (Existing) PreviousOutputReservation = *Existing;
			Receiver->OutputReservations.Add(Key, Reservation);
		}
	}
	UTexture2D* ReceivedTexture = Ownership.FindTexture(SpoutName);
	// One shared texture has one interpretation. Do not churn it when consumers disagree.
	if (ReceivedTexture && ReceivedTexture->GetPixelFormat() == PixelFormat && (ReceivedTexture->SRGB != 0) != bSRGB)
	{
		if (Receiver->FormatLogThrottle.ShouldLog())
			UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout receiver '%s': conflicting color space for an open session; use the established color space or close the session first."), *SpoutName.ToString());
		return false;
	}
	bool bReplacedOutput = false;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Spout_ReceiveEnsureOutput);
		bReplacedOutput = SpoutTextureUtils::EnsureTransientTexture(ReceivedTexture, static_cast<int32>(W), static_cast<int32>(H), PixelFormat, bSRGB);
	}
	if (bReplacedOutput)
	{
		if (!ReceivedTexture)
		{
			// Creation failed. Report it rather than queueing a copy with no destination; the
			// documented contract for a true return is that the destination texture exists.
			if (Receiver->OpenLogThrottle.ShouldLog()) UE_LOG(LogSpoutPlugin, Warning,
				TEXT("Spout receiver '%s': could not create a %ux%u %s texture for the incoming frame."),
				*SpoutName.ToString(), W, H, GetPixelFormatString(PixelFormat));
			return false;
		}

		// Adopting the replacement is also what drops the previous one: it loses this reference and
		// is collected once the caller's copy and any in-flight render command are gone. Nothing is
		// unrooted, released or garbage-marked by hand.
		Ownership.AdoptTexture(SpoutName, ReceivedTexture);
	}


	// The material is per CONSUMER, unlike the texture, which is per session. The rule lives in
	// ResolveConsumerMaterial.
	bool bPluginCreatedMaterial = false;
	UMaterialInstanceDynamic* Material = nullptr;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Spout_ReceiveResolveMaterial);
		Material = SpoutReceiverInternal::ResolveConsumerMaterial(
			Ownership, SpoutName, OutMat, InputMaterial, bPluginCreatedMaterial);
	}

	if (Material)
	{
		UTexture* PreviousTexture = Material->K2_GetTextureParameterValue(ParamName);
		Material->SetTextureParameterValue(ParamName, ReceivedTexture);
		// Registered even when the caller created it, so close can detach the parameter this plugin
		// set. Ownership of a caller's material is not claimed, and registering one never displaces
		// a plugin-created binding: the session holds a collection, not a single slot. De-duplicated
		// on (material, parameter), so calling this every tick does not grow it.
		Ownership.RegisterMaterialBinding(SpoutName, Material, ParamName, bPluginCreatedMaterial, PreviousTexture);
	}

	OutTexture = ReceivedTexture;
	OutMat = Material;

	// Capture only resource pointers on the game thread, never their render-owned RHI
	// fields. UE enqueues resource release/deletion after this command, including an
	// UpdateResource immediately after Receive; GC also waits for its release fence.
	FTextureResource* TextureResource = ReceivedTexture->GetResource();
	FTextureRenderTargetResource* OutputResource = nullptr;
	if (OptionalOutputRenderTarget)
	{
		// Refuse an incompatible authored target BEFORE resize can clear its contents.
		if (OptionalOutputRenderTarget->GetFormat() == PixelFormat && !OptionalOutputRenderTarget->bAutoGenerateMips)
		{
			if (OptionalOutputRenderTarget->SizeX != static_cast<int32>(W) || OptionalOutputRenderTarget->SizeY != static_cast<int32>(H))
			{
				OptionalOutputRenderTarget->ResizeTarget(static_cast<int32>(W), static_cast<int32>(H));
			}
			OutputResource = OptionalOutputRenderTarget->GameThread_GetRenderTargetResource();
		}
		else if (Receiver->OutputTargetLogThrottle.ShouldLog())
		{
			UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout receiver '%s': incompatible output render target left untouched; format must match and Auto Generate Mips must be disabled."), *SpoutName.ToString());
		}
	}
	const bool bUseGPUPath = CVarSpoutGPUReceiver.GetValueOnAnyThread() != 0;
	const TWeakObjectPtr<UObject> OutputIdentity(OptionalOutputRenderTarget);
	// The call is accepted. The first accepted call of this entry fixes the session's receive mode; every command
	// of the entry then carries the same value, so the render thread never sees the mode change within a session.
	if (!Receiver->bReceiveModeEstablished)
	{
		Receiver->bReceiveModeEstablished = true;
		Receiver->bAsyncReceiveRequested = bAsyncReceive;
		if (bAsyncReceive)
		{
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout receiver '%s': opened with Async Receive (Beta) requested. It stays in this mode until Close Receiver."), *SpoutName.ToString());
		}
	}
	const bool bAsyncRequested = Receiver->bAsyncReceiveRequested;
	FSpoutSession::Get().MarkQueued(Token);
	ENQUEUE_RENDER_COMMAND(SpoutRecvCmd)(
		[Token, Receiver, TextureResource, OutputResource, OutputIdentity, W, H, SharedHandle, Format, PixelFormat, bUseGPUPath, bAsyncRequested, QueuedWork, PreviousOutputReservation](FRHICommandListImmediate& RHICmdList)
		{
			if (!FSpoutSession::Get().IsCurrent(Token))
			{
				return;
			}
			// Measurement builds only: r.Spout.Instrument.Bypass bit 0 returns before any Spout
			// work, so the same load can be measured without it.
			SPOUT_INSTRUMENT(if (SpoutInstrument::BypassMask() & 1) return;)
			// Native source replacement and destination resolution share ONE render
			// command. Later game-thread receives cannot replace half of this pair.
			GReceiveCommandStart = FPlatformTime::Cycles64();
			GReceiveCommandUpdated = 0;
			ON_SCOPE_EXIT { GReceiveCommandStart = 0; GReceiveCommandUpdated = 0; };
			FSpoutD3DContext& RenderContext = FSpoutD3DContext::Get();
			if (!UpdateSharedReceiverTexture(*Receiver, W, H, SharedHandle, static_cast<DXGI_FORMAT>(Format), RenderContext))
			{
				return;
			}
			GReceiveCommandUpdated = FPlatformTime::Cycles64();
			TRefCountPtr<FRHITexture> TextureRHI = TextureResource ? TextureResource->GetTextureRHI() : nullptr;
			TRefCountPtr<FRHITexture> TargetRHI = OutputResource ? OutputResource->GetRenderTargetTexture() : nullptr;
			auto IsCompatible = [W, H, PixelFormat](const TRefCountPtr<FRHITexture>& Texture)
			{
				if (!Texture) return false;
				const FRHITextureDesc& Desc = Texture->GetDesc();
				return Desc.Extent.X == static_cast<int32>(W) && Desc.Extent.Y == static_cast<int32>(H)
					&& Desc.Format == PixelFormat && Desc.NumMips == 1 && Desc.NumSamples == 1
					&& Desc.ArraySize == 1 && Desc.Dimension == ETextureDimension::Texture2D;
			};
			if (!IsCompatible(TextureRHI)) TextureRHI.SafeRelease();
			if (!IsCompatible(TargetRHI)) TargetRHI.SafeRelease();
			if (!TextureRHI) return;
			// Restore a recreated local resource even if the upstream sender is paused.
			if (Receiver->ReceivedFrame && Receiver->ReceivedFrame != TextureRHI)
			{
				if (IsCompatible(Receiver->ReceivedFrame))
				{
					CopyReceivedFrame(RHICmdList, Receiver->ReceivedFrame, TextureRHI);
					Receiver->ReceivedFrame = TextureRHI;
				}
				else Receiver->ReceivedFrame.SafeRelease();
			}
			const uint64 PreviousFrame = Receiver->ReceivedFrameNumber;
			bool bCpuUploaded = false;
			if (ReceiveOnRenderThread_Async(*Receiver, bAsyncRequested, TextureRHI, PixelFormat, RHICmdList))
			{
				// Async Receive (Beta), requested by this session: the worker made the transfer; nothing more here.
			}
			else if (bUseGPUPath)
			{
				ReceiveOnRenderThread_GPU(Receiver, TextureRHI, nullptr, RHICmdList, Token);
			}
			else
			{
				ReceiveOnRenderThread_CPU(Receiver, TextureRHI, nullptr, RHICmdList, Token, bCpuUploaded);
			}
			if (PreviousFrame != Receiver->ReceivedFrameNumber) Receiver->ReceivedFrame = TextureRHI;
			for (auto It = Receiver->DestinationDeliveries.CreateIterator(); It; ++It)
			{
				if (!It.Key().IsValid(false, true)) It.RemoveCurrent();
			}
			if (TargetRHI && Receiver->ReceivedFrame)
			{
				auto& Delivery = Receiver->DestinationDeliveries.FindOrAdd(OutputIdentity);
				if (Delivery.Frame != Receiver->ReceivedFrameNumber || Delivery.Resource != TargetRHI)
				{
					CopyReceivedFrame(RHICmdList, Receiver->ReceivedFrame, TargetRHI, bCpuUploaded);
					Delivery.Resource = TargetRHI;
					Delivery.Frame = Receiver->ReceivedFrameNumber;
				}
			}
		});

	return true;
}

void FSpoutReceiver::Close(const FName SpoutName)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_CloseReceiver);
	// Cancel before releasing anything, so a receive queued earlier in this frame cannot copy into
	// the texture after the caller has been told the receiver is closed. Scoped to the Receiver
	// role: a Sender with the same FName keeps running.
	FSpoutSession::Get().CancelStream(SpoutName, ESpoutType::Receiver);

	// Registry is keyed by (Name, Type); scope removal to the Receiver entry only, leaving any
	// Sender entry with the same FName untouched. Dropping the entry releases the opened shared
	// DX11 texture, staging texture, and cached D3D11-on-12 wraps held by FSpoutSharedSender.
	if (FSpoutSenderRegistry::Get().Find(SpoutName, ESpoutType::Receiver))
	{
		FSpoutSenderRegistry::Get().Remove(SpoutName, ESpoutType::Receiver);
		UE_LOG(LogSpoutPlugin, Log, TEXT("Closed Spout Receiver: %s"), *SpoutName.ToString());
	}
}
