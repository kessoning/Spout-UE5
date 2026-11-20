// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Declares the singleton context used for Spout interop: the Spout helper objects, the interop path's device, and
 * shared mutexes for sender/receiver code. The D3D11-on-12 device exists only for the wrap path
 * (r.Spout.SharedSlotInterop 0, or no native device); the shared-slot path (the default) never creates it.
 */
#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "SpoutLogThrottle.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include <d3d11.h>
// Include the engine's d3d12.h (DirectX module include path) before the Windows SDK's d3d11on12.h.
// d3d11on12.h includes "d3d12.h" from its own SDK folder; if that copy is first, its include guard
// hides the engine's newer copy and the engine's d3dx12.h fails to compile on older Windows SDKs.
#include <d3d12.h>
#include <d3d11on12.h>
#include <wrl/client.h>
#include "SpoutDX/SpoutDirectX.h"
#include "SpoutDX/SpoutSenderNames.h"
#include "SpoutSdkSafety.h"
#include "SpoutGpuCompletion.h"
#include "Windows/HideWindowsPlatformTypes.h"
#ifdef GetNextSibling
#undef GetNextSibling
#endif
#endif

class FSpoutD3DContext
{
public:
	// Singleton lifetime is process-wide; created on first access and never destroyed explicitly.
	static FSpoutD3DContext& Get();

	// Lazily creates the Spout helpers, latches the interop path and, for the wrap path only, the D3D11-on-12 device.
	void InitializeIfNeeded();
	void Shutdown();

	bool IsInitialized() const;
	bool IsSpoutAvailable() const;

	/**
	 * The D3D11 device the latched interop path opens and creates Spout's shared textures with, on Unreal's adapter:
	 * the native device on the shared-slot path, the D3D11-on-12 device on the wrap path. Null before initialization.
	 */
	ID3D11Device* GetInteropDevice() const;

	// The D3D11-on-12 device and its context: wrap path only (null on the shared-slot path).
	ID3D11Device* GetD3D11Device() const;
	ID3D11DeviceContext* GetImmediateContext() const;
	ID3D11On12Device* GetD3D11On12Device() const;
	ID3D12CommandQueue* GetD3D12CommandQueue() const;

	/** Call only while holding D3D11ContextMutex. */
	bool PrepareGpuCompletion();
	HRESULT WaitForGpuCompletion();

	FSpoutSafeSenderNames* GetSenderNames() const;
	spoutDirectX* GetSpoutDirectX() const;

	FCriticalSection& GetD3D11ContextMutex();
	FCriticalSection& GetSpoutMutex();

private:
	FSpoutD3DContext() = default;
	// Creates the Spout helpers, latches the interop path and creates what that path needs; only called under InitMutex.
	void Initialize();
#if PLATFORM_WINDOWS
	// Creates the D3D11-on-12 device over Unreal's D3D12 device and graphics queue; only called under InitMutex.
	bool CreateD3D11On12Device(ID3D12Device* D3D12Device, ID3D12CommandQueue* D3D12Queue);
#endif

	// Serializes initialization/shutdown across threads.
	// Mutable so const accessors (IsInitialized/IsSpoutAvailable) can lock during reads.
	mutable FCriticalSection InitMutex;
	// Protects D3D11 immediate context use (not thread-safe).
	FCriticalSection D3D11ContextMutex;
	// Protects Spout global registry access.
	FCriticalSection SpoutMutex;
	bool bUnsupportedRHI = false;
	// Spout.dll did not load: no initialization is attempted again.
	bool bSpoutDllMissing = false;
	// The helpers exist, the interop path is latched, and that path's device exists.
	bool bInitialized = false;
	double NextInitializeAttempt = 0;
	FSpoutLogThrottle InitializationLogThrottle;

#if PLATFORM_WINDOWS
	Microsoft::WRL::ComPtr<ID3D11Device> D3D11Device;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> ImmediateContext;
	Microsoft::WRL::ComPtr<ID3D11On12Device> D3D11On12Device;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> D3D12CommandQueue;
	// Owned Spout helpers; lifetime matches D3D device lifetime.
	TUniquePtr<FSpoutSafeSenderNames> SpoutSenderNames;
	TUniquePtr<spoutDirectX> SpoutDirectX;
	FSpoutGpuCompletion GpuCompletion;
	FSpoutLogThrottle GpuFailureLogThrottle;
#endif
};
