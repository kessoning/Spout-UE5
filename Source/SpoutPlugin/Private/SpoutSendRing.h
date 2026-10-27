// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * The bookkeeping of the asynchronous render-target sender (development switch r.Spout.Dev.AsyncSend, off by default):
 * which slot the render thread may fill, which frame the worker works on, and how every accepted frame ends.
 * Engine-free, like SpoutSlotPool.h: it makes no graphics call and takes times and fence values as numbers.
 *
 * A source has a set of slots per identity (size and format); a new identity starts a new set, and an older set is
 * kept until its last frame is resolved. A slot is in use from the moment the render thread queues a copy into it
 * until its frame is resolved. The worker resolves a frame only once nothing may still use its slot: a published
 * frame's copy out of the slot is proven finished before the frame is announced, and a frame given up before its
 * drain was never copied out of the slot (Unreal's later fill into the same slot is ordered after the earlier one on
 * its own queue).
 *
 * Every accepted frame ends in exactly one outcome, in acceptance order:
 *   Published        copied into the shared texture, proven finished, announced
 *   Superseded       a newer frame of the source was ready before this one made any access attempt
 *   AccessAbandoned  it made at least one failed access attempt, then its deadline passed, a newer frame was ready,
 *                    or a flush or stop allowed it one attempt only
 *   Failed*          its fill did not finish, its installation was refused, its drain's signal failed, or a device
 *                    was removed
 * Accepted = Published + Superseded + AccessAbandoned + Failed + in flight, at every step.
 *
 * Frames resolved while a flush or the stream's stop allowed them one attempt are also counted apart, by outcome
 * (FlushOutcomes, StopOutcomes: subsets of Outcomes, never added to them). Resolving a frame is not a promise that an
 * external receiver sees it: a published frame may be overwritten by the next one before a receiver reads it, and a
 * frame abandoned at a stop is never announced.
 *
 * Not thread-safe: the owner calls every method under its own lock, held for the call only, never across a graphics
 * call or a wait.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace SpoutSendRing
{
	/** The largest number of slots a set may have. The sender uses two. */
	constexpr int MaxSlots = 4;

	enum class EOutcome : std::uint8_t
	{
		None = 0,             // not resolved yet
		Published = 1,
		Superseded = 2,
		AccessAbandoned = 3,
		FailedFill = 4,       // the fill did not finish in its bound, or Unreal's device was removed
		FailedInstall = 5,    // the shared texture or the registration for its identity could not be made
		FailedDrain = 6,      // the drain's signal failed (its completion proven by an event query)
		FailedRemoved = 7,    // the native device was removed
		Count = 8
	};

	/** Why the render thread queued nothing for a send. */
	enum class ENotAccepted : std::uint8_t
	{
		NoSlot = 0,           // every slot of the current set holds an unresolved frame
		Stopping = 1,         // the stream is stopping: it accepts nothing more
		Budget = 2,           // the retained-memory budget refused a new set
		Unpublishable = 3,    // the stream cannot publish this identity now: its last installation failed (probed again
		                      // after a back-off), its slot set failed, or the stream stopped for good
		Count = 4
	};

	/** Why a frame may make one access attempt only. A stop outranks a flush. */
	enum class ELimit : std::uint8_t
	{
		None = 0,
		Flush = 1,            // a flush (FlushRenderingCommands; a session stop's flush step too)
		Stop = 2              // the stream's stop (close, or the entry's destruction)
	};

	/** What the worker does next with the frame it works on. */
	enum class EStep : std::uint8_t
	{
		Attempt = 0,          // try access (one attempt of r.Spout.AccessWaitMs)
		Supersede = 1,        // a newer frame is ready and this one made no attempt
		Abandon = 2           // give it up for access
	};

	struct FFrame
	{
		std::uint64_t Id = 0;            // acceptance order, from 1
		std::uint64_t SetGeneration = 0; // the slot set (identity) it was filled into
		int Slot = -1;
		std::uint64_t FillValue = 0;     // the filled-fence value that proves its fill finished
		std::uint64_t FrameEndValue = 0; // the frame-end fence value after the frame's own work (0: none)
		double AcceptedAt = 0.0;
		double ReadyAt = -1.0;           // when its fill was observed finished; negative before
		int FailedAttempts = 0;
		bool bDraining = false;          // its copy out of the slot has been submitted
		bool bOneAttempt = false;        // a flush or stop allows it one access attempt only
		ELimit Limit = ELimit::None;     // which one (the strongest so far)
		EOutcome Outcome = EOutcome::None;
	};

	struct FCounters
	{
		std::int64_t Accepted = 0;
		std::int64_t NotAccepted[static_cast<int>(ENotAccepted::Count)] = {};
		std::int64_t Outcomes[static_cast<int>(EOutcome::Count)] = {};
		/** Of Outcomes: the frames a flush, or the stream's stop, allowed one attempt only when they were resolved. */
		std::int64_t FlushOutcomes[static_cast<int>(EOutcome::Count)] = {};
		std::int64_t StopOutcomes[static_cast<int>(EOutcome::Count)] = {};
		std::int64_t Attempts = 0;          // access attempts made
		std::int64_t FailedAttempts = 0;    // access attempts that did not get access
		std::int64_t RuleViolations = 0;    // a call that broke a rule (always 0 when the owner is correct)
		std::int64_t InFlight() const
		{
			std::int64_t Resolved = 0;
			for (int I = 1; I < static_cast<int>(EOutcome::Count); ++I) Resolved += Outcomes[I];
			return Accepted - Resolved;
		}
	};

	/**
	 * The worker's decision for a frame that is ready and not draining: attempt access, or end it. Deadline: the time
	 * by which it must have been published, counted from when it became ready.
	 */
	inline EStep NextStep(const FFrame& Frame, double Now, bool bNewerReady, double DeadlineSeconds)
	{
		if (bNewerReady)
		{
			return Frame.FailedAttempts == 0 ? EStep::Supersede : EStep::Abandon;
		}
		if (Frame.FailedAttempts > 0 && (Frame.bOneAttempt || (Frame.ReadyAt >= 0.0 && Now - Frame.ReadyAt >= DeadlineSeconds)))
		{
			return EStep::Abandon;
		}
		return EStep::Attempt;
	}

	class FRing
	{
	public:
		explicit FRing(int InSlotsPerSet) : SlotsPerSet(InSlotsPerSet < 1 ? 1 : (InSlotsPerSet > MaxSlots ? MaxSlots : InSlotsPerSet)) { Frames.reserve(4 * MaxSlots); }

		int GetSlotsPerSet() const { return SlotsPerSet; }

		/**
		 * The render thread made a new current set (a new identity). Generations only grow. Frames of older sets stay
		 * until they are resolved.
		 */
		void BeginSet(std::uint64_t Generation)
		{
			if (Generation <= LastSet) { ++Counters.RuleViolations; return; }
			CurrentSet = LastSet = Generation;
		}
		/**
		 * The current set is given up (its identity changed, or it failed) before a new one exists: no slot is free until
		 * BeginSet. Its frames stay until they are resolved.
		 */
		void EndSet() { CurrentSet = 0; }
		std::uint64_t GetCurrentSet() const { return CurrentSet; }

		/** Whether any unresolved frame still uses a slot of Generation's set. */
		bool SetInUse(std::uint64_t Generation) const
		{
			for (const FFrame& F : Frames) if (F.SetGeneration == Generation) return true;
			return false;
		}

		/**
		 * Render thread: a free slot of the current set, or -1 and why. A slot is free when no unresolved frame uses it.
		 */
		int FindFreeSlot(ENotAccepted& OutWhy) const
		{
			if (bStopping) { OutWhy = ENotAccepted::Stopping; return -1; }
			if (CurrentSet == 0) { OutWhy = ENotAccepted::NoSlot; return -1; }
			for (int S = 0; S < SlotsPerSet; ++S)
			{
				if (IsSlotFree(S)) return S;
			}
			OutWhy = ENotAccepted::NoSlot;
			return -1;
		}

		/** Render thread: counts a send that queued nothing. */
		void NotAccepted(ENotAccepted Why) { ++Counters.NotAccepted[static_cast<int>(Why)]; }

		/**
		 * Render thread: the fill of Slot (of the current set) was queued with FillValue. Returns the frame's id, or 0
		 * when the slot was not free (a rule violation: nothing is recorded).
		 */
		std::uint64_t Accept(int Slot, std::uint64_t FillValue, std::uint64_t FrameEndValue, double Now)
		{
			if (bStopping || Slot < 0 || Slot >= SlotsPerSet || CurrentSet == 0 || !IsSlotFree(Slot))
			{
				++Counters.RuleViolations;
				return 0;
			}
			FFrame F;
			F.Id = ++LastId;
			F.SetGeneration = CurrentSet;
			F.Slot = Slot;
			F.FillValue = FillValue;
			F.FrameEndValue = FrameEndValue;
			F.AcceptedAt = Now;
			Frames.push_back(F);
			++Counters.Accepted;
			return F.Id;
		}

		/** Worker: the oldest unresolved frame, or null. */
		FFrame* Oldest() { return Frames.empty() ? nullptr : &Frames.front(); }
		const FFrame* Oldest() const { return Frames.empty() ? nullptr : &Frames.front(); }

		/** Worker: frames newer than Id, oldest first (for the supersession check). */
		std::vector<const FFrame*> NewerThan(std::uint64_t Id) const
		{
			std::vector<const FFrame*> Out;
			for (const FFrame& F : Frames) if (F.Id > Id) Out.push_back(&F);
			return Out;
		}

		/** Worker: the frame's fill was observed finished. */
		void MarkReady(std::uint64_t Id, double Now)
		{
			FFrame* F = Find(Id);
			if (!F || F->bDraining) { ++Counters.RuleViolations; return; }
			if (F->ReadyAt < 0.0) F->ReadyAt = Now;
		}

		/** Worker: one access attempt was made for the frame; bGranted says whether it got access. */
		void NoteAttempt(std::uint64_t Id, bool bGranted)
		{
			FFrame* F = Find(Id);
			if (!F || F->bDraining || F->ReadyAt < 0.0) { ++Counters.RuleViolations; return; }
			++Counters.Attempts;
			if (!bGranted)
			{
				++Counters.FailedAttempts;
				++F->FailedAttempts;
			}
		}

		/** Worker: the frame's copy out of its slot was submitted (access held). */
		void BeginDrain(std::uint64_t Id)
		{
			FFrame* F = Find(Id);
			if (!F || F->bDraining || F->ReadyAt < 0.0 || F != Oldest()) { ++Counters.RuleViolations; return; }
			F->bDraining = true;
		}

		/**
		 * Worker: the frame ends with Outcome; its slot is free from now on. Frames end in acceptance order: only the
		 * oldest unresolved frame may be resolved. A published frame must have drained; a superseded or abandoned one
		 * must not have.
		 */
		bool Resolve(std::uint64_t Id, EOutcome Outcome)
		{
			FFrame* F = Find(Id);
			const bool bOrder = F && F == Oldest();
			const bool bConsistent = F && Outcome != EOutcome::None
				&& (Outcome != EOutcome::Published || F->bDraining)
				&& ((Outcome != EOutcome::Superseded && Outcome != EOutcome::AccessAbandoned) || !F->bDraining)
				&& (Outcome != EOutcome::Superseded || F->FailedAttempts == 0)
				&& (Outcome != EOutcome::AccessAbandoned || F->FailedAttempts > 0);
			if (!bOrder || !bConsistent)
			{
				++Counters.RuleViolations;
				return false;
			}
			if (Outcome == EOutcome::Published) LastPublishedId = F->Id;
			++Counters.Outcomes[static_cast<int>(Outcome)];
			if (F->Limit == ELimit::Stop) ++Counters.StopOutcomes[static_cast<int>(Outcome)];
			else if (F->Limit == ELimit::Flush) ++Counters.FlushOutcomes[static_cast<int>(Outcome)];
			LastResolvedId = F->Id;
			Frames.erase(Frames.begin());
			return true;
		}

		/** Flush or stop: every frame accepted up to now gets one access attempt only. Returns the last such frame's id. */
		std::uint64_t LimitToOneAttempt(ELimit Why = ELimit::Flush)
		{
			for (FFrame& F : Frames)
			{
				F.bOneAttempt = true;
				if (static_cast<int>(Why) > static_cast<int>(F.Limit)) F.Limit = Why;
			}
			return LastId;
		}

		/** The stream stops: nothing more is accepted; owed frames get one attempt each. */
		void BeginStop()
		{
			bStopping = true;
			LimitToOneAttempt(ELimit::Stop);
		}
		bool IsStopping() const { return bStopping; }

		/** Whether every frame accepted up to Id has been resolved. */
		bool ResolvedUpTo(std::uint64_t Id) const
		{
			return Frames.empty() || Frames.front().Id > Id;
		}

		std::uint64_t GetLastAcceptedId() const { return LastId; }
		std::uint64_t GetLastPublishedId() const { return LastPublishedId; }
		std::uint64_t GetLastResolvedId() const { return LastResolvedId; }
		bool IsEmpty() const { return Frames.empty(); }
		const FCounters& GetCounters() const { return Counters; }
		const std::vector<FFrame>& GetFrames() const { return Frames; }

		FFrame* Find(std::uint64_t Id)
		{
			for (FFrame& F : Frames) if (F.Id == Id) return &F;
			return nullptr;
		}

	private:
		bool IsSlotFree(int Slot) const
		{
			for (const FFrame& F : Frames) if (F.SetGeneration == CurrentSet && F.Slot == Slot) return false;
			return true;
		}

		const int SlotsPerSet;
		std::vector<FFrame> Frames;   // unresolved frames, acceptance order
		std::uint64_t CurrentSet = 0;
		std::uint64_t LastSet = 0;
		std::uint64_t LastId = 0;
		std::uint64_t LastResolvedId = 0;
		std::uint64_t LastPublishedId = 0;
		bool bStopping = false;
		FCounters Counters;
	};
}
