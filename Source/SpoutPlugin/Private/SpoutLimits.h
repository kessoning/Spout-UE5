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

	// ---- Experimental receive settings, all off by default. With both r.Spout.FrameWaitMs and
	// r.Spout.FrameCountRecheck at 0 none of the operations they add runs.
	// They are separate from the access wait above: that one waits for a peer to RELEASE the
	// texture, these concern a sender's next PUBLICATION. ----

	/**
	 * Upper clamp of r.Spout.FrameWaitMs and of r.Spout.FrameWaitBudgetMs, milliseconds: one receive
	 * call waits at most one millisecond, and all receivers together at most one millisecond per
	 * render frame. The same in every build configuration. A larger value of either setting is read
	 * as this limit.
	 */
	constexpr float MaxFrameWaitMs = 1.0f;
	/** Default of r.Spout.FrameWaitBudgetMs, milliseconds. */
	constexpr float DefaultFrameWaitBudgetMs = 1.0f;
	/** r.Spout.FrameWaitPollUs default and clamps, microseconds. */
	constexpr int32 DefaultFrameWaitPollUs = 150;
	constexpr int32 MinFrameWaitPollUs = 50;
	constexpr int32 MaxFrameWaitPollUs = 1000;

	/**
	 * Longest new-frame wait per receive call, seconds: r.Spout.FrameWaitMs clamped to
	 * 0..MaxFrameWaitMs. Zero (the default) disables the wait entirely.
	 */
	double FrameWaitSeconds();

	/** Total new-frame wait time allowed per render frame across all streams, seconds. */
	double FrameWaitBudgetSeconds();

	/** Interval between frame-count polls inside a wait, seconds. */
	double FrameWaitPollSeconds();

	/**
	 * Whether the sender's notification is read again while texture access is held and the read
	 * history is kept: r.Spout.FrameCountRecheck != 0, and always when the frame wait is enabled.
	 */
	bool FrameCountRecheck();

	/**
	 * Whether a receive stream may use the asynchronous path: r.Spout.AsyncReceive != 0. An internal,
	 * experimental switch, off by default. With it off the worker, its device and
	 * its slots are never created. Read on every receive call, so turning it off returns a stream to
	 * the synchronous path at its next call. A stream on the asynchronous path makes no publication
	 * wait: r.Spout.FrameWaitMs applies to the synchronous path only.
	 */
	bool AsyncReceive();

	/**
	 * r.Spout.AsyncReceive.GpuPriority, clamped to -7..7. A diagnostic of the asynchronous receive, 0 by default:
	 * the GPU scheduling priority asked for the worker's device relative to the process's other
	 * devices (IDXGIDevice::SetGPUThreadPriority). With 0 the call is not made. Read once, when the
	 * worker's device is created.
	 */
	int32 AsyncReceiveGpuPriority();

	/**
	 * r.Spout.SharedSlotInterop != 0 (default 1): the sender and the synchronous receiver use the shared-slot path
	 * (SpoutSharedSlot.h) instead of wrapping Unreal's resources for D3D11. Read ONCE per process, at the first interop
	 * initialization (SpoutInterop::LatchOnce); a later change has no effect until the process restarts.
	 */
	bool SharedSlotInteropRequested();
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
