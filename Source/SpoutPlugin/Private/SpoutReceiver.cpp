/**
 * Implements the Spout receiver flow.
 *
 * Two paths are supported, controlled by CVar r.Spout.GPUReceiver:
 *   - GPU path (default, CVar=1): opens the external sender's shared DX11 texture and
 *     copies it directly into the UE RHI destination via D3D11-on-12 wrapping.
 *     No CPU readback, no staging texture map.
 *   - CPU path (CVar=0): legacy fallback — copies into a D3D11 STAGING texture, maps it,
 *     uploads via RHIUpdateTexture2D straight from the mapped pointer. Preserved for
 *     bisection/debug parity with the previous build.
 */
#include "SpoutReceiver.h"
#include "Materials/MaterialInterface.h"
#include "SpoutD3DContext.h"
#include "SpoutD3DUtils.h"
#include "SpoutSenderRegistry.h"
#include "SpoutTextureUtils.h"
#include "SpoutModule.h"

#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "HAL/IConsoleManager.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "Engine/Texture.h"
#include "TextureResource.h"
#include "RenderResource.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTarget.h"
// UObject/Package.h is required for the full UPackage type so that
// GetTransientPackage() (UPackage*) implicitly converts to UObject* in
// non-Editor / non-unity builds where SharedPCH.UnrealEd doesn't apply.
#include "UObject/Package.h"

using Microsoft::WRL::ComPtr;

namespace
{
	static TAutoConsoleVariable<int32> CVarSpoutGPUReceiver(
		TEXT("r.Spout.GPUReceiver"),
		1,
		TEXT("Spout receiver path: 1 = GPU-direct via D3D11-on-12 (H2A, default), 0 = legacy CPU readback (H2B)."),
		ECVF_Default);

	bool QuerySenderInfo(const FName& SpoutName, unsigned int& OutWidth, unsigned int& OutHeight, HANDLE& OutHandle, unsigned long& OutFormat)
	{
		FSpoutD3DContext& Context = FSpoutD3DContext::Get();
		spoutSenderNames* SenderNames = Context.GetSenderNames();
		if (!SenderNames)
		{
			return false;
		}

		// Spout sender list is shared process-wide; guard with the mutex.
		FScopeLock SpoutLock(&Context.GetSpoutMutex());
		return SenderNames->GetSenderInfo(TCHAR_TO_UTF8(*SpoutName.ToString()), OutWidth, OutHeight, OutHandle, OutFormat);
	}

	bool UpdateSharedReceiverTexture(FSpoutSharedSender& Receiver, unsigned int Width, unsigned int Height, HANDLE SharedHandle, FSpoutD3DContext& Context)
	{
		// Runs on the game thread while the render thread may concurrently read these same fields
		// under ResourceMutex (GetSharedForGPUPath / GetOrCreateReceiverWrap). Hold the lock across
		// the unchanged-check AND the writes so the comparison and the swap are atomic w.r.t. the
		// render thread; otherwise a torn read can early-out and reuse a stale/freed handle.
		FScopeLock ReceiverLock(&Receiver.ResourceMutex);

		// Reuse the shared texture while the handle and dimensions are unchanged.
		if (Receiver.Width == Width && Receiver.Height == Height && Receiver.SharedHandle == SharedHandle && Receiver.SharedTexture)
		{
			return true;
		}

		// Open the shared DX11 texture from the external sender's handle. OpenSharedResource is a
		// device-level (free-threaded) call; holding ResourceMutex across it is acceptable because
		// this only happens on connect/resize/handle-change, not per frame.
		ComPtr<ID3D11Texture2D> NewSharedTexture;
		HRESULT hr = Context.GetD3D11Device()->OpenSharedResource(
			SharedHandle,
			IID_PPV_ARGS(NewSharedTexture.GetAddressOf())
		);

		if (FAILED(hr) || !NewSharedTexture)
		{
			UE_LOG(LogSpoutPlugin, Error,
				TEXT("Failed to open shared Spout texture for sender '%s' (hr=0x%08x)"),
				*Receiver.Name.ToString(),
				hr);
			return false;
		}

		Receiver.Width = Width;
		Receiver.Height = Height;
		Receiver.SharedHandle = SharedHandle;
		// SharedTexture is owned by this receiver; the sender owns the underlying resource.
		Receiver.SharedTexture = MoveTemp(NewSharedTexture);
		// Invalidate caches tied to previous geometry/handle.
		Receiver.StagingTexture.Reset();
		Receiver.CachedWrappedResource.Reset();
		Receiver.CachedNativeResource = nullptr;
		Receiver.CachedRTWrappedResource.Reset();
		Receiver.CachedRTNativeResource = nullptr;

		return true;
	}

	// GPU path: return just the shared texture + dimensions. No staging involvement.
	bool GetSharedForGPUPath(FSpoutSharedSender& Receiver, ComPtr<ID3D11Texture2D>& OutShared, uint32& OutWidth, uint32& OutHeight)
	{
		FScopeLock ReceiverLock(&Receiver.ResourceMutex);
		if (!Receiver.SharedTexture)
		{
			return false;
		}
		OutWidth = Receiver.Width;
		OutHeight = Receiver.Height;
		OutShared = Receiver.SharedTexture;
		return true;
	}

	// CPU path: return shared + staging (creating/resizing the staging texture on demand).
	bool GetSharedAndStaging(FSpoutSharedSender& Receiver, FSpoutD3DContext& Context, ComPtr<ID3D11Texture2D>& OutShared, ComPtr<ID3D11Texture2D>& OutStaging, uint32& OutWidth, uint32& OutHeight)
	{
		FScopeLock ReceiverLock(&Receiver.ResourceMutex);
		if (!Receiver.SharedTexture)
		{
			return false;
		}

		OutWidth = Receiver.Width;
		OutHeight = Receiver.Height;
		OutShared = Receiver.SharedTexture;

		// Staging texture is CPU-readable for readback into a UE UTexture2D.
		bool bNeedsResize = false;
		if (!Receiver.StagingTexture)
		{
			bNeedsResize = true;
		}
		else
		{
			D3D11_TEXTURE2D_DESC Desc;
			Receiver.StagingTexture->GetDesc(&Desc);
			if (Desc.Width != Receiver.Width || Desc.Height != Receiver.Height)
			{
				bNeedsResize = true;
			}
		}

		if (bNeedsResize)
		{
			D3D11_TEXTURE2D_DESC Desc;
			Receiver.SharedTexture->GetDesc(&Desc);

			Desc.Usage = D3D11_USAGE_STAGING;
			Desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			Desc.BindFlags = 0;
			Desc.MiscFlags = 0;
			Desc.SampleDesc.Count = 1;
			Desc.SampleDesc.Quality = 0;

			// Staging texture is owned by the receiver and recreated on size changes.
			ComPtr<ID3D11Texture2D> NewStaging;
			HRESULT hr = Context.GetD3D11Device()->CreateTexture2D(&Desc, nullptr, NewStaging.GetAddressOf());
			if (FAILED(hr))
			{
				UE_LOG(LogSpoutPlugin, Error, TEXT("Failed to create staging texture (hr=0x%08x)"), hr);
				return false;
			}

			Receiver.StagingTexture = MoveTemp(NewStaging);
		}

		OutStaging = Receiver.StagingTexture;
		return OutStaging != nullptr;
	}

	// Cache a D3D11-on-12 wrap of the primary UE RHI destination (UTexture backing).
	// Keyed on native D3D12 pointer so UE texture recreation invalidates the wrap.
	ComPtr<ID3D11Texture2D> GetOrCreateReceiverWrap(FSpoutSharedSender& Receiver, ID3D12Resource* NativeResource)
	{
		FScopeLock ReceiverLock(&Receiver.ResourceMutex);
		if (Receiver.CachedNativeResource != NativeResource || !Receiver.CachedWrappedResource)
		{
			Receiver.CachedWrappedResource = WrapD3D12Resource(NativeResource, D3D12_RESOURCE_STATE_COPY_DEST);
			Receiver.CachedNativeResource = NativeResource;
		}
		return Receiver.CachedWrappedResource;
	}

	// Cache a D3D11-on-12 wrap of the optional secondary UE RHI destination (render target).
	ComPtr<ID3D11Texture2D> GetOrCreateReceiverRTWrap(FSpoutSharedSender& Receiver, ID3D12Resource* NativeResource)
	{
		FScopeLock ReceiverLock(&Receiver.ResourceMutex);
		if (Receiver.CachedRTNativeResource != NativeResource || !Receiver.CachedRTWrappedResource)
		{
			Receiver.CachedRTWrappedResource = WrapD3D12Resource(NativeResource, D3D12_RESOURCE_STATE_COPY_DEST);
			Receiver.CachedRTNativeResource = NativeResource;
		}
		return Receiver.CachedRTWrappedResource;
	}

	EPixelFormat DxgiFormatToPixelFormat(unsigned long DxgiFormat)
	{
		switch (static_cast<DXGI_FORMAT>(DxgiFormat))
		{
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return PF_B8G8R8A8;

		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return PF_R8G8B8A8;

		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			return PF_A2B10G10R10;

		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			return PF_FloatRGBA;

		default:
			return PF_B8G8R8A8;
		}
	}

	// Whether the received texture should be sampled as sRGB. 8-bit color formats carry
	// gamma-encoded bytes (this is also what the sender publishes for UE sRGB render targets,
	// which it flattens to *_UNORM), so they must decode as sRGB — matching UTexture's default.
	// 10-bit and float formats carry linear/HDR data and must be sampled linearly.
	bool DxgiFormatPrefersSRGB(unsigned long DxgiFormat)
	{
		switch (static_cast<DXGI_FORMAT>(DxgiFormat))
		{
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return true;

		default:
			return false;
		}
	}

	// GPU-direct path (H2A): wrap UE RHI destinations as D3D11 textures via D3D11-on-12 and
	// issue CopyResource straight from the external shared texture. Zero CPU readback.
	void ReceiveOnRenderThread_GPU(
		TSharedPtr<FSpoutSharedSender> Receiver,
		TRefCountPtr<FRHITexture> DestTextureRHI,
		TRefCountPtr<FRHITexture> DestRenderTargetRHI,
		FRHICommandListImmediate& RHICmdList)
	{
		if (!DestTextureRHI.IsValid() && !DestRenderTargetRHI.IsValid())
		{
			return;
		}

		FSpoutD3DContext& Context = FSpoutD3DContext::Get();

		ComPtr<ID3D11Texture2D> SharedTexture;
		uint32 LocalWidth = 0;
		uint32 LocalHeight = 0;
		if (!GetSharedForGPUPath(*Receiver, SharedTexture, LocalWidth, LocalHeight))
		{
			return;
		}

		// Resolve native D3D12 resources up front. A null native means interop wrapping is
		// unavailable for that destination and it must be skipped (not fatal for the other).
		ID3D12Resource* DestNative = nullptr;
		ID3D12Resource* DestRTNative = nullptr;

		if (DestTextureRHI.IsValid())
		{
			DestNative = static_cast<ID3D12Resource*>(DestTextureRHI->GetNativeResource());
		}
		if (DestRenderTargetRHI.IsValid())
		{
			DestRTNative = static_cast<ID3D12Resource*>(DestRenderTargetRHI->GetNativeResource());
		}

		if (!DestNative && !DestRTNative)
		{
			return;
		}

		// Transition UE-tracked destinations into CopyDest. Must happen before the D3D11-on-12
		// Acquire, because Acquire assumes the resource is already in the state we declared
		// at CreateWrappedResource time.
		if (DestTextureRHI.IsValid() && DestNative)
		{
			RHICmdList.Transition(FRHITransitionInfo(DestTextureRHI, ERHIAccess::SRVMask, ERHIAccess::CopyDest));
		}
		if (DestRenderTargetRHI.IsValid() && DestRTNative)
		{
			RHICmdList.Transition(FRHITransitionInfo(DestRenderTargetRHI, ERHIAccess::SRVMask, ERHIAccess::CopyDest));
		}

		// Flush so D3D12 state transitions are submitted and visible to the D3D11-on-12 device
		// before Acquire/CopyResource runs. Mirrors the sender path.
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);

		{
			// Serialize D3D11 immediate-context access with the sender path.
			FScopeLock ContextLock(&Context.GetD3D11ContextMutex());

			ID3D11DeviceContext* ImmediateContext = Context.GetImmediateContext();
			ID3D11On12Device* D3D11On12 = Context.GetD3D11On12Device();
			if (ImmediateContext && D3D11On12)
			{
				ComPtr<ID3D11Texture2D> Wrapped;
				ComPtr<ID3D11Texture2D> WrappedRT;
				TArray<ID3D11Texture2D*, TInlineAllocator<2>> ToAcquire;

				if (DestTextureRHI.IsValid() && DestNative)
				{
					Wrapped = GetOrCreateReceiverWrap(*Receiver, DestNative);
					if (Wrapped)
					{
						ToAcquire.Add(Wrapped.Get());
					}
				}
				if (DestRenderTargetRHI.IsValid() && DestRTNative)
				{
					WrappedRT = GetOrCreateReceiverRTWrap(*Receiver, DestRTNative);
					if (WrappedRT)
					{
						ToAcquire.Add(WrappedRT.Get());
					}
				}

				if (ToAcquire.Num() > 0)
				{
					// Acquire every destination together. Acquire transitions the wrapped resources
					// into the state declared at wrap (COPY_DEST); the single scope-exit Release +
					// Flush covers all copies, so two destinations cost one GPU submit, not two.
					FScopedD3D11On12Acquire Acquire(ToAcquire);
					if (Acquire.IsValid())
					{
						if (Wrapped)
						{
							ImmediateContext->CopyResource(Wrapped.Get(), SharedTexture.Get());
						}
						if (WrappedRT)
						{
							ImmediateContext->CopyResource(WrappedRT.Get(), SharedTexture.Get());
						}
					}
				}
			}
		}

		// Always restore SRVMask, even if Wrap/Acquire failed, so UE's state tracking stays
		// consistent with our earlier Transition.
		if (DestTextureRHI.IsValid() && DestNative)
		{
			RHICmdList.Transition(FRHITransitionInfo(DestTextureRHI, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
		}
		if (DestRenderTargetRHI.IsValid() && DestRTNative)
		{
			RHICmdList.Transition(FRHITransitionInfo(DestRenderTargetRHI, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
		}
	}

	// CPU fallback path (Variant B from round 2). Copy into a STAGING texture, Map, issue
	// RHIUpdateTexture2D straight from the mapped pointer (no intermediate TArray copy),
	// defer Unmap until after both updates return.
	void ReceiveOnRenderThread_CPU(
		TSharedPtr<FSpoutSharedSender> Receiver,
		TRefCountPtr<FRHITexture> DestTextureRHI,
		TRefCountPtr<FRHITexture> DestRenderTargetRHI)
	{
		if (!DestTextureRHI.IsValid() && !DestRenderTargetRHI.IsValid())
		{
			return;
		}

		FSpoutD3DContext& Context = FSpoutD3DContext::Get();
		ComPtr<ID3D11Texture2D> SharedTexture;
		ComPtr<ID3D11Texture2D> StagingTexture;
		uint32 LocalWidth = 0;
		uint32 LocalHeight = 0;

		if (!GetSharedAndStaging(*Receiver, Context, SharedTexture, StagingTexture, LocalWidth, LocalHeight))
		{
			return;
		}

		// D3D11 immediate context use is serialized across sender/receiver paths.
		FScopeLock Lock(&Context.GetD3D11ContextMutex());
		ID3D11DeviceContext* ImmediateContext = Context.GetImmediateContext();
		if (!ImmediateContext || !SharedTexture || !StagingTexture)
		{
			return;
		}

		// Copy into a CPU-readable staging texture.
		ImmediateContext->CopyResource(StagingTexture.Get(), SharedTexture.Get());

		D3D11_MAPPED_SUBRESOURCE Mapped;
		if (SUCCEEDED(ImmediateContext->Map(StagingTexture.Get(), 0, D3D11_MAP_READ, 0, &Mapped)))
		{
			// FRHIComputeCommandList::UpdateTexture2D copies SourceData synchronously inside
			// the call, so Mapped.pData only needs to stay valid until both update calls return.
			const uint8* const SourceData = static_cast<const uint8*>(Mapped.pData);
			const FUpdateTextureRegion2D UpdateRegion(0, 0, 0, 0, LocalWidth, LocalHeight);

			if (DestTextureRHI.IsValid())
			{
				RHIUpdateTexture2D(
					DestTextureRHI,
					0,
					UpdateRegion,
					Mapped.RowPitch,
					SourceData
				);
			}

			if (DestRenderTargetRHI.IsValid())
			{
				RHIUpdateTexture2D(
					DestRenderTargetRHI,
					0,
					UpdateRegion,
					Mapped.RowPitch,
					SourceData
				);
			}

			ImmediateContext->Unmap(StagingTexture.Get(), 0);
		}
	}
}

bool FSpoutReceiver::Receive(const FName SpoutName, UMaterialInterface* InputMaterial, FName TextureParameterName, UMaterialInstanceDynamic*& OutMat, UTexture2D*& OutTexture, UTextureRenderTarget2D* OptionalOutputRenderTarget)
{
	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	Context.InitializeIfNeeded();

	if (!Context.GetSenderNames() || !Context.GetD3D11Device())
	{
		return false;
	}

	unsigned int W = 0;
	unsigned int H = 0;
	HANDLE SharedHandle = nullptr;
	unsigned long Format = 0;
	if (!QuerySenderInfo(SpoutName, W, H, SharedHandle, Format))
	{
		return false;
	}
	const EPixelFormat PixelFormat = DxgiFormatToPixelFormat(Format);
	const bool bSRGB = DxgiFormatPrefersSRGB(Format);

	TSharedPtr<FSpoutSharedSender> Receiver = FSpoutSenderRegistry::Get().FindOrAdd(SpoutName, ESpoutType::Receiver);
	if (!Receiver)
	{
		return false;
	}

	if (!UpdateSharedReceiverTexture(*Receiver, W, H, SharedHandle, Context))
	{
		return false;
	}

	// Create/resize the transient texture that UE will sample in materials.
	SpoutTextureUtils::EnsureTransientTexture(OutTexture, static_cast<int32>(W), static_cast<int32>(H), PixelFormat, bSRGB);

	const FName ParamName = TextureParameterName.IsNone() ? FName("SpoutTexture") : TextureParameterName;

	if (!OutMat && InputMaterial)
	{
		// Use the transient package as outer so the MID has a valid owning UObject graph
		// and is not at risk of being collected between creation and first sampling.
		OutMat = UMaterialInstanceDynamic::Create(InputMaterial, GetTransientPackage());
	}

	if (OutMat)
	{
		OutMat->SetTextureParameterValue(ParamName, OutTexture);
	}

	TRefCountPtr<FRHITexture> CapturedTextureRHI;
	if (OutTexture && OutTexture->GetResource())
	{
		// GetTexture2DRHI() returns the legacy FRHITexture2D interface which is unified
		// onto FRHITexture in UE5. GetTextureRHI() returns the current FTextureRHIRef.
		CapturedTextureRHI = OutTexture->GetResource()->GetTextureRHI();
	}

	TRefCountPtr<FRHITexture> CapturedRenderTargetRHI;
	if (OptionalOutputRenderTarget)
	{
		if (OptionalOutputRenderTarget->SizeX != static_cast<int32>(W) || OptionalOutputRenderTarget->SizeY != static_cast<int32>(H))
		{
			OptionalOutputRenderTarget->ResizeTarget(static_cast<int32>(W), static_cast<int32>(H));
		}

		if (FTextureRenderTargetResource* RenderTargetResource = OptionalOutputRenderTarget->GameThread_GetRenderTargetResource())
		{
			CapturedRenderTargetRHI = RenderTargetResource->GetRenderTargetTexture();
		}
	}

	// Read CVar once on the game thread so one frame uses a single coherent path.
	const bool bUseGPUPath = CVarSpoutGPUReceiver.GetValueOnAnyThread() != 0;

	ENQUEUE_RENDER_COMMAND(SpoutRecvCmd)(
		[Receiver, CapturedTextureRHI, CapturedRenderTargetRHI, bUseGPUPath](FRHICommandListImmediate& RHICmdList)
		{
			if (bUseGPUPath)
			{
				ReceiveOnRenderThread_GPU(Receiver, CapturedTextureRHI, CapturedRenderTargetRHI, RHICmdList);
			}
			else
			{
				ReceiveOnRenderThread_CPU(Receiver, CapturedTextureRHI, CapturedRenderTargetRHI);
			}
		});

	return true;
}

void FSpoutReceiver::Close(const FName SpoutName)
{
	// Registry is keyed by (Name, Type); scope removal to the Receiver entry only, leaving any
	// Sender entry with the same FName untouched. Dropping the entry releases the opened shared
	// DX11 texture, staging texture, and cached D3D11-on-12 wraps held by FSpoutSharedSender.
	if (FSpoutSenderRegistry::Get().Find(SpoutName, ESpoutType::Receiver))
	{
		FSpoutSenderRegistry::Get().Remove(SpoutName, ESpoutType::Receiver);
		UE_LOG(LogSpoutPlugin, Log, TEXT("Closed Spout Receiver: %s"), *SpoutName.ToString());
	}
}
