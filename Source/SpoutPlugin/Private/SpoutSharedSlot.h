// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * The shared-slot interop path: the sender and the synchronous receiver move pixels without wrapping any
 * engine-owned resource for D3D11.
 *
 * D3D11 work runs on the native D3D11 device the asynchronous receive creates (FSpoutAsyncDevice), on Unreal's
 * adapter. It touches only two kinds of resource: textures Spout peers share, and "slots". A slot is a texture this
 * plugin creates on Unreal's D3D12 device in a shared heap, with simultaneous access, opens on the native device by
 * NT handle and makes an RHI texture of. Unreal moves pixels between its own resources and a slot with an ordinary
 * RHI copy. The two devices are ordered by fences:
 *   receive  native copy Spout texture -> slot, signal "written"; Unreal's queue waits on it, copies slot ->
 *            destination, signals "read". A slot is written again only after its read completed (SpoutSlotPool.h).
 *   send     Unreal's queue copies source -> slot, signals "filled"; the CPU waits for it; the native device copies
 *            slot -> each Spout texture under that name's access and signals "drained"; the CPU waits for that.
 * No queue ever waits on a fence value that was not already observed complete on the CPU, so a failed or missing
 * signal can never leave a device waiting forever.
 *
 * Selection (SpoutInterop): r.Spout.SharedSlotInterop is read once per process, at the first interop
 * initialization, and the choice never changes afterwards. There is no live switching and no fallback that drops
 * native objects: a stream whose slot objects fail stops transferring and says why; its objects are retired.
 *
 * Lifetime (FSpoutGpuRetirement): an object that a submitted GPU operation may still use is never released
 * directly. It is handed, with the fence values that prove its operations finished (SpoutSlotRules.h), to the
 * retirement list, which releases it only once they are reached or a device was removed. A timed-out wait cancels
 * nothing; its value stays recorded.
 *
 * Lock order: an entry's ResourceMutex, then the retirement lock, then the native device's context lock. The context
 * lock is held for a copy, its signal and its flush, or for one event-query read, never across a wait. Spout access
 * is taken before the context lock, as the asynchronous worker does. No path holds the D3D11-on-12 context lock and
 * the native context lock together.
 */
#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "SpoutSlotPool.h"
#include "SpoutSlotRules.h"
#include <atomic>

class FRHICommandListImmediate;
class FRHITexture;
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

namespace SpoutInterop
{
	enum class EMode : uint8
	{
		Unlatched = 0,
		SharedSlot = 1,
		Wrap = 2
	};

	/**
	 * Reads r.Spout.SharedSlotInterop once per process and fixes the interop path. Called by
	 * FSpoutD3DContext::Initialize after its devices exist and before any stream does. With the setting at 1 it creates
	 * the native device; if that is refused the process uses the wrap path and says why. That choice is made before any
	 * slot object exists, so nothing is dropped. Later calls do nothing.
	 */
	void LatchOnce();

	EMode GetMode();
	bool IsSharedSlot();
	const TCHAR* ToString(EMode Mode);
	/** Why the latched path was chosen. Empty before the latch. */
	FString GetLatchReason();

	/** Game thread. Logs once per value when the setting no longer matches the value latched at startup. */
	void NoteSettingChange();

	/** Total CreateWrappedResource calls made in this process (SpoutD3DUtils.cpp). The shared-slot path makes none. */
	int64 GetWrapCallCount();
	void CountWrapCall();
}

/**
 * What one native copy of the shared-slot path ended with. Every outcome but NotStarted returns only once the copy
 * is proven finished or the native device was removed: the caller holds a peer's access until then.
 */
enum class ESpoutSlotCopy : uint8
{
	Completed = 0,     // copied, signalled, and proven finished within the bound
	Late = 1,          // proven finished, but after the receive's bound: discarded, never published
	SignalFailed = 2,  // copied, the signal call failed, and the event query issued after it proved it finished
	Removed = 3,       // the native device was removed: nothing it was given will run any more
	NotStarted = 4     // nothing was submitted (the set had failed, or a device was missing)
};

/**
 * Objects whose GPU work may still be running, released only once fences prove it finished (SpoutSlotRules.h).
 * Process-wide; any thread may retire or poll.
 */
class FSpoutGpuRetirement
{
public:
	static FSpoutGpuRetirement& Get();

	/** One set of retired objects and the proof of their work. */
	struct FRetiredSet
	{
		FString What;
		/** D3D12 view of the fence the native device signals after its copies. May be null. */
		Microsoft::WRL::ComPtr<ID3D12Fence> NativeFence;
		/** The fence Unreal's queue signals after the plugin's RHI copies. May be null. */
		Microsoft::WRL::ComPtr<ID3D12Fence> QueueFence;
		/** The event query issued on the native context after a copy whose signal failed. */
		Microsoft::WRL::ComPtr<ID3D11Query> Proof;
		/**
		 * Issued by the retirement itself when NativeFence's D3D12 view reports the largest value while the native
		 * device lives: Unreal's device was removed, and that view no longer reports native progress.
		 */
		Microsoft::WRL::ComPtr<ID3D11Query> ViewLostProof;
		SpoutSlotRules::FOutstanding Outstanding;
		TArray<FTextureRHIRef> Textures;
		// TRefCountPtr, not ComPtr: ComPtr breaks UE containers (UE 5.3).
		TArray<TRefCountPtr<IUnknown>> Objects;
		TSharedPtr<FSpoutResourceReservation, ESPMode::ThreadSafe> Reservation;
		HANDLE Event = nullptr;
		double RetiredAt = 0.0;
	};

	struct FStats
	{
		int64 Retired = 0;
		int64 Released = 0;
		int64 Leaked = 0;
		int32 Pending = 0;
	};

	/** Public for tests, which use their own instance. Production code uses Get(). */
	FSpoutGpuRetirement() = default;
	~FSpoutGpuRetirement();

	void Retire(TUniquePtr<FRetiredSet> Record);

	/** Releases every record whose work is proven finished. Returns how many were released. */
	int32 Poll();

	/**
	 * Shutdown, before the native device is released. Polls until every record is released or TimeoutMs passed.
	 * What is still pending then is kept alive for the rest of the process and logged: never released unproven.
	 */
	void DrainForShutdown(uint32 TimeoutMs);

	FStats GetStats() const;

#if WITH_DEV_AUTOMATION_TESTS
	/** Tests only: drops every record without a proof. Valid only for records holding no object a GPU uses. */
	void DiscardForTest();
#endif

private:
	bool IsIdle(FRetiredSet& Record);
	static void Release(FRetiredSet& Record);

	mutable FCriticalSection Mutex;
	TArray<TUniquePtr<FRetiredSet>> Records;
	int64 RetiredCount = 0;
	int64 ReleasedCount = 0;
	int64 LeakedCount = 0;
};

/** The synchronous receive's slots at one sender identity (receiver role). Render thread. */
class FSpoutSlotReceive
{
public:
	struct FIdentity
	{
		HANDLE SharedHandle = nullptr;
		uint32 Width = 0;
		uint32 Height = 0;
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
		bool operator==(const FIdentity& O) const { return SharedHandle == O.SharedHandle && Width == O.Width && Height == O.Height && Format == O.Format; }
		bool operator!=(const FIdentity& O) const { return !(*this == O); }
	};

	/** Three, as in the asynchronous receive: a slot is free again two receives after it was read. */
	static constexpr int32 NumSlots = 3;
	/**
	 * A native copy proven finished later than this is discarded (Late) and never published. The bound classifies; it
	 * never ends the wait: the peer's access is held until the copy is proven finished. As in the asynchronous receive.
	 */
	static constexpr uint32 CompletionTimeoutMs = 100;

	/** Null when a precondition fails; OutWhy says which. Nothing is submitted before it returns. */
	static TUniquePtr<FSpoutSlotReceive> Create(const FName& Name, const FIdentity& Identity, EPixelFormat PixelFormat, FString& OutWhy);

	/** Hands every object to FSpoutGpuRetirement with its outstanding work. Never releases one directly. */
	~FSpoutSlotReceive();

	const FIdentity& GetIdentity() const { return Identity; }
	uint64 GetGeneration() const { return Generation; }
	ID3D11Texture2D* GetNativeSource() const { return NativeSource.Get(); }
	bool IsKeyedSource() const { return bKeyedSource; }
	bool HasFailed() const { return bFailed; }
	const FString& GetFailure() const { return Failure; }

	/** A slot whose last read completed, or -1: the frame then stays pending and nothing waits. */
	int32 BeginWrite();

	/**
	 * Under the caller's Spout access: copies the source into Slot on the native device, signals, flushes, and waits
	 * until the copy is proven finished (or the native device is removed) before it returns, so that the caller never
	 * gives the peer its access back while the read may still run. Late when that took longer than
	 * CompletionTimeoutMs. OutValue is the written value on Completed.
	 */
	ESpoutSlotCopy CopyAndWait(int32 Slot, uint64& OutValue);

	/** The write into Slot was given up; it is never published. */
	void Abandon(int32 Slot, SpoutAsync::EAbandon Why);

	/**
	 * After a Completed copy: publishes Slot and queues, on RHICmdList, a wait on its written value, the copy into
	 * Destination and the read signal. False when nothing was queued.
	 */
	bool PublishAndConsume(int32 Slot, uint64 WrittenValue, FRHICommandListImmediate& RHICmdList, FRHITexture* Destination, ERHIAccess DestinationBefore, ERHIAccess DestinationAfter);

	/** A keyed source's release is recorded on the native context: submit it once access was given back. */
	void FlushKeyedRelease();

	const SpoutAsync::FSlotCounters& GetCounters() const { return Pool.GetCounters(); }
	const SpoutSlotRules::FOutstanding& GetOutstanding() const { return Outstanding; }

private:
	FSpoutSlotReceive(const FName& InName, const FIdentity& InIdentity);
	bool Initialize(EPixelFormat PixelFormat, FString& OutWhy);
	void Fail(const TCHAR* Why, HRESULT Result);

	const FName Name;
	const FIdentity Identity;
	const uint64 Generation;

	SpoutAsync::FSlotPool Pool{NumSlots};
	SpoutSlotRules::FOutstanding Outstanding;
	SpoutSlotRules::FTimeoutStreak Timeouts;
	uint64 WrittenValue = 0;
	uint64 ReadValue = 0;

	// Unreal's device.
	Microsoft::WRL::ComPtr<ID3D12Fence> WrittenFence;
	Microsoft::WRL::ComPtr<ID3D12Fence> ReadFence;
	Microsoft::WRL::ComPtr<ID3D12Resource> SlotResources[NumSlots];
	FTextureRHIRef SlotTextures[NumSlots];

	// The native device.
	Microsoft::WRL::ComPtr<ID3D11Texture2D> NativeSource;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> NativeSlots[NumSlots];
	Microsoft::WRL::ComPtr<ID3D11Fence> NativeWrittenFence;
	Microsoft::WRL::ComPtr<ID3D11Query> Proof;
	bool bKeyedSource = false;

	HANDLE Event = nullptr;
	TSharedPtr<FSpoutResourceReservation, ESPMode::ThreadSafe> Reservation;
	bool bFailed = false;
	FString Failure;
};

/** One sending source's slot (a render-target sender entry, or the viewport). Render thread. */
class FSpoutSlotSend
{
public:
	struct FIdentity
	{
		uint32 Width = 0;
		uint32 Height = 0;
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
		EPixelFormat PixelFormat = PF_Unknown;
		bool operator==(const FIdentity& O) const { return Width == O.Width && Height == O.Height && Format == O.Format && PixelFormat == O.PixelFormat; }
		bool operator!=(const FIdentity& O) const { return !(*this == O); }
	};

	/**
	 * Bound of the CPU wait for Unreal's queue to finish the copy into the slot. No access is held during it, and
	 * SubmitSpoutInteropTransitions has submitted the copy first. Longer than any frame the plugin is meant for,
	 * shorter than Unreal's own GPU hang detection.
	 */
	static constexpr uint32 FilledTimeoutMs = 1000;

	static TUniquePtr<FSpoutSlotSend> Create(const FString& What, const FIdentity& Identity, FString& OutWhy);
	~FSpoutSlotSend();

	const FIdentity& GetIdentity() const { return Identity; }
	bool HasFailed() const { return bFailed; }
	const FString& GetFailure() const { return Failure; }

	/** Whether Unreal's queue may write the slot: no native copy may still read it. */
	bool CanWrite() const;

	/**
	 * Queues, on RHICmdList: the source into its copy state, the copy source -> slot, the source back, and the filled
	 * signal. Returns the filled value. The caller submits afterwards.
	 */
	uint64 QueueFill(FRHICommandListImmediate& RHICmdList, FRHITexture* Source, ERHIAccess SourceBefore, ERHIAccess SourceCopy, ERHIAccess SourceAfter);

	/** Bounded CPU wait (FilledTimeoutMs) for Unreal's queue to finish the copy into the slot. */
	SpoutSlotRules::EWait WaitFilled(uint64 Value);

	/**
	 * Under Destination's Spout access: copies the slot into Destination on the native device and waits until that
	 * copy is proven finished, or the native device was removed. Never a wall-clock release while the GPU may still
	 * write the texture a peer reads (the rule of SpoutGpuCompletion.h). After a failed signal the proof is the event
	 * query; a query error that is not a removal is answered by asking again, never by releasing access.
	 */
	ESpoutSlotCopy DrainInto(ID3D11Texture2D* Destination);

	/** A keyed destination's release is recorded on the native context: submit it once access was given back. */
	void FlushKeyedRelease();

	const SpoutSlotRules::FOutstanding& GetOutstanding() const { return Outstanding; }

private:
	FSpoutSlotSend(const FString& InWhat, const FIdentity& InIdentity);
	bool Initialize(FString& OutWhy);
	void Fail(const TCHAR* Why, HRESULT Result);

	const FString What;
	const FIdentity Identity;
	SpoutSlotRules::FOutstanding Outstanding;
	uint64 FilledValue = 0;
	uint64 DrainedValue = 0;

	Microsoft::WRL::ComPtr<ID3D12Fence> FilledFence;
	Microsoft::WRL::ComPtr<ID3D12Fence> DrainedFence;
	Microsoft::WRL::ComPtr<ID3D12Resource> SlotResource;
	FTextureRHIRef SlotTexture;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> NativeSlot;
	Microsoft::WRL::ComPtr<ID3D11Fence> NativeDrainedFence;
	Microsoft::WRL::ComPtr<ID3D11Query> Proof;

	HANDLE Event = nullptr;
	TSharedPtr<FSpoutResourceReservation, ESPMode::ThreadSafe> Reservation;
	bool bFailed = false;
	FString Failure;
};

/**
 * Hands a native Spout texture whose last write could not be proven finished to the retirement list, where only
 * the removal of the native device releases it. Used instead of a plain release when a send's signal and its proof
 * both failed.
 */
void SpoutRetireUnprovenTexture(Microsoft::WRL::ComPtr<ID3D11Texture2D>&& Texture, const FString& What);

#if WITH_DEV_AUTOMATION_TESTS
/** Failure injection for the automation tests. Each counter makes that many next events take the failure branch. */
namespace SpoutSlotTestHooks
{
	/** The next native signal is reported failed without being made (receive and send). */
	extern std::atomic<int32> FailNativeSignals;
	/** The next receive copy is classified late (discarded) after it was proven finished. */
	extern std::atomic<int32> TimeoutReceiveWaits;
	/** The next send filled wait reports a timeout without waiting. */
	extern std::atomic<int32> TimeoutFilledWaits;
	/** The next native wait does not accept completion before this many milliseconds: a long GPU read or write. */
	extern std::atomic<int32> DelayNativeCompletionMs;
	/** The next proof queries that report completion are reported as errors instead (not removals). */
	extern std::atomic<int32> FailProofQueries;

	/** Arms a read-back of the next send source, taken right after its copy into the slot. Game thread. */
	void ArmSourceCapture();
	/** True once the armed read-back was taken; returns it. Game thread, after the send command ran. */
	bool TakeSourceCapture(TArray<FLinearColor>& OutPixels, FIntPoint& OutSize, EPixelFormat& OutFormat);
	/** Render thread, from QueueFill. */
	void OnSourceCopied(FRHICommandListImmediate& RHICmdList, FRHITexture* Source);
}
#endif

#endif // PLATFORM_WINDOWS
