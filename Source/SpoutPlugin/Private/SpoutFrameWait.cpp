// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#include "SpoutFrameWait.h"

#include "SpoutLimits.h"

namespace SpoutFrameWait
{
	namespace
	{
		// Render thread only.
		uint64 GBudgetFrame = MAX_uint64;
		double GBudgetUsedSeconds = 0.0;

		void RollBudget(uint64 RenderFrame)
		{
			if (GBudgetFrame != RenderFrame)
			{
				GBudgetFrame = RenderFrame;
				GBudgetUsedSeconds = 0.0;
			}
		}
	}

	double GrantFromFrameBudget(double RequestedSeconds, uint64 RenderFrame)
	{
		RollBudget(RenderFrame);
		const double Remaining = FMath::Max(0.0, SpoutLimits::FrameWaitBudgetSeconds() - GBudgetUsedSeconds);
		return FMath::Clamp(RequestedSeconds, 0.0, Remaining);
	}

	void ChargeFrameBudget(double UsedSeconds, uint64 RenderFrame)
	{
		RollBudget(RenderFrame);
		GBudgetUsedSeconds += FMath::Max(0.0, UsedSeconds);
	}

	double FrameBudgetUsedSeconds(uint64 RenderFrame)
	{
		RollBudget(RenderFrame);
		return GBudgetUsedSeconds;
	}

	bool RunBudgetedWait(double RequestedSeconds, uint64 RenderFrame, TFunctionRef<bool(double GrantedSeconds, FSpoutFrameWaitReport& Report)> Wait,
		FSpoutFrameWaitReport& OutReport, double& OutBudgetBeforeSeconds, double& OutBudgetAfterSeconds)
	{
		OutReport = FSpoutFrameWaitReport();
		OutBudgetBeforeSeconds = OutBudgetAfterSeconds = 0.0;
		if (RequestedSeconds <= 0.0)
		{
			return false; // the decision stays Disabled, and the budget is not even looked at
		}
		OutBudgetBeforeSeconds = OutBudgetAfterSeconds = FrameBudgetUsedSeconds(RenderFrame);
		const double GrantedSeconds = GrantFromFrameBudget(RequestedSeconds, RenderFrame);
		if (GrantedSeconds <= 0.0)
		{
			OutReport.Decision = EDecision::BudgetExhausted;
			return false;
		}
		const bool bOwed = Wait(GrantedSeconds, OutReport);
		if (OutReport.WaitEndCycles > OutReport.WaitBeginCycles)
		{
			ChargeFrameBudget(double(OutReport.WaitEndCycles - OutReport.WaitBeginCycles) * FPlatformTime::GetSecondsPerCycle64(), RenderFrame);
		}
		OutBudgetAfterSeconds = FrameBudgetUsedSeconds(RenderFrame);
		return bOwed;
	}
}
