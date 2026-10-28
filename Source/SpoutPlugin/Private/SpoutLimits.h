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

	// There is no process-wide switch for the asynchronous receive. r.Spout.AsyncReceive is retired: still
	// registered, ignored, and it logs a warning when set. Async Receive is chosen per receiver session
	// (FSpoutReceiver::Receive, bAsyncReceive) and decided once per session; see FSpoutSharedSender. A stream on the
	// asynchronous path makes no publication wait: r.Spout.FrameWaitMs applies to the synchronous path only.

	/**
	 * Logs the retirement warning when r.Spout.AsyncReceive already holds a value other than 0 at module startup (set
	 * by an ini file or the command line before the plugin registered it, which its change callback does not see).
	 */
	void WarnIfRetiredSettingsSet();

	/**
	 * r.Spout.AsyncReceive.GpuPriority, clamped to -7..7. A diagnostic of Async Receive, 0 by default:
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

	// ---- Development switches (not supported settings). The asynchronous render-target send is the default;
	// r.Spout.Dev.AsyncSend 0 selects the synchronous shared-slot send for diagnosis. ----

	/**
	 * r.Spout.Dev.AsyncSend != 0 (default 1): render-target senders on the shared-slot path use the asynchronous sender
	 * (SpoutAsyncSend.h). Read on the render thread when a sender entry sends without having decided: from its first
	 * send with the switch on, an entry keeps the asynchronous sender for its life. Turning it off applies to senders
	 * opened afterwards (close and send again).
	 */
	bool DevAsyncSend();

	/**
	 * r.Spout.Dev.AsyncSend.DrainAfter, 0 or 1 (default 0): when the asynchronous sender's worker copies a frame into
	 * the shared texture. 0: once the fill into its slot is observed finished. 1: also once Unreal's queue reached the
	 * fence signalled behind the frame's own work, or 100 ms after the fill. Read per send.
	 */
	int32 DevAsyncSendDrainAfter();
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
