// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the Spout receiver flow.
 *
 * Two paths are supported, controlled by CVar r.Spout.GPUReceiver:
 *   - GPU path (default, CVar=1): opens the external sender's shared DX11 texture and
 *     copies it directly into the UE RHI destination via D3D11-on-12 wrapping.
 *     No CPU readback, no staging texture map.
 *   - CPU path (CVar=0): fallback — copies into a D3D11 STAGING texture, maps it,
 *     uploads via RHIUpdateTexture2D straight from the mapped pointer. Kept for
 *     debugging.
 */
#include "SpoutReceiver.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "SpoutLimits.h"
#include "Materials/MaterialInterface.h"
#include "SpoutD3DContext.h"
#include "SpoutD3DUtils.h"
#include "SpoutFormatPolicy.h"
#include "SpoutFrameSync.h"
#include "SpoutLogThrottle.h"
#include "SpoutReceiverOwnership.h"
#include "SpoutRHIState.h"
#include "SpoutSenderRegistry.h"
#include "SpoutSession.h"
#include "SpoutTextureUtils.h"
#include "SpoutModule.h"
#include "SpoutValidation.h"

#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "HAL/IConsoleManager.h"
#include "PixelFormat.h"
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
#if WITH_DEV_AUTOMATION_TESTS
	TAtomic<bool> GFailNextCpuReadback{false};
#endif
	void CopyReceivedFrame(FRHICommandListImmediate& Cmd, FRHITexture* Source, FRHITexture* Destination)
	{
		if (Source == Destination) return;
		Cmd.Transition(FRHITransitionInfo(Source, ERHIAccess::Unknown, ERHIAccess::CopySrc));
		Cmd.Transition(FRHITransitionInfo(Destination, ERHIAccess::Unknown, ERHIAccess::CopyDest));
		Cmd.CopyTexture(Source, Destination, FRHICopyTextureInfo());
		Cmd.Transition(FRHITransitionInfo(Source, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		Cmd.Transition(FRHITransitionInfo(Destination, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
	}
	static TAutoConsoleVariable<int32> CVarSpoutGPUReceiver(
		TEXT("r.Spout.GPUReceiver"),
		1,
		TEXT("Spout receiver path: 1 = GPU-direct via D3D11-on-12 (H2A, default), 0 = legacy CPU readback (H2B)."),
		ECVF_Default);

	bool QuerySenderInfo(const ANSICHAR* Utf8Name, unsigned int& OutWidth, unsigned int& OutHeight, HANDLE& OutHandle, unsigned long& OutFormat)
	{
		// Initialize every output before the call so a failed lookup cannot leave the caller
		// reading whatever the SDK did or did not write.
		OutWidth = 0;
		OutHeight = 0;
		OutHandle = nullptr;
		OutFormat = 0;

		if (Utf8Name == nullptr || Utf8Name[0] == '\0')
		{
			return false;
		}

		FSpoutD3DContext& Context = FSpoutD3DContext::Get();
		spoutSenderNames* SenderNames = Context.GetSenderNames();
		if (!SenderNames)
		{
			return false;
		}

		// Spout sender list is shared process-wide; guard with the mutex.
		FScopeLock SpoutLock(&Context.GetSpoutMutex());
		return SenderNames->GetSenderInfo(Utf8Name, OutWidth, OutHeight, OutHandle, OutFormat);
	}

	bool UpdateSharedReceiverTexture(FSpoutSharedSender& Receiver, unsigned int Width, unsigned int Height, HANDLE SharedHandle, DXGI_FORMAT Format, FSpoutD3DContext& Context)
	{
		check(IsInRenderingThread());
		FScopeLock ReceiverLock(&Receiver.ResourceMutex);
		if (!Receiver.CanRetry(Width, Height, Format, SharedHandle)) return false;
		bool bFrameSyncOpen = Receiver.FrameSync.IsOpen();
		if (!bFrameSyncOpen)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Spout_ReceiverFrameSyncOpen);
			bFrameSyncOpen = Receiver.FrameSync.Open(Receiver.GetUtf8Name());
		}
		if (!bFrameSyncOpen)
		{
			Receiver.NativeFailure(E_FAIL);
			return false;
		}
		if (Receiver.Width == Width && Receiver.Height == Height && Receiver.SharedHandle == SharedHandle
			&& Receiver.SharedFormat == Format && Receiver.SharedTexture)
		{
			return true;
		}

		int64 FrameBytes = 0;
		if (!SpoutLimits::FrameBytes(Width, Height, SpoutFormatPolicy::ResolveReceiverFormat(Format).PixelFormat, FrameBytes)) return false;
		// Shared source, staging/readback upload and completed-image overlap.
		const auto Reservation = FSpoutResourceReservation::TryReserve(FrameBytes * 4);
		if (!Reservation) return false;

		// Open the shared DX11 texture from the external sender's handle. OpenSharedResource is a
		// device-level (free-threaded) call; holding ResourceMutex across it is acceptable because
		// this only happens on connect/resize/handle-change, not per frame.
		ComPtr<ID3D11Texture2D> NewSharedTexture;
		HRESULT hr = E_FAIL;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Spout_OpenSharedResource);
			hr = Context.GetD3D11Device()->OpenSharedResource(
				SharedHandle,
				IID_PPV_ARGS(NewSharedTexture.GetAddressOf())
			);
		}

		if (FAILED(hr) || !NewSharedTexture)
		{
			Receiver.NativeFailure(FAILED(hr) ? hr : E_FAIL);
			// A peer may resize or exit between metadata lookup and opening the handle.
			if (Receiver.OpenLogThrottle.ShouldLog())
			{
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout receiver '%s': shared texture unavailable (hr=0x%08x); receive will retry."), *Receiver.Name.ToString(), hr);
			}
			return false;
		}

		D3D11_TEXTURE2D_DESC NativeDesc = {};
		NewSharedTexture->GetDesc(&NativeDesc);
		const auto NativeFormat = SpoutFormatPolicy::ResolveReceiverFormat(NativeDesc.Format);
		const auto AdvertisedFormat = SpoutFormatPolicy::ResolveReceiverFormat(Format);
		if (NativeDesc.Width != Width || NativeDesc.Height != Height || NativeDesc.MipLevels != 1
			|| NativeDesc.ArraySize != 1 || NativeDesc.SampleDesc.Count != 1 || NativeDesc.SampleDesc.Quality != 0
			|| !NativeFormat.IsUsable() || NativeFormat.PixelFormat != AdvertisedFormat.PixelFormat)
		{
			Receiver.NativeFailure(E_INVALIDARG);
			if (Receiver.FormatLogThrottle.ShouldLog())
			{
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout receiver '%s': opened texture does not match the advertised size/format or single-mip/sample layout; frame rejected."), *Receiver.Name.ToString());
			}
			return false;
		}
		Receiver.NativeSuccess();
		Receiver.SharedFormat = Format;

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
		Receiver.ReceivedFrame.SafeRelease();
		Receiver.DestinationDeliveries.Empty();
		Receiver.NativeReservation = Reservation;
		Receiver.FrameSync.RequestCurrentFrame();

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
				Receiver.NativeFailure(hr);
				if (Receiver.OpenLogThrottle.ShouldLog()) UE_LOG(LogSpoutPlugin, Warning, TEXT("Failed to create staging texture (hr=0x%08x)"), hr);
				return false;
			}

			Receiver.StagingTexture = MoveTemp(NewStaging);
		}

		OutStaging = Receiver.StagingTexture;
		return OutStaging != nullptr;
	}

	// Cache a D3D11-on-12 wrap of the primary UE RHI destination (UTexture backing).
	// Keyed on native D3D12 pointer so UE texture recreation invalidates the wrap.
	ComPtr<ID3D11Texture2D> GetOrCreateReceiverWrap(FSpoutSharedSender& Receiver, ID3D12Resource* NativeResource, SpoutRHIState::EWrapState WrapState)
	{
		FScopeLock ReceiverLock(&Receiver.ResourceMutex);
		if (Receiver.CachedNativeResource != NativeResource || !Receiver.CachedWrappedResource)
		{
			Receiver.CachedWrappedResource = WrapD3D12Resource(NativeResource, WrapState);
			Receiver.CachedNativeResource = NativeResource;
		}
		return Receiver.CachedWrappedResource;
	}

	// Cache a D3D11-on-12 wrap of the optional secondary UE RHI destination (render target).
	ComPtr<ID3D11Texture2D> GetOrCreateReceiverRTWrap(FSpoutSharedSender& Receiver, ID3D12Resource* NativeResource, SpoutRHIState::EWrapState WrapState)
	{
		FScopeLock ReceiverLock(&Receiver.ResourceMutex);
		if (Receiver.CachedRTNativeResource != NativeResource || !Receiver.CachedRTWrappedResource)
		{
			Receiver.CachedRTWrappedResource = WrapD3D12Resource(NativeResource, WrapState);
			Receiver.CachedRTNativeResource = NativeResource;
		}
		return Receiver.CachedRTWrappedResource;
	}

	// Format classification for received frames comes from SpoutFormatPolicy, which is shared
	// with the sender path. A local switch that falls back to PF_B8G8R8A8 must be avoided here:
	// an external sender advertising a compressed, planar, depth or 32-bit float layout would
	// have its bytes copied into a BGRA8 destination and reinterpreted.

	// GPU-direct path: wrap UE RHI destinations as D3D11 textures via D3D11-on-12 and
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

		// Ask before doing anything expensive. When the sender has published nothing since the
		// last frame this copied, the shared texture still holds that frame, so the transitions,
		// the RHI flush and the GPU copy are all skipped rather than repeated for no gain.
		//
		// The answer is latched inside FSpoutFrameSync rather than re-asked, because GetNewFrame()
		// decrements a semaphore: if the access wait below fails, the frame stays pending and the
		// next call copies it instead of losing it.
		//
		// Called outside the access mutex on purpose. The mutex protects the COPY, not the count:
		// a frame counted here and copied a moment later under the mutex is still a whole,
		// untorn frame -- at worst a newer one than the count referred to.
		if (!Receiver->FrameSync.ConsumeNewFrame())
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

		// Both destination brackets come from SpoutRHIState, which is also where the wrap
		// states below come from. The transition must precede the D3D11-on-12 Acquire, which
		// requires the resource to already be in the state declared at CreateWrappedResource time
		// -- that pairing is what makes the wrap's in/out states correct.
		//
		// The previous access is declared Unknown rather than SRVMask. Neither destination's prior
		// state is owned by this plugin: the caller-supplied render target may last have been an
		// RTV, a UAV or a copy destination, and the plugin's own received texture is created by
		// UTexture2D::UpdateResource, whose initial access is the RHI's choice rather than the plugin's.
		// Unknown resolves the barrier from the RHI's own whole-resource tracking, which is right
		// on the first frame after creation as well as on every frame after that.
		const SpoutRHIState::FCopyBracket TextureBracket =
			SpoutRHIState::GetCopyBracket(SpoutRHIState::EResourceRole::ReceiverTexture);
		const SpoutRHIState::FCopyBracket RenderTargetBracket =
			SpoutRHIState::GetCopyBracket(SpoutRHIState::EResourceRole::ReceiverRenderTarget);

		if (DestTextureRHI.IsValid() && DestNative)
		{
			TransitionSpoutInteropTexture(RHICmdList, DestTextureRHI, TextureBracket.EntryBefore, TextureBracket.CopyAccess);
		}
		if (DestRenderTargetRHI.IsValid() && DestRTNative)
		{
			TransitionSpoutInteropTexture(RHICmdList, DestRenderTargetRHI, RenderTargetBracket.EntryBefore, RenderTargetBracket.CopyAccess);
		}

		// Flush so D3D12 state transitions are submitted and visible to the D3D11-on-12 device
		// before Acquire/CopyResource runs. Mirrors the sender path.
		SubmitSpoutInteropTransitions(RHICmdList);

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
					Wrapped = GetOrCreateReceiverWrap(*Receiver, DestNative, TextureBracket.WrapState);
					if (Wrapped)
					{
						ToAcquire.Add(Wrapped.Get());
					}
				}
				if (DestRenderTargetRHI.IsValid() && DestRTNative)
				{
					WrappedRT = GetOrCreateReceiverRTWrap(*Receiver, DestRTNative, RenderTargetBracket.WrapState);
					if (WrappedRT)
					{
						ToAcquire.Add(WrappedRT.Get());
					}
				}

				if (ToAcquire.Num() > 0)
				{
					// Take the sender's cross-process access mutex around the read. A failed wait
					// means the sender is mid-write, and reading anyway would produce a torn
					// frame. The scope owns nothing on failure, so nothing is released, and the
					// frame stays pending for the next call.
					FSpoutTextureAccessScope Access(Receiver->FrameSync, SharedTexture.Get());
					if (!Access.IsAcquired())
					{
						if (Receiver->AccessLogThrottle.ShouldLog())
						{
							UE_LOG(LogSpoutPlugin, Warning,
								TEXT("Spout receiver '%s': timed out waiting for shared-texture access; this frame was not read. ")
								TEXT("It stays pending and is picked up on a later frame."),
								*Receiver->Name.ToString());
						}
					}
					else
					{
						// Acquire all destinations in their already-submitted COPY_DEST state.
						// Wait for the copies while shared access remains held; scope exit then
						// releases the wrappers and submits their outgoing transitions.
						FScopedD3D11On12Acquire Acquire(ToAcquire);
						if (Acquire.IsValid() && Context.PrepareGpuCompletion())
						{
							if (Wrapped)
							{
								TRACE_CPUPROFILER_EVENT_SCOPE(Spout_RecvCopyResource);
								ImmediateContext->CopyResource(Wrapped.Get(), SharedTexture.Get());
							}
							if (WrappedRT)
							{
								ImmediateContext->CopyResource(WrappedRT.Get(), SharedTexture.Get());
							}

							// The frame has been read; stop holding it pending. Inside the acquire
							// scope so a wrap failure leaves it pending instead.
							if (SUCCEEDED(Context.WaitForGpuCompletion()))
							{
								Receiver->FrameSync.MarkFrameConsumed();
								++Receiver->ReceivedFrameNumber;
							}
						}
					}
				}
			}
		}

		// Always restore, even if the access wait, the wrap or the Acquire failed, so UE's state
		// tracking stays consistent with the earlier Transition. The declared previous state is the
		// copy access, never Unknown: the transition above is what set it.
		if (DestTextureRHI.IsValid() && DestNative)
		{
			TransitionSpoutInteropTexture(RHICmdList, DestTextureRHI, TextureBracket.CopyAccess, TextureBracket.ExitAfter);
		}
		if (DestRenderTargetRHI.IsValid() && DestRTNative)
		{
			TransitionSpoutInteropTexture(RHICmdList, DestRenderTargetRHI, RenderTargetBracket.CopyAccess, RenderTargetBracket.ExitAfter);
		}
	}

	// CPU fallback path. Copy into a STAGING texture, Map, issue
	// RHIUpdateTexture2D straight from the mapped pointer (no intermediate TArray copy),
	// defer Unmap until after both updates return.
	void ReceiveOnRenderThread_CPU(
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
		ComPtr<ID3D11Texture2D> StagingTexture;
		uint32 LocalWidth = 0;
		uint32 LocalHeight = 0;

		// Same rule as the GPU path: no new frame means the staging copy, the map and both
		// texture updates are all skipped.
		if (!Receiver->FrameSync.ConsumeNewFrame())
		{
			return;
		}

		if (!GetSharedAndStaging(*Receiver, Context, SharedTexture, StagingTexture, LocalWidth, LocalHeight))
		{
			return;
		}

		// D3D11 immediate context use is serialized across sender/receiver paths.
		FScopeLock Lock(&Context.GetD3D11ContextMutex());
		ID3D11DeviceContext* ImmediateContext = Context.GetImmediateContext();
		if (!ImmediateContext || !SharedTexture || !StagingTexture || !Context.PrepareGpuCompletion())
		{
			return;
		}

		{
			// Hold access until the GPU has finished reading the shared texture into
			// staging. Mapping and UE upload happen afterwards using only local data.
			FSpoutTextureAccessScope Access(Receiver->FrameSync, SharedTexture.Get());
			if (!Access.IsAcquired())
			{
				if (Receiver->AccessLogThrottle.ShouldLog())
				{
					UE_LOG(LogSpoutPlugin, Warning,
						TEXT("Spout receiver '%s': timed out waiting for shared-texture access on the CPU path; this frame was not read."),
						*Receiver->Name.ToString());
				}
				return;
			}

			// A Flush alone submits asynchronously and cannot hand ownership back safely.
			ImmediateContext->CopyResource(StagingTexture.Get(), SharedTexture.Get());
			if (FAILED(Context.WaitForGpuCompletion()))
			{
				return;
			}
		}

		D3D11_MAPPED_SUBRESOURCE Mapped = {};
		HRESULT MapResult;
#if WITH_DEV_AUTOMATION_TESTS
		if (GFailNextCpuReadback.Exchange(false)) MapResult = E_FAIL;
		else
#endif
		MapResult = ImmediateContext->Map(StagingTexture.Get(), 0, D3D11_MAP_READ, 0, &Mapped);
		if (SUCCEEDED(MapResult))
		{
			Receiver->LastNativeError.store(0);
			// FRHIComputeCommandList::UpdateTexture2D copies SourceData synchronously inside
			// the call, so Mapped.pData only needs to stay valid until both update calls return.
			const uint8* const SourceData = static_cast<const uint8*>(Mapped.pData);
			const FUpdateTextureRegion2D UpdateRegion(0, 0, 0, 0, LocalWidth, LocalHeight);

			// Use the command-list form: the global RHIUpdateTexture2D with an implied
			// immediate command list is deprecated in UE 5.8 and removed after it.
			if (DestTextureRHI.IsValid())
			{
				RHICmdList.UpdateTexture2D(
					DestTextureRHI,
					0,
					UpdateRegion,
					Mapped.RowPitch,
					SourceData
				);
			}

			if (DestRenderTargetRHI.IsValid())
			{
				RHICmdList.UpdateTexture2D(
					DestRenderTargetRHI,
					0,
					UpdateRegion,
					Mapped.RowPitch,
					SourceData
				);
			}

			ImmediateContext->Unmap(StagingTexture.Get(), 0);
			Receiver->FrameSync.MarkFrameConsumed();
			++Receiver->ReceivedFrameNumber;
		}
		else
		{
			++Receiver->NativeFailures; Receiver->LastNativeError.store(static_cast<int64>(MapResult));
			if (Receiver->OpenLogThrottle.ShouldLog()) UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout receiver '%s': CPU readback failed (hr=0x%08x); the frame remains pending for retry."), *Receiver->Name.ToString(), MapResult);
		}
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void SpoutReceiverInternal::FailNextCpuReadbackForTests() { GFailNextCpuReadback.Store(true); }
#endif

UMaterialInstanceDynamic* SpoutReceiverInternal::ResolveConsumerMaterial(
	FSpoutReceiverOwnership& Ownership,
	FName ReceiverName,
	UMaterialInstanceDynamic* CallerMaterial,
	UMaterialInterface* InputMaterial,
	bool& bOutPluginCreated)
{
	// A caller that already holds a material keeps it. There is deliberately NO fall back to "some
	// material already tracked for this receiver name": with two consumers of one name it would hand
	// the second consumer the FIRST consumer's instance and silently ignore its InputMaterial.
	UMaterialInstanceDynamic* Material = CallerMaterial;

	// Asked of the tracker rather than inferred here, so a material created by an earlier call is
	// still recognised as plugin-created on every later call.
	bOutPluginCreated = Ownership.OwnsMaterial(ReceiverName, Material);

	if (!Material && InputMaterial)
	{
		// Use the transient package as outer so the MID has a valid owning UObject graph and is not
		// at risk of being collected between creation and first sampling.
		Material = UMaterialInstanceDynamic::Create(InputMaterial, GetTransientPackage());
		bOutPluginCreated = Material != nullptr;
	}

	return Material;
}

bool FSpoutReceiver::Receive(const FName SpoutName, UMaterialInterface* InputMaterial, FName TextureParameterName, UMaterialInstanceDynamic*& OutMat, UTexture2D*& OutTexture, UTextureRenderTarget2D* OptionalOutputRenderTarget, ESpoutColorSpace ColorSpace)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_Receive);
	// Validate the requested name before touching the interop context. An invalid name can never
	// resolve to a sender, so there is nothing to initialize for it.
	TArray<ANSICHAR> Utf8Name;
	const SpoutValidation::EResult NameResult = SpoutValidation::ValidateName(SpoutName, &Utf8Name);
	if (NameResult != SpoutValidation::EResult::Ok)
	{
		static FSpoutLogThrottle NameLogThrottle(5.0);
		if (NameLogThrottle.ShouldLog())
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("SpoutReceiver: rejected sender name '%s' (%s)."),
				*SpoutName.ToString(),
				SpoutValidation::ToString(NameResult));
		}
		return false;
	}

	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	if (!FSpoutSession::Get().IsRunning()) return false;
	Context.InitializeIfNeeded();

	if (!Context.GetSenderNames() || !Context.GetD3D11Device())
	{
		return false;
	}

	unsigned int W = 0;
	unsigned int H = 0;
	HANDLE SharedHandle = nullptr;
	unsigned long Format = 0;
	if (!FSpoutSession::Get().IsRunning() || !QuerySenderInfo(Utf8Name.GetData(), W, H, SharedHandle, Format))
	{
		return false;
	}

	// Everything above came from another process through shared memory. Validate the whole set
	// before opening a shared resource or allocating a destination texture for it: dimensions are
	// widened to int64 first so a 4-billion uint32 stays large rather than wrapping to a plausible
	// negative, and an unsupported or unknown format is refused instead of being reinterpreted.
	const SpoutValidation::EResult MetadataResult = SpoutValidation::ValidateExternalSenderMetadata(
		static_cast<int64>(W),
		static_cast<int64>(H),
		SharedHandle,
		static_cast<uint32>(Format));
	if (MetadataResult != SpoutValidation::EResult::Ok)
	{
		static FSpoutLogThrottle MetadataLogThrottle;
		if (MetadataLogThrottle.ShouldLog())
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("Spout receiver '%s': sender metadata rejected (%s). Reported %ux%u, format %s, handle %s. ")
				TEXT("Supported formats are RGBA8, BGRA8, RGB10A2 and RGBA16F (including their typeless variants)."),
				*SpoutName.ToString(),
				SpoutValidation::ToString(MetadataResult),
				W,
				H,
				*SpoutFormatPolicy::DescribeDxgiFormat(static_cast<uint32>(Format)),
				SharedHandle ? TEXT("present") : TEXT("null"));
		}
		return false;
	}

	const SpoutFormatPolicy::FReceiverFormat ReceiverFormat = SpoutFormatPolicy::ResolveReceiverFormat(static_cast<uint32>(Format));
	const EPixelFormat PixelFormat = ReceiverFormat.PixelFormat;
	int64 FrameBytes = 0;
	if (!SpoutLimits::FrameBytes(W, H, PixelFormat, FrameBytes)) return false;

	// One explicit colour-space contract, resolved here and nowhere else. Auto follows the
	// documented per-format convention; an explicit request is honoured where it is representable
	// and REFUSED where it is not, rather than being silently dropped. bSRGB is part of the
	// destination texture's identity in EnsureTransientTexture, so changing the setting recreates
	// the texture instead of reinterpreting the existing one.
	bool bSRGB = false;
	const SpoutFormatPolicy::EColorSpaceResult ColorSpaceResult =
		SpoutFormatPolicy::ResolveReceiverColorSpace(ReceiverFormat, ColorSpace, bSRGB);
	if (ColorSpaceResult != SpoutFormatPolicy::EColorSpaceResult::Ok)
	{
		static FSpoutLogThrottle ColorLogThrottle;
		if (ColorLogThrottle.ShouldLog())
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("Spout receiver '%s': colour space %s cannot be applied to %s (%s). ")
				TEXT("sRGB decoding only exists for the 8-bit formats; 10-bit and 16-bit float frames are linear. ")
				TEXT("Set Color Space to Auto or Linear."),
				*SpoutName.ToString(),
				SpoutFormatPolicy::ToString(ColorSpace),
				*SpoutFormatPolicy::DescribeDxgiFormat(static_cast<uint32>(Format)),
				SpoutFormatPolicy::ToString(ColorSpaceResult));
		}
		return false;
	}

	// Discovery failures above retain no stream/native records. Once metadata is valid,
	// the explicit close/session lifecycle owns this connected name.
	const FSpoutStreamToken Token = FSpoutSession::Get().OpenStream(SpoutName, ESpoutType::Receiver);
	if (!Token.IsSet()) return false;
	const auto QueuedWork = FSpoutSession::Get().TryQueue(Token);
	if (!QueuedWork) return false;
	TSharedPtr<FSpoutSharedSender> Receiver;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Spout_ReceiveFindOrAddEntry);
		Receiver = FSpoutSenderRegistry::Get().FindOrAdd(SpoutName, ESpoutType::Receiver);
	}
	if (!Receiver || !Receiver->HasValidName()) return false;

	// The destination is whatever this plugin owns for this receiver name, not whatever the caller
	// happened to pass in. Ownership decides which object the receive path writes into and which
	// object the close path may release; a caller variable is an output rather than the authority.
	FSpoutReceiverOwnership& Ownership = FSpoutReceiverOwnership::Get();

	const FName ParamName = TextureParameterName.IsNone() ? FName("SpoutTexture") : TextureParameterName;
	if ((OutMat || InputMaterial) && !Ownership.HasMaterialBinding(SpoutName, OutMat, ParamName)
		&& Ownership.NumMaterialBindings(SpoutName) >= SpoutLimits::MaxConsumers()) return false;
	for (auto It = Receiver->OutputReservations.CreateIterator(); It; ++It)
		if (!It.Key().IsValid()) It.RemoveCurrent();
	TSharedPtr<FSpoutResourceReservation, ESPMode::ThreadSafe> PreviousOutputReservation;
	if (OptionalOutputRenderTarget && OptionalOutputRenderTarget->GetFormat() == PixelFormat && !OptionalOutputRenderTarget->bAutoGenerateMips)
	{
		const TWeakObjectPtr<UObject> Key(OptionalOutputRenderTarget);
		auto* Existing = Receiver->OutputReservations.Find(Key);
		if (!Existing && Receiver->OutputReservations.Num() >= SpoutLimits::MaxConsumers()) return false;
		if (!Existing || (*Existing)->GetBytes() != FrameBytes * 2)
		{
			const auto Reservation = FSpoutResourceReservation::TryReserve(FrameBytes * 2);
			if (!Reservation) return false;
			if (Existing) PreviousOutputReservation = *Existing;
			Receiver->OutputReservations.Add(Key, Reservation);
		}
	}
	UTexture2D* ReceivedTexture = Ownership.FindTexture(SpoutName);
	// One shared texture has one interpretation. Do not churn it when consumers disagree.
	if (ReceivedTexture && ReceivedTexture->GetPixelFormat() == PixelFormat && (ReceivedTexture->SRGB != 0) != bSRGB)
	{
		if (Receiver->FormatLogThrottle.ShouldLog())
			UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout receiver '%s': conflicting color space for an open session; use the established color space or close the session first."), *SpoutName.ToString());
		return false;
	}
	bool bReplacedOutput = false;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Spout_ReceiveEnsureOutput);
		bReplacedOutput = SpoutTextureUtils::EnsureTransientTexture(ReceivedTexture, static_cast<int32>(W), static_cast<int32>(H), PixelFormat, bSRGB);
	}
	if (bReplacedOutput)
	{
		if (!ReceivedTexture)
		{
			// Creation failed. Report it rather than queueing a copy with no destination; the
			// documented contract for a true return is that the destination texture exists.
			if (Receiver->OpenLogThrottle.ShouldLog()) UE_LOG(LogSpoutPlugin, Warning,
				TEXT("Spout receiver '%s': could not create a %ux%u %s texture for the incoming frame."),
				*SpoutName.ToString(), W, H, GetPixelFormatString(PixelFormat));
			return false;
		}

		// Adopting the replacement is also what drops the previous one: it loses this reference and
		// is collected once the caller's copy and any in-flight render command are gone. Nothing is
		// unrooted, released or garbage-marked by hand.
		Ownership.AdoptTexture(SpoutName, ReceivedTexture);
	}


	// The material is per CONSUMER, unlike the texture, which is per session. The rule lives in
	// ResolveConsumerMaterial.
	bool bPluginCreatedMaterial = false;
	UMaterialInstanceDynamic* Material = nullptr;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Spout_ReceiveResolveMaterial);
		Material = SpoutReceiverInternal::ResolveConsumerMaterial(
			Ownership, SpoutName, OutMat, InputMaterial, bPluginCreatedMaterial);
	}

	if (Material)
	{
		UTexture* PreviousTexture = Material->K2_GetTextureParameterValue(ParamName);
		Material->SetTextureParameterValue(ParamName, ReceivedTexture);
		// Registered even when the caller created it, so close can detach the parameter this plugin
		// set. Ownership of a caller's material is not claimed, and registering one never displaces
		// a plugin-created binding: the session holds a collection, not a single slot. De-duplicated
		// on (material, parameter), so calling this every tick does not grow it.
		Ownership.RegisterMaterialBinding(SpoutName, Material, ParamName, bPluginCreatedMaterial, PreviousTexture);
	}

	OutTexture = ReceivedTexture;
	OutMat = Material;

	// Capture only resource pointers on the game thread, never their render-owned RHI
	// fields. UE enqueues resource release/deletion after this command, including an
	// UpdateResource immediately after Receive; GC also waits for its release fence.
	FTextureResource* TextureResource = ReceivedTexture->GetResource();
	FTextureRenderTargetResource* OutputResource = nullptr;
	if (OptionalOutputRenderTarget)
	{
		// Refuse an incompatible authored target BEFORE resize can clear its contents.
		if (OptionalOutputRenderTarget->GetFormat() == PixelFormat && !OptionalOutputRenderTarget->bAutoGenerateMips)
		{
			if (OptionalOutputRenderTarget->SizeX != static_cast<int32>(W) || OptionalOutputRenderTarget->SizeY != static_cast<int32>(H))
			{
				OptionalOutputRenderTarget->ResizeTarget(static_cast<int32>(W), static_cast<int32>(H));
			}
			OutputResource = OptionalOutputRenderTarget->GameThread_GetRenderTargetResource();
		}
		else if (Receiver->OutputTargetLogThrottle.ShouldLog())
		{
			UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout receiver '%s': incompatible output render target left untouched; format must match and Auto Generate Mips must be disabled."), *SpoutName.ToString());
		}
	}
	const bool bUseGPUPath = CVarSpoutGPUReceiver.GetValueOnAnyThread() != 0;
	const TWeakObjectPtr<UObject> OutputIdentity(OptionalOutputRenderTarget);
	FSpoutSession::Get().MarkQueued(Token);
	ENQUEUE_RENDER_COMMAND(SpoutRecvCmd)(
		[Token, Receiver, TextureResource, OutputResource, OutputIdentity, W, H, SharedHandle, Format, PixelFormat, bUseGPUPath, QueuedWork, PreviousOutputReservation](FRHICommandListImmediate& RHICmdList)
		{
			if (!FSpoutSession::Get().IsCurrent(Token))
			{
				return;
			}
			// Native source replacement and destination resolution share ONE render
			// command. Later game-thread receives cannot replace half of this pair.
			FSpoutD3DContext& RenderContext = FSpoutD3DContext::Get();
			if (!UpdateSharedReceiverTexture(*Receiver, W, H, SharedHandle, static_cast<DXGI_FORMAT>(Format), RenderContext))
			{
				return;
			}
			TRefCountPtr<FRHITexture> TextureRHI = TextureResource ? TextureResource->GetTextureRHI() : nullptr;
			TRefCountPtr<FRHITexture> TargetRHI = OutputResource ? OutputResource->GetRenderTargetTexture() : nullptr;
			auto IsCompatible = [W, H, PixelFormat](const TRefCountPtr<FRHITexture>& Texture)
			{
				if (!Texture) return false;
				const FRHITextureDesc& Desc = Texture->GetDesc();
				return Desc.Extent.X == static_cast<int32>(W) && Desc.Extent.Y == static_cast<int32>(H)
					&& Desc.Format == PixelFormat && Desc.NumMips == 1 && Desc.NumSamples == 1
					&& Desc.ArraySize == 1 && Desc.Dimension == ETextureDimension::Texture2D;
			};
			if (!IsCompatible(TextureRHI)) TextureRHI.SafeRelease();
			if (!IsCompatible(TargetRHI)) TargetRHI.SafeRelease();
			if (!TextureRHI) return;
			// Restore a recreated local resource even if the upstream sender is paused.
			if (Receiver->ReceivedFrame && Receiver->ReceivedFrame != TextureRHI)
			{
				if (IsCompatible(Receiver->ReceivedFrame))
				{
					CopyReceivedFrame(RHICmdList, Receiver->ReceivedFrame, TextureRHI);
					Receiver->ReceivedFrame = TextureRHI;
				}
				else Receiver->ReceivedFrame.SafeRelease();
			}
			const uint64 PreviousFrame = Receiver->ReceivedFrameNumber;
			if (bUseGPUPath)
			{
				ReceiveOnRenderThread_GPU(Receiver, TextureRHI, nullptr, RHICmdList);
			}
			else
			{
				ReceiveOnRenderThread_CPU(Receiver, TextureRHI, nullptr, RHICmdList);
			}
			if (PreviousFrame != Receiver->ReceivedFrameNumber) Receiver->ReceivedFrame = TextureRHI;
			for (auto It = Receiver->DestinationDeliveries.CreateIterator(); It; ++It)
			{
				if (!It.Key().IsValid(false, true)) It.RemoveCurrent();
			}
			if (TargetRHI && Receiver->ReceivedFrame)
			{
				auto& Delivery = Receiver->DestinationDeliveries.FindOrAdd(OutputIdentity);
				if (Delivery.Frame != Receiver->ReceivedFrameNumber || Delivery.Resource != TargetRHI)
				{
					CopyReceivedFrame(RHICmdList, Receiver->ReceivedFrame, TargetRHI);
					Delivery.Resource = TargetRHI;
					Delivery.Frame = Receiver->ReceivedFrameNumber;
				}
			}
		});

	return true;
}

void FSpoutReceiver::Close(const FName SpoutName)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_CloseReceiver);
	// Cancel before releasing anything, so a receive queued earlier in this frame cannot copy into
	// the texture after the caller has been told the receiver is closed. Scoped to the Receiver
	// role: a Sender with the same FName keeps running.
	FSpoutSession::Get().CancelStream(SpoutName, ESpoutType::Receiver);

	// Registry is keyed by (Name, Type); scope removal to the Receiver entry only, leaving any
	// Sender entry with the same FName untouched. Dropping the entry releases the opened shared
	// DX11 texture, staging texture, and cached D3D11-on-12 wraps held by FSpoutSharedSender.
	if (FSpoutSenderRegistry::Get().Find(SpoutName, ESpoutType::Receiver))
	{
		FSpoutSenderRegistry::Get().Remove(SpoutName, ESpoutType::Receiver);
		UE_LOG(LogSpoutPlugin, Log, TEXT("Closed Spout Receiver: %s"), *SpoutName.ToString());
	}
}
