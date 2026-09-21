// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Declares the singleton D3D11-on-D3D12 context used for Spout interop, exposing devices,
 * Spout helper objects, and shared mutexes for sender/receiver code.
 */
#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "SpoutLogThrottle.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
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

	// Lazily initializes a D3D11-on-D3D12 device for Spout interop.
	void InitializeIfNeeded();
	void Shutdown();

	bool IsInitialized() const;
	bool IsSpoutAvailable() const;

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
	// Creates D3D11-on-12 device and Spout helpers; only called under InitMutex.
	void Initialize();

	// Serializes initialization/shutdown across threads.
	// Mutable so const accessors (IsInitialized/IsSpoutAvailable) can lock during reads.
	mutable FCriticalSection InitMutex;
	// Protects D3D11 immediate context use (not thread-safe).
	FCriticalSection D3D11ContextMutex;
	// Protects Spout global registry access.
	FCriticalSection SpoutMutex;
	bool bUnsupportedRHI = false;
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
