// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the Spout access-mutex and frame-count protocol.
 *
 * The state machine here is deliberately free of D3D, Spout and Unreal render threads: everything
 * the SDK does arrives through ISpoutFrameSyncBackend, so the whole of the protocol's behaviour
 * -- successful and failed locks, balanced release, repeated lock/unlock, restart, resize,
 * teardown while a peer holds the mutex -- can be driven through that interface alone.
 */
#include "SpoutFrameSync.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "HAL/PlatformTime.h"
#include <immintrin.h> // _mm_pause between frame-notification polls

#include "SpoutLimits.h"
#include "SpoutModule.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include <d3d11.h>
#include "SpoutDX/SpoutFrameCount.h"
#include "SpoutSdkSafety.h"
#include "Windows/HideWindowsPlatformTypes.h"
#ifdef GetNextSibling
#undef GetNextSibling
#endif
#endif

namespace
{
#if PLATFORM_WINDOWS
	/**
	 * Adapts the bundled spoutFrameCount to ISpoutFrameSyncBackend.
	 *
	 * One instance per stream, not one per process: spoutFrameCount keeps the access-mutex handle,
	 * the semaphore handle and the receiver's last-seen frame count as member state, all of which
	 * are per sender NAME. Sharing one instance between two streams would make them overwrite each
	 * other's notion of which frame they last saw.
	 */
	class FSpoutSdkFrameSyncBackend final : public ISpoutFrameSyncBackend
	{
	public:
		virtual bool CreateAccessMutex(const ANSICHAR* Utf8Name) override
		{
			return FrameCount.CreateAccessMutex(Utf8Name);
		}

		virtual void CloseAccessMutex() override
		{
			FrameCount.CloseAccessMutex();
		}

		virtual void EnableFrameCount(const ANSICHAR* Utf8Name) override
		{
			FrameCount.EnableFrameCount(Utf8Name);
		}

		virtual void CleanupFrameCount() override
		{
			FrameCount.CleanupFrameCount();
		}

		virtual bool CheckTextureAccess(void* SharedTexture, uint32 TimeoutMs) override
		{
			return FrameCount.CheckTextureAccessSafe(static_cast<ID3D11Texture2D*>(SharedTexture), TimeoutMs);
		}

		virtual bool AllowTextureAccess(void* SharedTexture) override
		{
			return FrameCount.AllowTextureAccess(static_cast<ID3D11Texture2D*>(SharedTexture));
		}

		virtual void SetNewFrame() override
		{
			FrameCount.SetNewFrame();
		}

		virtual bool GetNewFrame() override
		{
			return FrameCount.GetNewFrame();
		}

		virtual int64 GetSenderFrame() override
		{
			return static_cast<int64>(FrameCount.GetSenderFrame());
		}

		virtual bool IsFrameCountEnabled() override
		{
			return FrameCount.IsFrameCountEnabled();
		}

		virtual bool IsRecyclable() const override { return true; }

	private:
		FSpoutSafeFrameCount FrameCount;
	};

	// Closed SDK backends waiting for the next stream. Bounded so a burst of distinct names cannot
	// grow it without limit; each pooled object is a few hundred bytes and no kernel handles.
	// Streams are created and destroyed on both the game and render threads, hence the lock.
	FCriticalSection GFrameSyncBackendPoolMutex;
	TArray<TUniquePtr<ISpoutFrameSyncBackend>> GFrameSyncBackendPool;
	constexpr int32 GFrameSyncBackendPoolCapacity = 64;

	static_assert(static_cast<DWORD>(SpoutLimits::MaxAccessWaitMs) == FSpoutSafeFrameCount::MaxAccessWaitMs,
		"The setting's clamp and the SDK adapter's clamp must agree.");
#endif
}

TUniquePtr<ISpoutFrameSyncBackend> MakeSpoutSdkFrameSyncBackend()
{
#if PLATFORM_WINDOWS
	// Constructing spoutFrameCount is the first touch of a Spout symbol on this path, which is what
	// triggers the delay-load bind of Spout.dll. Binding a DLL that never loaded terminates the
	// process with 0xC06D007E. A null backend refuses access as well as Open.
	if (!FSpoutModule::IsSpoutDllLoaded())
	{
		return nullptr;
	}

	{
		FScopeLock Lock(&GFrameSyncBackendPoolMutex);
		if (GFrameSyncBackendPool.Num() > 0)
		{
			TUniquePtr<ISpoutFrameSyncBackend> Recycled = MoveTemp(GFrameSyncBackendPool.Last());
			GFrameSyncBackendPool.RemoveAt(GFrameSyncBackendPool.Num() - 1);
			return Recycled;
		}
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_ConstructSdkFrameCount);
	return MakeUnique<FSpoutSdkFrameSyncBackend>();
#else
	return nullptr;
#endif
}

void RecycleSpoutSdkFrameSyncBackend(TUniquePtr<ISpoutFrameSyncBackend> Backend)
{
#if PLATFORM_WINDOWS
	if (!Backend || !Backend->IsRecyclable())
	{
		return;
	}
	FScopeLock Lock(&GFrameSyncBackendPoolMutex);
	if (GFrameSyncBackendPool.Num() < GFrameSyncBackendPoolCapacity)
	{
		GFrameSyncBackendPool.Add(MoveTemp(Backend));
	}
#endif
	// Anything not pooled is destroyed here, as the unique pointer goes out of scope.
}

void DrainSpoutSdkFrameSyncBackendPool()
{
#if PLATFORM_WINDOWS
	TArray<TUniquePtr<ISpoutFrameSyncBackend>> Doomed;
	{
		FScopeLock Lock(&GFrameSyncBackendPoolMutex);
		Doomed = MoveTemp(GFrameSyncBackendPool);
		GFrameSyncBackendPool.Reset();
	}
	// Destroyed outside the lock: the SDK destructor is not something to hold a mutex across.
	Doomed.Reset();
#endif
}

int32 NumPooledSpoutSdkFrameSyncBackends()
{
#if PLATFORM_WINDOWS
	FScopeLock Lock(&GFrameSyncBackendPoolMutex);
	return GFrameSyncBackendPool.Num();
#else
	return 0;
#endif
}

int32 MaxPooledSpoutSdkFrameSyncBackends()
{
#if PLATFORM_WINDOWS
	return GFrameSyncBackendPoolCapacity;
#else
	return 0;
#endif
}

FSpoutFrameSync::FSpoutFrameSync(TUniquePtr<ISpoutFrameSyncBackend> InBackend)
	: Backend(MoveTemp(InBackend))
{
}

FSpoutFrameSync::~FSpoutFrameSync()
{
	// Close first: a pooled backend must hold no mutex, semaphore or pending frame. Close also
	// resets the plugin-side state, and Open reinitialises the SDK side for the next name.
	Close();
	RecycleSpoutSdkFrameSyncBackend(MoveTemp(Backend));
}

bool FSpoutFrameSync::Open(const ANSICHAR* Utf8Name)
{
	if (Utf8Name == nullptr || Utf8Name[0] == '\0')
	{
		return false;
	}

	FScopeLock Lock(&Mutex);

	if (!Backend)
	{
		return false;
	}

	if (bOpen)
	{
		// Already open for this name: nothing to do. This is the per-frame path, so it must stay a
		// comparison and nothing more.
		if (OpenUtf8Name.Num() > 1 && FCStringAnsi::Strcmp(OpenUtf8Name.GetData(), Utf8Name) == 0)
		{
			return true;
		}

		// A different name. Defensive -- a stream's name is fixed in practice -- but holding one
		// name's access mutex while counting another name's frames would be a silent protocol
		// error, so the previous pair is closed first.
		if (bAccessHeld)
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("Spout frame sync: refusing to reopen under a new name while texture access is held."));
			return false;
		}

		Backend->CloseAccessMutex();
		Backend->CleanupFrameCount();
		bOpen = false;
		Notify.Reset();
		if (bPredictorLive) Predictor.Reset();
		bPredictorLive = false;
		OpenUtf8Name.Reset();
	}

	const bool bMutexCreated = Backend->CreateAccessMutex(Utf8Name);
	if (!bMutexCreated)
	{
		Backend->CloseAccessMutex();
		return false;
	}

	// Frame counting is a machine-wide setting a user can turn off in SpoutSettings, so its absence
	// is not an error. When it is off the peer publishes no count, GetNewFrame() answers "new" for
	// every call, and the receiver copies unconditionally, which is the only correct answer when
	// there is nothing to compare.
	Backend->EnableFrameCount(Utf8Name);

	const int32 NameLength = FCStringAnsi::Strlen(Utf8Name);
	OpenUtf8Name.Reset(NameLength + 1);
	OpenUtf8Name.Append(Utf8Name, NameLength + 1);

	bOpen = true;
	// A newly opened name starts with nothing pending and no read history. Nothing can be pending
	// here already: a frame only becomes pending on an open stream.
	Notify.Reset();
	++OpenCount;
	return bMutexCreated;
}

void FSpoutFrameSync::Close()
{
	FScopeLock Lock(&Mutex);

	if (!bOpen)
	{
		// Nothing to close. This runs more often than it looks: the destructor closes, and the
		// ordered teardown closes explicitly before that, so the second call must not hand the SDK
		// a second CloseAccessMutex/CleanupFrameCount pair for a handle it has already given up.
		bAccessHeld = false;
		Notify.Reset();
		OpenUtf8Name.Reset();
		return;
	}

	if (!Backend)
	{
		bOpen = false;
		Notify.Reset();
		OpenUtf8Name.Reset();
		return;
	}

	if (bAccessHeld)
	{
		// Closing the mutex handle underneath a live acquire would abandon it, which every other
		// process on that name would then see as WAIT_ABANDONED. Teardown reaches this point only
		// after the render-command flush, so an access scope can no longer be live; if one somehow
		// is, saying so is better than corrupting the name for every peer.
		UE_LOG(LogSpoutPlugin, Warning,
			TEXT("Spout frame sync: closing while texture access is still held; the access mutex will be abandoned."));
	}

	Backend->CloseAccessMutex();
	Backend->CleanupFrameCount();

	bOpen = false;
	bAccessHeld = false;
	// A pending frame of a closed stream is dropped, never turned into a delivery, and nothing
	// learnt about one name carries over to the next.
	Notify.Reset();
	if (bPredictorLive) Predictor.Reset();
	bPredictorLive = false;
	OpenUtf8Name.Reset();
}

bool FSpoutFrameSync::IsOpen() const
{
	// Read without Mutex. An Async Receive worker holds Mutex through its access wait (up to r.Spout.AccessWaitMs), and
	// taking it here would make the render thread's receive call wait for that worker.
	// The answer is advisory at every call site, locked or not: a caller that finds the stream closed opens it (Open
	// re-checks under Mutex), and everything that relies on an open stream re-checks bOpen under Mutex. bOpen is written
	// under Mutex only.
	return bOpen.load();
}

void FSpoutFrameSync::PublishFrame()
{
	FScopeLock Lock(&Mutex);
	if (!Backend || !bOpen) return;

	++PublishedFrameCount;

	if (Backend)
	{
		Backend->SetNewFrame();
	}
}

bool FSpoutFrameSync::ConsumeNewFrame()
{
	FScopeLock Lock(&Mutex);

	if (!Backend || !bOpen)
	{
		return false;
	}

	// Asking is destructive -- GetNewFrame() decrements the frame semaphore -- so the answer is
	// latched rather than re-asked. Once a frame is known to be pending it stays pending until a
	// copy actually lands, so a timed-out access wait costs one frame of latency instead of losing
	// the frame entirely.
	const bool bSdkNew = Backend->GetNewFrame();

	// Experimental. With r.Spout.FrameWaitMs and r.Spout.FrameCountRecheck at their defaults (both
	// off) the rest of this function is two setting reads and the latch above.
	const bool bWaitEnabled = SpoutLimits::FrameWaitSeconds() > 0.0;
	const bool bTrack = bWaitEnabled || SpoutLimits::FrameCountRecheck();
	const bool bPending = Notify.BeginAttempt(bSdkNew, bTrack ? ReadBackendCountLocked() : FSpoutNotificationState::NoCount, bTrack);
	if (bWaitEnabled)
	{
		// Only this call is a read for the predictor: suspensions and probe times are counted in
		// receive attempts, and the read interval is estimated from these times.
		bPredictorLive = true;
		const double ReadSeconds = Now();
		Predictor.BeginRead(ReadSeconds);
		Predictor.ObserveCount(ReadSeconds, Notify.GetReadCount());
	}
	else if (bPredictorLive)
	{
		// The wait was turned off: forget the history, so turning it on again starts clean.
		Predictor.Reset();
		bPredictorLive = false;
	}

	return bPending;
}

void FSpoutFrameSync::RequestCurrentFrame()
{
	FScopeLock Lock(&Mutex);
	if (!bOpen) return;
	Notify.RequestCurrentFrame();
	// A new native source generation invalidates the publication history.
	if (bPredictorLive) Predictor.Reset();
}

void FSpoutFrameSync::MarkFrameConsumed()
{
	CompleteTransfer();
}

bool FSpoutFrameSync::CompleteTransfer()
{
	FScopeLock Lock(&Mutex);
	const bool bDelivered = Notify.CompleteTransfer();
	if (bDelivered) ++ConsumedFrameCount;
	return bDelivered;
}

bool FSpoutFrameSync::HasPendingFrame() const
{
	FScopeLock Lock(&Mutex);
	return Notify.IsOwed();
}

bool FSpoutFrameSync::IsAccessHeld() const
{
	FScopeLock Lock(&Mutex);
	return bAccessHeld;
}

int32 FSpoutFrameSync::GetAcquireCount() const
{
	FScopeLock Lock(&Mutex);
	return static_cast<int32>(FMath::Min<int64>(AcquireCount, MAX_int32));
}

int32 FSpoutFrameSync::GetReleaseCount() const
{
	FScopeLock Lock(&Mutex);
	return static_cast<int32>(FMath::Min<int64>(ReleaseCount, MAX_int32));
}

int32 FSpoutFrameSync::GetFailedAcquireCount() const
{
	return static_cast<int32>(FMath::Min<int64>(FailedAcquireCount.load(), MAX_int32));
}

int32 FSpoutFrameSync::GetPublishedFrameCount() const
{
	return static_cast<int32>(FMath::Min<int64>(PublishedFrameCount.load(), MAX_int32));
}

void FSpoutFrameSync::GetStatus(int64& OutFrames, int64& OutAccessMisses) const
{
	// No lock: the render thread holds Mutex for the whole of a bounded access wait.
	OutFrames = PublishedFrameCount.load() + ConsumedFrameCount.load();
	OutAccessMisses = FailedAcquireCount.load();
}

bool FSpoutFrameSync::AcquireTextureAccess(void* SharedTexture)
{
	FScopeLock Lock(&Mutex);
	++AcquireAttemptCount;

	if (bAccessHeld)
	{
		// Nested acquire. The Windows mutex would grant it (it is recursive for its owner) and then
		// need two releases to actually let go, so a single scope exit would leave it locked
		// forever. Refusing is the only outcome that cannot deadlock a peer.
		UE_LOG(LogSpoutPlugin, Warning,
			TEXT("Spout frame sync: texture access requested while already held; the nested request was refused."));
		++FailedAcquireCount;
		return false;
	}

	if (!Backend || !bOpen)
	{
		++FailedAcquireCount;
		return false;
	}

	// Resolved per attempt, so a changed setting applies from the next transfer. The wait is
	// bounded by the clamp and ends as soon as the peer releases; Close cannot run meanwhile,
	// because it takes Mutex and teardown flushes rendering commands first anyway.
	bool bGranted = false;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AccessWait);
		bGranted = Backend->CheckTextureAccess(SharedTexture, SpoutLimits::AccessWaitMs());
	}
	if (!bGranted)
	{
		// The wait timed out or failed: this process owns nothing. Not calling AllowTextureAccess
		// here is the point -- the SDK's own AllowAccess() releases whenever a handle exists, so
		// pairing it with a failed check hands away a mutex this process never took.
		++FailedAcquireCount;
		return false;
	}

	bAccessHeld = true;
	++AcquireCount;
	return true;
}

void FSpoutFrameSync::ReleaseTextureAccess(void* SharedTexture)
{
	FScopeLock Lock(&Mutex);

	if (!bAccessHeld)
	{
		// Only reachable if something released outside FSpoutTextureAccessScope.
		UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout frame sync: texture access released without being held."));
		return;
	}

	bAccessHeld = false;
	++ReleaseCount;

	if (Backend)
	{
		Backend->AllowTextureAccess(SharedTexture);
	}
}

// ---------------------------------------------------------------------------------------------
// Experimental: the new-frame wait and the read under access (r.Spout.FrameWaitMs and
// r.Spout.FrameCountRecheck, both off by default). Only the SDK's public calls are used. The frame
// count is a notification hint here and never an image identity; see SpoutNotificationState.h.
// ---------------------------------------------------------------------------------------------

int64 FSpoutFrameSync::ReadBackendCountLocked() const
{
	if (!Backend || !Backend->IsFrameCountEnabled()) return FSpoutNotificationState::NoCount;
	const int64 Count = Backend->GetSenderFrame();
	return Count > 0 ? Count : FSpoutNotificationState::NoCount;
}

void FSpoutFrameSync::FillPredictorTotalsLocked(FSpoutFrameWaitReport& OutReport) const
{
	OutReport.PredictorSuspensions = Predictor.GetSuspensions();
	OutReport.PredictorWastedWaits = Predictor.GetWastedWaits();
	OutReport.PredictorJitterSeconds = Predictor.GetJitterSeconds();
}

bool FSpoutFrameSync::WaitForPublication(double BoundSeconds, double PollSeconds, TFunctionRef<bool()> ShouldAbort, FSpoutFrameWaitReport& OutReport)
{
	OutReport = FSpoutFrameWaitReport();
	OutReport.BoundSeconds = BoundSeconds;

	int64 ReadCount = FSpoutNotificationState::NoCount;
	{
		FScopeLock Lock(&Mutex);
		if (!Backend || !bOpen)
		{
			OutReport.Decision = SpoutFrameWait::EDecision::NoCount;
			OutReport.Outcome = SpoutFrameWait::EOutcome::Closed;
			return false;
		}
		FillPredictorTotalsLocked(OutReport);
		if (bAccessHeld)
		{
			// The sender needs the access mutex to write the frame this call would wait for.
			// Waiting here could only time out, and would hold the sender up while it did.
			OutReport.Decision = SpoutFrameWait::EDecision::AccessHeld;
			return false;
		}
		if (Notify.IsOwed())
		{
			OutReport.Decision = SpoutFrameWait::EDecision::AlreadyNew;
			return true;
		}
		if (!Notify.IsCountKnown())
		{
			// No count: there is no publication to wait for, and every read is reported new anyway.
			OutReport.Decision = SpoutFrameWait::EDecision::NoCount;
			return false;
		}
		ReadCount = Notify.GetReadCount();
		OutReport.CountBefore = ReadCount;
		OutReport.CountAfter = ReadCount;
		// The read that began this attempt saw the count unchanged a moment ago; the rule starts from there.
		const SpoutFrameWait::FPrediction Prediction = Predictor.Predict(Now(), BoundSeconds);
		OutReport.Decision = Prediction.Decision;
		OutReport.PeriodSeconds = Prediction.PeriodSeconds;
		OutReport.PredictedEarliestSeconds = Prediction.EarliestSeconds;
		OutReport.PredictedLatestSeconds = Prediction.LatestSeconds;
		OutReport.bProbe = Prediction.bProbe;
		if (Prediction.Decision != SpoutFrameWait::EDecision::Wait || BoundSeconds <= 0.0)
		{
			return false;
		}
	}

	// The polling loop. The lock is taken for each poll and dropped in between.
	++FrameWaitAttempts;
	bool bPending = false;
	OutReport.WaitBeginCycles = FPlatformTime::Cycles64();
	const double Begin = Now();
	const double Deadline = Begin + BoundSeconds;
	const double Interval = FMath::Max(PollSeconds, 0.00002);
	double NextPoll = Begin + Interval;
	for (;;)
	{
		double T = Now();
		while (T < NextPoll && T < Deadline)
		{
			// A pause, not a yield. SwitchToThread gives the rest of this thread's time slice to
			// another thread that is ready on the same processor, and when this thread runs again
			// is then up to the scheduler, not to the bound. The cost of pausing instead is that
			// the render thread keeps its core busy for the length of the wait.
			_mm_pause();
			T = Now();
		}
		if (ShouldAbort())
		{
			OutReport.Outcome = SpoutFrameWait::EOutcome::Aborted;
			break;
		}
		FScopeLock Lock(&Mutex);
		if (!Backend || !bOpen)
		{
			OutReport.Outcome = SpoutFrameWait::EOutcome::Closed;
			break;
		}
		const bool bSdkNew = Backend->GetNewFrame();
		const int64 Count = ReadBackendCountLocked();
		T = Now();
		++OutReport.Polls;
		OutReport.CountAfter = Count;
		const FSpoutNotificationState::EPoll Poll = Notify.ObservePoll(bSdkNew, Count);
		Predictor.ObserveCount(T, Count);
		if (Poll == FSpoutNotificationState::EPoll::Published)
		{
			OutReport.Outcome = SpoutFrameWait::EOutcome::Published;
			Predictor.ObserveWaitSucceeded();
			++FrameWaitPublished;
			bPending = true;
			break;
		}
		if (Poll == FSpoutNotificationState::EPoll::CountLost)
		{
			// The count went away under the wait. The frame is pending, as on every read without a count.
			OutReport.Outcome = SpoutFrameWait::EOutcome::CountLost;
			bPending = true;
			break;
		}
		if (T >= Deadline)
		{
			OutReport.Outcome = SpoutFrameWait::EOutcome::TimedOut;
			Predictor.ObserveWaitTimedOut();
			++FrameWaitTimedOut;
			break;
		}
		NextPoll = T + Interval;
	}
	OutReport.WaitEndCycles = FPlatformTime::Cycles64();
	{
		// After the end time, so the budget is not charged for it.
		FScopeLock Lock(&Mutex);
		FillPredictorTotalsLocked(OutReport);
	}
	return bPending;
}

bool FSpoutFrameSync::BeginTransferWithoutNotification(bool bWaitMade)
{
	FScopeLock Lock(&Mutex);
	if (!Backend || !bOpen || !Notify.ShouldTransferWithoutNotification(bWaitMade))
	{
		return false;
	}
	Notify.BeginFallbackTransfer();
	return true;
}

void FSpoutFrameSync::ObserveNotificationUnderAccess()
{
	FScopeLock Lock(&Mutex);
	if (!Backend || !bOpen || !bAccessHeld)
	{
		return;
	}
	const bool bSdkNew = Backend->GetNewFrame();
	const int64 Count = ReadBackendCountLocked();
	Notify.ObserveUnderAccess(bSdkNew, Count);
	// Observed now, at the time it was read: after the copy's completion wait "the count was still
	// N" could be false.
	if (bPredictorLive) Predictor.ObserveCount(Now(), Count);
}

int64 FSpoutFrameSync::GetLastObservedSenderFrame()
{
	FScopeLock Lock(&Mutex);
	return Backend ? Backend->GetSenderFrame() : -1;
}

bool FSpoutFrameSync::PollNotification(bool& bOutCountUsable)
{
	FScopeLock Lock(&Mutex);
	bOutCountUsable = false;
	if (!Backend || !bOpen)
	{
		return false;
	}
	const bool bSdkNew = Backend->GetNewFrame();
	bOutCountUsable = ReadBackendCountLocked() != FSpoutNotificationState::NoCount;
	return bSdkNew;
}

FSpoutFrameSync::FNotificationStats FSpoutFrameSync::GetNotificationStats() const
{
	FScopeLock Lock(&Mutex);
	FNotificationStats Stats;
	Stats.Notifications = Notify.GetNotifications();
	Stats.Copies = Notify.GetCopies();
	Stats.CopiesWithoutNotification = Notify.GetCopiesWithoutNotification();
	Stats.CopiedAheadOfRead = Notify.GetCopiedAheadOfRead();
	Stats.Polls = Notify.GetPolls();
	Stats.Transients = Notify.GetTransients();
	Stats.CurrentFrameRequests = Notify.GetCurrentFrameRequests();
	Stats.Opens = OpenCount;
	Stats.ReadCount = Notify.GetReadCount();
	Stats.PreviousReadCount = Notify.GetPreviousReadCount();
	Stats.UnderAccessCount = Notify.GetUnderAccessCount();
	Stats.bNewUnderAccess = Notify.WasNewUnderAccess();
	return Stats;
}

void FSpoutFrameSync::GetFrameWaitStats(int64& OutAttempts, int64& OutPublished, int64& OutTimedOut) const
{
	OutAttempts = FrameWaitAttempts.load();
	OutPublished = FrameWaitPublished.load();
	OutTimedOut = FrameWaitTimedOut.load();
}

void FSpoutFrameSync::GetPredictorStats(int64& OutSuspensions, int64& OutInconsistent, int64& OutGlitches, int64& OutResets) const
{
	FScopeLock Lock(&Mutex);
	OutSuspensions = Predictor.GetSuspensions();
	OutInconsistent = Predictor.GetInconsistentObservations();
	OutGlitches = Predictor.GetGlitches();
	OutResets = Predictor.GetResets();
}

void FSpoutFrameSync::GetPredictorReadState(int64& OutReads, double& OutReadIntervalSeconds, int64& OutSuspendedReadsLeft, int64& OutReadsUntilProbe, int64& OutProbes) const
{
	FScopeLock Lock(&Mutex);
	OutReads = Predictor.GetReads();
	OutReadIntervalSeconds = Predictor.GetReadIntervalSeconds();
	OutSuspendedReadsLeft = Predictor.GetSuspendedReadsLeft();
	OutReadsUntilProbe = Predictor.GetReadsUntilProbe();
	OutProbes = Predictor.GetProbes();
}

double FSpoutFrameSync::Now() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ClockForTests) return ClockForTests();
#endif
	return SpoutFrameWait::NowSeconds();
}

namespace SpoutFrameWait
{
	bool CheckForNewFrame(FSpoutFrameSync& Sync, TFunctionRef<void(bool)> AfterRead,
		TFunctionRef<bool(FSpoutFrameWaitReport&, double&, double&)> TryWait, FSpoutReceiveCheck& Out)
	{
		Out = FSpoutReceiveCheck();
		Out.bNewFrameAtRead = Sync.ConsumeNewFrame();
		AfterRead(Out.bNewFrameAtRead);
		if (Out.bNewFrameAtRead)
		{
			return true;
		}
		Out.bWaitAsked = true;
		if (TryWait(Out.WaitReport, Out.BudgetBeforeSeconds, Out.BudgetAfterSeconds))
		{
			// The poll that saw the publication made the frame pending; nothing else to do here.
			Out.bPendingAfterWait = true;
			return true;
		}
		// Nothing pending. A wait that was made (and timed out, was aborted or found the stream
		// closed) decides alone. Without one, the sender having published since the previous read is
		// enough to copy, as it is with the settings off; see BeginTransferWithoutNotification.
		if (Sync.BeginTransferWithoutNotification(Out.WaitReport.WasWaitMade()))
		{
			Out.bTransferWithoutNotification = true;
			return true;
		}
		return false;
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void FSpoutFrameSync::SetClockForTests(TFunction<double()> InClock)
{
	FScopeLock Lock(&Mutex);
	ClockForTests = MoveTemp(InClock);
}

void FSpoutFrameSync::PrimePredictorForTests(double PeriodSeconds, double NextInSeconds, int64 CurrentCount)
{
	FScopeLock Lock(&Mutex);
	Predictor.Reset();
	bPredictorLive = true;
	const int32 Publications = SpoutFrameWait::FTuning::MinPublications + 10;
	const double Last = Now() + NextInSeconds - PeriodSeconds;
	for (int32 K = Publications; K >= 0; --K)
	{
		const double T = Last - double(K) * PeriodSeconds;
		// Seen at the previous count 50 us earlier, then at the new count: a 50 us observation.
		Predictor.ObserveCount(T - 0.00005, CurrentCount - K - 1);
		Predictor.ObserveCount(T, CurrentCount - K);
	}
}
#endif
