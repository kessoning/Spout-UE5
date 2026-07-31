/**
 * Declares a small rate limiter for diagnostics that can otherwise repeat every frame.
 *
 * Several failure conditions recur on every frame: an unsupported source format, a sender whose
 * registration keeps failing, a mismatched output render target. Each needs to be visible in the
 * log without flooding it, and the check has to be cheap enough to sit on the render thread.
 */
#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

#include <atomic>

/**
 * Allows one log per interval.
 *
 * The timestamp is atomic so the sender and receiver render-thread paths can share an instance,
 * but the compare-and-store is deliberately not a strict CAS loop: two threads racing at exactly
 * the interval boundary may both log once. A duplicated diagnostic line is harmless, and avoiding
 * a loop keeps this free of contention on the hot path.
 */
class FSpoutLogThrottle
{
public:
	explicit FSpoutLogThrottle(double InIntervalSeconds = 5.0)
		: IntervalSeconds(InIntervalSeconds)
	{
	}

	/** Returns true at most once per interval. The first call always returns true. */
	bool ShouldLog()
	{
		const double Now = FPlatformTime::Seconds();
		const double Last = LastLogSeconds.load(std::memory_order_relaxed);

		// bHasLogged separates "never logged" from "logged at time zero" without needing a
		// sentinel timestamp that a monotonic clock might legitimately produce.
		if (bHasLogged.load(std::memory_order_relaxed) && (Now - Last) < IntervalSeconds)
		{
			return false;
		}

		LastLogSeconds.store(Now, std::memory_order_relaxed);
		bHasLogged.store(true, std::memory_order_relaxed);
		return true;
	}

	/** Forgets the last log time so the next ShouldLog() reports again. */
	void Reset()
	{
		bHasLogged.store(false, std::memory_order_relaxed);
	}

private:
	std::atomic<double> LastLogSeconds{ 0.0 };
	std::atomic<bool> bHasLogged{ false };
	double IntervalSeconds;
};
