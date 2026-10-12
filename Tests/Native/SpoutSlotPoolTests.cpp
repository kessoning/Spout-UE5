// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
//
// Native tests of the asynchronous receive stream's bookkeeping (SpoutSlotPool.h), compiled from the plugin's own
// header without Unreal and without a GPU.
//
//   part 1  every transition of the slot pool, with expectations written by hand, including the four cases a real
//           pool rarely meets: a publication replaced before it was read, no free slot, a refused access and a
//           completion timeout
//   part 2  what starts a worker transfer (the transfer gate), with and without a usable frame count
//   part 3  a long seeded sequence of worker and reader steps against a lagging read fence, checked after every
//           step against the rules and against an account of where every publication went
#include "SpoutSlotPool.h"
#include <cstdint>
#include <cstdio>
#include <iostream>

using namespace SpoutAsync;

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
static ESlotState StateOf(const FSlotPool& P, int Slot) { return P.GetSlot(Slot).State; }
static std::int64_t Abandoned(const FSlotPool& P, EAbandon Why) { return P.GetCounters().Abandoned[static_cast<int>(Why)]; }

// ------------------------------------------------------------------------------------------------
// Part 1: the slot pool.
// ------------------------------------------------------------------------------------------------
static void PoolBasics()
{
	FSlotPool P;
	FConsumed C;
	Equal(P.GetNumSlots(), 3, "pool: three slots by default");
	Check(!P.HasReady(), "pool: nothing is ready at the start");
	Check(P.Consume(1, 1, C) == EConsume::Nothing && C.Slot == -1, "pool: the reader finds nothing at the start");
	Check(P.CanDestroy(0), "pool: an unused pool may be destroyed");
	Equal(P.OldestPendingReadValue(), 0, "pool: no read is pending at the start");

	const int A = P.BeginWrite(0);
	Equal(A, 0, "pool: the first write gets slot 0");
	Check(StateOf(P, 0) == ESlotState::Writing, "pool: and the slot is being written");
	Check(!P.CanDestroy(0), "pool: a pool with a write in progress may not be destroyed");
	Check(!P.HasReady(), "pool: a slot being written is not ready");
	Check(P.Consume(1, 1, C) == EConsume::Nothing, "pool: and the reader cannot take it");
	Equal(P.BeginWrite(0), -1, "pool: a second write while one is in progress is refused");
	Equal(P.GetCounters().RuleViolations, 1, "pool: and counted as a broken rule (one transfer in flight)");
	Equal(P.GetCounters().NoFreeSlot, 0, "pool: it is not counted as a missing slot");

	Equal(P.Publish(A, 7, 1, 100), 1, "pool: the first publication has sequence 1");
	Check(StateOf(P, 0) == ESlotState::Ready && P.HasReady(), "pool: the slot is ready");
	Equal(P.GetCounters().Published, 1, "pool: one publication");
	Check(P.Consume(7, 1, C) == EConsume::Consumed, "pool: the reader takes it");
	Check(C.Slot == 0 && C.Sequence == 1 && C.Generation == 7 && C.WrittenValue == 1 && C.ReadValue == 1 && C.Tag == 100, "pool: with the slot, the sequence, the written value to wait on and the read value to signal");
	Check(StateOf(P, 0) == ESlotState::Draining && P.GetSlot(0).ReadValue == 1, "pool: the slot drains until read value 1");
	Equal(P.GetCounters().Consumed, 1, "pool: one delivery");
	Check(P.Consume(7, 2, C) == EConsume::Nothing, "pool: the same publication is not taken twice");
	Check(!P.CanDestroy(0), "pool: a pool with a read not yet completed may not be destroyed");
	Check(P.CanDestroy(1), "pool: and may be once the read fence reached it");
	Equal(P.OldestPendingReadValue(), 1, "pool: the pending read is value 1");
}

static void PoolReplacedUnread()
{
	// The worker publishes twice between two reads: the first of the two was never read.
	FSlotPool P;
	FConsumed C;
	P.Publish(P.BeginWrite(0), 1, 1, 10);
	const int Second = P.BeginWrite(0);
	Equal(Second, 1, "replaced: the next write goes to another slot, the ready one is left alone");
	Check(StateOf(P, 0) == ESlotState::Ready, "replaced: the first publication is still ready while the second is written");
	Equal(P.Publish(Second, 1, 2, 20), 2, "replaced: the second publication");
	Check(StateOf(P, 0) == ESlotState::Free && StateOf(P, 1) == ESlotState::Ready, "replaced: the unread one is free at once, the new one is ready");
	Equal(P.GetCounters().ReplacedUnread, 1, "replaced: counted once");
	Check(P.Consume(1, 1, C) == EConsume::Consumed && C.Slot == 1 && C.Tag == 20 && C.Sequence == 2, "replaced: the reader gets the newer image");
	Equal(P.GetCounters().Consumed, 1, "replaced: one delivery; the replaced publication is not one");
	Equal(P.GetCounters().Published, 2, "replaced: two publications");
	Equal(P.BeginWrite(0), 0, "replaced: the slot that was never read is written again without waiting for any read");
}

static void PoolNoFreeSlot()
{
	// The reader's queue is behind: two reads are queued and not completed, and a third image is ready.
	FSlotPool P;
	FConsumed C;
	P.Publish(P.BeginWrite(0), 1, 1, 10);
	P.Consume(1, 1, C);                         // slot 0 drains until read 1
	P.Publish(P.BeginWrite(0), 1, 2, 20);
	P.Consume(1, 2, C);                         // slot 1 drains until read 2
	P.Publish(P.BeginWrite(0), 1, 3, 30);       // slot 2 is ready, unread
	Check(StateOf(P, 0) == ESlotState::Draining && StateOf(P, 1) == ESlotState::Draining && StateOf(P, 2) == ESlotState::Ready, "no slot: two draining, one ready");
	Equal(P.BeginWrite(0), -1, "no slot: with the read fence at 0 nothing can be written");
	Equal(P.GetCounters().NoFreeSlot, 1, "no slot: counted");
	Check(StateOf(P, 2) == ESlotState::Ready, "no slot: the ready image was not taken for writing");
	Equal(P.GetCounters().RuleViolations, 0, "no slot: and no rule was broken");
	Equal(P.OldestPendingReadValue(), 1, "no slot: the worker may wait for read value 1");
	Equal(P.BeginWrite(0), -1, "no slot: asking again changes nothing");
	Equal(P.GetCounters().NoFreeSlot, 2, "no slot: counted again");
	Equal(P.BeginWrite(1), 0, "no slot: once read 1 completed, slot 0 is written");
	Check(StateOf(P, 1) == ESlotState::Draining, "no slot: slot 1, whose read has not completed, is left alone");
	P.Publish(0, 1, 4, 40);
	Equal(P.GetCounters().ReplacedUnread, 1, "no slot: the image that waited in slot 2 was replaced unread");
	Equal(P.BeginWrite(1), 2, "no slot: the replaced slot is free at once");
	P.AbandonWrite(2, EAbandon::Cancelled);
	// With both reads completed the older one is taken first.
	FSlotPool Q;
	Q.Publish(Q.BeginWrite(0), 1, 1, 0); Q.Consume(1, 5, C);
	Q.Publish(Q.BeginWrite(0), 1, 2, 0); Q.Consume(1, 6, C);
	Q.Publish(Q.BeginWrite(0), 1, 3, 0);
	Equal(Q.BeginWrite(6), 0, "no slot: of two completed reads the older slot is written first");
}

static void PoolRefusedAccessAndTimeout()
{
	FSlotPool P;
	FConsumed C;
	P.Publish(P.BeginWrite(0), 1, 1, 10);       // slot 0 ready
	const int W = P.BeginWrite(0);
	Equal(W, 1, "refused: a slot is picked before access is asked for");
	P.AbandonWrite(W, EAbandon::AccessRefused);
	Check(StateOf(P, 1) == ESlotState::Free, "refused: the slot is free again");
	Check(StateOf(P, 0) == ESlotState::Ready, "refused: the image that was ready is untouched");
	Equal(Abandoned(P, EAbandon::AccessRefused), 1, "refused: counted");
	Equal(P.GetCounters().Published, 1, "refused: nothing was published");
	Check(P.Consume(1, 1, C) == EConsume::Consumed && C.Tag == 10, "refused: the reader still gets the earlier image");

	// A completion that timed out: the copy may still be in flight, the slot is never published from it.
	const int T = P.BeginWrite(0);
	Equal(T, 1, "timeout: the freed slot is picked again");
	P.AbandonWrite(T, EAbandon::CompletionTimeout);
	Equal(Abandoned(P, EAbandon::CompletionTimeout), 1, "timeout: counted");
	Check(StateOf(P, 1) == ESlotState::Free && !P.HasReady(), "timeout: the slot is free and nothing is ready");
	Check(P.Consume(1, 2, C) == EConsume::Nothing, "timeout: the reader is given nothing from it");
	Check(P.GetSlot(1).bLastWriteAbandoned, "timeout: the slot remembers its last write was given up");
	const int R = P.BeginWrite(0);
	Equal(R, 1, "timeout: the same slot is written again");
	Equal(P.Publish(R, 1, 3, 30), 2, "timeout: a later copy that completed is published from it");
	Equal(P.GetCounters().PublishedAfterReuse, 1, "timeout: counted as a publication into a slot whose previous write was given up");
	Check(!P.GetSlot(1).bLastWriteAbandoned, "timeout: and the mark is cleared");
	Check(P.Consume(1, 2, C) == EConsume::Consumed && C.Slot == 1 && C.WrittenValue == 3 && C.Tag == 30, "timeout: the reader waits on the later copy's written value");

	P.AbandonWrite(P.BeginWrite(2), EAbandon::CopyFailed);
	P.AbandonWrite(P.BeginWrite(2), EAbandon::Cancelled);
	Equal(Abandoned(P, EAbandon::CopyFailed), 1, "abandon: a failed copy is counted by itself");
	Equal(Abandoned(P, EAbandon::Cancelled), 1, "abandon: and so is a cancelled one");
	Equal(P.GetCounters().RuleViolations, 0, "abandon: no rule was broken in any of it");
}

static void PoolBrokenRules()
{
	FSlotPool P;
	FConsumed C;
	Equal(P.Publish(0, 1, 1, 0), 0, "rules: publishing a slot that is not being written publishes nothing");
	P.AbandonWrite(0, EAbandon::CopyFailed);
	P.AbandonWrite(7, EAbandon::CopyFailed);
	Equal(P.Publish(-1, 1, 1, 0), 0, "rules: nor does a slot that does not exist");
	Equal(P.GetCounters().RuleViolations, 4, "rules: each such call is counted");
	Equal(P.GetCounters().Published, 0, "rules: and nothing was published or abandoned");
	Equal(Abandoned(P, EAbandon::CopyFailed), 0, "rules: nothing was abandoned");

	P.Publish(P.BeginWrite(0), 1, 5, 0);
	Equal(P.Publish(P.BeginWrite(0), 1, 5, 0), 0, "rules: a written value that does not rise publishes nothing");
	Check(StateOf(P, 1) == ESlotState::Free && StateOf(P, 0) == ESlotState::Ready, "rules: the slot is free and the earlier image stays ready");
	Equal(P.GetCounters().RuleViolations, 5, "rules: counted");

	Check(P.Consume(1, 3, C) == EConsume::Consumed, "rules: a first read with value 3");
	P.Publish(P.BeginWrite(0), 1, 6, 0);
	Check(P.Consume(1, 3, C) == EConsume::Nothing, "rules: a read value that does not rise consumes nothing");
	Check(P.HasReady(), "rules: the image stays ready for a correct call");
	Equal(P.GetCounters().RuleViolations, 6, "rules: counted");
	Check(P.Consume(1, 4, C) == EConsume::Consumed, "rules: the correct call takes it");

	FSlotPool Small(0), Large(9);
	Equal(Small.GetNumSlots(), 1, "rules: a pool has at least one slot");
	Equal(Large.GetNumSlots(), MaxSlots, "rules: and at most the maximum");
	Equal(Small.BeginWrite(0), 0, "rules: a one-slot pool writes its slot");
	Small.Publish(0, 1, 1, 0);
	Equal(Small.BeginWrite(0), -1, "rules: and cannot write while that image is ready and unread");
	Equal(Small.GetCounters().NoFreeSlot, 1, "rules: which is a missing slot, not a replaced image");
}

static void PoolGenerationsAndRetirement()
{
	FSlotPool P;
	FConsumed C;
	P.Publish(P.BeginWrite(0), 1, 1, 10);
	Check(P.Consume(2, 1, C) == EConsume::DroppedOldGeneration, "generation: an image of generation 1 is dropped when the stream is at generation 2");
	Check(C.Slot == -1, "generation: the reader is given no slot");
	Check(StateOf(P, 0) == ESlotState::Free, "generation: the slot is free, it was never read");
	Equal(P.GetCounters().DroppedOldGeneration, 1, "generation: counted");
	Equal(P.GetCounters().Consumed, 0, "generation: it is not a delivery");
	Check(P.Consume(2, 1, C) == EConsume::Nothing, "generation: and nothing is left to take");

	// Retirement with a read queued, a write in progress and an image ready.
	FSlotPool R;
	R.Publish(R.BeginWrite(0), 1, 1, 0);
	R.Consume(1, 1, C);                          // slot 0 drains until read 1
	R.Publish(R.BeginWrite(0), 1, 2, 0);         // slot 1 ready
	const int W = R.BeginWrite(0);               // slot 2 being written
	R.Retire();
	Check(R.IsRetired(), "retire: retired");
	Check(!R.CanDestroy(1), "retire: not destroyable while a write is in progress");
	Equal(R.Publish(W, 1, 3, 0), 0, "retire: a copy that completes after retirement is not published");
	Equal(R.GetCounters().PublishedAfterRetire, 1, "retire: counted");
	Check(StateOf(R, 2) == ESlotState::Free && StateOf(R, 1) == ESlotState::Ready, "retire: its slot is free; the earlier ready image was not replaced");
	Equal(R.BeginWrite(9), -1, "retire: no further write is accepted");
	Equal(R.GetCounters().RefusedRetired, 1, "retire: counted apart from a missing slot");
	Equal(R.GetCounters().NoFreeSlot, 0, "retire: not as a missing slot");
	Check(R.Consume(1, 2, C) == EConsume::DroppedOldGeneration, "retire: the ready image of a retired pool is dropped, not read, even at its own generation");
	Check(!R.CanDestroy(0), "retire: not destroyable while the queued read has not completed");
	Check(R.CanDestroy(1), "retire: destroyable once it has");
	Equal(R.GetCounters().RuleViolations, 0, "retire: no rule was broken");
}

// ------------------------------------------------------------------------------------------------
// Part 2: the transfer gate.
// ------------------------------------------------------------------------------------------------
static void GateTests()
{
	{
		FTransferGate G;
		Check(!G.IsOwed(), "gate: nothing is owed before the stream opens");
		G.ObserveCounted(false);
		Check(!G.IsOwed(), "gate: a poll that finds nothing new owes nothing");
		G.Reset();
		Check(G.IsOwed(), "gate: open owes the current image");
		Equal(G.GetForced(), 1, "gate: one forced copy");
		G.ObserveCounted(false);
		Check(G.IsOwed(), "gate: it stays owed across polls (access refused, no slot, a timed-out completion)");
		G.OnPublished();
		Check(!G.IsOwed(), "gate: the completed copy clears it");
		G.ObserveCounted(false);
		Check(!G.IsOwed(), "gate: and it is not repeated for the same notification");
		G.ObserveCounted(true);
		Check(G.IsOwed(), "gate: a new notification owes a transfer");
		G.ObserveCounted(true);
		G.ObserveCounted(false);
		Check(G.IsOwed(), "gate: further polls, new or not, keep it owed");
		Equal(G.GetNotifications(), 2, "gate: two notifications seen");
		G.OnPublished();
		Check(!G.IsOwed(), "gate: one copy answers both");
		Equal(G.GetCompleted(), 2, "gate: two copies completed in all");
		G.Reset();
		Check(G.IsOwed(), "gate: a new generation owes the current image again");
		Equal(G.GetForced(), 2, "gate: a second forced copy");
	}
	{
		// A sender without a usable count: one transfer per image the reader consumed, never more.
		FTransferGate G;
		G.Reset();
		G.ObserveUncounted();
		Check(G.IsOwed(), "uncounted: the forced copy at the start is owed");
		Equal(G.GetUncountedStarts(), 0, "uncounted: and needs no credit");
		G.OnPublished();
		for (int K = 0; K < 1000; ++K) G.ObserveUncounted();
		Check(!G.IsOwed(), "uncounted: a thousand polls without a read start nothing: it never free-runs");
		Equal(G.GetUncountedStarts(), 0, "uncounted: no start counted");
		G.OnConsumed();
		G.ObserveUncounted();
		Check(G.IsOwed(), "uncounted: the reader consumed, so one transfer is owed");
		for (int K = 0; K < 10; ++K) G.ObserveUncounted();
		Equal(G.GetUncountedStarts(), 1, "uncounted: one start for one consumption, however often it is polled");
		Check(G.IsOwed(), "uncounted: it stays owed until it completes (a refused access is retried)");
		G.OnPublished();
		G.ObserveUncounted();
		Check(!G.IsOwed(), "uncounted: completed, and no further transfer without another consumption");
		G.OnConsumed();
		G.OnConsumed();
		G.ObserveUncounted();
		G.OnPublished();
		G.ObserveUncounted();
		Check(!G.IsOwed(), "uncounted: two consumptions before a poll are still one credit");
		Equal(G.GetUncountedStarts(), 2, "uncounted: two starts in all");
	}
	{
		// A sender that had a count and lost it: the credit of the last consumption is there.
		FTransferGate G;
		G.Reset();
		G.OnPublished();
		G.OnConsumed();
		G.ObserveCounted(true);
		G.OnPublished();
		G.ObserveUncounted();
		Check(G.IsOwed(), "uncounted: after the count is lost the last consumption still allows one transfer");
	}
}

// ------------------------------------------------------------------------------------------------
// Part 3: a long seeded sequence against a read fence that completes late.
// ------------------------------------------------------------------------------------------------
static std::uint32_t Next(std::uint32_t& Seed)
{
	Seed = Seed * 1664525u + 1013904223u;
	return Seed >> 8;
}

static void ModelRun(std::uint32_t Seed, int NumSlots, int Steps)
{
	FSlotPool P(NumSlots);
	FConsumed C;
	std::uint64_t WrittenN = 0, ReadN = 0, CompletedRead = 0, Generation = 1;
	int Writing = -1;                        // the slot the worker is writing, or -1
	std::int64_t BeginOk = 0, WroteBeforeReadCompleted = 0, ReadNotReady = 0, ReadOtherGeneration = 0, TwoReady = 0, TwoWriting = 0, ReadyHandedOut = 0;
	std::uint64_t PublishedGeneration[MaxSlots] = { 0, 0, 0, 0 };
	for (int Step = 0; Step < Steps; ++Step)
	{
		const std::uint32_t R = Next(Seed) % 100;
		if (R < 34)
		{
			// The worker: begin a write, or finish the one in progress.
			if (Writing < 0)
			{
				const ESlotState Before[MaxSlots] = { StateOf(P, 0), StateOf(P, 1), StateOf(P, 2), StateOf(P, 3) };
				std::uint64_t ReadBefore[MaxSlots];
				for (int I = 0; I < MaxSlots; ++I) ReadBefore[I] = P.GetSlot(I).ReadValue;
				Writing = P.BeginWrite(CompletedRead);
				if (Writing >= 0)
				{
					++BeginOk;
					if (Before[Writing] == ESlotState::Ready) ++ReadyHandedOut;
					if (Before[Writing] == ESlotState::Draining && ReadBefore[Writing] > CompletedRead) ++WroteBeforeReadCompleted;
				}
			}
			else
			{
				const std::uint32_t How = Next(Seed) % 10;
				if (How == 0) P.AbandonWrite(Writing, EAbandon::AccessRefused);
				else if (How == 1) P.AbandonWrite(Writing, EAbandon::CompletionTimeout);
				else { PublishedGeneration[Writing] = Generation; P.Publish(Writing, Generation, ++WrittenN, 0); }
				Writing = -1;
			}
		}
		else if (R < 64)
		{
			// The reader.
			const int ReadySlot = [&P]() { for (int I = 0; I < P.GetNumSlots(); ++I) if (StateOf(P, I) == ESlotState::Ready) return I; return -1; }();
			const EConsume E = P.Consume(Generation, ReadN + 1, C);
			if (E == EConsume::Consumed)
			{
				++ReadN;
				if (C.Slot != ReadySlot) ++ReadNotReady;
				if (PublishedGeneration[C.Slot] != Generation) ++ReadOtherGeneration;
			}
		}
		else if (R < 94)
		{
			// The reader's queue catches up by one read, at most to the last one queued.
			if (CompletedRead < ReadN) ++CompletedRead;
		}
		else if (R < 96)
		{
			++Generation;                    // a resize or a sender restart; the pool is kept, as a late drop must be safe
		}
		int Ready = 0, InWrite = 0;
		for (int I = 0; I < P.GetNumSlots(); ++I)
		{
			Ready += StateOf(P, I) == ESlotState::Ready;
			InWrite += StateOf(P, I) == ESlotState::Writing;
		}
		if (Ready > 1) ++TwoReady;
		if (InWrite > 1 || (InWrite == 1) != (Writing >= 0)) ++TwoWriting;
	}
	const FSlotCounters& N = P.GetCounters();
	int ReadyNow = 0;
	for (int I = 0; I < P.GetNumSlots(); ++I) ReadyNow += StateOf(P, I) == ESlotState::Ready;
	std::int64_t AbandonedAll = 0;
	for (int I = 0; I < static_cast<int>(EAbandon::Count); ++I) AbandonedAll += N.Abandoned[I];
	char Label[160];
	sprintf_s(Label, "model (%d slots): never more than one ready slot", NumSlots); Equal(TwoReady, 0, Label);
	sprintf_s(Label, "model (%d slots): never more than one slot being written, and always the one handed out", NumSlots); Equal(TwoWriting, 0, Label);
	sprintf_s(Label, "model (%d slots): a ready slot is never handed out for writing", NumSlots); Equal(ReadyHandedOut, 0, Label);
	sprintf_s(Label, "model (%d slots): no slot is written before its last read completed", NumSlots); Equal(WroteBeforeReadCompleted, 0, Label);
	sprintf_s(Label, "model (%d slots): the reader is only ever given the ready slot", NumSlots); Equal(ReadNotReady, 0, Label);
	sprintf_s(Label, "model (%d slots): no image of another generation is read", NumSlots); Equal(ReadOtherGeneration, 0, Label);
	sprintf_s(Label, "model (%d slots): no rule was broken", NumSlots); Equal(N.RuleViolations, 0, Label);
	sprintf_s(Label, "model (%d slots): every publication was consumed, replaced unread, dropped for its generation, or is still ready", NumSlots);
	Equal(N.Published, N.Consumed + N.ReplacedUnread + N.DroppedOldGeneration + ReadyNow, Label);
	sprintf_s(Label, "model (%d slots): every write that began was published or given up, or is in progress", NumSlots);
	Equal(BeginOk, N.Published + AbandonedAll + (Writing >= 0 ? 1 : 0), Label);
	sprintf_s(Label, "model (%d slots): the four rare cases all occurred (replaced %lld, no slot %lld, refused %lld, timed out %lld, reused %lld)", NumSlots,
		static_cast<long long>(N.ReplacedUnread), static_cast<long long>(N.NoFreeSlot), static_cast<long long>(N.Abandoned[0]), static_cast<long long>(N.Abandoned[2]), static_cast<long long>(N.PublishedAfterReuse));
	Check(N.ReplacedUnread > 100 && N.NoFreeSlot > 100 && N.Abandoned[0] > 100 && N.Abandoned[2] > 100 && N.PublishedAfterReuse > 100 && N.DroppedOldGeneration > 10, Label);
	std::cout << "INFO " << Label << "; published " << N.Published << ", consumed " << N.Consumed << ", dropped for generation " << N.DroppedOldGeneration << std::endl;
}

int main()
{
	PoolBasics();
	PoolReplacedUnread();
	PoolNoFreeSlot();
	PoolRefusedAccessAndTimeout();
	PoolBrokenRules();
	PoolGenerationsAndRetirement();
	const int PoolChecks = Checks;
	GateTests();
	const int GateChecks = Checks - PoolChecks;
	ModelRun(20261012u, 3, 400000);
	ModelRun(7u, 2, 400000);
	ModelRun(99u, 4, 400000);
	const int ModelChecks = Checks - PoolChecks - GateChecks;
	std::cout << "slot pool checks " << PoolChecks << ", transfer gate checks " << GateChecks << ", model checks " << ModelChecks << ", failures " << Failures << std::endl;
	return Failures ? 1 : 0;
}
