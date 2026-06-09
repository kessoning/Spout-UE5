/**
 * Declares D3D helper utilities for wrapping D3D12 resources and managing D3D11-on-12
 * acquire/release scopes used by sender/receiver interop.
 */
#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

class FScopedD3D11On12Acquire
{
public:
	// Acquires a single wrapped D3D11 resource for access through the D3D11-on-12 device.
	explicit FScopedD3D11On12Acquire(ID3D11Texture2D* InTexture);
	// Acquires several wrapped resources in one AcquireWrappedResources call so the single
	// scope-exit Release + Flush covers every copy (one GPU submit instead of one per resource).
	explicit FScopedD3D11On12Acquire(TConstArrayView<ID3D11Texture2D*> InTextures);
	// Releases the wrapped resources and flushes the D3D11 context once to submit pending work.
	~FScopedD3D11On12Acquire();

	// True if at least one resource was successfully acquired.
	bool IsValid() const;

private:
	void AcquireResources(TConstArrayView<ID3D11Texture2D*> InTextures);

	// Kept to release the wrapped resources on scope exit (inline storage for the common 1-2 case).
	TArray<Microsoft::WRL::ComPtr<ID3D11Resource>, TInlineAllocator<2>> WrappedResources;
};

// Wraps a D3D12 resource so it can be used by Spout's D3D11 path.
Microsoft::WRL::ComPtr<ID3D11Texture2D> WrapD3D12Resource(ID3D12Resource* Resource, D3D12_RESOURCE_STATES State);
