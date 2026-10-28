// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * The bookkeeping of an asynchronous receive stream (experimental, off by default): which private
 * slot a worker may write, which one the render thread reads, and what starts a worker transfer.
 *
 * A worker thread copies the sender's shared texture into a slot and publishes it; the render
 * thread copies the newest published slot into Unreal's own texture. Two GPU users touch a slot,
 * on two devices, and neither waits on the CPU for the other. Two fences order them:
 *   written  the worker signals it after each slot copy; the reader's queue waits on the slot's
 *            written value before its copy out of the slot
 *   read     the reader's queue signals it after its copy out of a slot; a slot is written again
 *            only when the read value recorded on it has been reached
 * This file holds the state only. It makes no graphics call and takes fence values as numbers, so
 * every transition can be driven directly, without a device. Engine-free, like
 * SpoutNotificationState.h.
 *
 * A slot is in exactly one state:
 *   Free      nobody uses it
 *   Writing   the worker's copy into it is in progress (at most one slot at a time)
 *   Ready     its copy completed and the reader has not taken it (at most MaxUnread slots at a time:
 *             the newest image, and with MaxUnread 2 the one before it; MaxUnread 2 is the experimental
 *             retention, r.Spout.Dev.Recv.HoldMaxAgeUs)
 *   Draining  the reader queued a copy out of it; it is free again when the read fence reaches
 *             the value recorded on it
 *
 * Rules the transitions keep:
 *   - a slot is written only when Free, or Draining with its read completed. A Ready slot is never
 *     written: with nothing else available the transfer is skipped and stays owed;
 *   - a slot is read only when Ready, and the reader is given the written value to wait on;
 *   - a newer publication replaces the oldest Ready slot when MaxUnread are Ready already, and the
 *     reader taking the newest replaces every older Ready slot; such a slot is free at once, because
 *     no reader ever touched it. It is counted, and it is not a delivery;
 *   - a write that was given up (access refused, copy failed, completion timed out) leaves the slot
 *     Free and never Ready. Its copy may still be in flight on the worker's device; the next
 *     write into the slot is ordered after it on the same device, and no reader sees the slot
 *     until a later copy completed and was published;
 *   - a slot of another generation (the sender's size, format or share handle changed, or the
 *     stream was reopened) is dropped by the reader and never read;
 *   - a retired pool accepts no write and publishes nothing; it may be destroyed when no slot is
 *     being written and every queued read has completed.
 *
 * Not thread-safe: the owner calls every method under its own lock, and holds that lock for the
 * call only, never across a graphics wait.
 */
#pragma once

#include <cstdint>

namespace SpoutAsync
{
	/** The largest pool. The asynchronous receive uses three slots. */
	constexpr int MaxSlots = 4;

	enum class ESlotState : std::uint8_t
	{
		Free = 0,
		Writing = 1,
		Ready = 2,
		Draining = 3
	};

	/** Why a write was given up. */
	enum class EAbandon : std::int32_t
	{
		AccessRefused = 0,      // the sender or another receiver held the texture for longer than the access wait
		CopyFailed = 1,         // the copy or its fence signal reported an error
		CompletionTimeout = 2,  // the copy did not complete on the GPU inside its bound
		Cancelled = 3,          // the stream closed or changed generation before the copy was made
		Count = 4
	};

	struct FSlotInfo
	{
		ESlotState State = ESlotState::Free;
		std::uint64_t Sequence = 0;      // of the publication in the slot; 0 before the first
		std::uint64_t Generation = 0;    // the stream generation it was copied under
		std::uint64_t WrittenValue = 0;  // the written-fence value of the copy in the slot
		std::uint64_t ReadValue = 0;     // the read-fence value of the copy queued out of it; 0 when not read since its last publication
		std::int64_t Tag = 0;            // the owner's own record of the transfer (its access time, say)
		bool bLastWriteAbandoned = false;
	};

	struct FSlotCounters
	{
		std::int64_t Published = 0;             // copies that completed and were made the newest image
		std::int64_t Consumed = 0;              // publications the reader queued a copy from: the deliveries
		std::int64_t ReplacedUnread = 0;        // publications replaced by a newer one before the reader took them
		std::int64_t DroppedOldGeneration = 0;  // publications of another generation, dropped by the reader unread
		std::int64_t NoFreeSlot = 0;            // transfers skipped because every slot was Ready or still being read
		std::int64_t Abandoned[static_cast<int>(EAbandon::Count)] = { 0, 0, 0, 0 };
		std::int64_t PublishedAfterReuse = 0;   // publications into a slot whose previous write had been given up
		std::int64_t RefusedRetired = 0;        // writes asked of a retired pool
		std::int64_t PublishedAfterRetire = 0;  // copies that completed after the pool was retired: never published
		std::int64_t RuleViolations = 0;        // calls the owner must never make; always 0 in a correct owner
	};

	/** What the reader took. */
	struct FConsumed
	{
		int Slot = -1;
		std::uint64_t Sequence = 0;
		std::uint64_t Generation = 0;
		std::uint64_t WrittenValue = 0;  // the reader's queue waits on this value of the written fence before its copy
		std::uint64_t ReadValue = 0;     // and signals this value of the read fence after it
		std::int64_t Tag = 0;
	};

	enum class EConsume : std::int32_t
	{
		Nothing = 0,              // no publication the reader has not taken
		Consumed = 1,             // the reader must now queue: wait(written), copy, signal(read)
		DroppedOldGeneration = 2  // the newest publication belonged to another generation: dropped unread
	};

	/** Which unread publication the reader takes when there are two (a pool that keeps two unread). */
	enum class ETake : std::int32_t
	{
		Newest = 0,  // the newest; every older unread one is replaced unread
		Oldest = 1   // the oldest; the newer one stays ready for the reader's next call
	};

	class FSlotPool
	{
	public:
		/**
		 * InMaxUnread is how many publications may wait for the reader: 1 (the newest only; a newer publication
		 * replaces it) or 2 (the newest and the one before it; a third replaces the oldest).
		 */
		explicit FSlotPool(int InNumSlots = 3, int InMaxUnread = 1)
			: NumSlots(InNumSlots < 1 ? 1 : (InNumSlots > MaxSlots ? MaxSlots : InNumSlots))
			, MaxUnread(InMaxUnread < 1 ? 1 : (InMaxUnread > 2 ? 2 : InMaxUnread))
		{
		}

		int GetNumSlots() const { return NumSlots; }
		int GetMaxUnread() const { return MaxUnread; }
		const FSlotInfo& GetSlot(int Slot) const { return Slots[Slot]; }
		const FSlotCounters& GetCounters() const { return Counters; }
		bool IsRetired() const { return bRetired; }

		// ---- the worker ----

		/**
		 * A slot to write into, or -1 when the transfer must be skipped (it stays owed).
		 * CompletedReadValue is the read fence's completed value now.
		 */
		int BeginWrite(std::uint64_t CompletedReadValue)
		{
			if (bRetired)
			{
				++Counters.RefusedRetired;
				return -1;
			}
			if (Find(ESlotState::Writing) >= 0)
			{
				++Counters.RuleViolations; // one transfer in flight
				return -1;
			}
			int Pick = Find(ESlotState::Free);
			if (Pick < 0)
			{
				// A slot the reader queued a copy from is free again only when that copy has completed.
				for (int I = 0; I < NumSlots; ++I)
				{
					if (Slots[I].State == ESlotState::Draining && Slots[I].ReadValue <= CompletedReadValue
						&& (Pick < 0 || Slots[I].ReadValue < Slots[Pick].ReadValue))
					{
						Pick = I;
					}
				}
			}
			if (Pick < 0)
			{
				++Counters.NoFreeSlot;
				return -1;
			}
			Slots[Pick].State = ESlotState::Writing;
			return Pick;
		}

		/**
		 * The smallest read value a Draining slot still waits for, or 0 when none is Draining. The worker may
		 * wait for the read fence to reach it, bounded, before it gives a transfer up for lack of a slot.
		 */
		std::uint64_t OldestPendingReadValue() const
		{
			std::uint64_t Oldest = 0;
			for (int I = 0; I < NumSlots; ++I)
			{
				if (Slots[I].State == ESlotState::Draining && (Oldest == 0 || Slots[I].ReadValue < Oldest))
				{
					Oldest = Slots[I].ReadValue;
				}
			}
			return Oldest;
		}

		/** The write into Slot was given up. The slot is free; nothing was published. */
		void AbandonWrite(int Slot, EAbandon Why)
		{
			if (!IsInState(Slot, ESlotState::Writing))
			{
				++Counters.RuleViolations;
				return;
			}
			Slots[Slot].State = ESlotState::Free;
			Slots[Slot].bLastWriteAbandoned = true;
			++Counters.Abandoned[static_cast<int>(Why)];
		}

		/**
		 * The copy into Slot completed on the GPU: it becomes the newest image. Returns its sequence number, or 0
		 * when nothing was published (the pool was retired meanwhile, or the call broke a rule).
		 * WrittenValue is the written-fence value signalled after the copy; it must rise with every publication.
		 */
		std::uint64_t Publish(int Slot, std::uint64_t Generation, std::uint64_t WrittenValue, std::int64_t Tag)
		{
			if (!IsInState(Slot, ESlotState::Writing))
			{
				++Counters.RuleViolations;
				return 0;
			}
			FSlotInfo& S = Slots[Slot];
			if (bRetired)
			{
				S.State = ESlotState::Free;
				++Counters.PublishedAfterRetire;
				return 0;
			}
			if (WrittenValue <= LastWrittenValue)
			{
				S.State = ESlotState::Free;
				++Counters.RuleViolations;
				return 0;
			}
			LastWrittenValue = WrittenValue;
			while (ReadyCount() >= MaxUnread)
			{
				// Never read, so no reader's copy is pending on it: free at once.
				Slots[FindReady(false)].State = ESlotState::Free;
				++Counters.ReplacedUnread;
			}
			if (S.bLastWriteAbandoned)
			{
				S.bLastWriteAbandoned = false;
				++Counters.PublishedAfterReuse;
			}
			S.State = ESlotState::Ready;
			S.Sequence = ++LastSequence;
			S.Generation = Generation;
			S.WrittenValue = WrittenValue;
			S.ReadValue = 0;
			S.Tag = Tag;
			++Counters.Published;
			return S.Sequence;
		}

		// ---- the reader (the render thread) ----

		/** Whether a publication waits for the reader. */
		bool HasReady() const { return Find(ESlotState::Ready) >= 0; }

		/** How many publications wait for the reader (at most the pool's MaxUnread). */
		int ReadyCount() const
		{
			int N = 0;
			for (int I = 0; I < NumSlots; ++I) N += Slots[I].State == ESlotState::Ready ? 1 : 0;
			return N;
		}

		/** The newest (or oldest) publication waiting for the reader, or null when none waits. */
		const FSlotInfo* PeekReady(bool bNewest) const
		{
			const int Slot = FindReady(bNewest);
			return Slot >= 0 ? &Slots[Slot] : nullptr;
		}

		/** Whether the worker's copy into a slot is in progress: a newer image may be published shortly. */
		bool HasWriting() const { return Find(ESlotState::Writing) >= 0; }

		/**
		 * Takes a publication: the newest (every older one waiting is then replaced unread), or with ETake::Oldest the
		 * oldest, the newer one staying ready. Publications of another generation, or of a retired pool, are dropped
		 * unread first. On Consumed the caller must queue, in this order and without fail: a wait on the written fence
		 * for Out.WrittenValue, its copy out of the slot, and a signal of the read fence with ReadValue. ReadValue must
		 * rise with every call that consumes.
		 */
		EConsume Consume(std::uint64_t CurrentGeneration, std::uint64_t ReadValue, FConsumed& Out, ETake Take = ETake::Newest)
		{
			Out = FConsumed();
			bool bDropped = false;
			for (int I = 0; I < NumSlots; ++I)
			{
				if (Slots[I].State == ESlotState::Ready && (bRetired || Slots[I].Generation != CurrentGeneration))
				{
					Slots[I].State = ESlotState::Free;
					++Counters.DroppedOldGeneration;
					bDropped = true;
				}
			}
			const int Slot = FindReady(Take == ETake::Newest);
			if (Slot < 0)
			{
				return bDropped ? EConsume::DroppedOldGeneration : EConsume::Nothing;
			}
			FSlotInfo& S = Slots[Slot];
			if (ReadValue <= LastReadValue)
			{
				++Counters.RuleViolations; // the slots stay Ready: nothing was queued for them
				return EConsume::Nothing;
			}
			LastReadValue = ReadValue;
			if (Take == ETake::Newest)
			{
				for (int I = 0; I < NumSlots; ++I)
				{
					if (I != Slot && Slots[I].State == ESlotState::Ready)
					{
						// Older and never read: free at once, as when a newer publication replaces it.
						Slots[I].State = ESlotState::Free;
						++Counters.ReplacedUnread;
					}
				}
			}
			S.State = ESlotState::Draining;
			S.ReadValue = ReadValue;
			++Counters.Consumed;
			Out.Slot = Slot;
			Out.Sequence = S.Sequence;
			Out.Generation = S.Generation;
			Out.WrittenValue = S.WrittenValue;
			Out.ReadValue = ReadValue;
			Out.Tag = S.Tag;
			return EConsume::Consumed;
		}

		// ---- retirement (a new generation, or close) ----

		/** No further write is accepted and nothing further is published or read. */
		void Retire() { bRetired = true; }

		/** Whether the slots may be destroyed: none is being written, and every queued read has completed. */
		bool CanDestroy(std::uint64_t CompletedReadValue) const
		{
			for (int I = 0; I < NumSlots; ++I)
			{
				if (Slots[I].State == ESlotState::Writing) return false;
				if (Slots[I].State == ESlotState::Draining && Slots[I].ReadValue > CompletedReadValue) return false;
			}
			return true;
		}

	private:
		int Find(ESlotState State) const
		{
			for (int I = 0; I < NumSlots; ++I)
			{
				if (Slots[I].State == State) return I;
			}
			return -1;
		}

		/** The Ready slot with the highest (or lowest) sequence, or -1. */
		int FindReady(bool bNewest) const
		{
			int Pick = -1;
			for (int I = 0; I < NumSlots; ++I)
			{
				if (Slots[I].State == ESlotState::Ready
					&& (Pick < 0 || (bNewest ? Slots[I].Sequence > Slots[Pick].Sequence : Slots[I].Sequence < Slots[Pick].Sequence)))
				{
					Pick = I;
				}
			}
			return Pick;
		}

		bool IsInState(int Slot, ESlotState State) const { return Slot >= 0 && Slot < NumSlots && Slots[Slot].State == State; }

		FSlotInfo Slots[MaxSlots];
		FSlotCounters Counters;
		int NumSlots;
		int MaxUnread;
		std::uint64_t LastSequence = 0;
		std::uint64_t LastWrittenValue = 0;
		std::uint64_t LastReadValue = 0;
		bool bRetired = false;
	};

	/**
	 * The bounded retention's rate guard (r.Spout.Dev.Recv.HoldMaxAgeUs): publications per
	 * reader call, in thousandths, averaged over about 64 calls. A reader slower than its source cannot show every
	 * frame whatever it takes, so keeping an older image would only add latency: the older of two is taken only while
	 * the source is at most LimitPermille per thousand faster than the reader. Not thread-safe: called under the
	 * owner's lock, like the pool.
	 */
	class FRateGuard
	{
	public:
		/** A publication (the worker). */
		void OnPublished() { ++SinceCall; }

		/** A reader call: updates the average and says whether the older of two may be taken. */
		bool OnCall(std::int64_t LimitPermille = 1030)
		{
			// Acc is 64 times the average, so that the integer update has no dead band: a plain "Avg += (x - Avg) / 64"
			// stops moving within 63 permille of the true rate, and occasional bursts then ratchet it upwards.
			Acc += SinceCall * 1000 - Acc / 64;
			SinceCall = 0;
			return GetPermille() <= LimitPermille;
		}

		std::int64_t GetPermille() const { return Acc / 64; }

	private:
		std::int64_t SinceCall = 0;
		std::int64_t Acc = 64 * 1000;
	};

	/**
	 * What starts a worker transfer, and what keeps it from being repeated.
	 *
	 *   - a new notification makes a transfer owed;
	 *   - an owed transfer is attempted, and attempted again after a refused access, a failed copy, a
	 *     timed-out completion or a missing slot, without another notification;
	 *   - a forced or initial copy (open, a new generation) is owed and is made once;
	 *   - a transfer that completed is not repeated for the same notification;
	 *   - a sender without a usable frame count gives no notification to wait for. The worker then
	 *     transfers at most once per image the reader consumed, and never free-runs: the forced copy
	 *     at the start needs no credit, and one credit is given each time the reader consumes.
	 *
	 * Not thread-safe: called under the owner's lock, like the pool.
	 */
	class FTransferGate
	{
	public:
		/** Open, or a new generation: the current image is owed whatever the notifications say. */
		void Reset()
		{
			bOwed = true;
			bUncountedCredit = false;
			++Forced;
		}

		/** The worker's poll of a sender that publishes a count. bNew is the SDK's answer. */
		void ObserveCounted(bool bNew)
		{
			if (bNew)
			{
				bOwed = true;
				++Notifications;
			}
		}

		/** The worker's poll of a sender without a usable count: owed only while a credit is left. */
		void ObserveUncounted()
		{
			if (bUncountedCredit && !bOwed)
			{
				bOwed = true;
				bUncountedCredit = false;
				++UncountedStarts;
			}
		}

		/** The reader consumed a publication: an uncounted sender may be copied once more. */
		void OnConsumed() { bUncountedCredit = true; }

		bool IsOwed() const { return bOwed; }

		/** The copy completed and was published. Owed is cleared here and only here. */
		void OnPublished()
		{
			bOwed = false;
			++Completed;
		}

		std::int64_t GetNotifications() const { return Notifications; }
		std::int64_t GetForced() const { return Forced; }
		std::int64_t GetUncountedStarts() const { return UncountedStarts; }
		std::int64_t GetCompleted() const { return Completed; }

	private:
		bool bOwed = false;
		bool bUncountedCredit = false;
		std::int64_t Notifications = 0;
		std::int64_t Forced = 0;
		std::int64_t UncountedStarts = 0;
		std::int64_t Completed = 0;
	};
}
