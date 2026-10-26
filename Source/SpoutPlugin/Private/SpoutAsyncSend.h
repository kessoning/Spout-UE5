// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * The asynchronous render-target sender. DEVELOPMENT SWITCH ONLY: r.Spout.Dev.AsyncSend, 0 by default. With it at 0
 * nothing here runs and the render-target send is the synchronous shared-slot send (SpoutSender.cpp).
 *
 * With it at 1, on the shared-slot path, each render-target sender entry gets one stream: a worker thread and slot
 * sets of two slots per source identity. The render thread only queues Unreal's copy of the source into a free slot
 * and the filled-fence signal, dispatches them, and hands the frame to the worker; it never waits and never takes
 * Spout's access. The worker waits for the fill (bounded), installs the entry's shared texture and registration for
 * the frame's identity, takes the name's access in attempts of r.Spout.AccessWaitMs, copies the slot into the shared
 * texture on the native device, waits until that copy is proven finished (fence, the event query after a failed
 * signal, or the native device's removal: never a wall-clock release), announces the frame and gives access back.
 *
 * Ownership: the render thread records Unreal's work only; the worker reads Unreal's fences only on the CPU
 * and never queues a wait on Unreal's queue. The worker is the only thread that uses the entry's shared texture,
 * registration, FrameSync access and announcement while the stream exists. Spout's named mutex is taken and given back
 * by the worker thread. The native context lock is held for one copy, signal and flush, or one query.
 *
 * Every accepted frame ends in one outcome (SpoutSendRing.h): Published, Superseded (a newer frame was ready before it
 * made any access attempt), AccessAbandoned (a failed attempt, then a newer ready frame, 100 ms after it was ready, or
 * a flush or stop that allows one attempt), or a failure. Frames end, and are announced, in acceptance order.
 *
 * Who waits:
 *   FlushRenderingCommands  the game thread, at OnFlushRenderingCommandsEnd: every frame accepted before it is
 *                           resolved, owed frames with one access attempt (ResolveAllOwed)
 *   close                   the thread that drops the entry's last reference (Stop): owed frames resolved with one
 *                           attempt each, frames that still need an installation fail, then the worker is joined
 *   session stop            the flush step, then the entries' destruction (nothing owed by then)
 * The only unbounded class is a drain's proof (as on the synchronous send): a GPU that stalls without a removal
 * keeps the access, the flush, the close and the stop waiting; a warning says so every 5 s.
 *
 * Lock order: the entry's ResourceMutex (installation) and FSpoutFrameSync's lock (access) are taken by the worker
 * one at a time, never with the stream's state lock. The state lock is a leaf: one bookkeeping call, never across a
 * graphics call, a lock or a wait. The process's list of streams is a leaf too.
 *
 * Memory: each slot set reserves two frames against r.Spout.MaxRetainedMiB (the shared texture keeps its own two);
 * a set given up when its identity changes keeps its reservation until FSpoutGpuRetirement releases it. A refused
 * reservation makes that send Not accepted (budget) and is tried again on the next send.
 */
#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "SpoutLogThrottle.h"
#include "SpoutSendRing.h"
#include "SpoutSession.h"
#include <atomic>

#if PLATFORM_WINDOWS
#include "RHI.h"
#include "RHIResources.h"
#include "SpoutRHIState.h"
#include "SpoutSlotRules.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include "Windows/HideWindowsPlatformTypes.h"

class FRunnableThread;
class FRHICommandListImmediate;
class FSpoutSharedSender;
class FSpoutResourceReservation;
class FEvent;

/** A stream's counters, read on any thread (a snapshot under the state lock). Internal: no public fields. */
struct FSpoutAsyncSendStats
{
	int64 Accepted = 0;
	int64 NotAccepted[static_cast<int>(SpoutSendRing::ENotAccepted::Count)] = {};
	int64 Outcomes[static_cast<int>(SpoutSendRing::EOutcome::Count)] = {};
	int64 InFlight = 0;
	/** Access attempts the worker made, and those that did not get access (each one also counts in AccessMisses). */
	int64 Attempts = 0;
	int64 FailedAttempts = 0;
	int64 RuleViolations = 0;
	/** Frames drained after the frame-end fence's 100 ms fallback (r.Spout.Dev.AsyncSend.DrainAfter 1). */
	int64 FrameEndFallbacks = 0;
	int64 SetsCreated = 0;
	int64 SetsRetired = 0;
	/** Published frames: acceptance to announcement, and first access attempt to access, in milliseconds. */
	double PublishLatencyMsSum = 0.0;
	double PublishLatencyMsMax = 0.0;
	double AccessWaitMsSum = 0.0;
	double AccessWaitMsMax = 0.0;

	int64 Published() const { return Outcomes[static_cast<int>(SpoutSendRing::EOutcome::Published)]; }
	int64 Superseded() const { return Outcomes[static_cast<int>(SpoutSendRing::EOutcome::Superseded)]; }
	int64 AccessAbandoned() const { return Outcomes[static_cast<int>(SpoutSendRing::EOutcome::AccessAbandoned)]; }
	int64 Failed() const
	{
		int64 N = 0;
		for (int32 I = static_cast<int32>(SpoutSendRing::EOutcome::FailedFill); I < static_cast<int32>(SpoutSendRing::EOutcome::Count); ++I) N += Outcomes[I];
		return N;
	}
};

class FSpoutAsyncSendStream final : public FRunnable
{
public:
	/** A source identity: one slot set each. */
	struct FIdentity
	{
		uint32 Width = 0;
		uint32 Height = 0;
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN; // the shared texture's format, as the synchronous slot send uses
		EPixelFormat PixelFormat = PF_Unknown;
		bool operator==(const FIdentity& O) const { return Width == O.Width && Height == O.Height && Format == O.Format && PixelFormat == O.PixelFormat; }
		bool operator!=(const FIdentity& O) const { return !(*this == O); }
	};

	/**
	 * The worker's installation of the entry's shared texture and registration for an identity, provided by
	 * SpoutSender.cpp: under FSpoutSession::ExecuteIfCurrent(Token), EnsureSharedSenderTexture, then the texture if the
	 * registration is active. False (and OutTexture null) when the token is no longer current or it failed.
	 */
	using FInstaller = TFunction<bool(FSpoutSharedSender& Entry, const FSpoutStreamToken& Token, const FIdentity& Identity, TRefCountPtr<ID3D11Texture2D>& OutTexture)>;

	static constexpr int32 SlotsPerSet = 2;
	/** Bound of the worker's wait for a fill, from acceptance (as FSpoutSlotSend::FilledTimeoutMs). */
	static constexpr uint32 FilledTimeoutMs = 1000;
	/** A ready frame that made a failed access attempt is abandoned this long after it became ready. */
	static constexpr double AccessDeadlineSeconds = 0.1;
	/** r.Spout.Dev.AsyncSend.DrainAfter 1: the longest wait for the frame-end fence after the fill was observed. */
	static constexpr double FrameEndFallbackSeconds = 0.1;

	/**
	 * Render thread. A stream for Entry, with its worker running, or null and why (the entry then uses the synchronous
	 * send for its life, and says so once). Entry must outlive the stream: its destructor calls Stop first.
	 */
	static TSharedPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe> Create(FSpoutSharedSender& Entry, FInstaller Installer, FString& OutWhy);

	/** Stop must have run (the entry's destructor calls it). */
	virtual ~FSpoutAsyncSendStream() override;

	/**
	 * Render thread. One render-target send: queues the copy of Source into a free slot and its fill signal on
	 * RHICmdList, dispatches them and hands the frame to the worker, or queues nothing (Not accepted, counted). Never
	 * waits. Bracket gives the source's states, as for the synchronous send.
	 */
	void Send(const FSpoutStreamToken& Token, FRHICommandListImmediate& RHICmdList, FRHITexture* Source, const SpoutRHIState::FCopyBracket& Bracket, const FIdentity& Identity);

	/**
	 * Any thread but the worker, once no render command can call Send any more (the entry's last reference is being
	 * dropped). Accepts nothing more, resolves every owed frame (one access attempt each, no installation), joins the
	 * worker and retires every slot set with its outstanding work. Waits as listed under "Who waits" in the file comment.
	 * Idempotent.
	 */
	void Stop();

	/** Game thread, at the end of a flush: frames accepted so far get one access attempt; returns once they are resolved. */
	void ResolveOwed();

	FSpoutAsyncSendStats GetStats() const;
	bool HasFailed() const { return bFailed.load(); }
	FString GetFailure() const;

	/** Stream objects alive in the process (tests). */
	static int32 GetLiveStreams();

	/** The flush hook (game thread): ResolveOwed on every live stream. */
	static void ResolveAllOwed();
	/** The frame-end hook (game thread): queues the frame-end fence's next signal behind this frame's render commands. */
	static void QueueFrameEndSignal();
	/** Module startup and shutdown (game thread). */
	static void RegisterHooks();
	static void UnregisterHooks();

private:
	struct FSet;
	struct FWork;

	FSpoutAsyncSendStream(FSpoutSharedSender& InEntry, FInstaller InInstaller);
	bool Initialize(FString& OutWhy);

	// FRunnable
	virtual uint32 Run() override;

	/** Worker: one step for the oldest frame. False when there was nothing to do. */
	bool Step();
	void WaitForFill(const FWork& W);
	void WorkReady(const FWork& W);
	/** Worker: the oldest frame ends with Outcome; retires sets nothing uses any more. */
	void ResolveOldest(uint64 Id, SpoutSendRing::EOutcome Outcome);
	/** Worker: the stream stops for good: every frame owed is resolved failed. */
	void FailStream(const FString& Why, SpoutSendRing::EOutcome OwedOutcome);
	/** State lock held: moves out the sets that are not current and that no frame uses. */
	void TakeUnusedSetsLocked(TArray<TSharedPtr<FSet, ESPMode::ThreadSafe>>& Out);
	void RetireSets(TArray<TSharedPtr<FSet, ESPMode::ThreadSafe>>& Sets);
	TSharedPtr<FSet, ESPMode::ThreadSafe> FindSetLocked(uint64 Generation) const;
	TSharedPtr<FSet, ESPMode::ThreadSafe> CreateSet(const FIdentity& Identity, FString& OutWhy, bool& bOutBudget);
	void LogSummary() const;

	FSpoutSharedSender& Entry;
	const FInstaller Installer;
	const FString What;

	mutable FCriticalSection StateMutex;
	SpoutSendRing::FRing Ring{SlotsPerSet};
	TArray<TSharedPtr<FSet, ESPMode::ThreadSafe>> Sets;
	FSpoutStreamToken Token;
	bool bStopRequested = false;
	// Counters beside the ring's (state lock).
	int64 FrameEndFallbacks = 0;
	int64 SetsCreated = 0;
	int64 SetsRetired = 0;
	double PublishLatencyMsSum = 0.0, PublishLatencyMsMax = 0.0, AccessWaitMsSum = 0.0, AccessWaitMsMax = 0.0;
	/** First attempt time of the oldest frame (worker only). */
	double FirstAttemptAt = -1.0;
	uint64 FirstAttemptFor = 0;

	// Render thread only: a set could not be created for this identity (not the budget); not retried until it changes.
	FIdentity RefusedFor;
	// State lock: after a failed installation, frames are not accepted before NextInstallProbe (a back-off from 10 ms
	// to 1 s); a new identity or a successful installation resets it.
	double NextInstallProbe = 0.0;
	double InstallBackoff = 0.01;

	/** The process's frame-end fence (DrainAfter 1), held for the worker's CPU reads. */
	Microsoft::WRL::ComPtr<ID3D12Fence> FrameEndFence;
	// Worker only.
	FSpoutLogThrottle AccessLogThrottle;
	FSpoutLogThrottle FillLogThrottle;

	std::atomic<bool> bFailed{false};
	mutable FCriticalSection FailureMutex;
	FString Failure;
	std::atomic<bool> bStopped{false};

	FEvent* WorkEvent = nullptr;
	FRunnableThread* Thread = nullptr;
	double CreatedAt = 0.0;
};

/** Internal accessor (no public fields): the counters of Name's render-target sender stream. False without one. */
bool SpoutGetAsyncSendStats(const FName& Name, FSpoutAsyncSendStats& Out);

#if WITH_DEV_AUTOMATION_TESTS
/** Failure injection for the automation tests. Each counter makes that many next events take the failure branch. */
namespace SpoutAsyncSendTestHooks
{
	/** The next fill waits report a timeout (the fill still runs). */
	extern std::atomic<int32> TimeoutFills;
	/** The next fill observed finished is not accepted before this many milliseconds: a slow Unreal queue. */
	extern std::atomic<int32> DelayFillMs;
	/** The next drain signals are reported failed without being made. */
	extern std::atomic<int32> FailSignals;
	/** The next drain proof is not accepted before this many milliseconds: a long native copy. */
	extern std::atomic<int32> DelayDrainMs;
	/** The next installations are refused. */
	extern std::atomic<int32> FailInstalls;
	/** The next stream creation is refused, as if its worker thread could not be started. */
	extern std::atomic<int32> FailCreations;
}
#endif

#endif // PLATFORM_WINDOWS
