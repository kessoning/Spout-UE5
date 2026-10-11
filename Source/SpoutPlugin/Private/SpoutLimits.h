// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#pragma once
#include "CoreMinimal.h"
#include "PixelFormat.h"

namespace SpoutLimits
{
	int32 MaxActiveStreams();
	int32 MaxPendingCommands();
	int32 MaxPendingPerStream();
	int32 MaxConsumers();
	bool FrameBytes(int64 Width, int64 Height, EPixelFormat Format, int64& OutBytes);
	int64 RetainedBytes();
	int64 PeakRetainedBytes();
	int64 RejectedAllocations();

	/** r.Spout.AccessWaitMs default and upper clamp. The maximum stays well under one 60 Hz frame. */
	constexpr int32 DefaultAccessWaitMs = 2;
	constexpr int32 MaxAccessWaitMs = 10;

	/**
	 * Longest wait, in milliseconds, for ONE attempt to take a shared texture's access lock while a
	 * peer holds it: r.Spout.AccessWaitMs clamped to 0..MaxAccessWaitMs. Zero only tries. Not a
	 * per-frame budget: every stream's transfer is its own attempt. Callable on any thread; the
	 * render thread reads its own copy of the setting.
	 */
	uint32 AccessWaitMs();
}

// Lifetime follows the actual retained texture/entry, including old outputs held by callers.
// Estimates reserve overlap/native staging, not exact driver committed memory.
class FSpoutResourceReservation
{
public:
	static TSharedPtr<FSpoutResourceReservation, ESPMode::ThreadSafe> TryReserve(int64 Bytes);
	~FSpoutResourceReservation();
	int64 GetBytes() const { return Bytes; }
private:
	explicit FSpoutResourceReservation(int64 InBytes) : Bytes(InBytes) {}
	int64 Bytes;
};
