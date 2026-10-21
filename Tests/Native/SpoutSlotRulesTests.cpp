// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
//
// Native tests of the shared-slot path's completion rules (SpoutSlotRules.h), compiled from the plugin's own header
// without Unreal and without a GPU.
//
//   part 1  every rule with expectations written by hand: reached values, removal, a failed signal with and without
//           a proof query, the send path's write guard, the timeout streak
//   part 2  a long seeded sequence of signals, completions, failures and timeouts on two simulated devices, checked
//           after every step: the set is never idle while any submitted operation can still run, and it becomes
//           idle once everything finished
#include "SpoutSlotRules.h"
#include <cstdint>
#include <iostream>
#include <vector>

using namespace SpoutSlotRules;

static int Failures = 0;
static int Checks = 0;
static void Check(bool Value, const char* Message)
{
	++Checks;
	if (!Value) { std::cout << "FAIL " << Message << std::endl; ++Failures; }
}

// ------------------------------------------------------------------------------------------------
// Part 1
// ------------------------------------------------------------------------------------------------
static void ReachedRules()
{
	Check(Reached(0, 0), "reached: nothing required is reached at once");
	Check(!Reached(4, 5), "reached: a smaller completed value has not reached");
	Check(Reached(5, 5) && Reached(9, 5), "reached: an equal or larger value has");
	Check(Reached(RemovedValue, 123456789), "reached: a removed device reaches every value");
}

static void NativeProgressRules()
{
	Check(NativeProgress(7, false, false) == 7, "native progress: an ordinary view value is the progress");
	Check(NativeProgress(RemovedValue, false, false) == 0, "native progress: Unreal's removal proves nothing about the native device");
	Check(NativeProgress(RemovedValue, false, true) == RemovedValue, "native progress: an event query on the native context proves its earlier work");
	Check(NativeProgress(0, true, false) == RemovedValue, "native progress: a removed native device runs nothing more");
	FOutstanding W;
	W.NativeSignalled(3);
	Check(!W.IsIdle(NativeProgress(RemovedValue, false, false), 0, false), "native progress: a set is not idle on Unreal's removal alone");
	Check(W.IsIdle(NativeProgress(RemovedValue, false, true), 0, false), "native progress: it is once the native query proves it");
}

static void OutstandingRules()
{
	FOutstanding W;
	Check(W.IsIdle(0, 0, false), "idle: a set that submitted nothing is idle");
	W.NativeSignalled(3);
	W.QueueSignalQueued(7);
	Check(!W.IsIdle(2, 7, false), "idle: not while the native fence is short of its last signal");
	Check(!W.IsIdle(3, 6, false), "idle: not while the queue fence is short of its last queued signal");
	Check(W.IsIdle(3, 7, false), "idle: once both fences reached their last values");
	W.NativeSignalled(2);
	W.QueueSignalQueued(5);
	Check(W.GetNativeRequired() == 3 && W.GetQueueRequired() == 7, "idle: an older value never lowers what is required");
	Check(W.IsIdle(RemovedValue, 7, false), "idle: a removed native fence counts as reached");
	Check(W.IsIdle(0, 7, true), "idle: a removed native device runs nothing more");
	Check(!W.IsIdle(0, 6, true), "idle: but the queue's own work must still have finished");

	FOutstanding F;
	F.NativeSignalled(4);
	F.NativeSignalFailed(true);
	Check(F.NeedsProofQuery(), "proof: a failed signal with a query issued waits for the query");
	Check(!F.IsIdle(4, 0, false), "proof: the fence alone does not prove the copy whose signal failed");
	Check(!F.CanQueueWrite(4, false), "proof: and the queue may not write the slot meanwhile");
	F.ProofCompleted();
	Check(F.GetProof() == EProof::Proven && !F.NeedsProofQuery(), "proof: the query's report proves it");
	Check(F.IsIdle(4, 0, false) && F.CanQueueWrite(4, false), "proof: then the set is idle and the slot writable");

	FOutstanding G;
	G.NativeSignalFailed(false);
	Check(G.GetProof() == EProof::Impossible, "proof: without a query nothing but removal proves it");
	Check(!G.IsIdle(RemovedValue, RemovedValue, false), "proof: not even fences at the removed value, while the native device is not removed");
	Check(G.IsIdle(0, 0, true), "proof: native device removal proves it");
	G.NativeSignalFailed(true);
	Check(G.GetProof() == EProof::Impossible, "proof: a later query does not weaken an impossible proof");
	G.ProofCompleted();
	Check(G.GetProof() == EProof::Impossible, "proof: nor does a report for a later query");

	FOutstanding H;
	H.NativeSignalFailed(true);
	H.NativeSignalFailed(false);
	Check(H.GetProof() == EProof::Impossible, "proof: a pending proof becomes impossible when a second failure has no query");

	FOutstanding S;
	Check(S.CanQueueWrite(0, false), "send guard: a fresh slot may be written");
	S.NativeSignalled(2);
	Check(!S.CanQueueWrite(1, false), "send guard: not while a native copy out of the slot may still run");
	Check(S.CanQueueWrite(2, false), "send guard: once it finished");
	Check(!S.CanQueueWrite(2, true), "send guard: never on a removed native device");
}

static void TimeoutStreak()
{
	FTimeoutStreak T;
	for (int I = 1; I < MaxConsecutiveTimeouts; ++I)
	{
		Check(!T.OnTimeout(), "streak: fewer timeouts than the limit keep the stream");
	}
	Check(T.OnTimeout(), "streak: the limit stops it");
	FTimeoutStreak U;
	for (int I = 1; I < MaxConsecutiveTimeouts; ++I) U.OnTimeout();
	U.OnSuccess();
	Check(U.GetCount() == 0 && !U.OnTimeout(), "streak: a success starts the count again");
}

// ------------------------------------------------------------------------------------------------
// Part 2: a model with two devices. Each device executes its submitted operations in order; a signal sets its
// fence when it executes. The model's truth is "an operation is still pending"; FOutstanding must never call the
// set idle while one is.
// ------------------------------------------------------------------------------------------------
struct FModelOp
{
	bool bSignal = false;
	std::uint64_t Value = 0;
};

struct FModelDevice
{
	std::vector<FModelOp> Pending;   // submitted, not yet executed, in order
	std::uint64_t Completed = 0;     // the fence
	bool bRemoved = false;
	std::uint64_t Next = 0;
	void ExecuteOne()
	{
		if (bRemoved || Pending.empty()) return;
		const FModelOp Op = Pending.front();
		Pending.erase(Pending.begin());
		if (Op.bSignal) Completed = Op.Value;
	}
	std::uint64_t Fence() const { return bRemoved ? RemovedValue : Completed; }
};

static std::uint32_t Rng = 1;
static std::uint32_t NextRandom()
{
	Rng = Rng * 1664525u + 1013904223u;
	return Rng >> 8;
}

static void ModelRun(std::uint32_t Seed, int Steps)
{
	Rng = Seed;
	FModelDevice Native, Queue;
	FOutstanding W;
	bool bProofQueryPending = false;   // the model's event query: done when everything submitted before it has executed
	std::size_t ProofMark = 0;         // how many native operations were submitted before the query
	std::size_t NativeSubmittedTotal = 0, NativeExecutedTotal = 0;
	int Violations = 0;
	for (int Step = 0; Step < Steps; ++Step)
	{
		const std::uint32_t R = NextRandom() % 100;
		if (R < 30)
		{
			// A native copy, then its signal; the signal fails 3 times in 100.
			Native.Pending.push_back(FModelOp());
			++NativeSubmittedTotal;
			const bool bSignalFails = (NextRandom() % 100) < 3;
			if (!bSignalFails)
			{
				const std::uint64_t V = ++Native.Next;
				Native.Pending.push_back(FModelOp{true, V});
				++NativeSubmittedTotal;
				W.NativeSignalled(V);
			}
			else
			{
				const bool bQuery = (NextRandom() % 4) != 0;
				W.NativeSignalFailed(bQuery);
				if (bQuery && W.NeedsProofQuery())
				{
					bProofQueryPending = true;
					ProofMark = NativeSubmittedTotal;
				}
			}
		}
		else if (R < 55)
		{
			Queue.Pending.push_back(FModelOp());
			const std::uint64_t V = ++Queue.Next;
			Queue.Pending.push_back(FModelOp{true, V});
			W.QueueSignalQueued(V);
		}
		else if (R < 75)
		{
			if (!Native.Pending.empty() && !Native.bRemoved) { Native.ExecuteOne(); ++NativeExecutedTotal; }
		}
		else if (R < 95)
		{
			Queue.ExecuteOne();
		}
		else if (R == 99 && (NextRandom() % 50) == 0)
		{
			Native.bRemoved = true;
		}

		if (bProofQueryPending && NativeExecutedTotal >= ProofMark)
		{
			W.ProofCompleted();
			bProofQueryPending = false;
		}
		const bool bTrulyIdle = (Native.bRemoved || Native.Pending.empty()) && Queue.Pending.empty();
		const bool bSaysIdle = W.IsIdle(Native.Fence(), Queue.Fence(), Native.bRemoved);
		if (bSaysIdle && !bTrulyIdle) ++Violations;
	}
	Check(Violations == 0, "model: the set is never called idle while an operation can still run");

	// Drain: everything executes; the set must then be idle unless a native failure had no proof and the device lives.
	while (!Native.bRemoved && !Native.Pending.empty()) { Native.ExecuteOne(); ++NativeExecutedTotal; }
	while (!Queue.Pending.empty()) Queue.ExecuteOne();
	if (bProofQueryPending) W.ProofCompleted();
	const bool bExpectIdle = Native.bRemoved || W.GetProof() != EProof::Impossible;
	Check(W.IsIdle(Native.Fence(), Queue.Fence(), Native.bRemoved) == bExpectIdle,
		"model: after everything executed the set is idle, unless an unprovable failure on a live device keeps it");
}

int main()
{
	ReachedRules();
	NativeProgressRules();
	OutstandingRules();
	TimeoutStreak();
	const int RuleChecks = Checks;
	for (std::uint32_t Seed : {20261014u, 7u, 99u, 123457u})
	{
		ModelRun(Seed, 200000);
	}
	const int ModelChecks = Checks - RuleChecks;
	std::cout << "slot rule checks " << RuleChecks << ", model checks " << ModelChecks << ", failures " << Failures << std::endl;
	return Failures ? 1 : 0;
}
