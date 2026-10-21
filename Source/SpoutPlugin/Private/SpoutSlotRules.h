// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * When GPU work that used the shared-slot path's objects is known to have completed. Engine-free, like
 * SpoutSlotPool.h, so every rule can be driven with plain numbers.
 *
 * The shared-slot path (SpoutSharedSlot.h) moves pixels between two devices: Unreal's graphics queue and the
 * plugin's own native D3D11 device. Each device signals one fence after the work it was given:
 *   Native  the native device signals it after each of its copies. A value is recorded only when the signal call
 *           succeeded: only then does the fence ever reach it.
 *   Queue   Unreal's queue signals it after each RHI copy the plugin queued there.
 * A signal is executed after all work submitted before it on the same device context or queue (D3D11
 * ID3D11DeviceContext4::Signal, D3D12 ID3D12CommandQueue::Signal). So a fence that reached the highest value
 * recorded for it proves that every operation submitted before that signal has finished.
 *
 * A wait that timed out cancels nothing. The work it waited for is still submitted, and the value its signal will
 * reach stays recorded here, so the objects it uses are released only once it is reached.
 *
 * A native copy whose signal call FAILED has no fence value that will prove it. Its completion is then proven by an
 * event query issued after it on the same context (it reports done only when all earlier work is done), or by the
 * removal of the device. When even the query cannot be issued, only removal proves it; until then the objects are
 * never released.
 *
 * A removed device makes a D3D12 fence report the largest value: every condition on it counts as reached, and
 * no further work of that device will ever run.
 *
 * Not thread-safe: the owner calls it under its own lock.
 */
#pragma once

#include <cstdint>

namespace SpoutSlotRules
{
	/** The completed value a fence reports once its device was removed. */
	constexpr std::uint64_t RemovedValue = ~static_cast<std::uint64_t>(0);

	/** Whether a fence that completed Completed has reached Required. A removed device reaches everything. */
	inline bool Reached(std::uint64_t Completed, std::uint64_t Required)
	{
		return Completed == RemovedValue || Completed >= Required;
	}

	/**
	 * The native device's progress as far as it is proven. The native fences are D3D12 fences on Unreal's device, read
	 * through their D3D12 view. When Unreal's device is removed that view reports RemovedValue, which says nothing about
	 * the native device, which may still be running. Then only an event query on the native context (bQueryProven)
	 * proves that its earlier work finished; until it does, nothing is proven (0). A removed native device runs nothing
	 * more: everything counts as reached.
	 */
	inline std::uint64_t NativeProgress(std::uint64_t ViewCompleted, bool bNativeRemoved, bool bQueryProven)
	{
		if (bNativeRemoved) return RemovedValue;
		if (ViewCompleted == RemovedValue) return bQueryProven ? RemovedValue : 0;
		return ViewCompleted;
	}

	/** How native work whose signal failed is proven to have finished. */
	enum class EProof : std::uint8_t
	{
		NotNeeded = 0,  // every native copy had a successful signal
		Pending = 1,    // an event query was issued after the unproven copy and has not reported yet
		Proven = 2,     // that query reported that all earlier work finished
		Impossible = 3  // no query could be issued: only device removal proves completion
	};

	/** The outstanding GPU work of one set of shared objects. */
	class FOutstanding
	{
	public:
		/** A native copy was submitted and its signal of Value succeeded. */
		void NativeSignalled(std::uint64_t Value)
		{
			if (Value > NativeRequired) NativeRequired = Value;
		}

		/** A signal of Value was queued on Unreal's queue after the plugin's RHI copy. */
		void QueueSignalQueued(std::uint64_t Value)
		{
			if (Value > QueueRequired) QueueRequired = Value;
		}

		/**
		 * A native copy was submitted but its signal call failed. bProofIssued says whether an event query was
		 * issued after it. A later failure never weakens an earlier state: Impossible stays Impossible.
		 */
		void NativeSignalFailed(bool bProofIssued)
		{
			if (Proof == EProof::Impossible) return;
			Proof = bProofIssued ? EProof::Pending : EProof::Impossible;
		}

		/** The event query issued after the unproven copy reported that all earlier work finished. */
		void ProofCompleted()
		{
			if (Proof == EProof::Pending) Proof = EProof::Proven;
		}

		/** Whether an event query result is still needed. */
		bool NeedsProofQuery() const { return Proof == EProof::Pending; }

		/**
		 * Whether every operation that used the objects has finished. NativeCompleted and QueueCompleted are the
		 * fences' completed values now (RemovedValue after a removal). bNativeRemoved says the native device was
		 * removed: nothing it was given will run any more.
		 */
		bool IsIdle(std::uint64_t NativeCompleted, std::uint64_t QueueCompleted, bool bNativeRemoved) const
		{
			const bool bNativeDone = bNativeRemoved
				|| (Reached(NativeCompleted, NativeRequired) && (Proof == EProof::NotNeeded || Proof == EProof::Proven));
			return bNativeDone && Reached(QueueCompleted, QueueRequired);
		}

		/**
		 * Send path: whether Unreal's queue may write the slot again. Only when no native copy may still be reading
		 * it: every recorded native signal reached, and no unproven native copy left.
		 */
		bool CanQueueWrite(std::uint64_t NativeCompleted, bool bNativeRemoved) const
		{
			if (bNativeRemoved) return false; // nothing to send with
			return Reached(NativeCompleted, NativeRequired) && (Proof == EProof::NotNeeded || Proof == EProof::Proven);
		}

		std::uint64_t GetNativeRequired() const { return NativeRequired; }
		std::uint64_t GetQueueRequired() const { return QueueRequired; }
		EProof GetProof() const { return Proof; }

	private:
		std::uint64_t NativeRequired = 0;
		std::uint64_t QueueRequired = 0;
		EProof Proof = EProof::NotNeeded;
	};

	/**
	 * What one bounded or unbounded CPU wait for a fence ended with. A wait whose completion event cannot be set up
	 * polls the fence instead, so there is no third way for a wait to fail.
	 */
	enum class EWait : std::uint8_t
	{
		Reached = 0,   // the value was reached
		TimedOut = 1,  // the bound passed first; the work is still submitted and still recorded
		Removed = 2    // a device was removed: the work will never run
	};

	/**
	 * Consecutive timed-out copies a stream accepts before it stops: the same number the asynchronous receive
	 * uses (MaxConsecutiveTimeouts in SpoutAsyncReceive.h), kept as its own constant here.
	 */
	constexpr int MaxConsecutiveTimeouts = 8;

	/** Counts consecutive timeouts; a success resets it. */
	class FTimeoutStreak
	{
	public:
		/** Returns true when the stream must stop. */
		bool OnTimeout() { return ++Count >= MaxConsecutiveTimeouts; }
		void OnSuccess() { Count = 0; }
		int GetCount() const { return Count; }

	private:
		int Count = 0;
	};
}
