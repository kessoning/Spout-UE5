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

		// Open this sender's access mutex and frame-count semaphore now that its handle
		// has been opened. Idempotent and keyed to the NAME, so a resize -- which lands here with a
		// new handle and new dimensions -- leaves the protocol state alone, while a close/reopen
		// gets a fresh instance whose first frame is correctly reported as new.
		if (!Receiver.FrameSync.IsOpen() && !Receiver.FrameSync.Open(Receiver.GetUtf8Name()))
		{
			// Not fatal: without the mutex the copy is simply unsynchronised, so frames still
			// flow.
			if (Receiver.AccessLogThrottle.ShouldLog())
			{
				UE_LOG(LogSpoutPlugin, Warning,
					TEXT("Spout receiver '%s': could not open the shared-texture access mutex; frames are read without cross-process locking."),
					*Receiver.Name.ToString());
			}
		}

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
			RHICmdList.Transition(FRHITransitionInfo(DestTextureRHI, TextureBracket.EntryBefore, TextureBracket.CopyAccess));
		}
		if (DestRenderTargetRHI.IsValid() && DestRTNative)
		{
			RHICmdList.Transition(FRHITransitionInfo(DestRenderTargetRHI, RenderTargetBracket.EntryBefore, RenderTargetBracket.CopyAccess));
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
						// Acquire every destination together. Acquire transitions the wrapped
						// resources into the state declared at wrap (COPY_DEST); the single
						// scope-exit Release + Flush covers all copies, so two destinations cost one
						// GPU submit, not two.
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

							// The frame has been read; stop holding it pending. Inside the acquire
							// scope so a wrap failure leaves it pending instead.
							Receiver->FrameSync.MarkFrameConsumed();
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
			RHICmdList.Transition(FRHITransitionInfo(DestTextureRHI, TextureBracket.CopyAccess, TextureBracket.ExitAfter));
		}
		if (DestRenderTargetRHI.IsValid() && DestRTNative)
		{
			RHICmdList.Transition(FRHITransitionInfo(DestRenderTargetRHI, RenderTargetBracket.CopyAccess, RenderTargetBracket.ExitAfter));
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
		if (!ImmediateContext || !SharedTexture || !StagingTexture)
		{
			return;
		}

		{
			// The access mutex covers the copy out of the shared texture and nothing else.
			//
			// Map(D3D11_MAP_READ) on a staging texture blocks until the GPU has finished writing
			// it, so holding the cross-process mutex across the readback would stall the sender
			// for the whole of it. The scope therefore ends before the map, exactly as the Spout
			// SDK's own receiver does.
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

			// Copy into a CPU-readable staging texture, and submit it before giving the shared
			// texture back to the sender.
			ImmediateContext->CopyResource(StagingTexture.Get(), SharedTexture.Get());
			ImmediateContext->Flush();
		}

		Receiver->FrameSync.MarkFrameConsumed();

		D3D11_MAPPED_SUBRESOURCE Mapped;
		if (SUCCEEDED(ImmediateContext->Map(StagingTexture.Get(), 0, D3D11_MAP_READ, 0, &Mapped)))
		{
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
		}
	}
}

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
	// Validate the requested name before touching the interop context. An invalid name can never
	// resolve to a sender, so there is nothing to initialize for it.
	const SpoutValidation::EResult NameResult = SpoutValidation::ValidateName(SpoutName);
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
	Context.InitializeIfNeeded();

	if (!Context.GetSenderNames() || !Context.GetD3D11Device())
	{
		return false;
	}

	// Refused while the session is stopping, so a Blueprint tick racing a PIE stop cannot create a
	// registry entry or a texture that the teardown running alongside it has already walked past.
	const FSpoutStreamToken Token = FSpoutSession::Get().OpenStream(SpoutName, ESpoutType::Receiver);
	if (!Token.IsSet())
	{
		return false;
	}

	TSharedPtr<FSpoutSharedSender> Receiver = FSpoutSenderRegistry::Get().FindOrAdd(SpoutName, ESpoutType::Receiver);
	if (!Receiver || !Receiver->HasValidName())
	{
		return false;
	}

	unsigned int W = 0;
	unsigned int H = 0;
	HANDLE SharedHandle = nullptr;
	unsigned long Format = 0;
	if (!QuerySenderInfo(Receiver->GetUtf8Name(), W, H, SharedHandle, Format))
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
		if (Receiver->FormatLogThrottle.ShouldLog())
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
		if (Receiver->FormatLogThrottle.ShouldLog())
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

	if (!UpdateSharedReceiverTexture(*Receiver, W, H, SharedHandle, Context))
	{
		return false;
	}

	// The destination is whatever this plugin owns for this receiver name, not whatever the caller
	// happened to pass in. Ownership decides which object the receive path writes into and which
	// object the close path may release; a caller variable is an output rather than the authority.
	FSpoutReceiverOwnership& Ownership = FSpoutReceiverOwnership::Get();

	UTexture2D* ReceivedTexture = Ownership.FindTexture(SpoutName);
	if (SpoutTextureUtils::EnsureTransientTexture(ReceivedTexture, static_cast<int32>(W), static_cast<int32>(H), PixelFormat, bSRGB))
	{
		if (!ReceivedTexture)
		{
			// Creation failed. Report it rather than queueing a copy with no destination; the
			// documented contract for a true return is that the destination texture exists.
			UE_LOG(LogSpoutPlugin, Error,
				TEXT("Spout receiver '%s': could not create a %ux%u %s texture for the incoming frame."),
				*SpoutName.ToString(), W, H, GetPixelFormatString(PixelFormat));
			return false;
		}

		// Adopting the replacement is also what drops the previous one: it loses this reference and
		// is collected once the caller's copy and any in-flight render command are gone. Nothing is
		// unrooted, released or garbage-marked by hand.
		Ownership.AdoptTexture(SpoutName, ReceivedTexture);
	}

	const FName ParamName = TextureParameterName.IsNone() ? FName("SpoutTexture") : TextureParameterName;

	// The material is per CONSUMER, unlike the texture, which is per session. The rule lives in
	// ResolveConsumerMaterial.
	bool bPluginCreatedMaterial = false;
	UMaterialInstanceDynamic* Material = SpoutReceiverInternal::ResolveConsumerMaterial(
		Ownership, SpoutName, OutMat, InputMaterial, bPluginCreatedMaterial);

	if (Material)
	{
		Material->SetTextureParameterValue(ParamName, ReceivedTexture);
		// Registered even when the caller created it, so close can detach the parameter this plugin
		// set. Ownership of a caller's material is not claimed, and registering one never displaces
		// a plugin-created binding: the session holds a collection, not a single slot. De-duplicated
		// on (material, parameter), so calling this every tick does not grow it.
		Ownership.RegisterMaterialBinding(SpoutName, Material, ParamName, bPluginCreatedMaterial);
	}

	OutTexture = ReceivedTexture;
	OutMat = Material;

	TRefCountPtr<FRHITexture> CapturedTextureRHI;
	if (ReceivedTexture->GetResource())
	{
		// GetTexture2DRHI() returns the legacy FRHITexture2D interface which is unified
		// onto FRHITexture in UE5. GetTextureRHI() returns the current FTextureRHIRef.
		CapturedTextureRHI = ReceivedTexture->GetResource()->GetTextureRHI();
	}

	TRefCountPtr<FRHITexture> CapturedRenderTargetRHI;
	if (OptionalOutputRenderTarget)
	{
		// A size mismatch is recoverable: resizing a render target to the incoming frame is the
		// documented behaviour of this node. A format or sample-count mismatch is not — silently
		// reformatting someone else's asset at runtime would be worse than the symptom.
		if (OptionalOutputRenderTarget->SizeX != static_cast<int32>(W) || OptionalOutputRenderTarget->SizeY != static_cast<int32>(H))
		{
			OptionalOutputRenderTarget->ResizeTarget(static_cast<int32>(W), static_cast<int32>(H));
		}

		// Compare against the resolved RHI resource rather than the UObject's authored properties:
		// the RHI descriptor is what CopyResource and UpdateTexture2D actually operate on, and it
		// carries the sample count, which UTextureRenderTarget2D does not expose directly. A resize
		// requested above may not be reflected until the next frame, in which case this frame is
		// skipped and the following one matches.
		TRefCountPtr<FRHITexture> CandidateRHI;
		if (FTextureRenderTargetResource* RenderTargetResource = OptionalOutputRenderTarget->GameThread_GetRenderTargetResource())
		{
			CandidateRHI = RenderTargetResource->GetRenderTargetTexture();
		}

		if (CandidateRHI.IsValid())
		{
			const FRHITextureDesc& OutputDesc = CandidateRHI->GetDesc();
			const SpoutFormatPolicy::EOutputTargetStatus OutputStatus = SpoutFormatPolicy::CheckOutputTarget(
				static_cast<int32>(W),
				static_cast<int32>(H),
				PixelFormat,
				OutputDesc.Extent.X,
				OutputDesc.Extent.Y,
				OutputDesc.Format,
				OutputDesc.NumSamples);

			if (OutputStatus == SpoutFormatPolicy::EOutputTargetStatus::Compatible)
			{
				CapturedRenderTargetRHI = MoveTemp(CandidateRHI);
				// Arm the limiter again so a later mismatch is reported immediately rather than
				// waiting out an interval that started before the target became compatible.
				Receiver->OutputTargetLogThrottle.Reset();
			}
			else if (Receiver->LastOutputTargetStatus != OutputStatus || Receiver->OutputTargetLogThrottle.ShouldLog())
			{
				// The mismatched target is deliberately NOT captured, so it is excluded from the
				// copy and left exactly as the caller had it. Capturing it anyway would hand it
				// to CopyResource (which D3D drops between differing descriptors) or to
				// UpdateTexture2D (which writes bytes the destination reinterprets), producing a
				// black or garbled target rather than an untouched one. The plugin-owned received
				// texture below is unaffected and keeps updating.
				UE_LOG(LogSpoutPlugin, Warning,
					TEXT("Spout receiver '%s': optional output render target is incompatible (%s) and was left untouched. ")
					TEXT("Frame is %dx%d %s single-sampled; target is %dx%d %s with %d sample(s). ")
					TEXT("The received texture output still updates. Recreate the render target to match the sender; ")
					TEXT("RTF RGBA8 sRGB matches the most common senders."),
					*SpoutName.ToString(),
					SpoutFormatPolicy::ToString(OutputStatus),
					static_cast<int32>(W),
					static_cast<int32>(H),
					GetPixelFormatString(PixelFormat),
					OutputDesc.Extent.X,
					OutputDesc.Extent.Y,
					GetPixelFormatString(OutputDesc.Format),
					OutputDesc.NumSamples);
			}

			Receiver->LastOutputTargetStatus = OutputStatus;
		}
	}

	// Read CVar once on the game thread so one frame uses a single coherent path.
	const bool bUseGPUPath = CVarSpoutGPUReceiver.GetValueOnAnyThread() != 0;

	ENQUEUE_RENDER_COMMAND(SpoutRecvCmd)(
		[Token, Receiver, CapturedTextureRHI, CapturedRenderTargetRHI, bUseGPUPath](FRHICommandListImmediate& RHICmdList)
		{
			// A receiver closed, or a session torn down, after this was queued makes the copy a
			// no-op. The captured RHI references keep the destinations alive either way, so this is
			// about not writing into a texture the caller has already been told is finished with.
			if (!FSpoutSession::Get().IsCurrent(Token))
			{
				return;
			}

			if (bUseGPUPath)
			{
				ReceiveOnRenderThread_GPU(Receiver, CapturedTextureRHI, CapturedRenderTargetRHI, RHICmdList);
			}
			else
			{
				ReceiveOnRenderThread_CPU(Receiver, CapturedTextureRHI, CapturedRenderTargetRHI, RHICmdList);
			}
		});

	return true;
}

void FSpoutReceiver::Close(const FName SpoutName)
{
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
