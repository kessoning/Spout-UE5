// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * The asynchronous receive path: Async Receive (Beta), off by default. A receiver session asks for it through the
 * Async Receive (Beta) input of the receive functions (FSpoutReceiver::Receive, bAsyncReceive); the session's first
 * accepted call fixes the request for the session's life. No process-wide setting enables it (r.Spout.AsyncReceive
 * is retired and ignored).
 *
 * The synchronous receive (SpoutReceiver.cpp) takes Spout access, copies and waits for the copy on
 * the render thread, on a D3D11-on-12 device that shares Unreal's graphics queue. Under GPU load
 * that wait is long, because the copy sits behind Unreal's own work. This path moves access, copy
 * and wait to a worker thread with its own native D3D11 device. The render thread then only takes
 * the newest completed image and queues one copy of it into the same Unreal texture as the synchronous path. It
 * takes no Spout access and waits for no GPU work.
 *
 * The synchronous path stays the default. A stream uses this path only while every precondition
 * holds; otherwise, and after any failure here, it uses the synchronous path and says why once.
 *
 * Parts:
 *   FSpoutAsyncDevice  one native D3D11 device per process, on the adapter Unreal renders with
 *   FSpoutAsyncStream  one receive stream at one sender identity (size, format, share handle):
 *                      three private slot textures, two shared fences, one worker thread
 * The bookkeeping (which slot may be written, which is read, what starts a transfer) is
 * SpoutSlotPool.h, which is engine-free.
 *
 * Ownership rules, all enforced here:
 *   - Spout access is taken and given back by the worker thread only, through
 *     FSpoutTextureAccessScope. The render thread never takes it for an asynchronous stream.
 *   - The worker never exits while it holds access: the stop request is seen between transfers.
 *   - Once access is taken and a copy submitted, access is given back only when that copy is proven
 *     finished (its written-fence value reached, or, after a failed signal, the event query issued
 *     after it reported done) or the worker's device was removed, as on the synchronous slot path
 *     (FSpoutSlotReceive::CopyAndWait). CompletionTimeoutMs classifies a copy as late; it never ends
 *     the wait. A late copy is never published and its transfer stays owed. A stalled GPU therefore
 *     keeps the sender's access held, and the stream's destruction (close, a new sender identity, a
 *     fallback, shutdown) waits for the worker until the copy is proven or the device is removed; a
 *     warning is logged every 5 s meanwhile.
 *   - A slot is written only when the read fence reached the value of its last read, and read
 *     only behind a queue wait on the written fence.
 *   - The stream's objects (slots, fences, the opened source, the proof query) are handed to
 *     FSpoutGpuRetirement with the fence values that prove their work finished, never released
 *     directly: the native copies it submitted and the reads Unreal queued out of its slots.
 *   - One stream object is one generation, with a number no other stream object of the process
 *     has. A new size, format or share handle destroys the stream and creates another, so an image
 *     of an old generation cannot reach the current output.
 *
 * Lock order: the stream's state lock is held for one bookkeeping call at a time and never
 * together with Spout access, with the device's context lock, or across a wait. The worker takes
 * FSpoutFrameSync::Mutex and the named access mutex as the synchronous path does, and the
 * device's context lock inside the access scope for the copy and its flush only.
 */
#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "RHI.h"
#include "SpoutInstrument.h"
#include "SpoutSlotPool.h"
#include "SpoutSlotRules.h"
#include <atomic>
#if SPOUT_WITH_INSTRUMENT
#include "HAL/Thread.h"
#endif

class FRHICommandListImmediate;
class FRHITexture;
class FRunnableThread;
class FSpoutFrameSync;
class FSpoutResourceReservation;

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include "Windows/HideWindowsPlatformTypes.h"
#ifdef GetNextSibling
#undef GetNextSibling
#endif

/** The worker's native D3D11 device. Not a D3D11-on-12 device: it has no part in Unreal's queue. */
class FSpoutAsyncDevice
{
public:
	static FSpoutAsyncDevice& Get();

	/**
	 * Creates the device on first use, on the adapter of Unreal's D3D12 device (the LUIDs are
	 * compared). False when the asynchronous path cannot be used in this process; OutWhy says why.
	 * A refusal is remembered: there is no retry loop.
	 */
	bool EnsureCreated(FString& OutWhy);

	/** Releases the device. Streams must be gone first; the ordered teardown calls it. */
	void Shutdown();

	/**
	 * With Unreal's debug layer on (-d3ddebug) the worker's device is a debug device too. This writes
	 * the messages its debug layer has stored to the log, one warning each, and forgets them. Called
	 * when a stream is destroyed and at shutdown, which also logs the total for the process.
	 */
	void ReportDebugLayer();

	/** Turns the asynchronous path off for the process (device removed, a failed fence or handle call). */
	void Disable(const TCHAR* Why, HRESULT Result);
	bool IsDisabled() const { return bDisabled.load(); }
#if WITH_DEV_AUTOMATION_TESTS
	/** Tests only: undoes Disable after an injected failure, while the device itself still exists and is not removed. */
	void ReenableForTest();
#endif

	ID3D11Device5* GetDevice() const { return Device.Get(); }
	ID3D11DeviceContext4* GetContext() const { return Context.Get(); }
	/** Guards the immediate context, which is not thread-safe. Held for a copy and its flush, never across a wait. */
	FCriticalSection& GetContextMutex() { return ContextMutex; }

private:
	FSpoutAsyncDevice() = default;
	void ReportDebugLayerLocked();

	FCriticalSection InitMutex;
	FCriticalSection ContextMutex;
	Microsoft::WRL::ComPtr<ID3D11Device5> Device;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext4> Context;
	Microsoft::WRL::ComPtr<ID3D11InfoQueue> InfoQueue;
	int64 DebugLayerMessages = 0;
	std::atomic<bool> bDisabled{false};
	FString DisabledWhy;
};

/** Counters of one asynchronous stream, as FSpoutAsyncStream::GetStats copies them. */
struct FSpoutAsyncStats
{
	int64 Published = 0;
	int64 Consumed = 0;
	int64 ReplacedUnread = 0;
	int64 NoFreeSlot = 0;
	int64 AccessRefused = 0;
	int64 CopyFailed = 0;
	int64 CompletionTimeouts = 0;
	int64 PublishedAfterReuse = 0;
	int64 RuleViolations = 0;
	int64 Polls = 0;
};

class FSpoutAsyncStream final : public FRunnable
{
public:
	/** What one sender identity is: a change of any field is a new generation and a new stream object. */
	struct FIdentity
	{
		HANDLE SharedHandle = nullptr;
		uint32 Width = 0;
		uint32 Height = 0;
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
		bool operator==(const FIdentity& O) const { return SharedHandle == O.SharedHandle && Width == O.Width && Height == O.Height && Format == O.Format; }
		bool operator!=(const FIdentity& O) const { return !(*this == O); }
	};

	/**
	 * Render thread. Builds the slots and fences for one sender identity and starts the worker.
	 * Null when a precondition fails; OutWhy then says which, and the caller uses the synchronous
	 * path. FrameSync must be open and must outlive the stream; the stream is the only user of its
	 * access and notification calls while it exists.
	 */
	static TUniquePtr<FSpoutAsyncStream> Create(const FName& Name, int32 InstrumentStream, FSpoutFrameSync& FrameSync, const FIdentity& Identity, EPixelFormat PixelFormat, FString& OutWhy);

	/** Any thread. Stops the worker (it finishes a transfer in progress and releases access) and releases everything. */
	virtual ~FSpoutAsyncStream();

	const FIdentity& GetIdentity() const { return Identity; }
	/** This stream object's generation: a number no other stream object of the process has. */
	uint64 GetGeneration() const { return Generation; }
	/**
	 * Stream objects alive in the process, any thread. Each owns its worker thread and joins it before it is gone,
	 * so 0 means no worker runs. Diagnostics and tests: a receiver that did not ask for Async Receive (Beta) never
	 * adds one, and a closed one leaves none behind.
	 */
	static int32 GetLiveStreams();

	/**
	 * Render thread. Takes the newest completed image, when there is one, and queues its copy into
	 * Destination: a queue wait on the written fence, the copy, a signal of the read fence. Returns
	 * whether a copy was queued; that copy is the delivery. Never blocks on GPU work.
	 */
	bool ConsumeOnRenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture* Destination, uint64 ReceivedBefore);

	/** The worker met an error that ends the asynchronous path for this stream: use the synchronous path. */
	bool HasFailed() const { return bFailed.load(); }
	const FString& GetFailure() const { return Failure; }

	FSpoutAsyncStats GetStats() const;

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	FSpoutAsyncStream(const FName& InName, int32 InInstrumentStream, FSpoutFrameSync& InFrameSync, const FIdentity& InIdentity);

	bool Initialize(EPixelFormat PixelFormat, FString& OutWhy);
	/** One attempt at the owed transfer. False when it was skipped or given up; it stays owed. */
	bool TransferOnce();
	void Fail(const TCHAR* Why, HRESULT Result);
	/** Waits on the stop event for at most the poll interval. */
	void WaitForNextPoll();
#if SPOUT_WITH_INSTRUMENT
	/**
	 * Measurement builds only: a thread that records when the read fence advances (row kind F), that
	 * is, when a copy Unreal queued out of a slot has completed on Unreal's queue. It reads the fence
	 * and writes log rows. It takes no lock of the stream and changes none of its state.
	 */
	TUniquePtr<SpoutInstrument::FDestinationFenceWatch> ReadWatch;
#endif

	static constexpr int32 NumSlots = 3;
	/** Fixed limits of the asynchronous path, not restrictions on what senders may do. */
	static constexpr int32 PollIntervalMicroseconds = 250;
	static constexpr uint32 NoSlotWaitMs = 2;
	/** A copy proven finished later than this is discarded (late); the bound never ends the wait while access is held. */
	static constexpr uint32 CompletionTimeoutMs = 100;
	static constexpr int32 MaxConsecutiveTimeouts = 8;

	const FName Name;
	const int32 InstrumentStream;
	FSpoutFrameSync& FrameSync;
	const FIdentity Identity;

	/** Guards Pool, Gate, LastReadValue and Outstanding. Held for one call; never across access, a device call or a wait. */
	mutable FCriticalSection StateMutex;
	SpoutAsync::FSlotPool Pool{NumSlots};
	SpoutAsync::FTransferGate Gate;
	uint64 LastReadValue = 0;
	/** The work the stream's objects may still be used by: native copies (worker) and reads queued by Unreal (render thread). */
	SpoutSlotRules::FOutstanding Outstanding;
	const uint64 Generation;

	// Unreal's device: the slots and both fences live there.
	Microsoft::WRL::ComPtr<ID3D12Fence> WrittenFence;
	Microsoft::WRL::ComPtr<ID3D12Fence> ReadFence;
	Microsoft::WRL::ComPtr<ID3D12Resource> SlotResources[NumSlots];
	FTextureRHIRef SlotTextures[NumSlots];

	// The worker's device: the sender's texture and the slots opened there, and the written fence.
	Microsoft::WRL::ComPtr<ID3D11Texture2D> WorkerSource;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> WorkerSlots[NumSlots];
	Microsoft::WRL::ComPtr<ID3D11Fence> WorkerWrittenFence;
	/** The event query that proves a copy whose written-fence signal failed. */
	Microsoft::WRL::ComPtr<ID3D11Query> Proof;
	bool bKeyedSource = false;

	TSharedPtr<FSpoutResourceReservation, ESPMode::ThreadSafe> Reservation;

	FRunnableThread* Thread = nullptr;
	HANDLE StopEvent = nullptr;
	HANDLE PollTimer = nullptr;
	HANDLE FenceEvent = nullptr;
	std::atomic<bool> bStop{false};
	std::atomic<bool> bFailed{false};
	FString Failure;

	// Worker thread only.
	uint64 WrittenValue = 0;
	int32 ConsecutiveTimeouts = 0;

	std::atomic<int64> Polls{0};
	std::atomic<int64> NoSlotSkips{0};
	/** What the worker thread cost, taken by the thread itself as it ends: CPU time in 100 ns units, and how long it ran. */
	std::atomic<int64> WorkerCpu100ns{-1};
	std::atomic<double> WorkerSeconds{0.0};
};

#if WITH_DEV_AUTOMATION_TESTS
/**
 * Failure injection for the automation tests, taken by the asynchronous receive's worker only (the shared-slot hooks of
 * SpoutSharedSlot.h are taken by whichever native wait comes next, which in a test with the plugin's own sender can be
 * the sender's).
 */
namespace SpoutAsyncReceiveTestHooks
{
	/** The worker's next copy is not accepted as finished before this many milliseconds; its real proof is still needed. */
	extern std::atomic<int32> DelayCompletionMs;
	/** The worker's next written-fence signals are reported failed without being made. */
	extern std::atomic<int32> FailSignals;
}
#endif

#endif // PLATFORM_WINDOWS
