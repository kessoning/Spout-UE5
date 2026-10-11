// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
//
// Native tests of the receive-side notification state (SpoutNotificationState.h) and the publication
// predictor (SpoutPublishPredictor.h), compiled from the plugin's own headers without Unreal.
//
//   part 1  every transition of the notification state, with expectations written by hand
//   part 2  the predictor on made-up times
//   part 3  the bundled Spout.dll: the public calls the plugin uses behave as part 1 assumes, also
//           while another receiver reads the same sender as fast as it can
//
// Part 3 needs frame counting enabled on this machine (the Spout default). When it is off the
// part is reported as skipped and the process exits with 2, so a skip is never read as a pass.
#include <windows.h>
#include <d3d11.h>
#include "SpoutDX/SpoutFrameCount.h"
#include "SpoutNotificationState.h"
#include "SpoutPublishPredictor.h"
#include <atomic>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "winmm.lib")

static int Failures = 0;
static int Checks = 0;
static void Check(bool Value, const char* Message)
{
	++Checks;
	if (!Value) { std::cout << "FAIL " << Message << std::endl; ++Failures; }
}
template <typename T, typename U>
static void Equal(T Actual, U Expected, const char* Message)
{
	++Checks;
	if (!(Actual == static_cast<T>(Expected)))
	{
		std::cout << "FAIL " << Message << ": got " << Actual << ", expected " << Expected << std::endl;
		++Failures;
	}
}

using FState = FSpoutNotificationState;
using EPoll = FSpoutNotificationState::EPoll;
static const std::int64_t None = FSpoutNotificationState::NoCount;

// ------------------------------------------------------------------------------------------------
// Part 1: the notification state. The SDK's answers are given by hand: (new?, count).
// ------------------------------------------------------------------------------------------------
static void StateWithSettingsOff()
{
	// Without tracking this is the pending latch and nothing else.
	FState S;
	Check(!S.IsOwed(), "off: nothing is owed at the start");
	Check(!S.BeginAttempt(false, None, false), "off: a read that finds nothing new owes nothing");
	Check(!S.ShouldTransferWithoutNotification(false), "off: no transfer without a notification, ever");
	Check(S.BeginAttempt(true, None, false), "off: a new notification is owed");
	Check(S.BeginAttempt(false, None, false), "off: it stays owed on the next read (access was refused, say)");
	Check(S.BeginAttempt(false, None, false), "off: and on the one after");
	Check(S.CompleteTransfer(), "off: the completed copy is a delivery");
	Check(!S.IsOwed(), "off: completion clears owed");
	Check(!S.BeginAttempt(false, None, false), "off: nothing is owed after it");
	Check(!S.IsCountKnown(), "off: no count is tracked");
	Equal(S.GetCopies(), 1, "off: one copy");
	Equal(S.GetCopiesWithoutNotification(), 0, "off: no copy without a notification");
	// A count passed while not tracking is ignored: switching tracking on later starts without a history.
	FState T;
	T.BeginAttempt(true, 7, false);
	T.CompleteTransfer();
	Check(!T.BeginAttempt(false, 8, true), "the first tracked read owes nothing by itself");
	Check(!T.ShouldTransferWithoutNotification(false), "the first tracked read has no previous count: no transfer without a notification");
	Check(!T.BeginAttempt(false, 9, true) && T.ShouldTransferWithoutNotification(false), "the second tracked read sees the count moved");
}

static void StateOrdinaryAndCopiedAhead()
{
	FState S;
	// Read: count 5 is new. Access: still 5. Copy completes.
	Check(S.BeginAttempt(true, 5, true), "a new notification at the read is owed");
	Check(S.IsCountKnown(), "the count is known");
	S.ObserveUnderAccess(false, 5);
	Check(S.IsOwed(), "the read under access clears nothing");
	Check(S.CompleteTransfer(), "the copy is a delivery");
	Equal(S.GetCopiedAheadOfRead(), 0, "not copied ahead");
	// Next attempt, nothing published: nothing new, the count did not move, no transfer.
	Check(!S.BeginAttempt(false, 5, true), "nothing new");
	Check(!S.ShouldTransferWithoutNotification(false), "the count did not move since the previous read: no transfer");
	// Read: 6 is new. The sender publishes 7 before access is taken: the read under access sees it.
	Check(S.BeginAttempt(true, 6, true), "6 is owed");
	S.ObserveUnderAccess(true, 7);
	Check(S.CompleteTransfer(), "delivered (the image is the one published as 7)");
	Equal(S.GetCopiedAheadOfRead(), 1, "counted as copied ahead of the read");
	// Next read: the SDK compares with 7 (its last call was the read under access), so nothing is
	// new. The count moved since the previous READ (6 to 7), and no wait is made: transfer anyway.
	Check(!S.BeginAttempt(false, 7, true), "after a copy ahead the next read finds nothing new");
	Check(S.ShouldTransferWithoutNotification(false), "but the count moved since the previous read: a transfer without a notification");
	Check(!S.ShouldTransferWithoutNotification(true), "a wait that was made decides alone: no transfer without a notification after it");
	S.BeginFallbackTransfer();
	S.ObserveUnderAccess(false, 7);
	Check(!S.IsOwed(), "nothing became owed under access");
	Check(S.CompleteTransfer(), "a copy without a notification is still a delivery");
	Equal(S.GetCopiesWithoutNotification(), 1, "and is counted as such");
	// The same, but the sender published 8 between the read and the access: the copy brings a new image.
	Check(S.BeginAttempt(true, 8, true), "8 is owed");
	S.ObserveUnderAccess(true, 9);
	S.CompleteTransfer();
	Check(!S.BeginAttempt(false, 9, true) && S.ShouldTransferWithoutNotification(false), "copied ahead again: a transfer without a notification");
	S.BeginFallbackTransfer();
	S.ObserveUnderAccess(true, 10);
	Check(S.IsOwed(), "a publication seen under access makes the transfer owed");
	Check(S.CompleteTransfer(), "delivered");
	Equal(S.GetCopiesWithoutNotification(), 1, "that one had a notification after all");
	Equal(S.GetCopies(), 5, "five copies in all");
}

static void StateFailuresKeepOwed()
{
	FState S;
	Check(S.BeginAttempt(true, 3, true), "3 is owed");
	// Access refused: nothing else is called. The next attempt still owes it, with or without news.
	Check(S.BeginAttempt(false, 3, true), "after a refused access the frame is still owed");
	// Access granted, the read under access finds nothing new, and the copy or its completion fails.
	S.ObserveUnderAccess(false, 3);
	Check(S.IsOwed(), "a failed copy clears nothing");
	Check(S.BeginAttempt(false, 3, true), "still owed on the next attempt, without a new notification");
	// Under access something newer is seen, then the completion wait fails: still owed.
	S.ObserveUnderAccess(true, 4);
	Check(S.BeginAttempt(false, 4, true), "still owed after a failed completion");
	S.ObserveUnderAccess(false, 4);
	Check(S.CompleteTransfer(), "the retry completes and delivers");
	Check(!S.IsOwed(), "and only then is nothing owed");
	Equal(S.GetCopies(), 1, "one copy, however many attempts it took");
	// A failed transfer without a notification is not retried by itself: nothing was owed.
	S.BeginAttempt(true, 5, true);
	S.ObserveUnderAccess(true, 6);
	S.CompleteTransfer();
	Check(!S.BeginAttempt(false, 6, true) && S.ShouldTransferWithoutNotification(false), "a transfer without a notification is due");
	S.BeginFallbackTransfer();
	S.ObserveUnderAccess(false, 6); // and the copy fails: CompleteTransfer is not called
	Check(!S.BeginAttempt(false, 6, true), "the next read owes nothing");
	Check(!S.ShouldTransferWithoutNotification(false), "and the count did not move since the previous read: nothing to do");
	Check(!S.IsFallback(), "a new attempt does not inherit the transfer without a notification");
}

static void StateCpuOrder()
{
	// CPU path: the read under access, the staging copy, the release, then the map and the upload,
	// and only then the completion. A publication made after the read under access is not known to
	// this state at all; the SDK reports it at the next read.
	FState S;
	S.BeginAttempt(true, 10, true);
	S.ObserveUnderAccess(false, 10);
	// ... access is released here; the sender publishes 11; the map fails: no completion.
	Check(S.BeginAttempt(true, 11, true), "the publication made after the release is new at the next read");
	S.ObserveUnderAccess(false, 11);
	Check(S.CompleteTransfer(), "the retry delivers");
	// The same with a successful map: completion after the release, then 12 is new at the next read.
	S.BeginAttempt(true, 12, true);
	S.ObserveUnderAccess(false, 12);
	Check(S.CompleteTransfer(), "completion after the release delivers");
	Check(S.BeginAttempt(true, 13, true), "a publication after the release is not consumed by that completion");
}

static void StateWaitPolls()
{
	FState S;
	S.BeginAttempt(true, 20, true);
	S.ObserveUnderAccess(false, 20);
	S.CompleteTransfer();
	Check(!S.BeginAttempt(false, 20, true), "nothing new at the read: a wait may be made");
	Check(S.ObservePoll(false, 20) == EPoll::Nothing, "a poll that sees the read's count is nothing");
	Check(S.ObservePoll(true, 19) == EPoll::Transient, "one below the read's count is another receiver's read");
	Check(!S.IsOwed(), "which owes nothing and does not end the wait");
	Check(S.ObservePoll(true, 20) == EPoll::Nothing, "the count coming back is not a publication either, whatever the SDK answered");
	Check(!S.IsOwed(), "still nothing owed");
	Check(S.ObservePoll(true, 21) == EPoll::Published, "a count above the read's is a publication");
	Check(S.IsOwed(), "which is owed at once: the poll is the latch");
	Equal(S.GetTransients(), 1, "one transient counted");
	Equal(S.GetPolls(), 4, "four polls counted");
	S.ObserveUnderAccess(false, 21);
	S.CompleteTransfer();
	// A wait that times out leaves nothing owed, and is not followed by a transfer without a notification.
	Check(!S.BeginAttempt(false, 21, true), "nothing new");
	Check(S.ObservePoll(false, 21) == EPoll::Nothing, "timed-out wait: nothing");
	Check(!S.IsOwed() && !S.ShouldTransferWithoutNotification(true), "nothing owed, no transfer without a notification after a wait");
	// The count starting again is a new notification.
	Check(!S.BeginAttempt(false, 21, true), "nothing new");
	Check(S.ObservePoll(true, 2) == EPoll::Published, "a count far below the read's means it started again: owed");
	S.ObserveUnderAccess(false, 2);
	S.CompleteTransfer();
	// The count going away under a wait.
	Check(!S.BeginAttempt(false, 2, true), "nothing new");
	Check(S.ObservePoll(true, None) == EPoll::CountLost, "no count any more ends the wait");
	Check(S.IsOwed(), "with the frame owed, as on every read without a count");
}

static void StateCountUnavailable()
{
	// Counting off, absent, or a sender that never publishes a count: the SDK answers "new" to every
	// read, the count is zero or missing, and every attempt copies and delivers. No wait is possible.
	FState S;
	for (int I = 0; I < 5; ++I)
	{
		Check(S.BeginAttempt(true, I % 2 ? 0 : None, true), "without a count every read is owed");
		Check(!S.IsCountKnown(), "and there is no count to wait on");
		S.ObserveUnderAccess(true, 0);
		Check(S.CompleteTransfer(), "every copy is a delivery");
	}
	Equal(S.GetCopies(), 5, "five reads, five copies");
	Equal(S.GetCopiesWithoutNotification(), 0, "all of them owed");
	// The count appearing later does not look like a move from "none".
	Check(S.BeginAttempt(true, 40, true), "the first counted read is owed (the SDK says new)");
	S.ObserveUnderAccess(false, 40);
	S.CompleteTransfer();
	Check(!S.BeginAttempt(false, 40, true) && !S.ShouldTransferWithoutNotification(false), "and the next read is quiet");
}

static void StateForcedFrameRestartAndReopen()
{
	FState S;
	S.BeginAttempt(true, 30, true);
	S.ObserveUnderAccess(false, 30);
	S.CompleteTransfer();
	// A forced current frame (a new shared texture): owed whatever the count says; the history is forgotten.
	S.RequestCurrentFrame();
	Check(S.IsOwed(), "a forced current frame is owed");
	Check(S.BeginAttempt(false, 30, true), "and stays owed through a read that finds nothing new");
	Check(!S.ShouldTransferWithoutNotification(false), "it is an owed transfer, not one without a notification");
	S.ObserveUnderAccess(false, 30);
	Check(S.CompleteTransfer(), "delivered although the count never moved");
	Equal(S.GetCurrentFrameRequests(), 1, "one forced frame counted");
	Check(!S.BeginAttempt(false, 30, true) && !S.ShouldTransferWithoutNotification(false), "the forgotten history does not look like a moved count");
	// A forced frame that fails to copy is still owed afterwards.
	S.RequestCurrentFrame();
	S.BeginAttempt(false, 30, true);
	S.ObserveUnderAccess(false, 30); // the copy fails
	Check(S.BeginAttempt(false, 30, true), "a forced frame whose copy failed is retried");
	S.ObserveUnderAccess(false, 30);
	S.CompleteTransfer();
	// The sender starts again: its count drops. The SDK reports it new (the count differs).
	Check(S.BeginAttempt(true, 1, true), "a restarted count is a new notification");
	S.ObserveUnderAccess(false, 1);
	Check(S.CompleteTransfer(), "delivered");
	// Close and open: everything is forgotten, and a frame owed at the close is not delivered.
	S.BeginAttempt(true, 2, true);
	Check(S.IsOwed(), "owed before the close");
	S.Reset();
	Check(!S.IsOwed(), "the close drops the owed frame");
	Check(!S.CompleteTransfer(), "a completion arriving after the close is not a delivery");
	Check(!S.BeginAttempt(false, 2, true) && !S.ShouldTransferWithoutNotification(false), "after the open there is no history");
	Check(S.BeginAttempt(true, 3, true), "the first notification after the open is owed");
	S.ObserveUnderAccess(false, 3);
	Check(S.CompleteTransfer(), "and its copy is a delivery");
}

static void StateNeverWithholdsOnEqualCounts()
{
	// The case a count-based duplicate test gets wrong. A transfer without a notification; the sender
	// publishes 51 before access is taken; another receiver's read lowers the count for an instant,
	// so the read under access sees 50 again and the SDK answers "nothing new". The copy holds the
	// NEW image. It must be delivered.
	FState S;
	S.BeginAttempt(true, 49, true);
	S.ObserveUnderAccess(true, 50);
	S.CompleteTransfer();
	Check(!S.BeginAttempt(false, 50, true) && S.ShouldTransferWithoutNotification(false), "a transfer without a notification is due");
	S.BeginFallbackTransfer();
	S.ObserveUnderAccess(false, 50); // the true count is 51; a foreign read made it look like 50
	Check(S.CompleteTransfer(), "the copy is delivered although every count involved is equal");
	// And the publication the transient hid is not lost: the SDK reports it at the next read.
	Check(S.BeginAttempt(true, 51, true), "the hidden publication is new at the next read");
}

// A long random schedule against a reference that knows the truth: every publication made before
// a completed copy must have been delivered by that copy or a later one, whatever the failures and
// transients in between. "Delivered" here means CompleteTransfer returned true after the publication.
static void StateRandomSchedule()
{
	std::uint32_t Seed = 12345;
	auto Next = [&Seed]() { Seed = Seed * 1664525u + 1013904223u; return Seed >> 8; };
	FState S;
	std::int64_t True = 100, SdkLast = 0, LastCopied = 0;
	int Undelivered = 0, Completed = 0, Transients = 0, Fallbacks = 0;
	// The SDK's GetNewFrame on a visible count: new when it differs from the last one it saw.
	auto Sdk = [&SdkLast](std::int64_t Visible, bool& bNew) { bNew = Visible != SdkLast; if (bNew) SdkLast = Visible; return Visible; };
	for (int Frame = 0; Frame < 200000; ++Frame)
	{
		if (Next() % 100 < 55) ++True;                       // the sender publishes between attempts
		const bool bTransientAtRead = Next() % 1000 < 3;
		bool bNew = false;
		const std::int64_t ReadCount = Sdk(True - (bTransientAtRead ? 1 : 0), bNew);
		bool bTransfer = S.BeginAttempt(bNew, ReadCount, true);
		if (!bTransfer && S.ShouldTransferWithoutNotification(false)) { S.BeginFallbackTransfer(); bTransfer = true; ++Fallbacks; }
		if (!bTransfer) continue;
		if (Next() % 100 < 4) continue;                      // access refused
		if (Next() % 100 < 30) ++True;                       // published between the read and the access: copied ahead
		const bool bTransientUnder = Next() % 1000 < 3;
		Transients += bTransientAtRead + bTransientUnder;
		const std::int64_t Under = Sdk(True - (bTransientUnder ? 1 : 0), bNew);
		S.ObserveUnderAccess(bNew, Under);
		const std::int64_t Image = True;                     // what the copy holds
		if (Next() % 100 < 3) continue;                      // the copy or its completion failed
		const bool bDelivered = S.CompleteTransfer();
		++Completed;
		if (!bDelivered) ++Undelivered;                      // a completed copy that was not delivered
		LastCopied = Image;
	}
	Equal(Undelivered, 0, "random schedule: every completed copy was delivered");
	Check(Completed > 100000 && Transients > 500 && Fallbacks > 1000, "random schedule: copies, transients and transfers without a notification all occurred");
	// Drain: with no further publication, the receiver must reach the newest image and then go quiet.
	for (int K = 0; K < 4; ++K)
	{
		bool bNew = false;
		const std::int64_t ReadCount = Sdk(True, bNew);
		bool bTransfer = S.BeginAttempt(bNew, ReadCount, true);
		if (!bTransfer && S.ShouldTransferWithoutNotification(false)) { S.BeginFallbackTransfer(); bTransfer = true; }
		if (!bTransfer) continue;
		const std::int64_t Under = Sdk(True, bNew);
		S.ObserveUnderAccess(bNew, Under);
		S.CompleteTransfer();
		LastCopied = True;
	}
	Equal(LastCopied, True, "random schedule: the newest publication was copied in the end");
	bool bNew = false;
	Check(!S.BeginAttempt((Sdk(True, bNew), bNew), True, true) && !S.ShouldTransferWithoutNotification(false), "random schedule: then nothing more is transferred");
}

// ------------------------------------------------------------------------------------------------
// Part 2: the predictor on made-up times.
// ------------------------------------------------------------------------------------------------
using SpoutFrameWait::EDecision;
using SpoutFrameWait::FPublishPredictor;
using SpoutFrameWait::FPrediction;
using SpoutFrameWait::FTuning;

// Feeds a regular sender observed closely: each publication K at Start + K * Period is seen at the
// previous count 50 us before it and at the new count at it.
static double FeedRegular(FPublishPredictor& P, double Start, double Period, std::int64_t FirstCount, int Publications)
{
	double T = Start;
	for (int K = 0; K < Publications; ++K)
	{
		T = Start + K * Period;
		P.ObserveCount(T - 0.00005, FirstCount + K - 1);
		P.ObserveCount(T, FirstCount + K);
	}
	return T;
}

static void PredictorTests()
{
	const double Period = 1.0 / 60.0;
	{
		FPublishPredictor P;
		Check(P.Predict(1.0, 0.001).Decision == EDecision::NoCount, "predictor: no count, no prediction");
		FeedRegular(P, 10.0, Period, 100, FTuning::MinPublications);   // 30 observations: 29 publications in the window
		Check(P.Predict(10.6, 0.001).Decision == EDecision::Warmup, "predictor: fewer than 30 publications in the window is warm-up");
		const double Last = FeedRegular(P, 10.0 + FTuning::MinPublications * Period, Period, 100 + FTuning::MinPublications, 12);
		const FPrediction Just = P.Predict(Last + Period - 0.0003, 0.001);
		Check(Just.Decision == EDecision::Wait, "predictor: a publication due in 0.3 ms is waited for with a 1 ms bound");
		Check(Just.PeriodSeconds > Period * 0.999 && Just.PeriodSeconds < Period * 1.001, "predictor: the period is estimated");
		Check(Just.EarliestSeconds <= Last + Period && Just.LatestSeconds >= Last + Period, "predictor: the window contains the true publication time");
		Check(P.Predict(Last + 0.004, 0.001).Decision == EDecision::NotImminent, "predictor: a publication due in 12 ms is not waited for");
		Check(P.Predict(Last + Period + 0.010, 0.001).Decision == EDecision::Overdue, "predictor: a publication 10 ms overdue is not waited for");
		// Another receiver's read: one below the last count. Ignored, counted, and the prediction is unchanged.
		P.ObserveCount(Last + 0.001, 100 + FTuning::MinPublications + 11 - 1);
		Equal(P.GetGlitches(), 1, "predictor: a count one below the last is a glitch");
		Check(P.Predict(Last + Period - 0.0003, 0.001).Decision == EDecision::Wait, "predictor: and changes nothing");
		// The count starting again resets what was learnt.
		P.ObserveCount(Last + 0.002, 3);
		Equal(P.GetResets(), 1, "predictor: a count far below the last resets the history");
		Check(P.Predict(Last + 0.003, 0.001).Decision == EDecision::Warmup, "predictor: after a reset it warms up again");
		// No count at all: nothing to predict.
		P.ObserveCount(Last + 0.004, -1);
		Check(P.Predict(Last + 0.005, 0.001).Decision == EDecision::NoCount, "predictor: a lost count means no prediction");
	}
	{
		// Two wasted waits in a row suspend waiting for 120 reads; reads are counted by BeginRead only.
		FPublishPredictor P;
		const double Last = FeedRegular(P, 10.0, Period, 100, 45);
		P.ObserveWaitTimedOut();
		Check(!P.IsSuspended(), "predictor: one wasted wait does not suspend");
		P.ObserveWaitTimedOut();
		Check(P.IsSuspended(), "predictor: two in a row suspend");
		Equal(P.GetSuspendedReadsLeft(), FTuning::SuspendReads, "predictor: for 120 reads");
		Check(P.Predict(Last + Period - 0.0003, 0.001).Decision == EDecision::Suspended, "predictor: suspended means no wait");
		for (int K = 0; K < 119; ++K) P.BeginRead();
		Check(P.IsSuspended(), "predictor: still suspended after 119 reads");
		P.ObserveCount(Last + 0.0001, 144); // observations are not reads
		Check(P.IsSuspended(), "predictor: an observation that is not a read does not shorten the suspension");
		P.BeginRead();
		Check(!P.IsSuspended(), "predictor: over after 120 reads");
		// The next suspension without a successful wait in between is twice as long.
		P.ObserveWaitTimedOut();
		P.ObserveWaitTimedOut();
		Equal(P.GetSuspendedReadsLeft(), 2 * FTuning::SuspendReads, "predictor: the second suspension is twice as long");
		Equal(P.GetSuspensions(), 2, "predictor: two suspensions");
	}
	{
		// A source at 30 fps read at 60 Hz is slower than the reader: not waited for.
		FPublishPredictor P;
		double Read = 10.0;
		for (int K = 0; K < 200; ++K) { P.BeginRead(Read); Read += Period; }
		const double Last = FeedRegular(P, 10.0, 2 * Period, 100, 45);
		Check(P.Predict(Last + 2 * Period - 0.0003, 0.001).Decision == EDecision::SlowerSource, "predictor: a 30 fps source read at 60 Hz is declined as slower");
		// The same source read at 30 Hz is not slower.
		FPublishPredictor Q;
		Read = 10.0;
		for (int K = 0; K < 200; ++K) { Q.BeginRead(Read); Read += 2 * Period; }
		const double LastQ = FeedRegular(Q, 10.0, 2 * Period, 100, 45);
		Check(Q.Predict(LastQ + 2 * Period - 0.0003, 0.001).Decision == EDecision::Wait, "predictor: the same source read at its own rate is waited for");
	}
}

// ------------------------------------------------------------------------------------------------
// Part 3: the bundled SDK, through its public calls only.
// ------------------------------------------------------------------------------------------------
static std::int64_t PublicCount(spoutFrameCount& Receiver)
{
	if (!Receiver.IsFrameCountEnabled()) return None;
	const long Count = Receiver.GetSenderFrame();
	return Count > 0 ? Count : None;
}

static int SdkTests()
{
	char Name[128];
	std::snprintf(Name, sizeof(Name), "SpoutNotifyTest_%lu_%llu", GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));
	spoutFrameCount Sender, Receiver;
	Sender.EnableFrameCount(Name);
	Receiver.EnableFrameCount(Name);
	if (!Receiver.IsFrameCountEnabled() || !Sender.IsFrameCountEnabled())
	{
		std::cout << "SKIPPED part 3: frame counting is disabled on this machine (SpoutSettings)" << std::endl;
		return 2;
	}

	// Before any publication the SDK answers "new" to every read and the count is zero: no count.
	Check(Receiver.GetNewFrame(), "sdk: before any publication a read is new");
	Check(Receiver.GetNewFrame(), "sdk: and so is the next one");
	Equal(Receiver.GetSenderFrame(), 0L, "sdk: with a count of zero");
	Equal(PublicCount(Receiver), None, "sdk: which the plugin takes as no count");

	// The state machine driven by the real calls: one publication, one notification, one copy.
	FState S;
	Sender.SetNewFrame();
	bool bNew = Receiver.GetNewFrame();
	Check(bNew, "sdk: a publication is new");
	Equal(Receiver.GetSenderFrame(), 1L, "sdk: the count after the first publication is 1");
	Check(S.BeginAttempt(bNew, PublicCount(Receiver), true), "sdk: owed");
	bNew = Receiver.GetNewFrame();
	Check(!bNew, "sdk: the read under access finds nothing new");
	S.ObserveUnderAccess(bNew, PublicCount(Receiver));
	Check(S.CompleteTransfer(), "sdk: delivered");
	bNew = Receiver.GetNewFrame();
	Check(!bNew && !S.BeginAttempt(bNew, PublicCount(Receiver), true), "sdk: the next read is quiet");

	// Copied ahead: published after the read, seen by the read under access, not reported again.
	Sender.SetNewFrame();                                   // 2
	bNew = Receiver.GetNewFrame();
	S.BeginAttempt(bNew, PublicCount(Receiver), true);
	Sender.SetNewFrame();                                   // 3, before access is taken
	bNew = Receiver.GetNewFrame();
	Check(bNew, "sdk: the read under access sees the publication made after the read");
	Equal(Receiver.GetSenderFrame(), 3L, "sdk: with count 3");
	S.ObserveUnderAccess(bNew, PublicCount(Receiver));
	S.CompleteTransfer();
	bNew = Receiver.GetNewFrame();
	Check(!bNew, "sdk: and the next read does not report it again");
	Check(!S.BeginAttempt(bNew, PublicCount(Receiver), true) && S.ShouldTransferWithoutNotification(false), "sdk: the count moved since the previous read: a transfer without a notification");

	// A wait: polls see nothing, then a publication.
	S.BeginFallbackTransfer();
	S.ObserveUnderAccess(Receiver.GetNewFrame(), PublicCount(Receiver));
	S.CompleteTransfer();
	bNew = Receiver.GetNewFrame();
	Check(!S.BeginAttempt(bNew, PublicCount(Receiver), true), "sdk: nothing new before the wait");
	bNew = Receiver.GetNewFrame();
	Check(S.ObservePoll(bNew, PublicCount(Receiver)) == EPoll::Nothing, "sdk: a poll without a publication is nothing");
	Sender.SetNewFrame();                                   // 4
	bNew = Receiver.GetNewFrame();
	Check(S.ObservePoll(bNew, PublicCount(Receiver)) == EPoll::Published, "sdk: a poll after a publication ends the wait");
	S.ObserveUnderAccess(Receiver.GetNewFrame(), PublicCount(Receiver));
	S.CompleteTransfer();

	// Another receiver reading the same sender, in its own SDK object. Each of its reads lowers the
	// semaphore for an instant. First as fast as it can: the count is then one low for most of the
	// time, which no real receiver does, and which shows what the count is worth as an identity.
	// With no publication at all, this receiver's reads must see the true count or one below it and
	// nothing else, and a wait must never take that for a publication.
	std::atomic<bool> bStop{false};
	std::atomic<int> PauseMicroseconds{0};
	std::atomic<long long> ForeignReads{0};
	std::thread Foreign([&]()
	{
		spoutFrameCount Other;
		Other.EnableFrameCount(Name);
		LARGE_INTEGER Frequency, Now, Until;
		QueryPerformanceFrequency(&Frequency);
		while (!bStop.load(std::memory_order_relaxed))
		{
			Other.GetNewFrame();
			ForeignReads.fetch_add(1, std::memory_order_relaxed);
			const int Pause = PauseMicroseconds.load(std::memory_order_relaxed);
			if (Pause > 0)
			{
				QueryPerformanceCounter(&Now);
				Until.QuadPart = Now.QuadPart + Frequency.QuadPart * Pause / 1000000;
				do { YieldProcessor(); QueryPerformanceCounter(&Now); } while (Now.QuadPart < Until.QuadPart);
			}
		}
		Other.CleanupFrameCount();
	});
	while (ForeignReads.load() < 1000) Sleep(1);
	const long TrueCount = 4;
	long long OneLow = 0, Wrong = 0, FalsePublications = 0;
	bNew = Receiver.GetNewFrame();
	S.BeginAttempt(bNew, TrueCount, true);                  // the baseline of the "wait" is the true count
	if (S.IsOwed()) { S.ObserveUnderAccess(false, TrueCount); S.CompleteTransfer(); S.BeginAttempt(false, TrueCount, true); }
	const int QuietPolls = 400000;
	for (int I = 0; I < QuietPolls; ++I)
	{
		bNew = Receiver.GetNewFrame();
		const long Count = Receiver.GetSenderFrame();
		if (Count == TrueCount - 1) ++OneLow;
		else if (Count != TrueCount) ++Wrong;
		if (S.ObservePoll(bNew, Count > 0 ? Count : None) == EPoll::Published) ++FalsePublications;
	}
	Equal(Wrong, 0LL, "sdk: beside another receiver a read sees the true count or one below it, nothing else");
	Equal(FalsePublications, 0LL, "sdk: and a wait never takes another receiver's read for a publication");
	std::cout << "INFO beside a receiver reading as fast as it can, the count was one low in " << OneLow << " of " << QuietPolls << " reads ("
		<< ForeignReads.load() << " foreign reads so far)" << std::endl;
	Check(OneLow > 0, "sdk: the one-low count was actually exercised");

	// The other receiver now reads every 50 microseconds: still far more often than any real one
	// (the fastest known polls at 1 kHz), and the count is one low for a small part of the time.
	// With publications, every one of them must become owed, and every copy is delivered. A read can
	// be hidden by the other receiver's read for as long as that receiver sits between its wait and
	// its release (longer than an instant if it is descheduled there). That delay belongs to the
	// SDK's protocol and is reported, not judged; what is judged is that the publication is noticed
	// as soon as the count shows it, and never lost.
	PauseMicroseconds = 50;
	Sleep(20);
	long long OneLowPaced = 0;
	for (int I = 0; I < 200000; ++I)
	{
		Receiver.GetNewFrame();
		if (Receiver.GetSenderFrame() == TrueCount - 1) ++OneLowPaced;
	}
	std::cout << "INFO beside a receiver reading every 50 us, the count was one low in " << OneLowPaced << " of 200000 reads" << std::endl;
	FState R;
	bNew = Receiver.GetNewFrame();
	R.BeginAttempt(bNew, PublicCount(Receiver), true);
	if (R.IsOwed()) { R.ObserveUnderAccess(Receiver.GetNewFrame(), PublicCount(Receiver)); R.CompleteTransfer(); }
	int Late = 0, Lost = 0, Undelivered = 0, MissedAfterVisible = 0;
	double LongestHiddenMs = 0.0;
	LARGE_INTEGER Frequency, Begin, Now;
	QueryPerformanceFrequency(&Frequency);
	const int Publications = 100000;
	long Expected = TrueCount;
	for (int I = 0; I < Publications; ++I)
	{
		Sender.SetNewFrame();
		++Expected;
		QueryPerformanceCounter(&Begin);
		int Attempts = 0;
		bool bOwed = false;
		double HiddenMs = 0.0;
		while (!bOwed && HiddenMs < 1000.0)
		{
			bNew = Receiver.GetNewFrame();
			const long Seen = Receiver.GetSenderFrame();
			bOwed = R.BeginAttempt(bNew, Seen > 0 ? Seen : None, true);
			++Attempts;
			// The publication is visible when the count reads as the number of publications so far.
			// A read that sees that and is still not owed would be a notification lost by this code.
			if (!bOwed && Seen == Expected && Attempts == 1) ++MissedAfterVisible;
			QueryPerformanceCounter(&Now);
			HiddenMs = 1000.0 * double(Now.QuadPart - Begin.QuadPart) / double(Frequency.QuadPart);
		}
		if (!bOwed) { ++Lost; continue; }
		if (Attempts > 1) { ++Late; if (HiddenMs > LongestHiddenMs) LongestHiddenMs = HiddenMs; }
		R.ObserveUnderAccess(Receiver.GetNewFrame(), PublicCount(Receiver));
		if (!R.CompleteTransfer()) ++Undelivered;
	}
	bStop = true;
	Foreign.join();
	Equal(Lost, 0, "sdk: beside a receiver reading every 50 us no publication is lost");
	Equal(MissedAfterVisible, 0, "sdk: a first read that sees the new count is always owed");
	Equal(Undelivered, 0, "sdk: and every copy is delivered");
	std::cout << "INFO " << Late << " of " << Publications << " publications were hidden from the first read by the other receiver's read; the longest stayed hidden for "
		<< LongestHiddenMs << " ms" << std::endl;
	Equal(Receiver.GetSenderFrame() >= TrueCount + Publications - 1, true, "sdk: the count ends at the number of publications (or one below, for an instant)");

	// Close and open, as FSpoutFrameSync does it (CleanupFrameCount, then EnableFrameCount): the SDK
	// object starts again, so the first read after the open is new although nothing was published,
	// and the count is still the sender's.
	Receiver.GetNewFrame();
	Check(!Receiver.GetNewFrame(), "sdk: before the reopen a second read is not new");
	const long CountBeforeReopen = Receiver.GetSenderFrame();
	Receiver.CleanupFrameCount();
	Equal(Receiver.GetSenderFrame(), 0L, "sdk: after the cleanup the object's count is zero");
	Receiver.EnableFrameCount(Name);
	Check(Receiver.GetNewFrame(), "sdk: the first read after the reopen is new");
	Equal(Receiver.GetSenderFrame(), CountBeforeReopen, "sdk: with the sender's count, which the semaphore kept");
	Check(!Receiver.GetNewFrame(), "sdk: and only once");

	// Counting switched off for this object: every read is new and there is no count.
	Receiver.DisableFrameCount();
	Check(Receiver.GetNewFrame() && Receiver.GetNewFrame(), "sdk: with counting disabled every read is new");
	Check(!Receiver.IsFrameCountEnabled(), "sdk: and the SDK says counting is off");
	Equal(PublicCount(Receiver), None, "sdk: so the plugin has no count and never waits");
	Sender.CleanupFrameCount();
	Receiver.CleanupFrameCount();
	return 0;
}

int main()
{
	StateWithSettingsOff();
	StateOrdinaryAndCopiedAhead();
	StateFailuresKeepOwed();
	StateCpuOrder();
	StateWaitPolls();
	StateCountUnavailable();
	StateForcedFrameRestartAndReopen();
	StateNeverWithholdsOnEqualCounts();
	StateRandomSchedule();
	const int AfterState = Checks;
	PredictorTests();
	const int AfterPredictor = Checks;
	const int Skipped = SdkTests();
	std::cout << "notification state checks " << AfterState << ", predictor checks " << (AfterPredictor - AfterState)
		<< ", SDK checks " << (Checks - AfterPredictor) << ", failures " << Failures << std::endl;
	if (Failures) return 1;
	return Skipped;
}
