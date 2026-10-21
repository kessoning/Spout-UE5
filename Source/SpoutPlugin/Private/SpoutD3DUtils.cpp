// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements D3D11-on-12 wrapping and scoped acquire/release helpers used by the Spout
 * sender path to copy from UE D3D12 textures into shared DX11 resources.
 */
#include "SpoutD3DUtils.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "SpoutD3DContext.h"
#include "SpoutModule.h"
#include "SpoutSharedSlot.h"
#include "ID3D12DynamicRHI.h"
#include "RHICommandList.h"
#include "Runtime/Launch/Resources/Version.h"

using Microsoft::WRL::ComPtr;

void TransitionSpoutInteropTexture(FRHICommandListImmediate& RHICmdList, FRHITexture* Texture, ERHIAccess Before, ERHIAccess After)
{
	RHICmdList.Transition(FRHITransitionInfo(Texture, Before, After));
#if ENGINE_MINOR_VERSION < 6
	// 5.3-5.5 HandleResourceTransitions ignores ordinary graphics-only transitions:
	// native barriers are normally inferred by the next RHI draw/copy. Here the next
	// copy is external, so use its public D3D12 interface to force that barrier.
	D3D12_RESOURCE_STATES NativeState;
	if (After == ERHIAccess::CopySrc) NativeState = D3D12_RESOURCE_STATE_COPY_SOURCE;
	else if (After == ERHIAccess::CopyDest) NativeState = D3D12_RESOURCE_STATE_COPY_DEST;
	else
	{
		check(After == ERHIAccess::SRVGraphics || After == ERHIAccess::SRVMask);
		NativeState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	}
	TRefCountPtr<FRHITexture> KeepAlive(Texture);
	RHICmdList.EnqueueLambda([KeepAlive, NativeState](FRHICommandListImmediate& Cmd)
	{
		GetID3D12DynamicRHI()->RHITransitionResource(Cmd, KeepAlive, NativeState, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	});
#endif
}

void SubmitSpoutInteropTransitions(FRHICommandListImmediate& RHICmdList)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SubmitInteropTransitions);
	RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
#if ENGINE_MINOR_VERSION >= 4
	// FlushRHIThread only drains translation. The D3D12 submission thread may
	// still hold those payloads while a raw D3D11On12 Flush overtakes them.
	GetID3D12DynamicRHI()->RHIRunOnQueue(ED3D12RHIRunOnQueueType::Graphics, [](ID3D12CommandQueue*) {}, true);
#else
	// 5.3 exposes no public submission-only fence. Its idle wait also drains
	// the submission queue; correctness costs a GPU synchronization here.
	RHICmdList.BlockUntilGPUIdle();
#endif
}

namespace
{
	/** The single translation from the plugin's state contract to D3D12's own enumeration. */
	D3D12_RESOURCE_STATES ToD3D12ResourceState(SpoutRHIState::EWrapState State)
	{
		return State == SpoutRHIState::EWrapState::CopySource
			? D3D12_RESOURCE_STATE_COPY_SOURCE
			: D3D12_RESOURCE_STATE_COPY_DEST;
	}
}

ComPtr<ID3D11Texture2D> WrapD3D12Resource(ID3D12Resource* Resource, SpoutRHIState::EWrapState State)
{
	if (!Resource)
	{
		return nullptr;
	}

	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	ID3D11On12Device* D3D11On12 = Context.GetD3D11On12Device();
	if (!D3D11On12)
	{
		return nullptr;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_WrapD3D12Resource);
	D3D12_RESOURCE_DESC Desc = Resource->GetDesc();

	D3D11_RESOURCE_FLAGS Flags = {};
	Flags.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
	{
		Flags.BindFlags |= D3D11_BIND_RENDER_TARGET;
	}

	// Wrap the resource so the D3D11 immediate context can copy into/out of it.
	//
	// InState and OutState are deliberately the same value. The caller submits the
	// transition into InState before AcquireWrappedResources; ReleaseWrappedResources
	// leaves it in OutState. The caller brackets the wrap with a UE transition into the matching
	// ERHIAccess and back out of it again, so making the two states identical is what lets UE's own
	// state tracking stay true across the acquire/release pair. Both come from
	// SpoutRHIState::GetCopyBracket, so they cannot disagree.
	const D3D12_RESOURCE_STATES NativeState = ToD3D12ResourceState(State);
	ComPtr<ID3D11Resource> WrappedResource;
	// Counted so that the shutdown log can show that the shared-slot path never wraps.
	SpoutInterop::CountWrapCall();
	HRESULT hr = D3D11On12->CreateWrappedResource(
		Resource,
		&Flags,
		NativeState,
		NativeState,
		IID_PPV_ARGS(WrappedResource.GetAddressOf())
	);

	if (FAILED(hr))
	{
		static FSpoutLogThrottle FailureThrottle;
		if (FailureThrottle.ShouldLog())
		{
			UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout could not wrap a %llux%u texture for interop (hr=0x%08x); copy deferred."), Desc.Width, Desc.Height, hr);
		}
		return nullptr;
	}

	ComPtr<ID3D11Texture2D> Result;
	WrappedResource.As(&Result);
	return Result;
}

ComPtr<ID3D11Texture2D> FSpoutWrapCache::GetOrCreate(ID3D12Resource* Resource, SpoutRHIState::EWrapState State)
{
	if (!Resource)
	{
		return nullptr;
	}

	if (TRefCountPtr<ID3D11Texture2D>* Existing = Entries.Find(Resource))
	{
		return ComPtr<ID3D11Texture2D>(Existing->GetReference());
	}

	// Drop everything rather than evicting a single entry: the cache is sized for a small, stable
	// working set (the swap chain's back buffers). Growing past it means that set changed, so the
	// remaining wraps are stale and keeping them only pins dead resources.
	if (Entries.Num() >= MaxEntries)
	{
		Entries.Reset();
	}

	ComPtr<ID3D11Texture2D> Wrapped = WrapD3D12Resource(Resource, State);
	if (Wrapped)
	{
		Entries.Add(Resource, TRefCountPtr<ID3D11Texture2D>(Wrapped.Get()));
	}

	return Wrapped;
}

void FSpoutWrapCache::Reset()
{
	Entries.Reset();
}

FScopedD3D11On12Acquire::FScopedD3D11On12Acquire(ID3D11Texture2D* InTexture)
{
	ID3D11Texture2D* Textures[1] = { InTexture };
	AcquireResources(MakeArrayView(Textures, 1));
}

FScopedD3D11On12Acquire::FScopedD3D11On12Acquire(TConstArrayView<ID3D11Texture2D*> InTextures)
{
	AcquireResources(InTextures);
}

void FScopedD3D11On12Acquire::AcquireResources(TConstArrayView<ID3D11Texture2D*> InTextures)
{
	// Query each texture for the ID3D11Resource interface required by the Acquire/Release APIs,
	// keeping only the ones that resolve. Null/failed entries are skipped, not fatal.
	TArray<ID3D11Resource*, TInlineAllocator<2>> RawResources;
	for (ID3D11Texture2D* Texture : InTextures)
	{
		if (!Texture)
		{
			continue;
		}

		ComPtr<ID3D11Resource> Resource;
		Texture->QueryInterface(IID_PPV_ARGS(Resource.GetAddressOf()));
		if (Resource)
		{
			RawResources.Add(Resource.Get());
			WrappedResources.Add(TRefCountPtr<ID3D11Resource>(Resource.Get()));
		}
	}

	if (RawResources.Num() == 0)
	{
		return;
	}

	ID3D11On12Device* D3D11On12 = FSpoutD3DContext::Get().GetD3D11On12Device();
	if (D3D11On12)
	{
		// Acquire ensures correct resource state for D3D11 use on the D3D12 device.
		TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AcquireWrappedResources);
		D3D11On12->AcquireWrappedResources(RawResources.GetData(), RawResources.Num());
	}
	else
	{
		// Nothing acquired; drop the refs so IsValid() reports false and the destructor is a no-op.
		WrappedResources.Reset();
	}
}

FScopedD3D11On12Acquire::~FScopedD3D11On12Acquire()
{
	if (WrappedResources.Num() == 0)
	{
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_ReleaseWrappedResourcesFlush);
	FSpoutD3DContext& Context = FSpoutD3DContext::Get();

	ID3D11On12Device* D3D11On12 = Context.GetD3D11On12Device();
	if (D3D11On12)
	{
		// Release transitions the wrapped resources back to their original D3D12 state.
		TArray<ID3D11Resource*, TInlineAllocator<2>> RawResources;
		RawResources.Reserve(WrappedResources.Num());
		for (const TRefCountPtr<ID3D11Resource>& Resource : WrappedResources)
		{
			RawResources.Add(Resource.GetReference());
		}
		D3D11On12->ReleaseWrappedResources(RawResources.GetData(), RawResources.Num());
	}

	ID3D11DeviceContext* ImmediateContext = Context.GetImmediateContext();
	if (ImmediateContext)
	{
		// Single flush for all released resources so D3D11 copies are visible to the D3D12 queue.
		ImmediateContext->Flush();
	}
}

bool FScopedD3D11On12Acquire::IsValid() const
{
	return WrappedResources.Num() > 0;
}
