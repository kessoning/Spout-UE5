// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// The receive attempt as FSpoutFrameSync performs it, on the SDK count model (SpoutSdkCountModel.h).
//
// The notification state itself (SpoutNotificationState.h) is tested transition by transition,
// without Unreal, in Tests/Native/SpoutNotificationTests.cpp. These tests cover what that cannot:
// the frame sync's locking and access scope around it, the check stage both receive paths share,
// the two completion orders (GPU: while access is held; CPU: after it was released), and that with
// the experimental settings at their defaults the receive attempt is a plain pending latch.
//
// A helper performs one receive attempt in the receive path's own order of calls:
//   the check stage (read, wait, transfer without a notification) -> access -> the read under
//   access (only with the recheck on) -> copy -> completion.
// Publications are placed by the model: before the attempt, between the read and the access, or
// hidden for one read by another receiver.
//
// Expectations are written out by hand per scenario; the frame count is only ever a notification
// here, and no expectation depends on two counts being equal.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
#include "SpoutFrameSync.h"
#include "SpoutFrameWait.h"
#include "SpoutLimits.h"
#include "SpoutSdkCountModel.h"
#include "HAL/IConsoleManager.h"

#define SPOUT_NOTIFICATION_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

namespace
{
	using SpoutFrameWait::EDecision;
	using SpoutFrameWait::EOutcome;

	/** Restores a console variable on scope exit. */
	struct FNotifyCVarScope
	{
		IConsoleVariable* Variable;
		FString Before;
		explicit FNotifyCVarScope(const TCHAR* Name) : Variable(IConsoleManager::Get().FindConsoleVariable(Name)), Before(Variable ? Variable->GetString() : FString()) {}
		~FNotifyCVarScope() { if (Variable) Variable->Set(*Before, ECVF_SetByCode); }
		void Set(const TCHAR* Value) { if (Variable) Variable->Set(Value, ECVF_SetByCode); }
	};

	void* const NotifyFakeTexture = reinterpret_cast<void*>(static_cast<UPTRINT>(0xB200));

	enum class EOrder { Gpu, Cpu };
	const EOrder BothOrders[] = { EOrder::Gpu, EOrder::Cpu };
	const TCHAR* OrderName(EOrder Order) { return Order == EOrder::Gpu ? TEXT("GPU order") : TEXT("CPU order"); }

	struct FAttempt
	{
		bool bTransfer = false;      // the check stage asked for a transfer
		bool bAcquired = false;
		bool bCompleted = false;     // the copy reached its completion point
		bool bDelivered = false;     // what CompleteTransfer returned
		FSpoutReceiveCheck Check;
	};

	/** Frame numbers no render frame has, one per attempt, so the wait budget starts fresh each time. */
	uint64 GNotifyTestFrame = MAX_uint64 - 100000;

	/**
	 * One receive attempt in the order the receive paths use. bCopySucceeds false models a failed
	 * copy, completion wait, map or upload: the completion point is not reached.
	 */
	FAttempt RunAttempt(FSpoutFrameSync& Sync, EOrder Order, bool bCopySucceeds = true)
	{
		FAttempt Out;
		const uint64 Frame = ++GNotifyTestFrame;
		Out.bTransfer = SpoutFrameWait::CheckForNewFrame(Sync, [](bool) {},
			[&Sync, Frame](FSpoutFrameWaitReport& Report, double& BudgetBefore, double& BudgetAfter)
			{
				return SpoutFrameWait::RunBudgetedWait(SpoutLimits::FrameWaitSeconds(), Frame,
					[&Sync](double Granted, FSpoutFrameWaitReport& R) { return Sync.WaitForPublication(Granted, SpoutLimits::FrameWaitPollSeconds(), []() { return false; }, R); },
					Report, BudgetBefore, BudgetAfter);
			},
			Out.Check);
		if (!Out.bTransfer) return Out;
		bool bCopied = false;
		{
			FSpoutTextureAccessScope Access(Sync, NotifyFakeTexture);
			Out.bAcquired = Access.IsAcquired();
			if (!Out.bAcquired) return Out;
			if (SpoutLimits::FrameCountRecheck()) Sync.ObserveNotificationUnderAccess();
			bCopied = bCopySucceeds;
			if (bCopied && Order == EOrder::Gpu)
			{
				// GPU path: the completion point is inside the access scope.
				Out.bCompleted = true;
				Out.bDelivered = Sync.CompleteTransfer();
			}
		}
		if (bCopied && Order == EOrder::Cpu)
		{
			// CPU path: access was released; the map and the upload succeeded.
			Out.bCompleted = true;
			Out.bDelivered = Sync.CompleteTransfer();
		}
		return Out;
	}

	TUniquePtr<FSpoutFrameSync> MakeNotifySync(FSpoutSdkCountModelBackend*& OutFake, const ANSICHAR* Name, int64 Publications = 100)
	{
		TUniquePtr<FSpoutSdkCountModelBackend> Owned = MakeUnique<FSpoutSdkCountModelBackend>();
		OutFake = Owned.Get();
		OutFake->SetPublications(Publications);
		TUniquePtr<FSpoutFrameSync> Sync = MakeUnique<FSpoutFrameSync>(MoveTemp(Owned));
		Sync->Open(Name);
		return Sync;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutNotificationOff, "SpoutPlugin.Notification.WithTheSettingsOffTheAttemptIsThePendingLatch", SPOUT_NOTIFICATION_FLAGS)
bool FSpoutNotificationOff::RunTest(const FString&)
{
	FNotifyCVarScope Wait(TEXT("r.Spout.FrameWaitMs")), Recheck(TEXT("r.Spout.FrameCountRecheck"));
	if (!Wait.Variable || !Recheck.Variable) { AddError(TEXT("the experimental settings are not registered")); return false; }
	Wait.Set(TEXT("0")); Recheck.Set(TEXT("0"));

	for (const EOrder Order : BothOrders)
	{
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeNotifySync(Fake, "SpoutNotifyOff");
		const TCHAR* Name = OrderName(Order);

		// One publication, one read, one copy. Exactly one SDK query per attempt: no read under access, no poll.
		int32 Queries = Fake->NewFrameQueries;
		FAttempt A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: the first read is new and its copy is a delivery"), Name), A.bTransfer && A.bCompleted && A.bDelivered);
		TestEqual(FString::Printf(TEXT("%s: one SDK query for the attempt"), Name), Fake->NewFrameQueries - Queries, 1);
		TestFalse(FString::Printf(TEXT("%s: no wait was asked for when the read found a frame"), Name), A.Check.bWaitAsked);

		// Nothing published: the read finds nothing, the wait declines as disabled, nothing is transferred.
		Queries = Fake->NewFrameQueries;
		A = RunAttempt(*Sync, Order);
		TestFalse(FString::Printf(TEXT("%s: nothing new, no transfer"), Name), A.bTransfer);
		TestEqual(FString::Printf(TEXT("%s: still one SDK query"), Name), Fake->NewFrameQueries - Queries, 1);
		TestEqual(FString::Printf(TEXT("%s: the wait is disabled"), Name), int32(A.Check.WaitReport.Decision), int32(EDecision::Disabled));
		TestFalse(FString::Printf(TEXT("%s: and there is no transfer without a notification"), Name), A.Check.bTransferWithoutNotification);
		TestEqual(FString::Printf(TEXT("%s: no access was attempted"), Name), Fake->AccessAttempts, 1);

		// Published, then published again before access: both are in the texture; with the settings off the
		// SDK reports the second one new again at the next read, and it is copied again.
		Fake->Publish();
		Fake->PublishAtNextAccess = 1;
		A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: delivered"), Name), A.bDelivered);
		A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: the publication made before the access is reported new at the next read and copied again"), Name), A.bTransfer && A.bDelivered);
		TestTrue(FString::Printf(TEXT("%s: as a pending frame, not as a transfer without a notification"), Name), A.Check.bNewFrameAtRead);
		A = RunAttempt(*Sync, Order);
		TestFalse(FString::Printf(TEXT("%s: then nothing"), Name), A.bTransfer);

		// A refused access keeps the frame pending; a failed copy too.
		Fake->Publish();
		Fake->FailAccessAttempts = 1;
		A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: refused access: nothing completed"), Name), A.bTransfer && !A.bAcquired && !A.bCompleted);
		TestTrue(FString::Printf(TEXT("%s: the frame stays pending"), Name), Sync->HasPendingFrame());
		A = RunAttempt(*Sync, Order, false);
		TestTrue(FString::Printf(TEXT("%s: failed copy: acquired, not completed"), Name), A.bAcquired && !A.bCompleted);
		TestTrue(FString::Printf(TEXT("%s: still pending"), Name), Sync->HasPendingFrame());
		A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: the retry delivers without a new notification"), Name), A.bDelivered && !Sync->HasPendingFrame());

		// No read history is kept, so switching a setting on later cannot see a "moved" count.
		const FSpoutFrameSync::FNotificationStats Stats = Sync->GetNotificationStats();
		TestEqual(FString::Printf(TEXT("%s: no read count is kept"), Name), Stats.ReadCount, int64(-1));
		TestEqual(FString::Printf(TEXT("%s: four copies"), Name), Stats.Copies, int64(4));
		TestEqual(FString::Printf(TEXT("%s: none without a notification"), Name), Stats.CopiesWithoutNotification, int64(0));
		int64 Frames = 0, Misses = 0;
		Sync->GetStatus(Frames, Misses);
		TestEqual(FString::Printf(TEXT("%s: the status counts the four copies"), Name), Frames, int64(4));
		TestEqual(FString::Printf(TEXT("%s: and the one refused access"), Name), Misses, int64(1));
		TestEqual(FString::Printf(TEXT("%s: every access taken was given back"), Name), Sync->GetAcquireCount(), Sync->GetReleaseCount());
	}

	// MarkFrameConsumed is the same completion, for the callers that do not need its answer.
	{
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeNotifySync(Fake, "SpoutNotifyOffMark");
		TestTrue(TEXT("a frame is pending"), Sync->ConsumeNewFrame());
		Sync->MarkFrameConsumed();
		TestFalse(TEXT("MarkFrameConsumed clears it"), Sync->HasPendingFrame());
		Sync->MarkFrameConsumed();
		int64 Frames = 0, Misses = 0;
		Sync->GetStatus(Frames, Misses);
		TestEqual(TEXT("a completion with nothing pending is not counted"), Frames, int64(1));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutNotificationRecheck, "SpoutPlugin.Notification.TheReadUnderAccessAndTheTransferWithoutANotification", SPOUT_NOTIFICATION_FLAGS)
bool FSpoutNotificationRecheck::RunTest(const FString&)
{
	FNotifyCVarScope Wait(TEXT("r.Spout.FrameWaitMs")), Recheck(TEXT("r.Spout.FrameCountRecheck"));
	if (!Wait.Variable || !Recheck.Variable) { AddError(TEXT("the experimental settings are not registered")); return false; }
	Wait.Set(TEXT("0")); Recheck.Set(TEXT("1"));

	for (const EOrder Order : BothOrders)
	{
		const TCHAR* Name = OrderName(Order);
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeNotifySync(Fake, "SpoutNotifyRecheck");

		// Ordinary: two SDK queries per transfer (the read and the read under access), one when nothing is new.
		int32 Queries = Fake->NewFrameQueries;
		FAttempt A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: delivered"), Name), A.bDelivered);
		TestEqual(FString::Printf(TEXT("%s: the read and the read under access"), Name), Fake->NewFrameQueries - Queries, 2);
		Queries = Fake->NewFrameQueries;
		A = RunAttempt(*Sync, Order);
		TestFalse(FString::Printf(TEXT("%s: nothing published: no transfer"), Name), A.bTransfer);
		TestEqual(FString::Printf(TEXT("%s: one query"), Name), Fake->NewFrameQueries - Queries, 1);

		// Copied ahead: published between the read and the access. The read under access sees it, so the
		// next read does not report it again...
		Fake->Publish();
		Fake->PublishAtNextAccess = 1;
		A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: copied ahead: delivered"), Name), A.bDelivered);
		TestEqual(FString::Printf(TEXT("%s: counted as copied ahead of the read"), Name), Sync->GetNotificationStats().CopiedAheadOfRead, int64(1));
		// ...but the count moved since the previous read, so the transfer is made anyway. Nothing was
		// published: the copy repeats the image, and it is still a delivery.
		A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: after a copy ahead the next attempt transfers without a notification"), Name), A.bTransfer && A.Check.bTransferWithoutNotification && !A.Check.bNewFrameAtRead);
		TestTrue(FString::Printf(TEXT("%s: and its copy is a delivery"), Name), A.bCompleted && A.bDelivered);
		TestEqual(FString::Printf(TEXT("%s: counted as a copy without a notification"), Name), Sync->GetNotificationStats().CopiesWithoutNotification, int64(1));
		A = RunAttempt(*Sync, Order);
		TestFalse(FString::Printf(TEXT("%s: then the count has not moved: nothing"), Name), A.bTransfer);

		// The frame such a transfer exists for: copied ahead, then the sender publishes between the next
		// read and its access. Without the transfer this publication would wait a whole frame and could
		// be overwritten; with it, it is copied now.
		Fake->Publish();
		Fake->PublishAtNextAccess = 1;
		RunAttempt(*Sync, Order);
		Fake->PublishAtNextAccess = 1;
		A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: the publication made before the access of a transfer without a notification is delivered"), Name), A.Check.bTransferWithoutNotification && A.bDelivered);
		TestEqual(FString::Printf(TEXT("%s: and that copy had a notification after all"), Name), Sync->GetNotificationStats().CopiesWithoutNotification, int64(1));

		// A count that looks unchanged does not withhold an image. Copied ahead; the next attempt transfers
		// without a notification; the sender publishes before its access; another receiver's read makes the
		// read under access see the old count, so the SDK answers "nothing new". The copy holds the new
		// image and must be delivered; the hidden publication is reported at the read after.
		Fake->Publish();
		Fake->PublishAtNextAccess = 1;
		RunAttempt(*Sync, Order);
		Fake->PublishAtNextAccess = 1;
		Fake->ForeignDecrementAtQuery = Fake->NewFrameQueries + 2;   // the second query of the attempt: the read under access
		const int64 CopiesBefore = Sync->GetNotificationStats().Copies;
		A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: a transfer without a notification whose counts all look equal is still delivered"), Name),
			A.Check.bTransferWithoutNotification && A.bCompleted && A.bDelivered);
		TestFalse(FString::Printf(TEXT("%s: (the read under access did answer nothing new)"), Name), Sync->GetNotificationStats().bNewUnderAccess);
		TestEqual(FString::Printf(TEXT("%s: one more copy"), Name), Sync->GetNotificationStats().Copies, CopiesBefore + 1);
		A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: the publication the other receiver hid is new at the next read"), Name), A.Check.bNewFrameAtRead && A.bDelivered);

		// Refused access and a failed copy keep the frame pending, with the read under access in between.
		A = RunAttempt(*Sync, Order);
		Fake->Publish();
		Fake->FailAccessAttempts = 1;
		A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: refused: pending"), Name), !A.bAcquired && Sync->HasPendingFrame());
		A = RunAttempt(*Sync, Order, false);
		TestTrue(FString::Printf(TEXT("%s: the read under access found nothing new, the copy failed: still pending"), Name), A.bAcquired && !A.bCompleted && Sync->HasPendingFrame());
		Fake->PublishAtNextAccess = 1;
		A = RunAttempt(*Sync, Order, false);
		TestTrue(FString::Printf(TEXT("%s: the read under access found something new, the copy failed: still pending"), Name), !A.bCompleted && Sync->HasPendingFrame());
		A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: the retry delivers"), Name), A.bDelivered && !Sync->HasPendingFrame());

		// A forced current frame is owed whatever the count says, and survives a failed copy.
		while (RunAttempt(*Sync, Order).bTransfer) {}
		Sync->RequestCurrentFrame();
		A = RunAttempt(*Sync, Order, false);
		TestTrue(FString::Printf(TEXT("%s: a forced current frame is attempted with nothing published"), Name), A.bTransfer && A.Check.bNewFrameAtRead);
		A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: and delivered on the retry"), Name), A.bDelivered);
		TestFalse(FString::Printf(TEXT("%s: its forgotten history does not look like a moved count"), Name), RunAttempt(*Sync, Order).bTransfer);

		// Close and open: a frame pending at the close is dropped, and the first copy after the open is a delivery.
		Fake->Publish();
		Fake->FailAccessAttempts = 1;
		RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: pending before the close"), Name), Sync->HasPendingFrame());
		Sync->Close();
		TestFalse(FString::Printf(TEXT("%s: the close drops it"), Name), Sync->HasPendingFrame());
		TestFalse(FString::Printf(TEXT("%s: a completion after the close is not a delivery"), Name), Sync->CompleteTransfer());
		TestFalse(FString::Printf(TEXT("%s: a closed stream attempts nothing"), Name), RunAttempt(*Sync, Order).bTransfer);
		Sync->Open("SpoutNotifyRecheck");
		A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: the first read after the open is new (the SDK object starts again) and delivered"), Name), A.bDelivered);
		TestEqual(FString::Printf(TEXT("%s: two opens counted"), Name), Sync->GetNotificationStats().Opens, int64(2));
		TestEqual(FString::Printf(TEXT("%s: every access taken was given back"), Name), Sync->GetAcquireCount(), Sync->GetReleaseCount());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutNotificationNoCount, "SpoutPlugin.Notification.WithoutACountEveryAttemptCopiesAndNeverWaits", SPOUT_NOTIFICATION_FLAGS)
bool FSpoutNotificationNoCount::RunTest(const FString&)
{
	FNotifyCVarScope Wait(TEXT("r.Spout.FrameWaitMs")), Recheck(TEXT("r.Spout.FrameCountRecheck"));
	if (!Wait.Variable || !Recheck.Variable) { AddError(TEXT("the experimental settings are not registered")); return false; }
	Wait.Set(TEXT("1"));

	struct FCase { const TCHAR* What; bool bCounting; bool bSemaphore; int64 Publications; };
	const FCase Cases[] = {
		{ TEXT("counting switched off"), false, true, 100 },
		{ TEXT("no semaphore"), true, false, 0 },
		{ TEXT("a sender that publishes no count"), true, true, 0 } };
	for (const FCase& Case : Cases)
	{
		for (const EOrder Order : BothOrders)
		{
			FSpoutSdkCountModelBackend* Fake = nullptr;
			TUniquePtr<FSpoutFrameSync> Sync = MakeNotifySync(Fake, "SpoutNotifyNoCount", Case.Publications);
			Fake->bCountingEnabled = Case.bCounting;
			if (!Case.bSemaphore) Fake->Semaphore.Reset();
			for (int32 K = 0; K < 5; ++K)
			{
				const FAttempt A = RunAttempt(*Sync, Order);
				TestTrue(FString::Printf(TEXT("%s, %s: attempt %d copies and delivers"), Case.What, OrderName(Order), K), A.bTransfer && A.bDelivered && A.Check.bNewFrameAtRead);
				TestFalse(FString::Printf(TEXT("%s, %s: attempt %d asks for no wait"), Case.What, OrderName(Order), K), A.Check.bWaitAsked);
			}
			int64 Attempts = 0, Published = 0, TimedOut = 0;
			Sync->GetFrameWaitStats(Attempts, Published, TimedOut);
			TestEqual(FString::Printf(TEXT("%s, %s: no wait was ever made"), Case.What, OrderName(Order)), Attempts, int64(0));
			TestEqual(FString::Printf(TEXT("%s, %s: no count is tracked"), Case.What, OrderName(Order)), Sync->GetNotificationStats().ReadCount, int64(-1));
			TestEqual(FString::Printf(TEXT("%s, %s: five copies, all with a notification"), Case.What, OrderName(Order)),
				Sync->GetNotificationStats().Copies - Sync->GetNotificationStats().CopiesWithoutNotification, int64(5));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutNotificationWait, "SpoutPlugin.Notification.TheWaitInsideTheCheckStage", SPOUT_NOTIFICATION_FLAGS)
bool FSpoutNotificationWait::RunTest(const FString&)
{
	FNotifyCVarScope Wait(TEXT("r.Spout.FrameWaitMs")), Recheck(TEXT("r.Spout.FrameCountRecheck")), Budget(TEXT("r.Spout.FrameWaitBudgetMs"));
	if (!Wait.Variable || !Recheck.Variable || !Budget.Variable) { AddError(TEXT("the experimental settings are not registered")); return false; }
	Wait.Set(TEXT("1")); Recheck.Set(TEXT("0")); Budget.Set(TEXT("1"));
	const double T = 1.0 / 60.0;

	for (const EOrder Order : BothOrders)
	{
		const TCHAR* Name = OrderName(Order);
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeNotifySync(Fake, "SpoutNotifyWait", 1000);
		// A test clock: one microsecond per reading (the wait's spin must see time pass), and one reader
		// period between attempts, so the predictor sees a reader at the source's own rate.
		TSharedRef<double> Clock = MakeShared<double>(1000.0);
		Sync->SetClockForTests([Clock]() { *Clock += 0.000001; return *Clock; });
		auto RunNext = [&Sync, Clock, Order, T](bool bCopySucceeds = true) { *Clock += T; return RunAttempt(*Sync, Order, bCopySucceeds); };
		FAttempt A = RunNext();
		TestTrue(FString::Printf(TEXT("%s: the first frame is delivered"), Name), A.bDelivered);
		*Clock += T;
		RunAttempt(*Sync, Order);     // a second read, so the read interval is known

		// Nothing new at the read, the next publication 0.3 ms away: the check stage waits, the third poll
		// sees the publication and the frame is copied in this attempt.
		*Clock += T;
		Sync->PrimePredictorForTests(T, 0.0003, Fake->SdkCountNow());
		Fake->PublishAtQuery = Fake->NewFrameQueries + 4;   // the read is the first query; polls are the next three
		A = RunAttempt(*Sync, Order);
		TestTrue(FString::Printf(TEXT("%s: the read found nothing and a wait was asked for"), Name), !A.Check.bNewFrameAtRead && A.Check.bWaitAsked);
		TestEqual(FString::Printf(TEXT("%s: it waited"), Name), int32(A.Check.WaitReport.Decision), int32(EDecision::Wait));
		TestEqual(FString::Printf(TEXT("%s: and the publication ended it"), Name), int32(A.Check.WaitReport.Outcome), int32(EOutcome::Published));
		TestTrue(FString::Printf(TEXT("%s: the frame the wait saw was copied and delivered in the same attempt"), Name), A.Check.bPendingAfterWait && A.bDelivered);
		TestFalse(FString::Printf(TEXT("%s: as a pending frame, not as a transfer without a notification"), Name), A.Check.bTransferWithoutNotification);
		TestTrue(FString::Printf(TEXT("%s: the budget was charged"), Name), A.Check.BudgetAfterSeconds > A.Check.BudgetBeforeSeconds);
		int64 Reads = 0, Left = 0, UntilProbe = 0, Probes = 0;
		double ReadInterval = 0.0;
		Sync->GetPredictorReadState(Reads, ReadInterval, Left, UntilProbe, Probes);
		const int64 ReadsBefore = Reads;

		// A wait that times out decides alone: no transfer, although the count moved since the previous read.
		*Clock += T;
		Sync->PrimePredictorForTests(T, 0.0003, Fake->SdkCountNow());
		A = RunAttempt(*Sync, Order);
		TestEqual(FString::Printf(TEXT("%s: timed out"), Name), int32(A.Check.WaitReport.Outcome), int32(EOutcome::TimedOut));
		TestFalse(FString::Printf(TEXT("%s: a timed-out wait is not followed by a transfer"), Name), A.bTransfer);
		Sync->GetPredictorReadState(Reads, ReadInterval, Left, UntilProbe, Probes);
		TestEqual(FString::Printf(TEXT("%s: one attempt is one read for the predictor, however many polls"), Name), Reads, ReadsBefore + 1);

		// A wait that is declined (the publication is not due) leaves the decision to the read history: the
		// count moved since the previous read (the copy before last was seen by a poll), so it transfers.
		Fake->Publish();
		RunNext();                                             // new at the read: delivered
		Fake->Publish();
		Fake->PublishAtNextAccess = 1;
		RunNext();                                             // copied ahead
		*Clock += T;
		Sync->PrimePredictorForTests(T, 0.008, Fake->SdkCountNow());   // the next publication is 8 ms away: not imminent
		A = RunAttempt(*Sync, Order);
		TestEqual(FString::Printf(TEXT("%s: the wait declined"), Name), int32(A.Check.WaitReport.Decision), int32(EDecision::NotImminent));
		TestTrue(FString::Printf(TEXT("%s: so the transfer without a notification is made"), Name), A.Check.bTransferWithoutNotification && A.bDelivered);

		// An empty budget is also "no wait was made".
		Fake->Publish();
		Fake->PublishAtNextAccess = 1;
		RunNext();                                             // copied ahead again
		Budget.Set(TEXT("0"));
		A = RunNext();
		TestEqual(FString::Printf(TEXT("%s: the budget is spent"), Name), int32(A.Check.WaitReport.Decision), int32(EDecision::BudgetExhausted));
		TestTrue(FString::Printf(TEXT("%s: the transfer without a notification is made"), Name), A.Check.bTransferWithoutNotification && A.bDelivered);
		Budget.Set(TEXT("1"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutNotificationBundledSdk, "SpoutPlugin.Notification.BundledSdkAgreesWithTheModel", SPOUT_NOTIFICATION_FLAGS)
bool FSpoutNotificationBundledSdk::RunTest(const FString&)
{
	// The same short script on the real SDK backend and on the model. What the plugin relies on: a
	// publication is new once; the count is the number of publications; a read under access after a
	// publication is new and the read after it is not.
	FNotifyCVarScope Wait(TEXT("r.Spout.FrameWaitMs")), Recheck(TEXT("r.Spout.FrameCountRecheck"));
	if (!Wait.Variable || !Recheck.Variable) { AddError(TEXT("the experimental settings are not registered")); return false; }
	Wait.Set(TEXT("0")); Recheck.Set(TEXT("1"));

	TUniquePtr<ISpoutFrameSyncBackend> SenderBackend = MakeSpoutSdkFrameSyncBackend();
	TUniquePtr<ISpoutFrameSyncBackend> ReceiverBackend = MakeSpoutSdkFrameSyncBackend();
	if (!SenderBackend || !ReceiverBackend)
	{
		AddWarning(TEXT("Spout.dll is not loaded in this process: the bundled SDK was not compared with the model."));
		return true;
	}
	const FString UniqueName = FString::Printf(TEXT("SpoutNotifySdk_%u_%llu"), FPlatformProcess::GetCurrentProcessId(), FPlatformTime::Cycles64());
	const FTCHARToUTF8 Utf8(*UniqueName);
	FSpoutFrameSync Sender(MoveTemp(SenderBackend));
	FSpoutFrameSync Receiver(MoveTemp(ReceiverBackend));
	if (!TestTrue(TEXT("the sender opens"), Sender.Open(Utf8.Get())) || !TestTrue(TEXT("the receiver opens"), Receiver.Open(Utf8.Get()))) return false;

	FSpoutSdkCountModelBackend* Model = nullptr;
	TUniquePtr<FSpoutFrameSync> ModelSync = MakeNotifySync(Model, "SpoutNotifyModel", 0);

	// Before any publication: every read is new, and there is no count.
	const bool bRealFirst = Receiver.ConsumeNewFrame();
	TestEqual(TEXT("before any publication a read is new, as in the model"), bRealFirst, ModelSync->ConsumeNewFrame());
	TestEqual(TEXT("with a count of zero, as in the model"), Receiver.GetLastObservedSenderFrame(), ModelSync->GetLastObservedSenderFrame());
	TestEqual(TEXT("which the plugin takes as no count"), Receiver.GetNotificationStats().ReadCount, int64(-1));
	Receiver.CompleteTransfer();
	ModelSync->CompleteTransfer();

	// With frame counting switched off on this machine (SpoutSettings) the SDK publishes no count and
	// answers "new" to every read. There is then nothing to compare, and the test says so.
	Sender.PublishFrame(); Model->Publish();
	const bool bRealOne = Receiver.ConsumeNewFrame();
	const bool bModelOne = ModelSync->ConsumeNewFrame();
	if (Receiver.GetNotificationStats().ReadCount < 0)
	{
		AddWarning(TEXT("frame counting is disabled on this machine (SpoutSettings): the bundled SDK was not compared with the model."));
		return true;
	}
	TestTrue(TEXT("one publication is new, as in the model"), bRealOne && bModelOne);
	TestEqual(TEXT("with the count at 1"), Receiver.GetLastObservedSenderFrame(), int64(1));
	TestEqual(TEXT("as in the model"), ModelSync->GetLastObservedSenderFrame(), int64(1));
	Receiver.CompleteTransfer(); ModelSync->CompleteTransfer();

	auto Step = [this, &Sender, &Receiver, Model, &ModelSync](int32 Publications, const TCHAR* What)
	{
		for (int32 K = 0; K < Publications; ++K) { Sender.PublishFrame(); Model->Publish(); }
		const bool bReal = Receiver.ConsumeNewFrame();
		const bool bModel = ModelSync->ConsumeNewFrame();
		TestEqual(FString::Printf(TEXT("%s: the read answers as the model does"), What), bReal, bModel);
		TestEqual(FString::Printf(TEXT("%s: with the same count"), What), Receiver.GetLastObservedSenderFrame(), ModelSync->GetLastObservedSenderFrame());
		TestEqual(FString::Printf(TEXT("%s: and the same read count"), What), Receiver.GetNotificationStats().ReadCount, ModelSync->GetNotificationStats().ReadCount);
		return bReal;
	};
	TestFalse(TEXT("and only once"), Step(0, TEXT("no publication")));
	TestTrue(TEXT("three publications are one notification"), Step(3, TEXT("three publications")));
	TestEqual(TEXT("with the count at 4"), Receiver.GetLastObservedSenderFrame(), int64(4));

	// The read under access, with a publication made after the read.
	Sender.PublishFrame(); Model->Publish();
	{
		FSpoutTextureAccessScope Real(Receiver, nullptr);
		FSpoutTextureAccessScope Fake(*ModelSync, NotifyFakeTexture);
		if (TestTrue(TEXT("access acquired on the real name"), Real.IsAcquired()) && TestTrue(TEXT("and on the model"), Fake.IsAcquired()))
		{
			Receiver.ObserveNotificationUnderAccess();
			ModelSync->ObserveNotificationUnderAccess();
			TestEqual(TEXT("the read under access sees the publication, as in the model"), Receiver.GetNotificationStats().bNewUnderAccess, ModelSync->GetNotificationStats().bNewUnderAccess);
			TestEqual(TEXT("with count 5"), Receiver.GetNotificationStats().UnderAccessCount, int64(5));
			TestEqual(TEXT("as in the model"), ModelSync->GetNotificationStats().UnderAccessCount, int64(5));
			TestTrue(TEXT("the copy is a delivery"), Receiver.CompleteTransfer());
			ModelSync->CompleteTransfer();
		}
	}
	TestFalse(TEXT("the read after it does not report that publication again"), Step(0, TEXT("after the read under access")));
	TestTrue(TEXT("the count moved since the previous read: a transfer without a notification"), Receiver.BeginTransferWithoutNotification(false));
	TestTrue(TEXT("as in the model"), ModelSync->BeginTransferWithoutNotification(false));
	TestEqual(TEXT("every access taken was given back"), Receiver.GetAcquireCount(), Receiver.GetReleaseCount());
	return true;
}
#endif
