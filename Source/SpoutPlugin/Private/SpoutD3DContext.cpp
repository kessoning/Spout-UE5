/**
 * Implements creation and teardown of the D3D11-on-D3D12 bridge and Spout helpers using
 * UE's D3D12 RHI, providing a shared context for sender and receiver paths.
 */
#include "SpoutD3DContext.h"
#include "SpoutModule.h"

#include "RHI.h"

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
	if (D3D11Device)
	{
		return;
	}
	Initialize();
#endif
}

void FSpoutD3DContext::Initialize()
{
#if PLATFORM_WINDOWS
	// Spout uses D3D11 interop; in UE5 we bridge via D3D11-on-12.
	if (!GDynamicRHI || GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::D3D12)
	{
		UE_LOG(LogSpoutPlugin, Error, TEXT("SpoutPlugin requires DX12 RHI."));
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
		UE_LOG(LogSpoutPlugin, Error, TEXT("Failed to create D3D11On12 device (hr=0x%08x)"), hr);
		return;
	}

	Microsoft::WRL::ComPtr<ID3D11On12Device> LocalOn12;
	hr = LocalD3D11.As(&LocalOn12);
	if (FAILED(hr))
	{
		UE_LOG(LogSpoutPlugin, Error, TEXT("Failed to query ID3D11On12Device (hr=0x%08x)"), hr);
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
	SpoutSenderNames = MakeUnique<spoutSenderNames>();
	SpoutDirectX = MakeUnique<spoutDirectX>();

	UE_LOG(LogSpoutPlugin, Log, TEXT("SpoutDX11on12 Initialized."));
#endif
}

void FSpoutD3DContext::Shutdown()
{
#if PLATFORM_WINDOWS
	// Serialize with InitializeIfNeeded/IsInitialized/IsSpoutAvailable. InitMutex is documented to
	// guard Shutdown(); taking it here makes that contract real and prevents an init racing teardown.
	FScopeLock Lock(&InitMutex);

	// Release in reverse creation order; shared resources should already be idle.
	SpoutSenderNames.Reset();
	SpoutDirectX.Reset();

	ImmediateContext.Reset();
	D3D11On12Device.Reset();
	D3D11Device.Reset();
	D3D12CommandQueue.Reset();
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

spoutSenderNames* FSpoutD3DContext::GetSenderNames() const
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
