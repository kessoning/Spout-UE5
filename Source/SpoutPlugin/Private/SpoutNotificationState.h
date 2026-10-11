// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * One receive stream's view of its sender's frame notifications.
 *
 * Built on the SDK's public calls only: GetNewFrame (is there something new since the caller's last call),
 * GetSenderFrame (the count that call saw) and IsFrameCountEnabled. This class holds the state and
 * takes the answers of those calls as arguments; FSpoutFrameSync makes the calls. Engine-free, like
 * SpoutSdkSafety.h, so every transition can be driven directly without the engine.
 *
 * The frame count is a notification hint and nothing more. It never identifies an image. Another
 * receiver's GetNewFrame lowers the semaphore for an instant, so a count read at any moment,
 * including while this process holds the texture access mutex, can be one below the true one. No
 * decision here that could withhold an image rests on two counts being equal.
 *
 * State:
 *   owed            a transfer is owed and has not completed successfully
 *   read count      the count at this attempt's read, and at the previous attempt's
 *   count known     the SDK reported a count above zero at this attempt's read
 *
 * Transitions (the methods below, one per event):
 *   an attempt begins (one read)          a new notification sets owed; the read count is recorded
 *   a poll inside a wait                  a count above the read's sets owed and ends the wait
 *   no wait made, count moved since the
 *     previous attempt's read             a transfer is made although nothing is owed (a fallback)
 *   access refused                        nothing changes: owed survives to the next attempt
 *   the read under access                 a new notification sets owed; nothing is cleared
 *   the copy or its completion fails      nothing changes: owed survives
 *   the copy completes                    owed is cleared, here and only here
 *   a forced current frame                owed is set; the read history is forgotten
 *   close, or open                        everything is forgotten
 *
 * Every successful copy is a delivery. A fallback copy can
 * repeat the image already delivered; it is counted (GetCopiesWithoutNotification) and still
 * delivered, because telling a repeat from a new image would mean trusting a count.
 *
 * Not thread-safe: the owner calls it under its own lock.
 */
#pragma once

#include <cstdint>

class FSpoutNotificationState
{
public:
	/** No count: frame counting is off or unavailable, or the sender has not published a count. */
	static constexpr std::int64_t NoCount = -1;

	enum class EPoll : std::int32_t
	{
		Nothing = 0,     // the count is where the read left it
		Published = 1,   // the count rose (or started again): the transfer is owed and the wait ends
		Transient = 2,   // one below the read's count: another receiver's read, ignored
		CountLost = 3    // no count any more: the transfer is owed and the wait ends
	};

	/** Close, or Open of a name: everything is forgotten. */
	void Reset()
	{
		bOwed = false;
		ForgetReadHistory();
		bAttemptTracked = false;
		bFallback = false;
		bNewUnderAccess = false;
		UnderAccessCount = NoCount;
	}

	/** A new native source generation: the current image is owed whatever the count says. */
	void RequestCurrentFrame()
	{
		bOwed = true;
		ForgetReadHistory();
		++CurrentFrameRequests;
	}

	/**
	 * An attempt begins: its one read. bSdkNew is GetNewFrame's answer and Count what GetSenderFrame
	 * returned after it (NoCount when counting is unavailable or the count is zero). bTrack says
	 * whether the read history is kept, which only the experimental settings ask for; without it this
	 * is the pending latch and nothing else. Returns whether a transfer is owed.
	 */
	bool BeginAttempt(bool bSdkNew, std::int64_t Count, bool bTrack)
	{
		bFallback = false;
		bNewUnderAccess = false;
		UnderAccessCount = NoCount;
		bAttemptTracked = bTrack;
		if (bSdkNew)
		{
			bOwed = true;
			++Notifications;
		}
		if (bTrack)
		{
			PreviousReadCount = ReadCount;
			ReadCount = Count > 0 ? Count : NoCount;
		}
		else
		{
			ForgetReadHistory();
		}
		return bOwed;
	}

	/** Whether this attempt's read saw a count. Without one there is no publication to wait for. */
	bool IsCountKnown() const { return bAttemptTracked && ReadCount != NoCount; }

	/**
	 * One poll inside a wait: GetNewFrame's answer and the count after it. Only a count above the one
	 * this attempt's read saw is a publication. A count exactly one below it is another receiver's
	 * read and is ignored, whatever the SDK's answer was: ending the wait on it would give up the
	 * frame the wait is for. A count further below means the count started again.
	 */
	EPoll ObservePoll(bool bSdkNew, std::int64_t Count)
	{
		++Polls;
		if (Count <= 0 || ReadCount == NoCount)
		{
			bOwed = true;
			return EPoll::CountLost;
		}
		if (Count > ReadCount || Count < ReadCount - 1)
		{
			bOwed = true;
			++Notifications;
			return EPoll::Published;
		}
		if (Count == ReadCount - 1)
		{
			++Transients;
			return EPoll::Transient;
		}
		(void)bSdkNew;
		return EPoll::Nothing;
	}

	/**
	 * Whether to transfer although nothing is owed. Only when the read history is kept, no wait was
	 * made in this attempt, and the count at this read differs from the count at the previous
	 * attempt's read: the sender published in between, the previous transfer may have copied that
	 * publication already, and the one after it could be overwritten before the next read.
	 */
	bool ShouldTransferWithoutNotification(bool bWaitMade) const
	{
		return bAttemptTracked && !bOwed && !bWaitMade && ReadCount != NoCount && PreviousReadCount != NoCount && ReadCount != PreviousReadCount;
	}

	/** This attempt transfers without a notification. */
	void BeginFallbackTransfer() { bFallback = true; }

	/**
	 * The read made while texture access is held. A new notification makes the transfer owed. Nothing
	 * is cleared: reading under access does not prove that a copy will complete.
	 */
	void ObserveUnderAccess(bool bSdkNew, std::int64_t Count)
	{
		bNewUnderAccess = bSdkNew;
		UnderAccessCount = Count > 0 ? Count : NoCount;
		if (bSdkNew)
		{
			bOwed = true;
			++Notifications;
		}
		if (UnderAccessCount != NoCount && ReadCount != NoCount && UnderAccessCount != ReadCount) ++CopiedAheadOfRead;
	}

	/**
	 * The copy completed successfully. The only place where owed is cleared. Returns whether the copy
	 * was owed or a fallback, which is always true for a copy the receive path made: every successful
	 * copy is a delivery.
	 */
	bool CompleteTransfer()
	{
		const bool bCounted = bOwed || bFallback;
		if (bFallback && !bOwed) ++CopiesWithoutNotification;
		bOwed = false;
		bFallback = false;
		++Copies;
		return bCounted;
	}

	bool IsOwed() const { return bOwed; }
	bool IsFallback() const { return bFallback; }
	std::int64_t GetReadCount() const { return ReadCount; }
	std::int64_t GetPreviousReadCount() const { return PreviousReadCount; }
	std::int64_t GetUnderAccessCount() const { return UnderAccessCount; }
	bool WasNewUnderAccess() const { return bNewUnderAccess; }

	/** Lifetime counters; never reset. Diagnostics, measurement logs and tests. */
	std::int64_t GetNotifications() const { return Notifications; }
	std::int64_t GetCopies() const { return Copies; }
	std::int64_t GetCopiesWithoutNotification() const { return CopiesWithoutNotification; }
	std::int64_t GetCopiedAheadOfRead() const { return CopiedAheadOfRead; }
	std::int64_t GetPolls() const { return Polls; }
	std::int64_t GetTransients() const { return Transients; }
	std::int64_t GetCurrentFrameRequests() const { return CurrentFrameRequests; }

private:
	void ForgetReadHistory()
	{
		ReadCount = NoCount;
		PreviousReadCount = NoCount;
	}

	bool bOwed = false;
	bool bAttemptTracked = false;
	bool bFallback = false;
	bool bNewUnderAccess = false;
	std::int64_t ReadCount = NoCount;
	std::int64_t PreviousReadCount = NoCount;
	std::int64_t UnderAccessCount = NoCount;

	std::int64_t Notifications = 0;
	std::int64_t Copies = 0;
	std::int64_t CopiesWithoutNotification = 0;
	std::int64_t CopiedAheadOfRead = 0;
	std::int64_t Polls = 0;
	std::int64_t Transients = 0;
	std::int64_t CurrentFrameRequests = 0;
};
