// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * The experimental new-frame wait (r.Spout.FrameWaitMs, off by default).
 *
 * When a receive call finds no new frame, and the stream's own history says the sender's next
 * publication is due within the wait bound, the render thread polls the sender's frame
 * notification for at most that bound instead of returning at once and showing the previous frame
 * for another whole Unreal frame. With the setting at 0 nothing in this file runs.
 *
 * What this file owns:
 *   - the per-render-frame wait budget shared by every stream (r.Spout.FrameWaitBudgetMs);
 *   - the report of one receive call's wait.
 * What it does not own: the prediction (SpoutPublishPredictor.h), the polling loop and the
 * notification state (FSpoutFrameSync), the settings (SpoutLimits) and the call site
 * (SpoutReceiver.cpp).
 *
 * Distinct from the texture-access wait (r.Spout.AccessWaitMs): that one waits for the peer to
 * RELEASE the access mutex and runs inside FSpoutTextureAccessScope. This one waits for the peer to
 * PUBLISH and runs before any access attempt, never while this process holds the access mutex,
 * because the sender needs that mutex to write the very frame being waited for.
 */
#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"
#include "Templates/Function.h"
#include "SpoutPublishPredictor.h"

namespace SpoutFrameWait
{
	/** The plugin-wide clock for the predictor and the wait: QPC seconds. */
	FORCEINLINE double NowSeconds() { return double(FPlatformTime::Cycles64()) * FPlatformTime::GetSecondsPerCycle64(); }

	/**
	 * Per-render-frame wait budget across all streams (r.Spout.FrameWaitBudgetMs). Render thread only.
	 * Grant returns how long a wait may last now; Charge records what it actually took.
	 */
	double GrantFromFrameBudget(double RequestedSeconds, uint64 RenderFrame);
	void ChargeFrameBudget(double UsedSeconds, uint64 RenderFrame);
	double FrameBudgetUsedSeconds(uint64 RenderFrame);
}

/** What one receive call's new-frame wait did. Filled by FSpoutFrameSync::WaitForPublication. */
struct FSpoutFrameWaitReport
{
	SpoutFrameWait::EDecision Decision = SpoutFrameWait::EDecision::Disabled;
	SpoutFrameWait::EOutcome Outcome = SpoutFrameWait::EOutcome::NotAttempted;
	double PeriodSeconds = 0.0;
	double PredictedEarliestSeconds = 0.0;
	double PredictedLatestSeconds = 0.0;
	/** QPC cycles around the polling loop; 0 when no wait was attempted. */
	uint64 WaitBeginCycles = 0;
	uint64 WaitEndCycles = 0;
	double BoundSeconds = 0.0;
	int32 Polls = 0;
	/** The count at the read that began the attempt, and at the last poll; -1 when there is none. */
	int64 CountBefore = -1;
	int64 CountAfter = -1;
	/** The predictor's totals after this call; -1 when not read. */
	int64 PredictorSuspensions = -1;
	int64 PredictorWastedWaits = -1;
	/** The predictor's jitter allowance after this call, seconds; -1 when not read. */
	double PredictorJitterSeconds = -1.0;
	/** This call's wait was a probe of a suspension. */
	bool bProbe = false;

	/** Whether a wait was actually made (it polled at least once or ended for another reason), as opposed to declined. */
	bool WasWaitMade() const { return Decision == SpoutFrameWait::EDecision::Wait || Outcome != SpoutFrameWait::EOutcome::NotAttempted; }
};

namespace SpoutFrameWait
{
	/**
	 * One receive call's wait as the receiver performs it: take a grant from the render frame's budget,
	 * wait with that grant (never longer), and charge the budget with what the wait actually took.
	 * A request of zero leaves the decision Disabled; an empty budget gives BudgetExhausted without
	 * calling Wait. Render thread only, like the budget. Kept here rather than file-local in the receiver.
	 */
	bool RunBudgetedWait(double RequestedSeconds, uint64 RenderFrame, TFunctionRef<bool(double GrantedSeconds, FSpoutFrameWaitReport& Report)> Wait,
		FSpoutFrameWaitReport& OutReport, double& OutBudgetBeforeSeconds, double& OutBudgetAfterSeconds);
}
