// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Debug-layer capture with evidence of its own completeness, for sessions run with -d3ddebug. Inactive (no call
 * into any debug interface) when Unreal's debug layer is off, and absent from shipping builds.
 *
 * D3D12 (Unreal's device): a message callback registered with D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS, so storage
 * filters and the stored-message limit cannot hide a message from it; the engine's own storage filter and its list
 * of ignored ids are not consulted. It counts every message by severity and id and logs corruption, error and
 * warning messages (the first ones of each id with their text, all of them in the counts). Completeness evidence:
 *   - the registration's result and cookie;
 *   - a canary: right after registering, the plugin creates and releases one fence, and the messages the debug
 *     layer emits for that (informational ones) must arrive; a callback that receives nothing is not proven live;
 *   - the window: from registration to the summary at shutdown. Messages outside it are not this capture's.
 * Unreal 5.3 registers no callback of its own; 5.8 does, for its log. This one is independent of either.
 *
 * Native D3D11 (the plugin's own device): its info queue keeps corruption, errors and warnings (the asynchronous
 * receive denies only info and message severities) and FSpoutAsyncDevice::ReportDebugLayer logs them. This module
 * logs the evidence that nothing was hidden: the storage and retrieval filters as read back, the stored-message
 * limit, and how many messages that limit discarded.
 *
 * Log lines start with "Spout debug capture:" and are written at Log verbosity, so that they never count as warnings
 * or errors; the engine's own lines are unchanged.
 */
#pragma once

#include "CoreMinimal.h"

namespace SpoutDebugCapture
{
	/** The id the D3D12 debug layer reports for a failed reflection of a wrapped resource's D3D11 flags. */
	constexpr int32 WrapReflectionMessageId = 916;

	struct FCounts
	{
		bool bRegistered = false;
		int64 Canary = 0;
		/** Indexed by D3D12_MESSAGE_SEVERITY: corruption, error, warning, info, message. */
		int64 BySeverity[5] = { 0, 0, 0, 0, 0 };
		int64 WrapReflection = 0;
	};

	/** Registers the D3D12 callback when Unreal's debug layer is on. Once per interop initialization. */
	void Start();

	/**
	 * Unregisters the D3D12 callback, waits (at most 2 s) until no callback is still running, and only then logs the
	 * summary (counts by severity and by id): the summary includes every message the callback received. Shutdown,
	 * before the devices go.
	 */
	void Stop();

	/** Logs the native D3D11 layer's capture record. Where names the checkpoint. */
	void ReportNative(const TCHAR* Where);

	FCounts GetCounts();
}

/**
 * The D3D diagnostic: bounded raw evidence for selected D3D12 debug-layer warnings (ids 1008, 1424, 1328, 202, 210).
 * Compiled only in builds made with the marker file SpoutD3DDiag.enable (SpoutPlugin.Build.cs), and active only in a
 * process started with -SpoutD3DDiag and the D3D12 debug layer. It never changes what the plugin does; it only logs:
 *   - for the first few occurrences of each of those ids, the raw return addresses of the thread that made the D3D12
 *     call (the debug layer calls the capture's callback on that thread), for offline symbolization;
 *   - at shutdown, the loaded modules with their base, size, and PDB identity (GUID and age), so that the addresses
 *     can be mapped to module and offset and symbolized later;
 *   - for the first few transitions at each place the plugin queues one, the texture's debug name, its native
 *     resource pointer and the requested accesses, on the thread that queues it.
 * None of it establishes a cause by itself: the records are meant to be correlated offline.
 */
#if !defined(SPOUT_WITH_D3D_DIAG)
#define SPOUT_WITH_D3D_DIAG 0
#endif

#if SPOUT_WITH_D3D_DIAG && PLATFORM_WINDOWS && !UE_BUILD_SHIPPING
#include "RHIAccess.h"
class FRHITexture;

namespace SpoutD3DDiag
{
	/** The process was started with -SpoutD3DDiag (read once). */
	bool IsOn();

	/** Logs one transition the plugin queues (bounded per Site): the texture, its native resource and the accesses. */
	void NoteTransition(const TCHAR* Site, FRHITexture* Texture, ERHIAccess Before, ERHIAccess After);
}
#define SPOUT_D3D_DIAG_TRANSITION(Site, Texture, Before, After) SpoutD3DDiag::NoteTransition(TEXT(Site), (Texture), (Before), (After))
#else
#define SPOUT_D3D_DIAG_TRANSITION(Site, Texture, Before, After)
#endif
