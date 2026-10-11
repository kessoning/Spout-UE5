// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Declares the Spout cross-process synchronisation model.
 *
 * Copying into and out of a shared texture with a bare CopyResource is not enough on its own:
 * the peer process may be writing the same surface at the same time. Spout defines a protocol
 * for this, which the bundled SDK provides and the bundled Spout.dll exports. It is made of
 * four calls:
 *
 *   - CheckTextureAccess / AllowTextureAccess : the named, cross-process texture-access mutex.
 *   - SetNewFrame / GetNewFrame               : the frame-count semaphore that tells a receiver
 *                                               whether the sender has published anything new.
 *
 * Note that the plugin's own SpoutMutex is an FCriticalSection and is not a substitute. It
 * serialises threads inside this process and says nothing at all to a Spout peer in another
 * one.
 *
 * Two things are deliberately NOT done the obvious way:
 *
 * 1. The SDK calls go through ISpoutFrameSyncBackend rather than being called inline. The cases
 *    that have to be covered are successful lock, failed lock, repeated lock/unlock, shutdown
 *    while waiting, restart and resize. If the SDK is called directly, none of that can be
 *    exercised without a second process holding a real named mutex. Behind an interface, every
 *    one of those cases is a few lines of substitute backend. ISpoutSenderNameBackend /
 *    FSpoutSenderRegistration are shaped the same way, for the same reason.
 *
 * 2. Access is taken through FSpoutTextureAccessScope, never by calling Acquire/Release in a
 *    pair by hand. "Every successful lock released exactly once" and "no forgotten unlocks" are
 *    then properties of the type rather than of the reviewer's attention -- including on the
 *    early-return and exception paths, and including the case the SDK gets wrong if you follow
 *    its own examples too literally: spoutFrameCount::AllowAccess() calls ReleaseMutex()
 *    whenever a handle exists, even when the matching CheckAccess() timed out and this process
 *    never owned the mutex.
 *
 * State is guarded by a critical section. Acquired Windows mutex ownership must be
 * released on the same thread; production access scopes run entirely on the render thread.
 * The resource/copy lock order, outermost first, is:
 *
 *      FSpoutD3DContext::D3D11ContextMutex      (D3D11 immediate context and the wrap caches)
 *   -> FSpoutSharedSender::ResourceMutex        (one entry's shared texture, handle and wraps)
 *   -> FSpoutD3DContext::SpoutMutex             (Spout's process-global sender-name list)
 *   -> FSpoutFrameSync::Mutex                   (this object)
 *   -> the Spout named access mutex             (cross-process, bounded wait)
 *
 * A path may enter at any level and skip levels; it may never take an outer lock while holding
 * an inner one. Both the send and the receive path take the named access mutex from inside the
 * D3D11 context mutex, never the other way round, so the two cannot invert on each other. An
 * attempt on a busy peer waits at most r.Spout.AccessWaitMs (default 2 ms, clamped to 0..10,
 * zero only tries), per attempt, while holding the locks above; Windows scheduling can stretch
 * the wait. The game thread's status query never takes FSpoutFrameSync::Mutex, so it cannot be
 * held up by that wait. Once access is acquired, the caller waits for GPU completion before
 * releasing it. This GPU wait can stall the render thread. Keyed resources use the same bounded
 * wait and key zero, matching the Spout protocol.
 * Sender installation additionally holds Session::Mutex before the registry/resource locks;
 * it never flushes RHI work or waits for the game thread while holding that mutex.
 *
 * FSpoutD3DContext::InitMutex is outside this order entirely: it guards only device creation and
 * teardown and is never held while any of the above is taken.
 */
#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Templates/UniquePtr.h"
#include "Templates/Function.h"
#include "SpoutFrameWait.h"
#include "SpoutNotificationState.h"
#include <atomic>

/**
 * The Spout synchronisation operations this plugin needs.
 *
 * The shared texture is passed as void* for the same reason ISpoutSenderNameBackend takes the
 * share handle as void*: this layer forwards an opaque resource identity to the SDK and has no
 * use for the D3D11 type, and keeping <d3d11.h> out of the header lets the protocol be driven
 * through this interface without a device.
 *
 * Names arrive as null-terminated UTF-8, which is what the SDK consumes and what
 * SpoutValidation produces.
 */
class ISpoutFrameSyncBackend
{
public:
	virtual ~ISpoutFrameSyncBackend() = default;

	/** Creates or opens this sender name's cross-process access mutex. */
	virtual bool CreateAccessMutex(const ANSICHAR* Utf8Name) = 0;

	/** Closes the access mutex. Must not be called while access is held. */
	virtual void CloseAccessMutex() = 0;

	/** Creates or opens this sender name's frame-count semaphore. */
	virtual void EnableFrameCount(const ANSICHAR* Utf8Name) = 0;

	/** Closes the frame-count semaphore. */
	virtual void CleanupFrameCount() = 0;

	/**
	 * Waits up to TimeoutMs for exclusive access to the shared texture; zero only tries.
	 *
	 * False means the wait timed out or failed and this process owns nothing, so the matching
	 * release must NOT be issued.
	 */
	virtual bool CheckTextureAccess(void* SharedTexture, uint32 TimeoutMs) = 0;

	/** Gives back access previously granted by CheckTextureAccess. */
	virtual bool AllowTextureAccess(void* SharedTexture) = 0;

	/** Sender: announces that one frame has been written into the shared texture. */
	virtual void SetNewFrame() = 0;

	/**
	 * Receiver: whether the peer has published a frame since the last call.
	 *
	 * True when the peer does not publish a frame count at all, which is the SDK's own
	 * behaviour and the only safe answer: without a count there is no way to tell a repeated
	 * frame from a new one, and copying is the option that cannot drop data.
	 */
	virtual bool GetNewFrame() = 0;

	/**
	 * The frame count the most recent GetNewFrame saw (the SDK's public GetSenderFrame), or -1 when
	 * the backend has none. Zero means the sender has not published a count.
	 *
	 * A notification hint and nothing more. It never identifies an image: another receiver's
	 * GetNewFrame lowers the count for an instant, so the value can be one below the true count
	 * whenever it is read, including while this process holds texture access.
	 */
	virtual int64 GetSenderFrame() { return -1; }

	/** Whether frame counting is switched on for this stream (the SDK's public IsFrameCountEnabled). */
	virtual bool IsFrameCountEnabled() { return false; }

	/**
	 * True when a closed backend may be handed to a later stream instead of being destroyed.
	 * Only the SDK backend says yes: constructing spoutFrameCount queries the display mode and
	 * timer resolution, which is comparatively expensive, and a stream restart constructs two of
	 * them. Every other backend keeps this default and is destroyed rather than pooled.
	 */
	virtual bool IsRecyclable() const { return false; }
};

/**
 * Builds the real SDK-backed implementation, or null when Spout is unavailable in this process.
 *
 * Prefers a recycled backend: a backend returned by RecycleSpoutSdkFrameSyncBackend holds no
 * mutex, semaphore or name state that Open does not reinitialise, so reuse is invisible to the
 * protocol and skips the expensive SDK construction on every stream restart.
 */
TUniquePtr<ISpoutFrameSyncBackend> MakeSpoutSdkFrameSyncBackend();

/**
 * Returns a closed backend to the process-wide pool, or destroys it when the pool is full or the
 * backend is not recyclable. Called by FSpoutFrameSync after Close; never with an open backend.
 */
void RecycleSpoutSdkFrameSyncBackend(TUniquePtr<ISpoutFrameSyncBackend> Backend);

/**
 * Destroys every pooled backend. Must run before Spout.dll is unloaded, because the SDK
 * destructor is in that DLL; FSpoutD3DContext::Shutdown calls it.
 */
void DrainSpoutSdkFrameSyncBackendPool();

/** Number of pooled backends. Diagnostics and tests. */
int32 NumPooledSpoutSdkFrameSyncBackends();

/** Upper bound on pooled backends; anything beyond it is destroyed on recycle. */
int32 MaxPooledSpoutSdkFrameSyncBackends();

/**
 * One stream's half of the Spout access and frame-count protocol.
 *
 * Owns the backend, so a stream that is torn down closes its mutex and semaphore with it. All
 * of the protocol's state lives here rather than being spread across the send and receive
 * paths:
 *
 *   - whether the access mutex and frame semaphore are open at all;
 *   - whether this object currently holds cross-process access (so a second acquire is refused
 *     instead of producing an unbalanced release);
 *   - whether a frame reported new by the peer has actually been consumed by a copy yet.
 *
 * That last one is the reason ConsumeNewFrame/MarkFrameConsumed are two calls rather than one.
 * GetNewFrame() decrements a semaphore: asking is destructive. If the copy that the answer was
 * for then does not happen -- because the access wait timed out, or a destination went away --
 * the frame would be lost with no way to notice. Holding the answer until a copy actually
 * lands makes a failed access attempt cost latency instead of a frame, so skipped frames stay
 * consistent.
 *
 * With no backend, or after a failed Open, access and frame publication are refused.
 */
class FSpoutFrameSync
{
public:
	explicit FSpoutFrameSync(TUniquePtr<ISpoutFrameSyncBackend> InBackend);
	~FSpoutFrameSync();

	FSpoutFrameSync(const FSpoutFrameSync&) = delete;
	FSpoutFrameSync& operator=(const FSpoutFrameSync&) = delete;

	/**
	 * Opens the access mutex and frame-count semaphore for one sender name.
	 *
	 * Idempotent: a second call for the same name does nothing, which is what lets the send and
	 * receive paths call it on the frame the stream becomes publishable without tracking whether
	 * they already have. A name change closes the previous pair first, so a stream can never hold
	 * one name's mutex while counting another name's frames.
	 *
	 * Returns whether the access mutex is open afterwards. Frame counting can be disabled
	 * machine-wide in SpoutSettings, so its absence is not a failure and is not reported as one.
	 */
	bool Open(const ANSICHAR* Utf8Name);

	/**
	 * Closes both.
	 *
	 * Idempotent: a second call does nothing. That matters because the ordered teardown closes
	 * explicitly and the destructor closes again, and handing the SDK a second close for a handle
	 * it has already released is not something to rely on being harmless.
	 */
	void Close();

	bool IsOpen() const;

	/** Sender: call after GPU copy completion, before releasing access. */
	void PublishFrame();

	/**
	 * Receiver: whether there is a frame worth copying.
	 *
	 * True when the peer has published since the last consumed frame, or when a previously
	 * reported frame has still not been copied. False means the shared texture holds the same
	 * frame that was already copied, so the whole receive -- transitions, RHI flush and GPU copy
	 * -- can be skipped.
	 *
	 * This call BEGINS a receive attempt: it is the attempt's one read. Call it once per attempt.
	 * With the experimental settings on it also records the count for the read history and counts
	 * a read for the predictor; a poll inside WaitForPublication is not a read.
	 */
	bool ConsumeNewFrame();

	/** Receiver: records that the pending frame was actually copied. Same as CompleteTransfer. */
	void MarkFrameConsumed();

	/**
	 * Receiver: the copy completed successfully. This is the only place where a pending frame stops
	 * being pending: the GPU path calls it after the copy's GPU completion while access is still
	 * held, the CPU path after access was released and the map and the upload succeeded. A refused
	 * access, a failed copy, a failed completion wait and a cancelled stream never reach it, so the
	 * frame stays pending and is tried again.
	 *
	 * Returns whether the copy counts as a delivery: true for every copy made for a pending frame or
	 * as a transfer without a notification (see BeginTransferWithoutNotification). Every successful
	 * copy is a delivery. A copy that repeats the image already delivered is not told apart, because
	 * that would mean trusting a frame count as an image identity.
	 */
	bool CompleteTransfer();

	/** Request the current image again after a native source generation changes. */
	void RequestCurrentFrame();

	/** True while a frame reported new by the peer has not yet been copied. Diagnostics and tests. */
	bool HasPendingFrame() const;

	// ---- Experimental (r.Spout.FrameWaitMs, r.Spout.FrameCountRecheck; both off by default). None
	// of the following changes anything unless one of the two settings is on. Only the SDK's public
	// calls are used: GetNewFrame, GetSenderFrame and IsFrameCountEnabled. ----

	/**
	 * Waits for the sender's next publication, for at most BoundSeconds, polling the frame
	 * notification every PollSeconds. Call only after ConsumeNewFrame returned false, and never from
	 * inside an FSpoutTextureAccessScope: the sender needs the access mutex to write the frame being
	 * waited for, so a wait attempted while this object holds access is refused
	 * (EDecision::AccessHeld) without a single poll.
	 *
	 * The wait is attempted only when the sender publishes a count and this stream's publication
	 * history predicts the next publication inside the bound (FPublishPredictor). A poll is the
	 * SDK's GetNewFrame; it ends the wait only when the count has risen above the count the read saw.
	 * The frame-sync lock is taken per poll, not across the wait, so Close() on another thread is not
	 * held up and is noticed at the next poll. ShouldAbort is asked before every poll, outside the
	 * lock; it is how a cancelled stream or an exiting engine ends the wait early.
	 *
	 * Returns true when a frame is pending afterwards. The publication the wait saw is latched by
	 * the poll that saw it; nothing else needs to be called to keep it.
	 */
	bool WaitForPublication(double BoundSeconds, double PollSeconds, TFunctionRef<bool()> ShouldAbort, FSpoutFrameWaitReport& OutReport);

	/**
	 * Whether this attempt transfers although no frame is pending, and if so marks it. True only
	 * when the read history is kept, no wait was made in this attempt (bWaitMade false), and the
	 * count at this attempt's read differs from the count at the previous attempt's read. The
	 * previous transfer may have copied that publication already; the one after it could be
	 * overwritten before the next read, so the copy is made now, as it is with the settings off.
	 */
	bool BeginTransferWithoutNotification(bool bWaitMade);

	/**
	 * Reads the sender's notification while this object holds texture access. A sender publishes
	 * before it releases access, so a publication made before this point is seen here instead of
	 * being reported new again at the next read, after its image was already copied. A new
	 * notification makes the frame pending; nothing is cleared, because reading under access does
	 * not prove that the copy will complete. Does nothing when access is not held.
	 */
	void ObserveNotificationUnderAccess();

	/** The count the most recent read, poll or read under access saw, or -1. Measurement logs and tests. */
	int64 GetLastObservedSenderFrame();

	/** Counters of the notification state. Takes the lock. Measurement logs and tests. */
	struct FNotificationStats
	{
		int64 Notifications = 0;
		int64 Copies = 0;
		int64 CopiesWithoutNotification = 0;
		int64 CopiedAheadOfRead = 0;
		int64 Polls = 0;
		int64 Transients = 0;
		int64 CurrentFrameRequests = 0;
		int64 Opens = 0;
		int64 ReadCount = -1;
		int64 PreviousReadCount = -1;
		int64 UnderAccessCount = -1;
		/** What the read under access answered in the current attempt. */
		bool bNewUnderAccess = false;
	};
	FNotificationStats GetNotificationStats() const;

	/** Counters of the new-frame wait. Lock-free. Diagnostics and tests. */
	void GetFrameWaitStats(int64& OutAttempts, int64& OutPublished, int64& OutTimedOut) const;

	/** Predictor counters (suspensions, inconsistent observations, glitches, resets). Takes the lock. Tests. */
	void GetPredictorStats(int64& OutSuspensions, int64& OutInconsistent, int64& OutGlitches, int64& OutResets) const;

	/**
	 * The predictor's once-per-attempt state: reads begun (one per ConsumeNewFrame while the wait is
	 * on), the read-interval estimate (seconds, 0 until two timed reads), reads left in the current
	 * suspension, reads until the next probe time, and probe waits made. Takes the lock. Tests.
	 */
	void GetPredictorReadState(int64& OutReads, double& OutReadIntervalSeconds, int64& OutSuspendedReadsLeft, int64& OutReadsUntilProbe, int64& OutProbes) const;

#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * Tests only: the clock this object reads instead of QPC seconds; an empty function restores QPC.
	 * The wait's spin reads it on every iteration, so a test clock must move forward on every call
	 * or the wait never ends.
	 */
	void SetClockForTests(TFunction<double()> InClock);

	/**
	 * Tests only: gives the predictor a regular publication history at PeriodSeconds whose next
	 * publication falls NextInSeconds from now. CurrentCount must be above 50. Lets a test reach the
	 * polling loop without replaying seconds of real time.
	 */
	void PrimePredictorForTests(double PeriodSeconds, double NextInSeconds, int64 CurrentCount);
#endif

	/** True while this object holds cross-process access. Diagnostics and tests. */
	bool IsAccessHeld() const;

	/** Successful acquires, matching releases, timed-out acquires, and published frames. Tests. */
	int32 GetAcquireCount() const;
	int32 GetReleaseCount() const;
	int32 GetFailedAcquireCount() const;
	int32 GetPublishedFrameCount() const;
	/** Lock-free; the two values are read separately, not as one atomic snapshot. */
	void GetStatus(int64& OutFrames, int64& OutAccessMisses) const;

private:
	friend class FSpoutTextureAccessScope;

	/** Only FSpoutTextureAccessScope may take and give back access, so the two cannot be separated. */
	bool AcquireTextureAccess(void* SharedTexture);
	void ReleaseTextureAccess(void* SharedTexture);

	/** The clock of the predictor and the wait: QPC seconds unless a test set one. */
	double Now() const;
	/** The backend's count as the notification state takes it: -1 unless counting is on and the count is above zero. Mutex held. */
	int64 ReadBackendCountLocked() const;
	void FillPredictorTotalsLocked(FSpoutFrameWaitReport& OutReport) const;

	mutable FCriticalSection Mutex;

	TUniquePtr<ISpoutFrameSyncBackend> Backend;

	/** Null-terminated copy of the name Open() succeeded with. Empty when nothing is open. */
	TArray<ANSICHAR> OpenUtf8Name;

	bool bOpen = false;
	bool bAccessHeld = false;
	/** Whether a frame is pending, and with the experimental settings on the read history. */
	FSpoutNotificationState Notify;

	int64 AcquireCount = 0;
	int64 ReleaseCount = 0;
	// Written under Mutex; GetStatus reads them without it, because the render thread can hold
	// Mutex for a whole bounded access wait and the game thread must not queue behind that.
	std::atomic<int64> FailedAcquireCount{0};
	std::atomic<int64> PublishedFrameCount{0};
	std::atomic<int64> ConsumedFrameCount{0};

	// Experimental new-frame wait, guarded by Mutex unless atomic.
	SpoutFrameWait::FPublishPredictor Predictor;
	/** True while the predictor is being fed (the wait is enabled); it is reset when the wait is turned off. */
	bool bPredictorLive = false;
	/** Opens over this object's lifetime (the already-open fast path does not count). */
	int64 OpenCount = 0;
	std::atomic<int64> FrameWaitAttempts{0};
	std::atomic<int64> FrameWaitPublished{0};
	std::atomic<int64> FrameWaitTimedOut{0};
#if WITH_DEV_AUTOMATION_TESTS
	/** Set only by tests, before the object is used; empty means QPC. */
	TFunction<double()> ClockForTests;
#endif
};

/** What the check stage of one receive attempt did. */
struct FSpoutReceiveCheck
{
	/** What the attempt's read (ConsumeNewFrame) answered. */
	bool bNewFrameAtRead = false;
	/** The read found nothing new, so the wait was asked for; it may have declined at once (WaitReport.Decision). */
	bool bWaitAsked = false;
	/** A frame became pending during the wait. */
	bool bPendingAfterWait = false;
	/** Nothing is pending and no wait was made, but the sender published since the previous read: the transfer is made anyway. */
	bool bTransferWithoutNotification = false;
	FSpoutFrameWaitReport WaitReport;
	double BudgetBeforeSeconds = 0.0;
	double BudgetAfterSeconds = 0.0;
};

namespace SpoutFrameWait
{
	/**
	 * The check stage of one receive attempt, the same for the GPU and the CPU receive path. One
	 * read, which begins the attempt (ConsumeNewFrame). When it found nothing new, the wait (TryWait:
	 * the receiver's budgeted new-frame wait, which declines at once when r.Spout.FrameWaitMs is 0).
	 * When there is still nothing pending and no wait was made, the transfer without a notification
	 * (FSpoutFrameSync::BeginTransferWithoutNotification). AfterRead runs between the read and the
	 * wait. Returns whether a transfer is to be attempted.
	 *
	 * With both experimental settings off this is ConsumeNewFrame and nothing else: TryWait declines
	 * and the transfer without a notification needs the read history, which is not kept.
	 */
	bool CheckForNewFrame(FSpoutFrameSync& Sync, TFunctionRef<void(bool bNewFrameAtRead)> AfterRead,
		TFunctionRef<bool(FSpoutFrameWaitReport& OutReport, double& OutBudgetBeforeSeconds, double& OutBudgetAfterSeconds)> TryWait,
		FSpoutReceiveCheck& Out);
}

/**
 * Scoped cross-process access to one shared texture.
 *
 * The only way to take the Spout access mutex. Construction attempts the wait; destruction gives
 * access back if and only if the wait succeeded. A caller that finds IsAcquired() false must skip
 * the copy: it owns nothing, and releasing anyway is the SDK's own bug.
 *
 * Deliberately not copyable or movable -- an access token that can be duplicated is an access
 * token that can be released twice.
 */
class FSpoutTextureAccessScope
{
public:
	FSpoutTextureAccessScope(FSpoutFrameSync& InFrameSync, void* InSharedTexture)
		: FrameSync(InFrameSync)
		, SharedTexture(InSharedTexture)
		, bAcquired(InFrameSync.AcquireTextureAccess(InSharedTexture))
	{
	}

	~FSpoutTextureAccessScope()
	{
		if (bAcquired)
		{
			FrameSync.ReleaseTextureAccess(SharedTexture);
		}
	}

	FSpoutTextureAccessScope(const FSpoutTextureAccessScope&) = delete;
	FSpoutTextureAccessScope& operator=(const FSpoutTextureAccessScope&) = delete;

	/** Whether this process owns the shared texture for the lifetime of the scope. */
	bool IsAcquired() const { return bAcquired; }

private:
	FSpoutFrameSync& FrameSync;
	void* SharedTexture;
	bool bAcquired;
};
