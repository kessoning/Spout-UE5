/**
 * Implements the Spout sender flow by copying UE D3D12 textures into shared DX11 textures
 * via D3D11-on-12, coordinating with the sender registry and D3D context.
 */
#include "SpoutSender.h"
#include "SpoutD3DContext.h"
#include "SpoutD3DUtils.h"
#include "SpoutSenderRegistry.h"
#include "SpoutModule.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "RenderResource.h"
#include "RenderingThread.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "Widgets/SWindow.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture.h"
#include "TextureResource.h"
#include "Engine/TextureRenderTarget.h"

// UE 5.8 changed FSlateRenderer::OnBackBufferReadyToPresent()'s second delegate parameter from
// const FTextureRHIRef& to ISlateViewportProvider& (the back buffer is fetched from the provider).
// Guard on engine version so UE 5.3-5.7 keep the original signature. EngineVersionComparison.h
// also pulls in ENGINE_*_VERSION (via Version.h), so no separate Version.h include is needed.
#include "Misc/EngineVersionComparison.h"
#define SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER (!UE_VERSION_OLDER_THAN(5, 8, 0))

#if SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER
#include "Slate/SlateViewportProvider.h"
#endif

using Microsoft::WRL::ComPtr;

namespace
{
	DXGI_FORMAT SanitizeDxgiFormat(DXGI_FORMAT InFormat)
	{
		switch (InFormat)
		{
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default:
			return InFormat;
		}
	}

	struct FSpoutSendSource
	{
		// RHI texture to read from; resolved on the game thread before enqueueing.
		FTextureRHIRef SourceRHI;
		// Used to restore access state after copy.
		bool bIsViewport = false;
	};

	bool BuildSendSource(ESpoutSendTextureFrom SendTextureFrom, UTextureRenderTarget2D* TextureRenderTarget2D, FSpoutSendSource& OutSource)
	{
		if (SendTextureFrom == ESpoutSendTextureFrom::TextureRenderTarget2D)
		{
			if (!TextureRenderTarget2D)
			{
				return false;
			}

			FTextureRenderTargetResource* RTResource = TextureRenderTarget2D->GameThread_GetRenderTargetResource();
			if (!RTResource)
			{
				return false;
			}

			// Resolve the RHI texture on the game thread where the UObject is guaranteed alive,
			// so the render-thread lambda captures a ref-counted handle instead of a raw pointer.
			OutSource.SourceRHI = RTResource->GetRenderTargetTexture();
			if (!OutSource.SourceRHI.IsValid())
			{
				return false;
			}

			OutSource.bIsViewport = false;
			return true;
		}

		return false;
	}

	void EnsureSharedSenderTexture(FSpoutSharedSender& Sender, uint32 Width, uint32 Height, DXGI_FORMAT Format, FSpoutD3DContext& Context);
	ComPtr<ID3D11Texture2D> GetWrappedResource(FSpoutSharedSender& Sender, ID3D12Resource* NativeResource);

	/**
	 * Copies SourceRHI into the shared texture of every named sender.
	 *
	 * All destinations share ONE state transition, ONE RHI flush and ONE D3D11-on-12 acquire scope,
	 * so N senders publishing the same source cost a single GPU round trip instead of N.
	 *
	 * SourceWrapCache supplies the D3D11 wrap of the source. The viewport path passes its own cache
	 * because the swap chain rotates through several back buffers and a per-sender single-entry cache
	 * would re-wrap nearly every frame. The render-target path passes null and uses the sender's own
	 * cache instead, which is keyed to that sender's source; it always sends exactly one name, so only
	 * the first entry's cache is consulted.
	 */
	void SendTextureOnRenderThread(
		TConstArrayView<FName> SpoutNames,
		const FTextureRHIRef& SourceRHI,
		bool bIsViewport,
		FRHICommandListImmediate& RHICmdList,
		FSpoutWrapCache* SourceWrapCache)
	{
		if (SpoutNames.Num() == 0 || !SourceRHI.IsValid())
		{
			return;
		}

		FSpoutD3DContext& Context = FSpoutD3DContext::Get();
		if (!Context.IsSpoutAvailable())
		{
			return;
		}

		if (!Context.GetImmediateContext() || !Context.GetD3D11On12Device())
		{
			return;
		}

		// UE D3D12 textures expose their native resource for interop.
		ID3D12Resource* NativeRes = static_cast<ID3D12Resource*>(SourceRHI->GetNativeResource());
		if (!NativeRes)
		{
			return;
		}

		// Transition for copy and flush so the wrapped D3D11 context can see up-to-date data.
		// Viewport backbuffer comes in Present state; render targets come in SRVMask after scene render.
		// The flush acts on the RHI command list, not the D3D11 immediate context, so it is issued
		// BEFORE taking D3D11ContextMutex — keeping the heavy RHI-thread flush out of the lock and
		// matching the receiver path.
		const ERHIAccess FromState = bIsViewport ? ERHIAccess::Present : ERHIAccess::SRVMask;
		RHICmdList.Transition(FRHITransitionInfo(SourceRHI, FromState, ERHIAccess::CopySrc));
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);

		D3D12_RESOURCE_DESC Desc = NativeRes->GetDesc();

		DXGI_FORMAT Format = SanitizeDxgiFormat(Desc.Format);
		if (Format == DXGI_FORMAT_UNKNOWN)
		{
			// Fall back to a common format if the backbuffer format cannot be used.
			Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		}

		// Resolve every destination first: shared-texture creation and Spout registration are
		// device-level (free-threaded), so they stay outside D3D11ContextMutex.
		// TRefCountPtr, not ComPtr: ComPtr overloads operator& to return a ComPtrRef, which breaks
		// UE container internals that take the address of an element (a hard compile error on UE 5.3).
		TArray<TRefCountPtr<ID3D11Texture2D>, TInlineAllocator<4>> Destinations;
		TSharedPtr<FSpoutSharedSender> WrapOwner;

		for (const FName& SpoutName : SpoutNames)
		{
			TSharedPtr<FSpoutSharedSender> Sender = FSpoutSenderRegistry::Get().FindOrAdd(SpoutName, ESpoutType::Sender);
			if (!Sender)
			{
				continue;
			}

			EnsureSharedSenderTexture(*Sender, static_cast<uint32>(Desc.Width), static_cast<uint32>(Desc.Height), Format, Context);

			ComPtr<ID3D11Texture2D> SharedTexture;
			{
				FScopeLock SenderLock(&Sender->ResourceMutex);
				SharedTexture = Sender->SharedTexture;
			}

			if (SharedTexture)
			{
				Destinations.Add(TRefCountPtr<ID3D11Texture2D>(SharedTexture.Get()));
				if (!WrapOwner)
				{
					WrapOwner = Sender;
				}
			}
		}

		if (Destinations.Num() > 0)
		{
			// D3D11 immediate context is not thread-safe; guard its usage (and the wrap caches it
			// shares with the receiver path).
			FScopeLock D3DLock(&Context.GetD3D11ContextMutex());

			ComPtr<ID3D11Texture2D> WrappedSource = SourceWrapCache
				? SourceWrapCache->GetOrCreate(NativeRes, D3D12_RESOURCE_STATE_COPY_SOURCE)
				: GetWrappedResource(*WrapOwner, NativeRes);

			if (WrappedSource)
			{
				// One acquire/release/flush scope covers every copy, so publishing to N senders
				// submits once instead of N times.
				FScopedD3D11On12Acquire Acquire(WrappedSource.Get());
				if (Acquire.IsValid())
				{
					for (const TRefCountPtr<ID3D11Texture2D>& Destination : Destinations)
					{
						Context.GetImmediateContext()->CopyResource(Destination.GetReference(), WrappedSource.Get());
					}
				}
			}
		}

		// Restore the source access state after the copy. Runs on every path once the resource has
		// been transitioned to CopySrc (including when the sender lookup or wrap fails) so UE's state
		// tracking stays consistent.
		// Restore to SRVMask (not just SRVGraphics) to mirror the SRVMask the texture was
		// transitioned from, so a later compute-shader read does not trip RHI validation.
		RHICmdList.Transition(FRHITransitionInfo(
			SourceRHI,
			ERHIAccess::CopySrc,
			bIsViewport ? ERHIAccess::Present : ERHIAccess::SRVMask
		));
	}

	class FSpoutViewportSender
	{
	public:
		static FSpoutViewportSender& Get()
		{
			static FSpoutViewportSender Instance;
			return Instance;
		}

		bool QueueSend(const FName& SpoutName)
		{
			if (SpoutName.IsNone())
			{
				return false;
			}

			if (!RegisterIfNeeded())
			{
				return false;
			}

			// Resolve the viewport's window here, on the game thread. GEngine->GameViewport and its
			// window are game-thread state; the back-buffer delegate runs on the render thread and
			// must not read them.
			const SWindow* WindowPtr = nullptr;
			if (GEngine && GEngine->GameViewport)
			{
				if (const TSharedPtr<SWindow> Window = GEngine->GameViewport->GetWindow())
				{
					WindowPtr = Window.Get();
				}
			}

			FScopeLock Lock(&PendingMutex);
			// Stored as a raw pointer used only for identity comparison against the window the
			// delegate hands us. It is never dereferenced, so a destroyed window cannot be touched
			// through it.
			TargetWindow = WindowPtr;
			PendingSenders.Add(SpoutName);
			return true;
		}

		void Shutdown()
		{
			if (bRegistered && FSlateApplication::IsInitialized())
			{
				if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
				{
					Renderer->OnBackBufferReadyToPresent().Remove(BackBufferHandle);
				}
			}

			bRegistered = false;
			BackBufferHandle.Reset();

			{
				// The cache is otherwise touched only on the render thread under this mutex.
				FSpoutD3DContext& Context = FSpoutD3DContext::Get();
				FScopeLock D3DLock(&Context.GetD3D11ContextMutex());
				BackBufferWrapCache.Reset();
			}

			FScopeLock Lock(&PendingMutex);
			PendingSenders.Reset();
			TargetWindow = nullptr;
		}

	private:
		bool RegisterIfNeeded()
		{
			if (bRegistered)
			{
				return true;
			}

			if (!GEngine || !GEngine->GameViewport)
			{
				return false;
			}

			if (!FSlateApplication::IsInitialized())
			{
				return false;
			}

			FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer();
			if (!Renderer)
			{
				return false;
			}

			if (!GEngine->GameViewport->GetWindow().IsValid())
			{
				return false;
			}

			BackBufferHandle =
			Renderer->OnBackBufferReadyToPresent().AddLambda(
#if SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER
				[this](SWindow& SlateWindow, ISlateViewportProvider& ViewportProvider)
#else
				[this](SWindow& SlateWindow, const FTextureRHIRef& IncomingBackBuffer)
#endif
				{
#if SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER
					// UE 5.8+: obtain the back buffer from the viewport provider. This lambda runs on
					// the render thread (delegate contract), where GetBackBufferResource() is valid; the
					// returned raw FRHITexture* is held by an FTextureRHIRef (TRefCountPtr AddRefs),
					// keeping it alive for the ENQUEUE_RENDER_COMMAND copy below.
					const FTextureRHIRef BackBuffer = ViewportProvider.GetBackBufferResource();
#else
					const FTextureRHIRef& BackBuffer = IncomingBackBuffer;
#endif
					if (!BackBuffer.IsValid())
					{
						return;
					}

					TArray<FName> NamesToSend;
					{
						FScopeLock Lock(&PendingMutex);

						// The delegate fires for every window; only consume the queue for the one the
						// game viewport last reported. Pure pointer identity, resolved on the game
						// thread in QueueSend.
						if (TargetWindow != &SlateWindow)
						{
							return;
						}

						NamesToSend = PendingSenders.Array();
						PendingSenders.Reset();
					}

					if (NamesToSend.Num() == 0)
					{
						return;
					}

					const FTextureRHIRef BackBufferCopy = BackBuffer;

					ENQUEUE_RENDER_COMMAND(SpoutViewportSend)(
						[this, NamesToSend = MoveTemp(NamesToSend), BackBufferCopy](FRHICommandListImmediate& RHICmdList)
						{
							// Every queued sender publishes the same back buffer, so they share one
							// transition, one flush and one acquire scope.
							SendTextureOnRenderThread(
								NamesToSend,
								BackBufferCopy,
								true,
								RHICmdList,
								&BackBufferWrapCache
							);
						}
					);
				}
			);

			bRegistered = BackBufferHandle.IsValid();
			return bRegistered;
		}

		FCriticalSection PendingMutex;
		TSet<FName> PendingSenders;
		// Identity of the game viewport's window, resolved on the game thread. Compared against the
		// delegate's window and never dereferenced. Guarded by PendingMutex.
		const SWindow* TargetWindow = nullptr;
		FDelegateHandle BackBufferHandle;
		bool bRegistered = false;
		// Wraps of the swap chain's back buffers. Render-thread only, under the D3D11 context mutex.
		FSpoutWrapCache BackBufferWrapCache;
	};

	void EnsureSharedSenderTexture(FSpoutSharedSender& Sender, uint32 Width, uint32 Height, DXGI_FORMAT Format, FSpoutD3DContext& Context)
	{
		// Sender state can be accessed on the render thread and by BP calls.
		FScopeLock SenderLock(&Sender.ResourceMutex);
		if (Sender.SharedTexture && Sender.Width == Width && Sender.Height == Height && Sender.SharedFormat == Format)
		{
			return;
		}

		Sender.SharedTexture.Reset();
		Sender.SharedHandle = nullptr;
		Sender.CachedWrappedResource.Reset();
		Sender.CachedNativeResource = nullptr;
		Sender.Width = Width;
		Sender.Height = Height;
		Sender.SharedFormat = Format;

		spoutDirectX* SpoutDX = Context.GetSpoutDirectX();
		if (!SpoutDX)
		{
			return;
		}

		// Spout creates a DX11 shared texture and returns a shared handle.
		const bool bCreated = SpoutDX->CreateSharedDX11Texture(
			Context.GetD3D11Device(),
			Width,
			Height,
			Format,
			Sender.SharedTexture.GetAddressOf(),
			Sender.SharedHandle
		);

		if (bCreated && Sender.SharedTexture && Sender.SharedHandle)
		{
			spoutSenderNames* SenderNames = Context.GetSenderNames();
			if (SenderNames)
			{
				// Register or update this sender in Spout's global registry.
				FScopeLock SpoutLock(&Context.GetSpoutMutex());
				if (!Sender.bRegistered)
				{
					Sender.bRegistered = SenderNames->CreateSender(
						TCHAR_TO_UTF8(*Sender.Name.ToString()),
						Sender.Width,
						Sender.Height,
						Sender.SharedHandle,
						Format
					);
				}
				else
				{
					SenderNames->UpdateSender(
						TCHAR_TO_UTF8(*Sender.Name.ToString()),
						Sender.Width,
						Sender.Height,
						Sender.SharedHandle,
						Format
					);
				}
			}
		}
	}

	ComPtr<ID3D11Texture2D> GetWrappedResource(FSpoutSharedSender& Sender, ID3D12Resource* NativeResource)
	{
		// Cache the wrapped resource per native D3D12 texture to avoid re-wrapping every frame.
		FScopeLock SenderLock(&Sender.ResourceMutex);
		if (Sender.CachedNativeResource != NativeResource || !Sender.CachedWrappedResource)
		{
			Sender.CachedWrappedResource = WrapD3D12Resource(NativeResource, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Sender.CachedNativeResource = NativeResource;
		}

		return Sender.CachedWrappedResource;
	}
}

bool FSpoutSender::Send(FName SpoutName, ESpoutSendTextureFrom SendTextureFrom, UTextureRenderTarget2D* TextureRenderTarget2D)
{
	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	Context.InitializeIfNeeded();

	if (!Context.IsSpoutAvailable())
	{
		return false;
	}

	if (SendTextureFrom == ESpoutSendTextureFrom::GameViewport)
	{
		if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
		{
			return false;
		}

		// Queue the request; capture happens when the backbuffer is ready to present.
		return FSpoutViewportSender::Get().QueueSend(SpoutName);
	}

	FSpoutSendSource SendSource;
	if (!BuildSendSource(SendTextureFrom, TextureRenderTarget2D, SendSource))
	{
		return false;
	}

	// The copy must run on the render thread to safely touch RHI resources.
	ENQUEUE_RENDER_COMMAND(SpoutSenderCmd)(
		[SpoutName, SendSource](FRHICommandListImmediate& RHICmdList)
		{
			if (!SendSource.SourceRHI.IsValid())
			{
				return;
			}

			// Single destination; the sender's own wrap cache is keyed to this source.
			SendTextureOnRenderThread(
				MakeArrayView(&SpoutName, 1),
				SendSource.SourceRHI,
				SendSource.bIsViewport,
				RHICmdList,
				nullptr
			);
		});

	return true;
}

void FSpoutSender::Close(FName SpoutName)
{
	// Registry is keyed by (Name, Type); Close must scope to the Sender entry only,
	// leaving any Receiver entry with the same FName untouched.
	if (FSpoutSenderRegistry::Get().Find(SpoutName, ESpoutType::Sender))
	{
		FSpoutSenderRegistry::Get().Remove(SpoutName, ESpoutType::Sender);
		UE_LOG(LogSpoutPlugin, Log, TEXT("Closed Spout Sender: %s"), *SpoutName.ToString());
	}
}

void FSpoutSender::Shutdown()
{
	FSpoutViewportSender::Get().Shutdown();
}
