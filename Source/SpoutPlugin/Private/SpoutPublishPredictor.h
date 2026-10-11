// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Predicts when a Spout sender will publish its next frame, from the frame counts a receiver has
 * observed. Used only by the experimental new-frame wait (r.Spout.FrameWaitMs, off by default).
 *
 * Engine-free on purpose, like SpoutSdkSafety.h: no Unreal type, no SDK, no clock of its own.
 *
 * Every observation is "at time T the count was C". A count above the last one seen bounds the
 * newest publication to (the last time the lower count was seen, T]. The period is the count delta
 * over a sliding window divided into its span, so publications the receiver never copied still
 * count. Bounds are carried forward one period per publication and intersected with each new
 * observation.
 *
 * The count is used as a notification of activity and for nothing else. It never identifies an
 * image: another receiver's read lowers the count for an instant, so a count read at any time,
 * including under the texture access mutex, can be one below the true one.
 *
 * Not thread-safe: the owner (FSpoutFrameSync) calls it under its own lock.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace SpoutFrameWait
{
	/**
	 * Which part of the predicted publication window must be reachable before a wait is attempted.
	 * The reachable interval is [now - overdue tolerance, now + bound].
	 *
	 * UpperEdge: the whole window must end inside the bound.
	 * Overlap: any overlap of the window with the reachable interval is enough.
	 * Conditional (the default): the count was still unchanged at now, so the publication lies in
	 * the part of the window after now; wait only if at least FTuning::MinConditionalShare of that
	 * part lies within the bound.
	 */
	enum class EWaitRule : std::int32_t
	{
		UpperEdge = 0,
		Overlap = 1,
		Conditional = 2
	};

	/**
	 * What evidence of trouble does to waiting.
	 *
	 * Original: an observation outside the carried window adds to a score that suspends waiting; so
	 * do wasted waits. The jitter allowance is fixed.
	 *
	 * Adaptive (the default): an inaccurate prediction is not evidence that waiting is unproductive.
	 * An inconsistent observation widens the jitter allowance (bounded) and never suspends; a late
	 * publication keeps the carried lower edge. Only wasted waits suspend, and each suspension that
	 * follows another without a successful wait in between is twice as long (bounded). An escalated
	 * suspension is probed: at most one wait every FTuning::ProbeIntervalReads reads, and only where
	 * the rest of the rule would wait; a probe that gets its frame ends the suspension.
	 *
	 * A source slower than the reader on average is not waited for (EDecision::SlowerSource). That
	 * is a tradeoff, not a free saving: for a source at half the read rate it costs freshness that a
	 * wait would have bought, and for a source slightly slower than the reader it avoids waits that
	 * mostly time out.
	 */
	enum class ERecovery : std::int32_t
	{
		Original = 0,
		Adaptive = 1
	};

	/** Why a wait was or was not attempted on one receive call. The values are written to measurement logs. */
	enum class EDecision : std::int32_t
	{
		Disabled = 0,        // r.Spout.FrameWaitMs is 0
		NoCount = 1,         // the sender publishes no usable frame count
		Warmup = 2,          // too few publications observed to estimate the period, or no bounds yet
		Suspended = 3,       // waiting suspended after wasted waits
		Overdue = 4,         // the predicted publication is further in the past than tolerated
		NotImminent = 5,     // the predicted window is not reachable within the bound
		BudgetExhausted = 6, // other streams used this render frame's wait budget
		AccessHeld = 7,      // refused: this stream holds the access mutex (it must never wait then)
		AlreadyNew = 8,      // a frame is already owed; no wait needed
		Wait = 9,            // a wait was attempted
		SlowerSource = 10    // the source's period is longer than the read interval
	};

	/** How an attempted wait ended. The values are written to measurement logs. */
	enum class EOutcome : std::int32_t
	{
		NotAttempted = 0,
		Published = 1,   // the count rose inside the bound
		TimedOut = 2,    // the bound elapsed first (a wasted wait)
		Aborted = 3,     // the stream was cancelled or the engine is exiting
		Closed = 4,      // the frame sync was closed underneath the wait
		CountLost = 5    // the count stopped being available during the wait
	};

	/** Tunables that are not console variables. Fixed here so the behaviour is reproducible. */
	struct FTuning
	{
		/** Publications needed inside the window before the period is trusted. */
		static constexpr std::int32_t MinPublications = 30;
		/** Shortest span of the period window that is trusted, seconds. */
		static constexpr double MinWindowSeconds = 0.25;
		/** Samples older than this leave the period window, seconds. */
		static constexpr double WindowSeconds = 4.0;
		static constexpr std::int32_t MaxWindowSamples = 512;
		/** Sender jitter allowed on each side of a predicted publication, seconds; the floor of the adaptive allowance. */
		static constexpr double JitterAllowanceSeconds = 0.00025;
		/** The most the adaptive jitter allowance can grow to, seconds. */
		static constexpr double JitterAllowanceMaxSeconds = 0.004;
		/** Consistent observations over which a widened allowance decays most of the way back (1/e). */
		static constexpr double JitterDecayPublications = 256.0;
		/** EWaitRule::Conditional: least share of the window after now that must lie within the bound. */
		static constexpr double MinConditionalShare = 0.15;
		/** A source whose period exceeds the read interval by more than this fraction is slower than the reader. */
		static constexpr double SlowerSourceMargin = 0.02;
		/** Smoothing of the read interval estimate, per read. */
		static constexpr double ReadIntervalSmoothing = 0.05;
		/** A publication predicted this far in the past is still waited for; older means paused or late. */
		static constexpr double OverdueToleranceSeconds = 0.004;
		/** ...but never more than this fraction of the source period. */
		static constexpr double OverdueToleranceOfPeriod = 0.25;
		/**
		 * Wasted-wait score: a wait that times out adds WastedWaitScore, one that gets its frame takes
		 * SuccessfulWaitCredit off (never below zero), and reaching WasteScoreToSuspend suspends
		 * waiting. Two wasted waits in a row suspend. Between two suspensions the wasted waits are at
		 * most (successful waits + 8) / 3.
		 */
		static constexpr std::int32_t WastedWaitScore = 3;
		static constexpr std::int32_t SuccessfulWaitCredit = 1;
		static constexpr std::int32_t WasteScoreToSuspend = 6;
		/** ERecovery::Original: inconsistency score that suspends waiting (+4 per inconsistent observation, -1 per consistent one). */
		static constexpr std::int32_t InconsistencyToSuspend = 12;
		/** Reads for which waiting stays suspended (adaptive: the first suspension after a successful wait). */
		static constexpr std::int64_t SuspendReads = 120;
		/** The longest adaptive suspension, reads. */
		static constexpr std::int64_t MaxSuspendReads = 7680;
		/** During a suspension, at most one probe wait every this many reads. */
		static constexpr std::int64_t ProbeIntervalReads = SuspendReads;
	};

	struct FPrediction
	{
		EDecision Decision = EDecision::Disabled;
		/** Estimated source period, seconds; 0 when unknown. */
		double PeriodSeconds = 0.0;
		/** Uncertainty of the period per publication, seconds. */
		double PeriodErrorSeconds = 0.0;
		/** Bounds of the predicted next publication on the caller's clock, seconds; 0 when unknown. */
		double EarliestSeconds = 0.0;
		double LatestSeconds = 0.0;
		/** Share of the window after now that lies within the bound (EWaitRule::Conditional); -1 when not computed. */
		double ConditionalShare = -1.0;
		/** Jitter allowance used for this prediction, seconds. */
		double JitterSeconds = 0.0;
		/** This Wait is a probe of a suspension. */
		bool bProbe = false;
	};

	class FPublishPredictor
	{
	public:
		/**
		 * Clears what was learnt about the sender (bounds, period, jitter allowance). The rule, the
		 * recovery policy, the suspension and its escalation, the read interval (the reader's, not
		 * the sender's) and the statistics survive: a sender that restarts every second must not be
		 * able to clear a suspension by doing so.
		 */
		void Reset()
		{
			Window.clear();
			bHaveCount = false;
			bHaveBounds = false;
			LastCount = 0;
			LastSeenSeconds = 0.0;
			PubLoSeconds = 0.0;
			PubHiSeconds = 0.0;
			JitterSeconds = FTuning::JitterAllowanceSeconds;
			WasteScore = 0;
			InconsistencyScore = 0;
			++Resets;
		}

		void SetRule(EWaitRule InRule) { Rule = InRule; }
		EWaitRule GetRule() const { return Rule; }
		void SetRecovery(ERecovery InRecovery) { Recovery = InRecovery; }
		ERecovery GetRecovery() const { return Recovery; }

		/**
		 * One count observation of any kind: a read, a poll inside a wait, or a read under access.
		 * Only differences matter. NowSeconds is when the count was read. A negative count means no
		 * count is available.
		 */
		void ObserveCount(double NowSeconds, std::int64_t Count)
		{
			if (Count < 0)
			{
				if (bHaveCount) Reset();
				return;
			}
			if (!bHaveCount)
			{
				bHaveCount = true;
				bHaveBounds = false;
				LastCount = Count;
				LastSeenSeconds = NowSeconds;
				return;
			}
			if (Count == LastCount)
			{
				LastSeenSeconds = (std::max)(LastSeenSeconds, NowSeconds);
				return;
			}
			if (Count == LastCount - 1)
			{
				// One below the last count: the instant in which another receiver's read has lowered
				// the semaphore. Not a new fact about the sender; ignored and counted.
				++Glitches;
				return;
			}
			if (Count < LastCount)
			{
				// The count started again.
				Reset();
				bHaveCount = true;
				LastCount = Count;
				LastSeenSeconds = NowSeconds;
				return;
			}

			// Count > LastCount: the newest publication happened after the lower count was last seen
			// and no later than now.
			const std::int64_t Steps = Count - LastCount;
			const double ObservedLo = LastSeenSeconds;
			const double ObservedHi = NowSeconds;

			double Period = 0.0, PeriodError = 0.0;
			const bool bPeriod = EstimatePeriod(Period, PeriodError);
			bool bConsistent = false;
			bool bBoundsSet = false;
			if (bHaveBounds && bPeriod)
			{
				const double Slack = double(Steps) * (PeriodError + JitterSeconds);
				const double CarriedLo = PubLoSeconds + double(Steps) * Period - Slack;
				const double CarriedHi = PubHiSeconds + double(Steps) * Period + Slack;
				const double Lo = (std::max)(ObservedLo, CarriedLo);
				const double Hi = (std::min)(ObservedHi, CarriedHi);
				if (Lo <= Hi)
				{
					PubLoSeconds = Lo;
					PubHiSeconds = Hi;
					bConsistent = true;
					InconsistencyScore = (std::max)(0, InconsistencyScore - 1);
					if (Recovery == ERecovery::Adaptive)
					{
						// A widened allowance decays back towards the floor while the sender behaves.
						JitterSeconds -= (JitterSeconds - FTuning::JitterAllowanceSeconds) / FTuning::JitterDecayPublications;
					}
				}
				else
				{
					// The publication did not fall where a regular sender would have put it.
					++InconsistentObservations;
					if (Recovery == ERecovery::Original)
					{
						InconsistencyScore += 4;
						if (InconsistencyScore >= FTuning::InconsistencyToSuspend) Suspend(true);
					}
					else
					{
						// Allow as much more jitter per publication as the observation missed by
						// (bounded); no suspension.
						const double Miss = (std::max)(ObservedLo - CarriedHi, CarriedLo - ObservedHi);
						JitterSeconds = (std::min)(FTuning::JitterAllowanceMaxSeconds, (std::max)(JitterSeconds, JitterSeconds + Miss / double(Steps)));
						if (ObservedLo > CarriedHi)
						{
							// Late. A sender cannot publish before its slot, only after it, so a late
							// publication is no evidence that the next one will be late: keep the
							// carried lower edge and extend the upper edge to the observation.
							PubLoSeconds = CarriedLo;
							PubHiSeconds = ObservedHi;
							bBoundsSet = true;
						}
					}
				}
			}
			if (!bConsistent && !bBoundsSet)
			{
				PubLoSeconds = ObservedLo;
				PubHiSeconds = ObservedHi;
			}
			bHaveBounds = true;

			// The period window uses the raw observation, never the carried bounds, so the estimate
			// cannot feed on itself.
			FSample Sample;
			Sample.TimeSeconds = ObservedHi;
			Sample.WidthSeconds = (std::max)(0.0, ObservedHi - ObservedLo);
			Sample.Count = Count;
			Window.push_back(Sample);
			std::size_t Drop = 0;
			while (Drop + 2 < Window.size() && (NowSeconds - Window[Drop].TimeSeconds > FTuning::WindowSeconds || Window.size() - Drop > std::size_t(FTuning::MaxWindowSamples))) ++Drop;
			if (Drop > 0) Window.erase(Window.begin(), Window.begin() + static_cast<std::ptrdiff_t>(Drop));

			LastCount = Count;
			LastSeenSeconds = NowSeconds;
		}

		/**
		 * Marks the start of one receive attempt's read; suspension lengths and probe times are counted
		 * in these. With a time (>= 0) the read interval is also estimated. Exactly once per receive
		 * attempt: a poll inside a wait is not a read.
		 */
		void BeginRead(double NowSeconds = -1.0)
		{
			++ReadIndex;
			if (NowSeconds < 0.0) return;
			if (LastReadSeconds >= 0.0)
			{
				const double Delta = NowSeconds - LastReadSeconds;
				if (Delta > 0.0 && Delta < 0.1)
				{
					ReadIntervalSeconds = (ReadIntervalSeconds <= 0.0) ? Delta : ReadIntervalSeconds + FTuning::ReadIntervalSmoothing * (Delta - ReadIntervalSeconds);
				}
			}
			LastReadSeconds = NowSeconds;
		}

		/** A wait ended without a publication. While suspended (adaptive) the wait was a probe. */
		void ObserveWaitTimedOut()
		{
			++WastedWaits;
			if (Recovery == ERecovery::Adaptive && IsSuspended())
			{
				// A probe that timed out. The suspension stays as it is; the next probe is one interval later.
				++Probes;
				NextProbeRead = ReadIndex + FTuning::ProbeIntervalReads;
				return;
			}
			WasteScore += FTuning::WastedWaitScore;
			if (WasteScore >= FTuning::WasteScoreToSuspend) Suspend(false);
		}

		/** A wait ended with a publication. While suspended (adaptive) the wait was a probe, and the suspension ends. */
		void ObserveWaitSucceeded()
		{
			WasteScore = (std::max)(0, WasteScore - FTuning::SuccessfulWaitCredit);
			if (Recovery == ERecovery::Adaptive && IsSuspended())
			{
				// The escalation is kept until an ordinary wait succeeds, so a probe that succeeds by
				// chance on an irregular source does not restart it from the base length.
				++Probes;
				++ProbeSuccesses;
				SuspendedUntilRead = ReadIndex;
				return;
			}
			// Waiting worked: the next suspension, if any, starts again from the base length.
			NextSuspendReads = FTuning::SuspendReads;
		}

		/** Whether to wait now, given that the current read found no new frame. */
		FPrediction Predict(double NowSeconds, double WaitSeconds) const
		{
			FPrediction Out;
			if (!bHaveCount)
			{
				Out.Decision = EDecision::NoCount;
				return Out;
			}
			double Period = 0.0, PeriodError = 0.0;
			if (!EstimatePeriod(Period, PeriodError) || !bHaveBounds)
			{
				Out.Decision = EDecision::Warmup;
				return Out;
			}
			Out.PeriodSeconds = Period;
			Out.PeriodErrorSeconds = PeriodError;
			Out.JitterSeconds = JitterSeconds;
			const double Slack = PeriodError + JitterSeconds;
			Out.EarliestSeconds = PubLoSeconds + Period - Slack;
			Out.LatestSeconds = PubHiSeconds + Period + Slack;
			if (IsSuspended())
			{
				// An escalated suspension may be probed where the rest of the rule would wait; any
				// other outcome of the rule stays Suspended.
				if (Recovery != ERecovery::Adaptive || ReadIndex < NextProbeRead)
				{
					Out.Decision = EDecision::Suspended;
					return Out;
				}
				Decide(Out, NowSeconds, WaitSeconds, Period);
				if (Out.Decision == EDecision::Wait)
				{
					Out.bProbe = true;
				}
				else
				{
					Out.Decision = EDecision::Suspended;
				}
				return Out;
			}
			Decide(Out, NowSeconds, WaitSeconds, Period);
			return Out;
		}

		std::int64_t GetLastCount() const { return bHaveCount ? LastCount : -1; }
		std::int64_t GetWastedWaits() const { return WastedWaits; }
		std::int64_t GetSuspensions() const { return Suspensions; }
		std::int64_t GetSuspensionsForInconsistency() const { return SuspensionsForInconsistency; }
		std::int64_t GetInconsistentObservations() const { return InconsistentObservations; }
		std::int64_t GetResets() const { return Resets; }
		std::int64_t GetGlitches() const { return Glitches; }
		bool IsSuspended() const { return ReadIndex < SuspendedUntilRead; }
		/** Reads left in the current suspension; 0 when not suspended. */
		std::int64_t GetSuspendedReadsLeft() const { return (std::max)(std::int64_t(0), SuspendedUntilRead - ReadIndex); }
		/** Length of the next suspension, reads. */
		std::int64_t GetNextSuspendReads() const { return Recovery == ERecovery::Adaptive ? NextSuspendReads : FTuning::SuspendReads; }
		double GetJitterSeconds() const { return JitterSeconds; }
		double GetReadIntervalSeconds() const { return ReadIntervalSeconds; }
		/** Reads begun so far (BeginRead calls); survives Reset(). */
		std::int64_t GetReads() const { return ReadIndex; }
		/** Probe waits made, and those that got their frame. */
		std::int64_t GetProbes() const { return Probes; }
		std::int64_t GetProbeSuccesses() const { return ProbeSuccesses; }
		/** Reads until the next probe time; meaningful only while suspended with the adaptive recovery. */
		std::int64_t GetReadsUntilProbe() const { return (std::max)(std::int64_t(0), NextProbeRead - ReadIndex); }

	private:
		struct FSample
		{
			double TimeSeconds = 0.0;  // upper bound of the publication time of Count
			double WidthSeconds = 0.0; // width of the observation that bounded it
			std::int64_t Count = 0;
		};

		bool EstimatePeriod(double& OutPeriod, double& OutError) const
		{
			OutPeriod = 0.0;
			OutError = 0.0;
			if (Window.size() < 2) return false;
			const FSample& First = Window.front();
			const FSample& Last = Window.back();
			const std::int64_t Publications = Last.Count - First.Count;
			const double Span = Last.TimeSeconds - First.TimeSeconds;
			if (Publications < FTuning::MinPublications || Span < FTuning::MinWindowSeconds) return false;
			OutPeriod = Span / double(Publications);
			// Each end of the span is known only to within the width of the observation that bounded it.
			OutError = (First.WidthSeconds + Last.WidthSeconds) / double(Publications);
			return OutPeriod > 0.0;
		}

		void Suspend(bool bForInconsistency)
		{
			const std::int64_t Length = (Recovery == ERecovery::Adaptive) ? NextSuspendReads : FTuning::SuspendReads;
			SuspendedUntilRead = ReadIndex + Length;
			// The first probe one interval after the start, so the base suspension is never probed.
			NextProbeRead = ReadIndex + FTuning::ProbeIntervalReads;
			if (Recovery == ERecovery::Adaptive)
			{
				// Another suspension before any wait has got its frame doubles the next one.
				NextSuspendReads = (std::min)(FTuning::MaxSuspendReads, 2 * NextSuspendReads);
			}
			WasteScore = 0;
			InconsistencyScore = 0;
			++Suspensions;
			if (bForInconsistency) ++SuspensionsForInconsistency;
		}

		/** The rule after the suspension test: slower source, overdue, then the decision rule. */
		void Decide(FPrediction& Out, double NowSeconds, double WaitSeconds, double Period) const
		{
			if (Recovery == ERecovery::Adaptive && ReadIntervalSeconds > 0.0 && Period > ReadIntervalSeconds * (1.0 + FTuning::SlowerSourceMargin))
			{
				Out.Decision = EDecision::SlowerSource;
				return;
			}
			const double OverdueTolerance = (std::min)(FTuning::OverdueToleranceSeconds, FTuning::OverdueToleranceOfPeriod * Period);
			if (Out.LatestSeconds < NowSeconds - OverdueTolerance)
			{
				// A regular sender would already have published. It is paused, slower, or late by
				// more than a wait could cover; waiting again every frame would only add latency.
				Out.Decision = EDecision::Overdue;
				return;
			}
			bool bReachable = false;
			switch (Rule)
			{
			case EWaitRule::UpperEdge:
				bReachable = Out.LatestSeconds <= NowSeconds + WaitSeconds;
				break;
			case EWaitRule::Overlap:
				bReachable = Out.EarliestSeconds <= NowSeconds + WaitSeconds;
				break;
			case EWaitRule::Conditional:
			default:
			{
				// The count was unchanged at now, so the publication lies in [max(earliest, now), latest].
				const double From = (std::max)(Out.EarliestSeconds, NowSeconds);
				if (Out.LatestSeconds <= NowSeconds)
				{
					Out.ConditionalShare = 1.0; // due already (the overdue test excluded windows too far in the past)
				}
				else if (From >= NowSeconds + WaitSeconds)
				{
					Out.ConditionalShare = 0.0;
				}
				else
				{
					Out.ConditionalShare = (Out.LatestSeconds > From) ? ((std::min)(Out.LatestSeconds, NowSeconds + WaitSeconds) - From) / (Out.LatestSeconds - From) : 1.0;
				}
				bReachable = Out.ConditionalShare >= FTuning::MinConditionalShare;
				break;
			}
			}
			Out.Decision = bReachable ? EDecision::Wait : EDecision::NotImminent;
		}

		std::vector<FSample> Window;
		EWaitRule Rule = EWaitRule::Conditional;
		ERecovery Recovery = ERecovery::Adaptive;
		bool bHaveCount = false;
		bool bHaveBounds = false;
		std::int64_t LastCount = 0;
		/** Most recent time the count was observed equal to LastCount. */
		double LastSeenSeconds = 0.0;
		/** Bounds of the publication time of LastCount. */
		double PubLoSeconds = 0.0;
		double PubHiSeconds = 0.0;
		/** Jitter allowance per publication, seconds (fixed at the floor with ERecovery::Original). */
		double JitterSeconds = FTuning::JitterAllowanceSeconds;
		/** Smoothed interval between reads, seconds; 0 until two timed reads. */
		double ReadIntervalSeconds = 0.0;
		double LastReadSeconds = -1.0;
		std::int32_t WasteScore = 0;
		std::int32_t InconsistencyScore = 0;
		std::int64_t ReadIndex = 0;
		std::int64_t SuspendedUntilRead = 0;
		std::int64_t NextSuspendReads = FTuning::SuspendReads;
		/** The first read at which the current suspension may be probed. */
		std::int64_t NextProbeRead = 0;
		std::int64_t Probes = 0;
		std::int64_t ProbeSuccesses = 0;
		std::int64_t WastedWaits = 0;
		std::int64_t Suspensions = 0;
		std::int64_t SuspensionsForInconsistency = 0;
		std::int64_t InconsistentObservations = 0;
		std::int64_t Resets = 0;
		std::int64_t Glitches = 0;
	};
}
