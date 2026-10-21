// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements creation and teardown of the D3D11-on-D3D12 bridge and Spout helpers using
 * UE's D3D12 RHI, providing a shared context for sender and receiver paths.
 */
#include "SpoutD3DContext.h"
#include "SpoutAsyncReceive.h"
#include "SpoutDebugCapture.h"
#include "SpoutFrameSync.h"
#include "SpoutModule.h"
#include "SpoutSharedSlot.h"

#include "RHI.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

bool FSpoutD3DContext::PrepareGpuCompletion()
{
	const bool bReady = GpuCompletion.Prepare(ImmediateContext.Get());
	if (!bReady && GpuFailureLogThrottle.ShouldLog())
	{
		UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout GPU completion query unavailable (hr=0x%08x); copy deferred."), GpuCompletion.GetLastResult());
	}
	return bReady;
}

HRESULT FSpoutD3DContext::WaitForGpuCompletion()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_WaitForGPUCompletion);
	const HRESULT Result = GpuCompletion.Wait(ImmediateContext.Get());
	if (FAILED(Result) && GpuFailureLogThrottle.ShouldLog())
	{
		UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout GPU copy did not complete (hr=0x%08x); frame not acknowledged."), Result);
	}
	return Result;
}

FSpoutD3DContext& FSpoutD3DContext::Get()
{
	static FSpoutD3DContext Instance;
	return Instance;
}

void FSpoutD3DContext::InitializeIfNeeded()
{
#if PLATFORM_WINDOWS
	FScopeLock Lock(&InitMutex);
	// Initialization is idempotent and guarded by InitMutex.
	if (D3D11Device || bUnsupportedRHI || FPlatformTime::Seconds() < NextInitializeAttempt)
	{
		return;
	}
	NextInitializeAttempt = FPlatformTime::Seconds() + 1.0;
	Initialize();
#endif
}

void FSpoutD3DContext::Initialize()
{
#if PLATFORM_WINDOWS
	// Spout uses D3D11 interop; in UE5 the plugin bridges via D3D11-on-12.
	if (!GDynamicRHI) return;
	if (GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::D3D12)
	{
		bUnsupportedRHI = true;
		UE_LOG(LogSpoutPlugin, Warning, TEXT("SpoutPlugin requires DX12 RHI; interop disabled for this session."));
		return;
	}

	ID3D12Device* D3D12Dev = static_cast<ID3D12Device*>(GDynamicRHI->RHIGetNativeDevice());
	ID3D12CommandQueue* D3D12Queue = static_cast<ID3D12CommandQueue*>(GDynamicRHI->RHIGetNativeGraphicsQueue());

	if (!D3D12Dev || !D3D12Queue)
	{
		return;
	}

	UINT DeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	D3D_FEATURE_LEVEL FeatureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

	// Create a D3D11 device that wraps the engine's D3D12 device + graphics queue.
	// D3D11On12CreateDevice expects IUnknown* const*; build a proper array rather than
	// reinterpret-casting an ID3D12CommandQueue** (which violates strict aliasing).
	IUnknown* const CommandQueues[] = { static_cast<IUnknown*>(D3D12Queue) };

	Microsoft::WRL::ComPtr<ID3D11Device> LocalD3D11;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> LocalContext;
	HRESULT hr = D3D11On12CreateDevice(
		D3D12Dev,
		DeviceFlags,
		FeatureLevels,
		2,
		CommandQueues,
		1,
		0,
		LocalD3D11.GetAddressOf(),
		LocalContext.GetAddressOf(),
		nullptr
	);

	if (FAILED(hr))
	{
		if (InitializationLogThrottle.ShouldLog()) UE_LOG(LogSpoutPlugin, Error, TEXT("Failed to create D3D11On12 device (hr=0x%08x, device reason=0x%08x)"), hr, D3D12Dev->GetDeviceRemovedReason());
		return;
	}

	Microsoft::WRL::ComPtr<ID3D11On12Device> LocalOn12;
	hr = LocalD3D11.As(&LocalOn12);
	if (FAILED(hr))
	{
		if (InitializationLogThrottle.ShouldLog()) UE_LOG(LogSpoutPlugin, Error, TEXT("Failed to query ID3D11On12Device (hr=0x%08x)"), hr);
		return;
	}

	D3D12CommandQueue = D3D12Queue;
	D3D11Device = MoveTemp(LocalD3D11);
	ImmediateContext = MoveTemp(LocalContext);
	D3D11On12Device = MoveTemp(LocalOn12);

	// Guard the first touch of any Spout symbol: constructing spoutSenderNames/spoutDirectX
	// triggers the delay-load bind of Spout.dll, which crashes with 0xC06D007E if the DLL
	// never loaded. Bailing here leaves the Spout helpers null, so IsSpoutAvailable() stays
	// false and FSpoutSender::Send returns cleanly instead of crashing the editor.
	if (!FSpoutModule::IsSpoutDllLoaded())
	{
		UE_LOG(LogSpoutPlugin, Error, TEXT("Spout.dll failed to load at startup; Spout is disabled. See earlier log."));
		return;
	}

	// Spout helpers manage shared textures and the global sender registry.
	SpoutSenderNames = MakeUnique<FSpoutSafeSenderNames>();
	SpoutDirectX = MakeUnique<spoutDirectX>();

	UE_LOG(LogSpoutPlugin, Log, TEXT("SpoutDX11on12 Initialized."));

	// Before any stream exists: the debug capture first, so that the native device's creation is inside its
	// window, then the one-time choice of the interop path (r.Spout.SharedSlotInterop).
	SpoutDebugCapture::Start();
	SpoutInterop::LatchOnce();
#endif
}

void FSpoutD3DContext::Shutdown()
{
#if PLATFORM_WINDOWS
	// Serialize with InitializeIfNeeded/IsInitialized/IsSpoutAvailable. InitMutex is documented to
	// guard Shutdown(); taking it here makes that contract real and prevents an init racing teardown.
	FScopeLock Lock(&InitMutex);

	// The shared-slot path's retired objects, before the native device goes: each is released only once its GPU
	// work is proven finished; what cannot be proven within the bound is kept, never released (SpoutSharedSlot.h).
	FSpoutGpuRetirement::Get().DrainForShutdown(2000);
	UE_LOG(LogSpoutPlugin, Log, TEXT("Spout interop at shutdown: path=%s wrap_calls=%lld"),
		SpoutInterop::ToString(SpoutInterop::GetMode()), SpoutInterop::GetWrapCallCount());
	SpoutDebugCapture::ReportNative(TEXT("shutdown"));
	SpoutDebugCapture::Stop();

	// The asynchronous receive's device. Its streams were destroyed with their registry entries.
	FSpoutAsyncDevice::Get().Shutdown();

	// Pooled frame-count backends are SDK objects; destroy them while Spout.dll is still loaded.
	DrainSpoutSdkFrameSyncBackendPool();

	// Release in reverse creation order; shared resources should already be idle.
	SpoutSenderNames.Reset();
	SpoutDirectX.Reset();
	GpuCompletion.Reset();

	ImmediateContext.Reset();
	D3D11On12Device.Reset();
	D3D11Device.Reset();
	D3D12CommandQueue.Reset();
	bUnsupportedRHI = false;
	NextInitializeAttempt = 0;
#endif
}

bool FSpoutD3DContext::IsInitialized() const
{
#if PLATFORM_WINDOWS
	// InitMutex also guards Shutdown(), so a concurrent teardown cannot race with this read.
	FScopeLock Lock(&InitMutex);
	return D3D11Device != nullptr;
#else
	return false;
#endif
}

bool FSpoutD3DContext::IsSpoutAvailable() const
{
#if PLATFORM_WINDOWS
	FScopeLock Lock(&InitMutex);
	return D3D11Device && D3D11On12Device && SpoutSenderNames && SpoutDirectX;
#else
	return false;
#endif
}

ID3D11Device* FSpoutD3DContext::GetD3D11Device() const
{
#if PLATFORM_WINDOWS
	return D3D11Device.Get();
#else
	return nullptr;
#endif
}

ID3D11DeviceContext* FSpoutD3DContext::GetImmediateContext() const
{
#if PLATFORM_WINDOWS
	return ImmediateContext.Get();
#else
	return nullptr;
#endif
}

ID3D11On12Device* FSpoutD3DContext::GetD3D11On12Device() const
{
#if PLATFORM_WINDOWS
	return D3D11On12Device.Get();
#else
	return nullptr;
#endif
}

ID3D12CommandQueue* FSpoutD3DContext::GetD3D12CommandQueue() const
{
#if PLATFORM_WINDOWS
	return D3D12CommandQueue.Get();
#else
	return nullptr;
#endif
}

FSpoutSafeSenderNames* FSpoutD3DContext::GetSenderNames() const
{
#if PLATFORM_WINDOWS
	return SpoutSenderNames.Get();
#else
	return nullptr;
#endif
}

spoutDirectX* FSpoutD3DContext::GetSpoutDirectX() const
{
#if PLATFORM_WINDOWS
	return SpoutDirectX.Get();
#else
	return nullptr;
#endif
}

FCriticalSection& FSpoutD3DContext::GetD3D11ContextMutex()
{
	return D3D11ContextMutex;
}

FCriticalSection& FSpoutD3DContext::GetSpoutMutex()
{
	return SpoutMutex;
}
