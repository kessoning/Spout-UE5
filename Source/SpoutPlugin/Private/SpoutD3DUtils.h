/**
 * Declares D3D helper utilities for wrapping D3D12 resources and managing D3D11-on-12
 * acquire/release scopes used by sender/receiver interop.
 */
#pragma once

#include "CoreMinimal.h"
#include "Templates/RefCounting.h"

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
	TArray<TRefCountPtr<ID3D11Resource>, TInlineAllocator<2>> WrappedResources;
};

// Wraps a D3D12 resource so it can be used by Spout's D3D11 path.
Microsoft::WRL::ComPtr<ID3D11Texture2D> WrapD3D12Resource(ID3D12Resource* Resource, D3D12_RESOURCE_STATES State);

/**
 * Small bounded cache of D3D11-on-12 wraps, keyed by the native D3D12 resource.
 *
 * Wrapping is a device call, so it must not run per frame. A single-entry cache is enough when the
 * source is one stable texture (a render target), but NOT for the viewport back buffer: the swap
 * chain rotates through several back buffers, so a single entry would miss on almost every frame
 * and re-wrap continuously. Keying a few entries covers the whole rotation.
 *
 * Entries keep the wrapped resource (and therefore the underlying D3D12 resource) alive, so the
 * cache is dropped wholesale once it grows past MaxEntries and on Reset(). Exceeding the bound
 * means the working set changed, at which point the old wraps are stale anyway.
 *
 * Not internally synchronized: callers must hold the D3D11 immediate-context mutex.
 */
class FSpoutWrapCache
{
public:
	// Returns the cached wrap for Resource, creating it on first use. Null if wrapping fails.
	Microsoft::WRL::ComPtr<ID3D11Texture2D> GetOrCreate(ID3D12Resource* Resource, D3D12_RESOURCE_STATES State);

	// Drops every cached wrap, releasing the references they hold.
	void Reset();

private:
	static constexpr int32 MaxEntries = 8;

	// TRefCountPtr, not ComPtr: ComPtr overloads operator& to return a ComPtrRef, which breaks
	// UE container internals that take the address of an element (a hard compile error on UE 5.3).
	TMap<ID3D12Resource*, TRefCountPtr<ID3D11Texture2D>> Entries;
};
