// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
//
// Native tests of the asynchronous render-target sender's bookkeeping (SpoutSendRing.h), compiled from the plugin's
// own header without Unreal and without a GPU.
//
//   part 1  every outcome and every refusal, with expectations written by hand: accept, publish, supersede, abandon
//           (newer frame, deadline, one attempt), the four failures, no free slot, delayed fill completion, delayed
//           drain proof, identity change with work outstanding, budget refusal and retry, flush and stop resolution,
//           a restarted stream, and every broken rule
//   part 2  long seeded runs of a model render thread, worker, Unreal queue, native device and receiver against the
//           ring, checked after every step: the identity Accepted = outcomes + in flight, frames resolved once and in
//           order, a slot never written while its frame is unresolved, announcements in fill order, bounded frames
#include "SpoutSendRing.h"
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iostream>
#include <random>
#include <vector>

using namespace SpoutSendRing;

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
		std::cout << "FAIL " << Message << ": got " << static_cast<long long>(Actual) << ", expected " << static_cast<long long>(Expected) << std::endl;
		++Failures;
	}
}
static std::int64_t Out(const FRing& R, EOutcome O) { return R.GetCounters().Outcomes[static_cast<int>(O)]; }
static std::int64_t Refused(const FRing& R, ENotAccepted W) { return R.GetCounters().NotAccepted[static_cast<int>(W)]; }
static bool IdentityHolds(const FRing& R)
{
	const FCounters& C = R.GetCounters();
	return C.InFlight() == static_cast<std::int64_t>(R.GetFrames().size()) && C.InFlight() >= 0;
}

// ------------------------------------------------------------------------------------------------
// Part 1: the ring by hand.
// ------------------------------------------------------------------------------------------------
static void Publishes()
{
	FRing R(2);
	ENotAccepted Why = ENotAccepted::Count;
	Equal(R.GetSlotsPerSet(), 2, "publish: two slots per set");
	Equal(R.FindFreeSlot(Why), -1, "publish: no slot before the first set");
	Check(Why == ENotAccepted::NoSlot, "publish: and the reason is no slot");
	R.BeginSet(1);
	const int S = R.FindFreeSlot(Why);
	Equal(S, 0, "publish: the first send gets slot 0");
	const std::uint64_t Id = R.Accept(S, 11, 0, 1.0);
	Equal(Id, 1, "publish: the first accepted frame has id 1");
	Equal(R.GetCounters().Accepted, 1, "publish: one accepted");
	Check(IdentityHolds(R), "publish: identity after acceptance (one in flight)");
	Equal(R.FindFreeSlot(Why), 1, "publish: the next send gets the other slot");
	const FFrame* F = R.Oldest();
	Check(F && F->Id == 1 && F->Slot == 0 && F->FillValue == 11 && F->SetGeneration == 1 && F->ReadyAt < 0.0, "publish: the frame records its slot, fill value and set");
	R.MarkReady(Id, 1.004);
	Check(R.Oldest()->ReadyAt == 1.004, "publish: ready when its fill was observed finished");
	Check(NextStep(*R.Oldest(), 1.004, false, 0.1) == EStep::Attempt, "publish: a ready frame with nothing newer attempts access");
	R.NoteAttempt(Id, true);
	R.BeginDrain(Id);
	Check(R.Oldest()->bDraining, "publish: draining once its copy out of the slot was submitted");
	Equal(R.FindFreeSlot(Why), 1, "publish: its slot is not free while it drains");
	Check(R.Resolve(Id, EOutcome::Published), "publish: resolved published after the drain was proven");
	Equal(Out(R, EOutcome::Published), 1, "publish: one published");
	Equal(R.GetLastPublishedId(), 1, "publish: the last published frame");
	Equal(R.GetCounters().Attempts, 1, "publish: one attempt");
	Equal(R.GetCounters().FailedAttempts, 0, "publish: none failed");
	Equal(R.FindFreeSlot(Why), 0, "publish: slot 0 is free again");
	Check(R.IsEmpty() && IdentityHolds(R), "publish: nothing in flight");
	Equal(R.GetCounters().RuleViolations, 0, "publish: no rule broken");
}

static void SupersedesAndAbandons()
{
	// A newer frame ready before the older made an attempt: the older is superseded.
	{
		FRing R(2);
		ENotAccepted Why;
		R.BeginSet(1);
		const std::uint64_t A = R.Accept(R.FindFreeSlot(Why), 1, 0, 0.0);
		const std::uint64_t B = R.Accept(R.FindFreeSlot(Why), 2, 0, 0.016);
		Equal(R.FindFreeSlot(Why), -1, "supersede: both slots in use");
		Check(Why == ENotAccepted::NoSlot, "supersede: a third send finds no slot");
		R.NotAccepted(Why);
		Equal(Refused(R, ENotAccepted::NoSlot), 1, "supersede: counted as not accepted (no slot)");
		R.MarkReady(A, 0.010);
		R.MarkReady(B, 0.020);
		Check(NextStep(*R.Oldest(), 0.020, true, 0.1) == EStep::Supersede, "supersede: the older frame, no attempt made, is superseded");
		Check(R.Resolve(A, EOutcome::Superseded), "supersede: resolved");
		Check(NextStep(*R.Oldest(), 0.020, false, 0.1) == EStep::Attempt, "supersede: the newer one attempts");
		Equal(R.FindFreeSlot(Why), 0, "supersede: the superseded frame's slot is free at once (nothing read it)");
		Check(IdentityHolds(R), "supersede: identity");
	}
	// A failed attempt, then a newer frame ready: abandoned (not superseded).
	{
		FRing R(2);
		ENotAccepted Why;
		R.BeginSet(1);
		const std::uint64_t A = R.Accept(R.FindFreeSlot(Why), 1, 0, 0.0);
		R.MarkReady(A, 0.001);
		R.NoteAttempt(A, false);
		Equal(R.Oldest()->FailedAttempts, 1, "abandon: one failed attempt recorded on the frame");
		Check(NextStep(*R.Oldest(), 0.003, false, 0.1) == EStep::Attempt, "abandon: before its deadline it tries again");
		const std::uint64_t B = R.Accept(R.FindFreeSlot(Why), 2, 0, 0.016);
		R.MarkReady(B, 0.017);
		Check(NextStep(*R.Oldest(), 0.017, true, 0.1) == EStep::Abandon, "abandon: a newer frame is ready after a failed attempt");
		Check(!R.Resolve(A, EOutcome::Superseded), "abandon: a frame that made a failed attempt cannot be superseded");
		Equal(R.GetCounters().RuleViolations, 1, "abandon: that was a broken rule");
		Check(R.Resolve(A, EOutcome::AccessAbandoned), "abandon: resolved abandoned");
		Equal(R.GetCounters().FailedAttempts, 1, "abandon: failed attempts stay counted apart from abandoned frames");
		Equal(Out(R, EOutcome::AccessAbandoned), 1, "abandon: one frame abandoned");
	}
	// The deadline: abandoned 100 ms after it became ready, only once an attempt failed.
	{
		FRing R(2);
		ENotAccepted Why;
		R.BeginSet(1);
		const std::uint64_t A = R.Accept(R.FindFreeSlot(Why), 1, 0, 0.0);
		R.MarkReady(A, 0.010);
		Check(NextStep(*R.Oldest(), 0.200, false, 0.1) == EStep::Attempt, "deadline: a frame that made no attempt always gets one");
		R.NoteAttempt(A, false);
		Check(NextStep(*R.Oldest(), 0.1099, false, 0.1) == EStep::Attempt, "deadline: before ready + 100 ms it tries again");
		Check(NextStep(*R.Oldest(), 0.1100, false, 0.1) == EStep::Abandon, "deadline: at ready + 100 ms it is abandoned");
		R.NoteAttempt(A, false);
		R.NoteAttempt(A, true);
		R.BeginDrain(A);
		Check(!R.Resolve(A, EOutcome::AccessAbandoned), "deadline: a draining frame cannot be abandoned");
		Check(R.Resolve(A, EOutcome::Published), "deadline: a frame that got access after failed attempts publishes");
		Equal(R.GetCounters().Attempts, 3, "deadline: three attempts");
		Equal(R.GetCounters().FailedAttempts, 2, "deadline: two failed");
	}
}

static void Failures4()
{
	FRing R(2);
	ENotAccepted Why;
	R.BeginSet(1);
	// A fill that did not finish within its bound.
	const std::uint64_t A = R.Accept(R.FindFreeSlot(Why), 1, 0, 0.0);
	Check(R.Resolve(A, EOutcome::FailedFill), "fail: a frame whose fill did not finish fails without being ready");
	// An installation refused (the shared texture or the registration).
	const std::uint64_t B = R.Accept(R.FindFreeSlot(Why), 2, 0, 1.0);
	R.MarkReady(B, 1.001);
	Check(R.Resolve(B, EOutcome::FailedInstall), "fail: installation refused");
	// A drain whose signal failed: proven by the event query, then failed.
	const std::uint64_t C = R.Accept(R.FindFreeSlot(Why), 3, 0, 2.0);
	R.MarkReady(C, 2.001);
	R.NoteAttempt(C, true);
	R.BeginDrain(C);
	Check(R.Resolve(C, EOutcome::FailedDrain), "fail: a drain whose signal failed");
	// The native device removed while it drained.
	const std::uint64_t D = R.Accept(R.FindFreeSlot(Why), 4, 0, 3.0);
	R.MarkReady(D, 3.001);
	R.NoteAttempt(D, true);
	R.BeginDrain(D);
	Check(R.Resolve(D, EOutcome::FailedRemoved), "fail: native device removed");
	Equal(Out(R, EOutcome::FailedFill) + Out(R, EOutcome::FailedInstall) + Out(R, EOutcome::FailedDrain) + Out(R, EOutcome::FailedRemoved), 4, "fail: four failures, one of each");
	Equal(R.GetLastPublishedId(), 0, "fail: nothing published");
	Check(IdentityHolds(R) && R.IsEmpty(), "fail: identity, nothing in flight");
	Equal(R.GetCounters().RuleViolations, 0, "fail: no rule broken");
}

static void DelayedCompletions()
{
	// A delayed fill: the frame cannot attempt access, the render thread keeps its slot, and a later frame may still
	// be accepted into the other slot without waiting.
	FRing R(2);
	ENotAccepted Why;
	R.BeginSet(1);
	const std::uint64_t A = R.Accept(R.FindFreeSlot(Why), 1, 0, 0.0);
	R.NoteAttempt(A, false);
	Equal(R.GetCounters().RuleViolations, 1, "delayed fill: an attempt before the fill was observed is a broken rule");
	Equal(R.GetCounters().Attempts, 0, "delayed fill: and not counted as an attempt");
	R.BeginDrain(A);
	Equal(R.GetCounters().RuleViolations, 2, "delayed fill: so is a drain");
	const std::uint64_t B = R.Accept(R.FindFreeSlot(Why), 2, 0, 0.016);
	Check(B == 2, "delayed fill: the next send uses the other slot without waiting");
	Equal(R.FindFreeSlot(Why), -1, "delayed fill: the third finds no slot");
	// A delayed drain proof: the frame keeps its slot until resolved; the render thread never waits for it.
	R.MarkReady(A, 0.5);
	R.NoteAttempt(A, true);
	R.BeginDrain(A);
	R.MarkReady(A, 0.6);
	Equal(R.GetCounters().RuleViolations, 3, "delayed drain: a draining frame cannot be marked ready again");
	Equal(R.FindFreeSlot(Why), -1, "delayed drain: no slot while its proof is outstanding");
	Equal(R.Accept(0, 9, 0, 0.7), 0, "delayed drain: a fill into the draining frame's slot is refused");
	Equal(R.GetCounters().RuleViolations, 4, "delayed drain: as a broken rule");
	Check(!R.Resolve(B, EOutcome::Superseded), "order: only the oldest frame may be resolved");
	Equal(R.GetCounters().RuleViolations, 5, "order: as a broken rule");
	Check(R.Resolve(A, EOutcome::Published), "delayed drain: published once proven");
	Equal(R.FindFreeSlot(Why), 0, "delayed drain: its slot is free after that");
	Check(IdentityHolds(R), "delayed: identity");
}

static void IdentityChange()
{
	FRing R(2);
	ENotAccepted Why;
	R.BeginSet(1);
	const std::uint64_t A = R.Accept(R.FindFreeSlot(Why), 1, 0, 0.0);
	const std::uint64_t B = R.Accept(R.FindFreeSlot(Why), 2, 0, 0.016);
	// A resize with both frames owed: a new set, its slots free at once; the old set stays in use.
	R.BeginSet(2);
	Equal(R.GetCurrentSet(), 2, "resize: the new set is current");
	Equal(R.FindFreeSlot(Why), 0, "resize: the new set's slot 0 is free");
	const std::uint64_t C = R.Accept(0, 3, 0, 0.033);
	Check(R.Find(C)->SetGeneration == 2, "resize: the new frame belongs to the new set");
	Check(R.SetInUse(1) && R.SetInUse(2), "resize: both sets in use");
	R.BeginSet(2);
	Equal(R.GetCounters().RuleViolations, 1, "resize: generations only grow");
	R.MarkReady(A, 0.01); R.MarkReady(B, 0.02); R.MarkReady(C, 0.04);
	// The old frames end in order; the newest of the new identity supersedes them.
	Check(NextStep(*R.Oldest(), 0.04, true, 0.1) == EStep::Supersede && R.Resolve(A, EOutcome::Superseded), "resize: old frame A superseded");
	Check(NextStep(*R.Oldest(), 0.04, true, 0.1) == EStep::Supersede && R.Resolve(B, EOutcome::Superseded), "resize: old frame B superseded");
	Check(!R.SetInUse(1), "resize: the old set is no longer in use and may be retired");
	R.NoteAttempt(C, true); R.BeginDrain(C);
	Check(R.Resolve(C, EOutcome::Published), "resize: the new identity's frame publishes");
	// The budget refuses the next identity: the owner ends the current set; no slot is free; the next send retries.
	R.EndSet();
	Equal(R.GetCurrentSet(), 0, "budget: no current set after a refusal");
	Equal(R.FindFreeSlot(Why), -1, "budget: no slot");
	R.NotAccepted(ENotAccepted::Budget);
	R.NotAccepted(ENotAccepted::Budget);
	Equal(Refused(R, ENotAccepted::Budget), 2, "budget: each refused send counted, none latched");
	R.BeginSet(3);
	Equal(R.FindFreeSlot(Why), 0, "budget: the retry that got its reservation has a slot");
	Check(R.Accept(0, 4, 0, 0.2) == 4, "budget: and accepts");
	Check(IdentityHolds(R), "resize: identity");
}

static void FlushAndStop()
{
	FRing R(2);
	ENotAccepted Why;
	R.BeginSet(1);
	const std::uint64_t A = R.Accept(R.FindFreeSlot(Why), 1, 0, 0.0);
	R.MarkReady(A, 0.001);
	// A flush: every frame accepted so far gets one attempt.
	const std::uint64_t UpTo = R.LimitToOneAttempt();
	Equal(UpTo, A, "flush: owed up to the last accepted frame");
	Check(!R.ResolvedUpTo(UpTo), "flush: not resolved yet");
	Check(NextStep(*R.Oldest(), 0.002, false, 0.1) == EStep::Attempt, "flush: the owed frame gets its attempt");
	R.NoteAttempt(A, false);
	Check(NextStep(*R.Oldest(), 0.003, false, 0.1) == EStep::Abandon, "flush: one failed attempt abandons it before its deadline");
	Check(R.Resolve(A, EOutcome::AccessAbandoned), "flush: abandoned");
	Check(R.ResolvedUpTo(UpTo), "flush: the flush's frames are resolved");
	// A frame accepted after the flush keeps the normal bound.
	const std::uint64_t B = R.Accept(R.FindFreeSlot(Why), 2, 0, 0.1);
	R.MarkReady(B, 0.101);
	R.NoteAttempt(B, false);
	Check(NextStep(*R.Oldest(), 0.102, false, 0.1) == EStep::Attempt, "flush: a later frame is not limited");
	// A stop: nothing more accepted; owed frames get one attempt.
	R.BeginStop();
	Check(R.IsStopping(), "stop: stopping");
	Equal(R.FindFreeSlot(Why), -1, "stop: no slot");
	Check(Why == ENotAccepted::Stopping, "stop: because the stream stops");
	Equal(R.Accept(1, 3, 0, 0.2), 0, "stop: an acceptance while stopping is refused");
	Equal(R.GetCounters().RuleViolations, 1, "stop: as a broken rule");
	Check(NextStep(*R.Oldest(), 0.103, false, 0.1) == EStep::Abandon, "stop: the owed frame, already failed once, is abandoned");
	Check(R.Resolve(B, EOutcome::AccessAbandoned), "stop: resolved");
	Check(R.IsEmpty() && R.ResolvedUpTo(R.GetLastAcceptedId()), "stop: nothing owed; the worker may exit");
	Check(IdentityHolds(R), "stop: identity");
	// A restarted stream (same name) is a new ring: it starts from nothing and owes nothing of the old one.
	FRing Restarted(2);
	Check(Restarted.IsEmpty() && Restarted.GetLastAcceptedId() == 0 && Restarted.GetCounters().Accepted == 0, "restart: a new stream owes nothing");
}

static void BrokenRules()
{
	FRing R(2);
	ENotAccepted Why;
	R.BeginSet(1);
	Equal(R.Accept(-1, 1, 0, 0.0), 0, "rules: a negative slot");
	Equal(R.Accept(2, 1, 0, 0.0), 0, "rules: a slot beyond the set");
	const std::uint64_t A = R.Accept(R.FindFreeSlot(Why), 1, 0, 0.0);
	Equal(R.Accept(0, 2, 0, 0.0), 0, "rules: a slot in use");
	R.MarkReady(A, 0.01);
	Check(!R.Resolve(A, EOutcome::Published), "rules: published without a drain");
	Check(!R.Resolve(A, EOutcome::AccessAbandoned), "rules: abandoned without a failed attempt");
	Check(!R.Resolve(A, EOutcome::None), "rules: no outcome");
	Check(!R.Resolve(99, EOutcome::FailedFill), "rules: an unknown frame");
	R.MarkReady(99, 0.0);
	R.NoteAttempt(99, true);
	R.BeginDrain(99);
	Equal(R.GetCounters().RuleViolations, 10, "rules: ten broken rules, each counted");
	Check(IdentityHolds(R), "rules: nothing broken changed the account");
	Equal(R.GetCounters().Accepted, 1, "rules: one frame accepted");
	FRing Clamped(9);
	Equal(Clamped.GetSlotsPerSet(), MaxSlots, "rules: the slot count is clamped");
}

// ------------------------------------------------------------------------------------------------
// Part 2: a model of the two devices, the threads and a receiver, against the ring.
// ------------------------------------------------------------------------------------------------
namespace
{
	struct FModelFrame
	{
		std::uint64_t Id = 0;
		std::uint64_t Set = 0;
		int Slot = -1;
		double FillDoneAt = 0.0;   // when the Unreal queue finishes the fill (infinite: stalls)
		double DrainDoneAt = -1.0; // when the native copy out of the slot finishes
		bool bDrainSignalFails = false;
	};
}

static void ModelRun(unsigned Seed, int Steps)
{
	std::mt19937 Rng(Seed);
	auto Uniform = [&Rng](double Lo, double Hi) { return std::uniform_real_distribution<double>(Lo, Hi)(Rng); };
	auto Percent = [&Rng]() { return static_cast<int>(Rng() % 100u); };

	FRing R(2);
	std::uint64_t Set = 0;
	std::uint64_t FillValue = 0;
	double Now = 0.0;
	double QueueFreeAt = 0.0;          // the Unreal queue runs fills in order
	double ReceiverHoldsUntil = -1.0;  // a peer holds the access mutex until then
	std::vector<FModelFrame> Model;    // the model's view of unresolved frames
	std::vector<std::uint64_t> Published;
	// What each slot of each set is: the frame whose fill was last queued into it, unresolved or not.
	std::int64_t WroteUnresolved = 0, ResolvedTwice = 0, OutOfOrder = 0, PublishedUndrained = 0, IdentityBroken = 0, ReadBeforeFilled = 0;
	std::int64_t MaxInFlight = 0, Flushes = 0, Stops = 0, Resizes = 0, BudgetRefusals = 0, Stalls = 0;
	// The retained-memory budget, counted in slot sets: a new set is refused while this many are still reserved
	// (sets with unresolved frames, which keep their reservation until retired, plus the current one).
	const int BudgetSets = 3;
	auto SetsReserved = [&R](std::uint64_t Newest)
	{
		int N = R.GetCurrentSet() != 0 ? 1 : 0;
		for (std::uint64_t G = 1; G <= Newest; ++G) if (G != R.GetCurrentSet() && R.SetInUse(G)) ++N;
		return N;
	};
	const double Deadline = 0.1;
	FCounters Total;  // summed over every ring of the run (a stop starts a new one)
	auto AddTotal = [&Total](const FCounters& C)
	{
		Total.Accepted += C.Accepted;
		for (int I = 0; I < static_cast<int>(EOutcome::Count); ++I) Total.Outcomes[I] += C.Outcomes[I];
		for (int I = 0; I < static_cast<int>(ENotAccepted::Count); ++I) Total.NotAccepted[I] += C.NotAccepted[I];
		Total.Attempts += C.Attempts;
		Total.FailedAttempts += C.FailedAttempts;
	};

	auto FindModel = [&Model](std::uint64_t Id) -> FModelFrame*
	{
		for (FModelFrame& M : Model) if (M.Id == Id) return &M;
		return nullptr;
	};
	auto Resolve = [&](std::uint64_t Id, EOutcome O)
	{
		FModelFrame* M = FindModel(Id);
		if (!M) { ++ResolvedTwice; return; }
		if (!R.Resolve(Id, O)) { ++OutOfOrder; return; }
		if (O == EOutcome::Published)
		{
			if (M->DrainDoneAt < 0.0 || M->DrainDoneAt > Now) ++PublishedUndrained;
			if (!Published.empty() && Published.back() >= Id) ++OutOfOrder;
			Published.push_back(Id);
		}
		Model.erase(Model.begin());
	};

	for (int Step = 0; Step < Steps; ++Step)
	{
		Now += Uniform(0.0005, 0.004);
		const int P = Percent();
		if (P < 28)
		{
			// Render thread: a send. Sometimes the source's size changes; sometimes the budget refuses the new set.
			if (Set == 0 || Percent() < 2)
			{
				if (Set != 0) { R.EndSet(); ++Resizes; }
			}
			if (R.GetCurrentSet() == 0)
			{
				if (SetsReserved(Set) >= BudgetSets)
				{
					// Older sets still hold their reservations: the new one does not fit until one is retired. Counted
					// and retried on the next send, never latched.
					R.NotAccepted(ENotAccepted::Budget);
					++BudgetRefusals;
					continue;
				}
				R.BeginSet(++Set);
			}
			ENotAccepted Why;
			const int S = R.FindFreeSlot(Why);
			if (S < 0) { R.NotAccepted(Why); continue; }
			for (const FModelFrame& M : Model) if (M.Set == Set && M.Slot == S) ++WroteUnresolved;
			FModelFrame M;
			M.Set = Set;
			M.Slot = S;
			const bool bStall = Percent() < 1;
			Stalls += bStall;
			QueueFreeAt = (QueueFreeAt > Now ? QueueFreeAt : Now) + (bStall ? 1.5 : Uniform(0.001, 0.012));
			M.FillDoneAt = QueueFreeAt;
			M.Id = R.Accept(S, ++FillValue, 0, Now);
			if (M.Id == 0) { ++IdentityBroken; continue; }
			Model.push_back(M);
		}
		else if (P < 34)
		{
			// A receiver takes the access mutex for a while.
			if (ReceiverHoldsUntil < Now) ReceiverHoldsUntil = Now + Uniform(0.0005, Percent() < 3 ? 0.2 : 0.008);
		}
		else if (P < 35)
		{
			// A flush: frames accepted so far get one attempt; the model's worker resolves them in the next steps.
			R.LimitToOneAttempt();
			++Flushes;
		}
		else if (P < 35 + 1 && Percent() < 5)
		{
			// The stream stops and a new one starts with the same name (Close, then Send): the old ring resolves what it
			// owes, here at once with one attempt each; the model then continues with a new ring.
			R.BeginStop();
			++Stops;
			int Guard = 0;
			while (!R.IsEmpty() && Guard++ < 100)
			{
				FFrame& F = *R.Oldest();
				FModelFrame& M = *FindModel(F.Id);
				if (F.bDraining)
				{
					// A drain in progress: its proof is waited for (the stop's only unbounded class).
					if (M.DrainDoneAt > Now) Now = M.DrainDoneAt;
					Resolve(F.Id, M.bDrainSignalFails ? EOutcome::FailedDrain : EOutcome::Published);
					continue;
				}
				if (F.ReadyAt < 0.0)
				{
					if (M.FillDoneAt - F.AcceptedAt > 1.0) { Now = (Now > F.AcceptedAt + 1.0 ? Now : F.AcceptedAt + 1.0); Resolve(F.Id, EOutcome::FailedFill); continue; }
					if (M.FillDoneAt > Now) Now = M.FillDoneAt;
					R.MarkReady(F.Id, Now);
				}
				const EStep Step2 = NextStep(F, Now, false, Deadline);
				if (Step2 == EStep::Abandon) { Resolve(F.Id, EOutcome::AccessAbandoned); continue; }
				const bool bGranted = ReceiverHoldsUntil < Now;
				R.NoteAttempt(F.Id, bGranted);
				if (!bGranted) { Now += 0.002; continue; }
				R.BeginDrain(F.Id);
				M.DrainDoneAt = Now + Uniform(0.0002, 0.003);
				Now = M.DrainDoneAt;
				Resolve(F.Id, EOutcome::Published);
			}
			if (!IdentityHolds(R) || !R.IsEmpty()) ++IdentityBroken;
			const FCounters C = R.GetCounters();
			MaxInFlight = MaxInFlight > C.InFlight() ? MaxInFlight : C.InFlight();
			// Checks of the stopped ring before it goes, then a fresh one.
			std::int64_t Resolved = 0;
			for (int I = 1; I < static_cast<int>(EOutcome::Count); ++I) Resolved += C.Outcomes[I];
			if (C.Accepted != Resolved) ++IdentityBroken;
			if (C.RuleViolations != 0) ++IdentityBroken;
			AddTotal(C);
			R.~FRing();
			new (&R) FRing(2);
			Set = 0;
			Model.clear();
			Published.clear();
			continue;
		}
		else
		{
			// The worker: one step for the oldest frame.
			FFrame* F = R.Oldest();
			if (!F) continue;
			FModelFrame* M = FindModel(F->Id);
			if (!M) { ++IdentityBroken; continue; }
			if (F->bDraining)
			{
				if (Now >= M->DrainDoneAt) Resolve(F->Id, M->bDrainSignalFails ? EOutcome::FailedDrain : EOutcome::Published);
				continue;
			}
			if (F->ReadyAt < 0.0)
			{
				if (Now >= M->FillDoneAt) R.MarkReady(F->Id, Now);
				else if (Now - F->AcceptedAt >= 1.0) { Resolve(F->Id, EOutcome::FailedFill); continue; }
				else continue;
			}
			// Newer frames whose fill was observed are ready.
			bool bNewerReady = false;
			for (FModelFrame& N : Model)
			{
				if (N.Id <= F->Id) continue;
				if (Now >= N.FillDoneAt) { R.MarkReady(N.Id, Now); bNewerReady = true; }
			}
			F = R.Oldest();
			M = FindModel(F->Id);
			switch (NextStep(*F, Now, bNewerReady, Deadline))
			{
			case EStep::Supersede: Resolve(F->Id, EOutcome::Superseded); break;
			case EStep::Abandon: Resolve(F->Id, EOutcome::AccessAbandoned); break;
			case EStep::Attempt:
			{
				if (Percent() < 1) { Resolve(F->Id, EOutcome::FailedInstall); break; }
				if (M->FillDoneAt > Now) { ++ReadBeforeFilled; break; }
				const bool bGranted = ReceiverHoldsUntil < Now;
				R.NoteAttempt(F->Id, bGranted);
				if (bGranted)
				{
					R.BeginDrain(F->Id);
					M->bDrainSignalFails = Percent() < 1;
					M->DrainDoneAt = Now + Uniform(0.0002, Percent() < 2 ? 0.05 : 0.003);
				}
				break;
			}
			}
		}
		const FCounters& C = R.GetCounters();
		if (!IdentityHolds(R) || C.InFlight() != static_cast<std::int64_t>(Model.size())) ++IdentityBroken;
		MaxInFlight = MaxInFlight > C.InFlight() ? MaxInFlight : C.InFlight();
	}
	const FCounters& C = R.GetCounters();
	AddTotal(C);
	char Label[200];
	sprintf_s(Label, "model %u: no slot was written while a frame using it was unresolved", Seed); Equal(WroteUnresolved, 0, Label);
	sprintf_s(Label, "model %u: no frame resolved twice or out of order; announcements in fill order", Seed); Equal(ResolvedTwice + OutOfOrder, 0, Label);
	sprintf_s(Label, "model %u: no frame announced before its drain finished", Seed); Equal(PublishedUndrained, 0, Label);
	sprintf_s(Label, "model %u: no slot read before its fill finished", Seed); Equal(ReadBeforeFilled, 0, Label);
	sprintf_s(Label, "model %u: Accepted = outcomes + in flight after every step", Seed); Equal(IdentityBroken, 0, Label);
	sprintf_s(Label, "model %u: no rule broken", Seed); Equal(C.RuleViolations, 0, Label);
	sprintf_s(Label, "model %u: in flight never above the budget's %d sets of two slots (max %lld)", Seed, BudgetSets, static_cast<long long>(MaxInFlight));
	Check(MaxInFlight <= 2 * BudgetSets, Label);
	sprintf_s(Label, "model %u: the rare cases occurred (flushes %lld, stops %lld, resizes %lld, budget refusals %lld, stalls %lld)", Seed,
		static_cast<long long>(Flushes), static_cast<long long>(Stops), static_cast<long long>(Resizes), static_cast<long long>(BudgetRefusals), static_cast<long long>(Stalls));
	Check(Flushes > 10 && Stops > 2 && Resizes > 10 && BudgetRefusals > 0 && Stalls > 10, Label);
	std::cout << "INFO " << Label << "; all rings: accepted " << Total.Accepted << ", published " << Total.Outcomes[1] << ", superseded " << Total.Outcomes[2]
		<< ", abandoned " << Total.Outcomes[3] << ", failed fill/install/drain/removed " << Total.Outcomes[4] << "/" << Total.Outcomes[5] << "/" << Total.Outcomes[6] << "/" << Total.Outcomes[7]
		<< ", not accepted no slot/budget " << Total.NotAccepted[0] << "/" << Total.NotAccepted[2]
		<< ", attempts " << Total.Attempts << " (failed " << Total.FailedAttempts << ")" << std::endl;
}

int main()
{
	Publishes();
	SupersedesAndAbandons();
	Failures4();
	DelayedCompletions();
	IdentityChange();
	FlushAndStop();
	BrokenRules();
	const int RingChecks = Checks;
	ModelRun(20261026u, 400000);
	ModelRun(7u, 400000);
	ModelRun(99u, 400000);
	std::cout << "send ring checks " << RingChecks << ", model checks " << (Checks - RingChecks) << ", failures " << Failures << std::endl;
	return Failures ? 1 : 0;
}
