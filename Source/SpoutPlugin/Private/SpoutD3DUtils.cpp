/**
 * Implements D3D11-on-12 wrapping and scoped acquire/release helpers used by the Spout
 * sender path to copy from UE D3D12 textures into shared DX11 resources.
 */
#include "SpoutD3DUtils.h"
#include "SpoutD3DContext.h"

using Microsoft::WRL::ComPtr;

ComPtr<ID3D11Texture2D> WrapD3D12Resource(ID3D12Resource* Resource, D3D12_RESOURCE_STATES State)
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
	ComPtr<ID3D11Resource> WrappedResource;
	HRESULT hr = D3D11On12->CreateWrappedResource(
		Resource,
		&Flags,
		State,
		State,
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
