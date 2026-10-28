// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * The native device's fences, waits and proofs, shared by every user of the plugin's native D3D11 device: the
 * synchronous shared-slot paths (SpoutSharedSlot.h), Async Receive (SpoutAsyncReceive.h) and the asynchronous
 * render-target sender (SpoutAsyncSend.h). Defined in SpoutSharedSlot.cpp, next to the shared-slot path's own
 * helpers; the rules are those of SpoutSharedSlot.h and SpoutSlotRules.h:
 *   - access to a texture a peer uses is given back only once the native GPU work on it is proven finished (its fence
 *     value reached, or the event query issued after it reported done), or the native device was removed;
 *   - no queue waits on a fence value not already observed complete on the CPU;
 *   - objects a submitted GPU operation may still use are handed to FSpoutGpuRetirement, never released directly.
 * The synchronous slot path's failure injection (SpoutSlotTestHooks) is not taken by Signal and WaitForProof: each
 * worker has its own hooks, so a failure injected for one path is never consumed by another path's worker.
 */
#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS

#include "RHI.h"
#include "SpoutSlotRules.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include "Windows/HideWindowsPlatformTypes.h"

class FRHICommandListImmediate;

/** How a wait for native work proven finished ended. */
enum class ESpoutNativeProof : uint8
{
	InTime = 0,   // proven finished within the bound
	Late = 1,     // proven finished, after the bound
	Removed = 2   // the native device was removed: nothing it was given will run any more
};

namespace SpoutNativeWork
{
	/** Whether the plugin's native device is missing or removed. */
	bool NativeRemoved();

	/** Whether Unreal's D3D12 device is missing or removed. */
	bool UnrealRemoved();

	/**
	 * Waits until the native work submitted before Value's signal is proven finished, or the native device is removed.
	 * It never returns earlier, whatever BoundMs is: callers hold a peer's access. BoundMs only classifies the result.
	 * The proof is the fence (read through its D3D12 view View) when the signal succeeded, else the event query Proof
	 * issued right after the work; when Unreal's device is removed the proof moves to an event query. A warning is
	 * logged every 5 s while it waits. NotBeforeMs is for tests only (a GPU operation that takes that long); it delays
	 * the acceptance of a real proof and is ignored outside automation-test builds.
	 */
	ESpoutNativeProof WaitForProof(ID3D12Fence* View, uint64 Value, bool bSignalled, ID3D11Query* Proof, uint32 BoundMs, HANDLE Event,
		const FString& What, int32 NotBeforeMs = 0);

	/** The native signal of a copy. The native context lock is held by the caller. */
	HRESULT Signal(ID3D11DeviceContext4* Context, ID3D11Fence* Fence, uint64 Value);

	/** An event query on the native device, for proving work whose signal failed. */
	HRESULT CreateProof(Microsoft::WRL::ComPtr<ID3D11Query>& Out);

	/**
	 * Waits, at most TimeoutMs, until a fence Unreal's queue signals reached Value. Used while no access is held. A
	 * removal of either device ends it.
	 */
	SpoutSlotRules::EWait WaitForQueueFence(ID3D12Fence* Fence, uint64 Value, uint32 TimeoutMs, HANDLE Event);

	/** A D3D12 fence on Unreal's device; bShared also opens it on the native device (OutNative). */
	HRESULT CreateFencePair(bool bShared, Microsoft::WRL::ComPtr<ID3D12Fence>& Out12, Microsoft::WRL::ComPtr<ID3D11Fence>* OutNative);

	/**
	 * A slot: a committed texture on Unreal's device in a shared heap with simultaneous access, opened on the native
	 * device, and an RHI texture of it.
	 */
	HRESULT CreateSlotTexture(uint32 Width, uint32 Height, DXGI_FORMAT Format, EPixelFormat PixelFormat,
		Microsoft::WRL::ComPtr<ID3D12Resource>& OutResource, Microsoft::WRL::ComPtr<ID3D11Texture2D>& OutNative, FTextureRHIRef& OutTexture);

	/** Queues a wait of Unreal's graphics queue on a fence value already observed complete on the CPU. RHI thread. */
	void QueueWait(FRHICommandListImmediate& Cmd, ID3D12Fence* Fence, uint64 Value);

	/** Adds a COM object to a retired set's object list (null is skipped). */
	void AddRetiredObject(TArray<TRefCountPtr<IUnknown>>& Objects, IUnknown* Object);
}

#endif // PLATFORM_WINDOWS
