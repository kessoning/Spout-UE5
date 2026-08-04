// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements D3D11-on-12 wrapping and scoped acquire/release helpers used by the Spout
 * sender path to copy from UE D3D12 textures into shared DX11 resources.
 */
#include "SpoutD3DUtils.h"
#include "SpoutD3DContext.h"

using Microsoft::WRL::ComPtr;

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

	D3D12_RESOURCE_DESC Desc = Resource->GetDesc();

	D3D11_RESOURCE_FLAGS Flags = {};
	Flags.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
	{
		Flags.BindFlags |= D3D11_BIND_RENDER_TARGET;
	}

	// Wrap the resource so the D3D11 immediate context can copy into/out of it.
	//
	// InState and OutState are deliberately the same value. AcquireWrappedResources transitions the
	// resource INTO InState and requires it to already be there; ReleaseWrappedResources transitions
	// it back to OutState. The caller brackets the wrap with a UE transition into the matching
	// ERHIAccess and back out of it again, so making the two states identical is what lets UE's own
	// state tracking stay true across the acquire/release pair. Both come from
	// SpoutRHIState::GetCopyBracket, so they cannot disagree.
	const D3D12_RESOURCE_STATES NativeState = ToD3D12ResourceState(State);
	ComPtr<ID3D11Resource> WrappedResource;
	HRESULT hr = D3D11On12->CreateWrappedResource(
		Resource,
		&Flags,
		NativeState,
		NativeState,
		IID_PPV_ARGS(WrappedResource.GetAddressOf())
	);

	if (FAILED(hr))
	{
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
