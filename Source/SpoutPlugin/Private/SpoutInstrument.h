// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Measurement instrumentation. Compiled only into measurement builds.
 *
 * SPOUT_WITH_INSTRUMENT is 0 unless SpoutPlugin.Build.cs finds the marker file
 * SpoutInstrument.enable in the plugin's root folder (which no release contains). With 0, the
 * SPOUT_INSTRUMENT(...) macro expands to nothing and SpoutInstrument.cpp compiles to nothing: a
 * release build contains no instrumentation code, no extra console variable and no extra branch.
 * A measurement build comes from the same sources with the marker present.
 *
 * In a measurement build everything is still gated at run time by r.Spout.Instrument (default 0).
 * With 1, the render thread records one row of QPC timestamps (and GPU timestamps around the copy)
 * per receive call, per send destination and per viewport send command, and a background thread
 * writes them to the CSV named by r.Spout.InstrumentPath.
 *
 * Row fields (instrumentVersion 6; the first 47 columns are those of version 5):
 *   tDetect    render thread, immediately before the read that begins the receive attempt
 *   tDrain0/1  around SubmitSpoutInteropTransitions (the RHI-thread flush and submission wait)
 *   tWrap0/1   around the D3D11-on-12 wrap lookup or creation
 *   tAccess0   before the Spout access attempt; tAcquire after it (acquired says whether it succeeded).
 *              Recorded on both receive paths: the image a transfer copies is the sender's last
 *              publication at or before tAcquire.
 *   tCopyRec   after CopyResource was recorded on the D3D11 immediate context (not execution)
 *   tGpuDone   after the CPU-observed GPU completion of the copy
 *   tPublish   sender only, after SetNewFrame returned
 *   tRelease   after the access mutex was released
 *   tRestore   after the restore transition was recorded on the RHI command list; CPU path: the end of the call
 *   gpuT0/T1   GPU timestamps bracketing the copy, gpuFreq from the disjoint query
 *   senderFrame, countAfterWait, countUnderAccess   frame counts: notification hints, never image identities
 *   delivered, receivedBefore/After, frameRequests, syncOpens   the downstream delivery of this call
 *   completed  1 when the transfer reached its completion point (FSpoutFrameSync::CompleteTransfer)
 *   newUnderAccess  what the read under access answered: -1 not read, 0 nothing new, 1 new
 */
#pragma once

#include "CoreMinimal.h"

#ifndef SPOUT_WITH_INSTRUMENT
#define SPOUT_WITH_INSTRUMENT 0
#endif

#if SPOUT_WITH_INSTRUMENT

/** The enclosed statements exist only in a measurement build. */
#define SPOUT_INSTRUMENT(...) __VA_ARGS__

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include "Windows/HideWindowsPlatformTypes.h"
#ifdef GetNextSibling
#undef GetNextSibling
#endif
#endif

namespace SpoutInstrument
{
	constexpr int32 Version = 6;

	/** r.Spout.Instrument != 0. Safe on any thread. */
	bool IsEnabled();

	/**
	 * r.Spout.Instrument.Bypass, for comparing the same load without Spout work. Bit 0 makes
	 * receive render commands return before any Spout work, bit 1 does the same for send render
	 * commands. 0 by default.
	 */
	int32 BypassMask();

	FORCEINLINE uint64 Now() { return FPlatformTime::Cycles64(); }

	struct FRecord
	{
		TCHAR Kind = TEXT('?');   // R receive call (GPU path), C receive call (CPU path), S send destination, V viewport send command
		int32 Stream = -1;        // id from RegisterStream; -2 for the viewport command
		uint64 FrameRT = 0;       // GFrameCounterRenderThread when the record started
		uint64 TDetect = 0;
		int32 NewFrame = -1;
		int64 SenderFrame = -1;   // the frame count the read saw
		int32 Pending = -1;       // receiver: a frame was already pending before this call
		uint64 TDrain0 = 0, TDrain1 = 0, TWrap0 = 0, TWrap1 = 0, TAccess0 = 0, TAcquire = 0;
		int32 Acquired = -1;
		uint64 TCopyRec = 0, TGpuDone = 0, TPublish = 0, TRelease = 0, TRestore = 0;
		uint64 GpuT0 = 0, GpuT1 = 0, GpuFreq = 0;
		int32 GpuDisjoint = -1;   // -1 not attempted, -2 not resolved in time, -3 bracket not closed, 0/1 disjoint flag
		int32 Hr = 0;             // HRESULT of the GPU completion wait

		int32 WaitDecision = -1;      // SpoutFrameWait::EDecision; -1 when no decision was taken on this call
		int32 WaitOutcome = -1;       // SpoutFrameWait::EOutcome
		uint64 TWait0 = 0, TWait1 = 0; // around the polling loop; 0 when no wait was attempted
		int32 WaitPolls = 0;
		double WaitBoundUs = 0.0;     // the bound granted to this call (the setting, limited by the frame budget)
		double PredLoUs = 0.0, PredHiUs = 0.0; // predicted publication, microseconds relative to tDetect
		double PeriodUs = 0.0;        // estimated source period
		int64 CountAfterWait = -1;    // frame count at the last poll of the wait
		int64 CountUnderAccess = -1;  // frame count read while texture access was held
		double BudgetBeforeUs = 0.0, BudgetAfterUs = 0.0; // frame-wait budget used in this render frame before and after this call
		int32 Flags = 0;              // bit 0 frame wait enabled, bit 1 recheck enabled, bit 3 a frame became pending during the wait, bit 4 the wait was a probe that ended Published or TimedOut, bit 5 a transfer made without a notification
		int64 PredSuspensions = -1;
		int64 PredWastedWaits = -1;
		double PredJitterUs = -1.0;
		int64 AckBefore = -2;         // kept so the first 47 columns match version 5; always -2 (nothing is acknowledged by count)
		int32 Delivered = -1;         // what CompleteTransfer returned (0 or 1); -1 when the transfer did not complete
		uint64 ReceivedBefore = 0;    // the receiver's ReceivedFrameNumber when the call began (it drives the downstream deliveries)
		uint64 ReceivedAfter = 0;     // ... and when the row was recorded
		int64 FrameRequests = -1;     // forced current frames so far (cumulative)
		int64 SyncOpens = -1;         // opens so far (cumulative)
		int32 Completed = 0;          // version 6: the transfer reached its completion point
		int32 NewUnderAccess = -1;    // version 6: the read under access; -1 not read
	};

	/** The CSV's column line (no #meta line, no newline), and one row exactly as the writer formats it. */
	const TCHAR* ColumnHeader();
	FString FormatRecordLine(const FRecord& Record);

	/** Registers a stream name for the CSV (#stream line) and returns its id. */
	int32 RegisterStream(const FName& Name, const TCHAR* Role);

	/** Queues one row for the writer thread. Never blocks on I/O. */
	void Push(const FRecord& Record);

#if PLATFORM_WINDOWS
	/** Writes a #calib line (QPC, GPU timestamp, GPU timestamp frequency) at most once per second. */
	void Calibrate(ID3D12CommandQueue* Queue);

	/**
	 * GPU timestamp bracket on the D3D11-on-12 immediate context. One set of queries, reused; the
	 * caller resolves after its own GPU completion wait, so the queries are complete by then and the
	 * resolve does not stall. Used only under FSpoutD3DContext::D3D11ContextMutex.
	 */
	class FGpuBracket
	{
	public:
		void Begin(ID3D11DeviceContext* Context);
		void End(ID3D11DeviceContext* Context);
		void Resolve(ID3D11DeviceContext* Context, FRecord& Record);
	private:
		Microsoft::WRL::ComPtr<ID3D11Query> Disjoint, T0, T1;
		bool bActive = false;
		bool bEnded = false;
	};

	FGpuBracket& GetGpuBracket();
#endif
}

#else // !SPOUT_WITH_INSTRUMENT

/** Not a measurement build: the enclosed statements are not compiled. */
#define SPOUT_INSTRUMENT(...)

#endif
