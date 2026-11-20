// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#pragma once
#include "CoreMinimal.h"
#include "PixelFormat.h"

// The development switches (r.Spout.Dev.*) and the asynchronous sender's outcome log exist outside Shipping builds
// only. A Shipping build behaves as with their defaults.
#define SPOUT_WITH_DEV_SETTINGS (!UE_BUILD_SHIPPING)

namespace SpoutLimits
{
	/** Defaults of the limit console variables, shared with the Project Settings page (USpoutSettings). */
	constexpr int32 DefaultMaxActiveStreams = 32;
	constexpr int32 DefaultMaxPendingCommands = 128;
	constexpr int32 DefaultMaxPendingPerStream = 4;
	constexpr int32 DefaultMaxConsumersPerReceiver = 32;
	constexpr int32 DefaultMaxTextureDimension = 8192;
	constexpr int32 DefaultMaxFrameMiB = 128;
	constexpr int32 DefaultMaxRetainedMiB = 1024;

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

	// There is no process-wide switch for the asynchronous receive. Async Receive is chosen per receiver session
	// (FSpoutReceiver::Receive, bAsyncReceive) and decided once per session; see FSpoutSharedSender. A stream on the
	// asynchronous path makes no publication wait: r.Spout.FrameWaitMs applies to the synchronous path only.

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
	// r.Spout.Dev.AsyncSend 0 selects the synchronous shared-slot send for diagnosis. Shipping builds do not register
	// these variables (SPOUT_WITH_DEV_SETTINGS) and use their defaults. ----

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

	/**
	 * r.Spout.Dev.CreateOn12Device != 0 (default 0): the D3D11-on-12 device is created at the first Spout initialization
	 * even on the shared-slot path, which never uses it (it costs a hitch at first use). For comparing the two; read once.
	 */
	bool DevCreateOn12Device();

	/**
	 * r.Spout.Dev.ReserveReceiverOpenBudget != 0 (default 0): a receiver reserves four frames of retained-memory budget
	 * whenever it opens a sender, as before, instead of reserving its CPU readback's memory only when it creates the
	 * staging texture. For comparing the two; read when a receiver opens or resizes.
	 */
	bool DevReserveReceiverOpenBudget();

	/**
	 * r.Spout.Dev.ResolveSendsTogether != 0 (default 1): the flush hook gives every asynchronous sender's owed frames
	 * their attempt at once and waits for all of them together, woken as each resolves. 0: one sender after another,
	 * checking every millisecond, as before. Read per flush.
	 */
	bool DevResolveSendsTogether();

	/**
	 * r.Spout.Dev.ReleaseViewportTextures != 0 (default 1): when the last sender of a viewport source closes, that
	 * source's own GPU textures (its shared slot, Game Viewport Only's copy texture, back-buffer wraps) are released; the
	 * next sender makes them again. 0 keeps them until Close All Streams, as before. Read when a sender closes.
	 */
	bool DevReleaseViewportTextures();
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
