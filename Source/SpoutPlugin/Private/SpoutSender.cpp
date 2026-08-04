// Copyright (c) 2023 RockmanZuyi
// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the Spout sender flow by copying UE D3D12 textures into shared DX11 textures
 * via D3D11-on-12, coordinating with the sender registry and D3D context.
 */
#include "SpoutSender.h"
#include "SpoutD3DContext.h"
#include "SpoutD3DUtils.h"
#include "SpoutFormatPolicy.h"
#include "SpoutFrameSync.h"
#include "SpoutLogThrottle.h"
#include "SpoutRHIState.h"
#include "SpoutSenderRegistry.h"
#include "SpoutModule.h"
#include "SpoutSession.h"
#include "SpoutValidation.h"

#include <atomic>

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

// UE 5.3 declares FOnBackBufferReadyToPresent with the plain DECLARE_MULTICAST_DELEGATE_TwoParams,
// whose invocation list has NO synchronisation (FDefaultDelegateUserPolicy ->
// FNotThreadSafe[NotChecked]DelegateMode: the delegate's access scopes are empty). UE 5.4 switched
// it to DECLARE_TS_MULTICAST_DELEGATE_TwoParams, whose FThreadSafeDelegateMode takes a real
// critical section around Add, Remove and Broadcast alike. Since the delegate is broadcast on the
// rendering thread, mutating the list from the game thread is a data race on 5.3 and only on 5.3,
// so 5.3 alone performs the mutation on the rendering thread.
#define SPOUT_BACKBUFFER_DELEGATE_IS_THREAD_SAFE (!UE_VERSION_OLDER_THAN(5, 4, 0))

#if SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER
#include "Slate/SlateViewportProvider.h"
#endif

using Microsoft::WRL::ComPtr;

namespace
{
	/**
	 * Rate limiter for the unsupported-source-format diagnostic.
	 *
	 * Scoped to the send path rather than to one sender entry: the format comes from the shared
	 * source resource, so every name publishing that source hits the same condition in the same
	 * frame and one line per interval describes all of them.
	 */
	FSpoutLogThrottle GSenderFormatLogThrottle(5.0);

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

			// Reject an unusable render target on the game thread, before a render command is
			// queued. The render thread validates the resolved RHI resource again as the
			// authoritative check, but failing here keeps an obviously invalid request from
			// costing a queued command and a GPU flush every frame.
			const SpoutValidation::EResult ExtentResult = SpoutValidation::ValidateDimensions(
				TextureRenderTarget2D->SizeX,
				TextureRenderTarget2D->SizeY);
			if (ExtentResult != SpoutValidation::EResult::Ok)
			{
				static FSpoutLogThrottle ExtentLogThrottle(5.0);
				if (ExtentLogThrottle.ShouldLog())
				{
					UE_LOG(LogSpoutPlugin, Warning,
						TEXT("SpoutSender: render target '%s' has invalid dimensions %dx%d (%s)."),
						*TextureRenderTarget2D->GetName(),
						TextureRenderTarget2D->SizeX,
						TextureRenderTarget2D->SizeY,
						SpoutValidation::ToString(ExtentResult));
				}
				return false;
			}

			const EPixelFormat RenderTargetFormat = TextureRenderTarget2D->GetFormat();
			if (SpoutValidation::ValidatePixelFormat(RenderTargetFormat) != SpoutValidation::EResult::Ok)
			{
				static FSpoutLogThrottle FormatLogThrottle(5.0);
				if (FormatLogThrottle.ShouldLog())
				{
					UE_LOG(LogSpoutPlugin, Warning,
						TEXT("SpoutSender: render target '%s' has pixel format %s, which this plugin cannot share. ")
						TEXT("Supported formats are B8G8R8A8, R8G8B8A8, A2B10G10R10 and FloatRGBA."),
						*TextureRenderTarget2D->GetName(),
						GetPixelFormatString(RenderTargetFormat));
				}
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

	bool EnsureSharedSenderTexture(FSpoutSharedSender& Sender, uint32 Width, uint32 Height, DXGI_FORMAT Format, FSpoutD3DContext& Context);
	ComPtr<ID3D11Texture2D> GetWrappedResource(FSpoutSharedSender& Sender, ID3D12Resource* NativeResource, SpoutRHIState::EWrapState WrapState);

	/** One resolved publication target: the entry that owns the protocol state, and its texture. */
	struct FSpoutSendDestination
	{
		TSharedPtr<FSpoutSharedSender> Entry;
		// TRefCountPtr, not ComPtr: ComPtr overloads operator& to return a ComPtrRef, which breaks
		// UE container internals that take the address of an element (a hard compile error on UE 5.3).
		TRefCountPtr<ID3D11Texture2D> SharedTexture;
	};

	/**
	 * Copies SourceRHI into the shared texture of every named sender.
	 *
	 * All destinations share ONE state transition, ONE RHI flush and ONE D3D11-on-12 acquire scope
	 * of the source, so N senders publishing the same source still cost a single source round trip.
	 * The per-destination part cannot be shared: each name has its own cross-process access mutex,
	 * and the copy into a shared texture has to be submitted before that name's frame is announced.
	 *
	 * Takes tokens rather than names. Between the Blueprint call that queued this command and this
	 * function running, the sender may have been closed or the whole session torn down; a name
	 * carries none of that, a token carries all of it. Tokens are filtered here, before the source
	 * is transitioned, so a fully cancelled batch also costs no GPU work.
	 *
	 * SourceWrapCache supplies the D3D11 wrap of the source. The viewport path passes its own cache
	 * because the swap chain rotates through several back buffers and a per-sender single-entry cache
	 * would re-wrap nearly every frame. The render-target path passes null and uses the sender's own
	 * cache instead, which is keyed to that sender's source; it always sends exactly one name, so only
	 * the first entry's cache is consulted.
	 */
	void SendTextureOnRenderThread(
		TConstArrayView<FSpoutStreamToken> Tokens,
		const FTextureRHIRef& SourceRHI,
		bool bIsViewport,
		FRHICommandListImmediate& RHICmdList,
		FSpoutWrapCache* SourceWrapCache)
	{
		if (Tokens.Num() == 0 || !SourceRHI.IsValid())
		{
			return;
		}

		// Cancellation check first, before anything with a cost. Everything below this point is
		// skipped entirely for work that was closed or torn down after it was queued: no registry
		// entry is created, no name is registered, no resource is transitioned and no copy is made.
		FSpoutSession& Session = FSpoutSession::Get();
		TArray<FSpoutStreamToken, TInlineAllocator<4>> LiveTokens;
		for (const FSpoutStreamToken& Token : Tokens)
		{
			if (Session.IsCurrent(Token))
			{
				LiveTokens.Add(Token);
			}
		}

		if (LiveTokens.Num() == 0)
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

		// Resolve the source description and its format BEFORE any state transition. Reading the
		// D3D12 descriptor needs no particular resource state, and deciding here means an
		// unsupported source returns without having transitioned the resource or paid for an RHI
		// flush — so there is no state to unwind on the reject path.
		D3D12_RESOURCE_DESC Desc = NativeRes->GetDesc();

		const SpoutFormatPolicy::FSenderFormat SourceFormat = SpoutFormatPolicy::ResolveSenderFormat(Desc.Format);
		if (!SourceFormat.IsUsable())
		{
			// Refuse rather than substitute. Turning an unknown format into a BGRA8 shared texture
			// advertises a layout the source does not have, and passing an unsupported one
			// straight to CreateSharedDX11Texture is no better; both produce silently dropped or
			// garbled frames. Naming the format here makes the cause visible instead.
			if (GSenderFormatLogThrottle.ShouldLog())
			{
				UE_LOG(LogSpoutPlugin, Warning,
					TEXT("Spout sender: source texture format %s is %s and cannot be shared. ")
					TEXT("Nothing was published for %d name(s) (first: '%s'). ")
					TEXT("Supported source formats are RGBA8, BGRA8, RGB10A2 and RGBA16F (including their typeless variants)."),
					*SpoutFormatPolicy::DescribeDxgiFormat(static_cast<uint32>(Desc.Format)),
					SpoutFormatPolicy::ToString(SourceFormat.Support),
					LiveTokens.Num(),
					*LiveTokens[0].Name.ToString());
			}
			return;
		}

		const DXGI_FORMAT Format = SourceFormat.SharedFormat;

		// Validate the source extent before creating a shared texture from it.
		const SpoutValidation::EResult ExtentResult = SpoutValidation::ValidateDimensions(
			static_cast<int64>(Desc.Width),
			static_cast<int64>(Desc.Height));
		if (ExtentResult != SpoutValidation::EResult::Ok)
		{
			if (GSenderFormatLogThrottle.ShouldLog())
			{
				UE_LOG(LogSpoutPlugin, Warning,
					TEXT("Spout sender: source texture extent %llux%llu is invalid (%s); nothing was published for '%s'."),
					static_cast<uint64>(Desc.Width),
					static_cast<uint64>(Desc.Height),
					SpoutValidation::ToString(ExtentResult),
					*LiveTokens[0].Name.ToString());
			}
			return;
		}

		// Resolve every destination BEFORE anything with a GPU cost. Shared-texture creation and
		// Spout registration are device-level (free-threaded), so they stay outside
		// D3D11ContextMutex — and doing them first means a batch with nothing publishable (a sender
		// whose registration is failing, say) costs no state transition and no RHI flush at all.
		TArray<FSpoutSendDestination, TInlineAllocator<4>> Destinations;

		for (const FSpoutStreamToken& Token : LiveTokens)
		{
			// Re-checked immediately before the registry entry is created. FindOrAdd is the point
			// of no return: creating the entry here for a name that was closed a moment ago
			// resurrects the sender that Close was supposed to have removed.
			if (!Session.IsCurrent(Token))
			{
				continue;
			}

			TSharedPtr<FSpoutSharedSender> Sender = FSpoutSenderRegistry::Get().FindOrAdd(Token.Name, ESpoutType::Sender);
			if (!Sender)
			{
				continue;
			}

			// Gate on the registration state, not on the texture existing. A live shared texture
			// says nothing about whether Spout accepted the name, and publishing into a texture
			// whose name is unregistered (or whose advertised handle is stale) sends the frames
			// nowhere a receiver can find them.
			if (!EnsureSharedSenderTexture(*Sender, static_cast<uint32>(Desc.Width), static_cast<uint32>(Desc.Height), Format, Context))
			{
				continue;
			}

			ComPtr<ID3D11Texture2D> SharedTexture;
			{
				FScopeLock SenderLock(&Sender->ResourceMutex);
				// Re-check under the lock: the registration and the texture must agree at the
				// moment the destination is captured.
				if (Sender->Registration.IsActive())
				{
					SharedTexture = Sender->SharedTexture;
				}
			}

			if (SharedTexture)
			{
				FSpoutSendDestination Destination;
				Destination.Entry = Sender;
				Destination.SharedTexture = TRefCountPtr<ID3D11Texture2D>(SharedTexture.Get());
				Destinations.Add(MoveTemp(Destination));
			}
		}

		if (Destinations.Num() == 0)
		{
			// Nothing to publish. Nothing has been transitioned or flushed, so there is nothing to
			// unwind either.
			return;
		}

		// The whole state bracket comes from SpoutRHIState, which is also what the wrap
		// state below comes from, so the ERHIAccess the resource is transitioned into and the
		// D3D12 state declared to CreateWrappedResource cannot disagree.
		//
		// EntryBefore is Unknown for both source roles because neither resource's previous access
		// is this plugin's to know: the viewport back buffer's access at broadcast time is not
		// contractual across UE 5.3-5.8, and a caller-supplied render target may last have been an
		// RTV, a UAV or a copy destination. See SpoutRHIState.h.
		//
		// The flush acts on the RHI command list, not the D3D11 immediate context, so it is issued
		// BEFORE taking D3D11ContextMutex — keeping the heavy RHI-thread flush out of the lock and
		// matching the receiver path. It also has to precede AcquireWrappedResources, which
		// requires the resource to already be in the state the wrap declared.
		const SpoutRHIState::FCopyBracket Bracket = SpoutRHIState::GetCopyBracket(
			bIsViewport
				? SpoutRHIState::EResourceRole::ViewportBackBuffer
				: SpoutRHIState::EResourceRole::SenderRenderTarget);

		RHICmdList.Transition(FRHITransitionInfo(SourceRHI, Bracket.EntryBefore, Bracket.CopyAccess));
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);

		{
			// D3D11 immediate context is not thread-safe; guard its usage (and the wrap caches it
			// shares with the receiver path).
			FScopeLock D3DLock(&Context.GetD3D11ContextMutex());

			ComPtr<ID3D11Texture2D> WrappedSource = SourceWrapCache
				? SourceWrapCache->GetOrCreate(NativeRes, Bracket.WrapState)
				: GetWrappedResource(*Destinations[0].Entry, NativeRes, Bracket.WrapState);

			if (WrappedSource)
			{
				// One acquire/release scope covers every copy: the source is wrapped once and
				// transitioned once no matter how many names publish it.
				FScopedD3D11On12Acquire Acquire(WrappedSource.Get());
				if (Acquire.IsValid())
				{
					for (const FSpoutSendDestination& Destination : Destinations)
					{
						// Take this name's cross-process access mutex around its copy, one name at
						// a time. Never several at once: each destination is a different named
						// mutex, and holding two of them would make the lock order depend on the
						// order names happen to be queued in.
						//
						// A failed wait means a peer is still reading this texture. Skipping is
						// the correct response -- overwriting it would tear the frame the peer is
						// mid-read of -- and the scope owns nothing, so nothing is released.
						FSpoutTextureAccessScope Access(
							Destination.Entry->FrameSync,
							Destination.SharedTexture.GetReference());

						if (!Access.IsAcquired())
						{
							if (Destination.Entry->AccessLogThrottle.ShouldLog())
							{
								UE_LOG(LogSpoutPlugin, Warning,
									TEXT("Spout sender '%s': timed out waiting for shared-texture access; this frame was not published. ")
									TEXT("A receiver is holding the texture longer than the sender's frame time."),
									*Destination.Entry->Name.ToString());
							}
							continue;
						}

						Context.GetImmediateContext()->CopyResource(
							Destination.SharedTexture.GetReference(),
							WrappedSource.Get());

						// Submit before announcing. SetNewFrame tells receivers the texture holds a
						// new frame, and releasing access lets them read it, so both have to follow
						// the copy actually reaching the queue. This is the order the Spout SDK's
						// own sender uses.
						Context.GetImmediateContext()->Flush();
						Destination.Entry->FrameSync.PublishFrame();
					}
				}
			}
		}

		// Restore the source access state after the copy. Runs on every path once the resource has
		// been transitioned (including when the wrap or the acquire fails) so UE's state tracking
		// stays consistent.
		//
		// The declared previous state is the copy access, never Unknown: the transition above is
		// what set it and no other code has touched the resource since.
		//
		// The viewport back buffer is restored to SRVGraphics, which is precisely what UE 5.3/5.4
		// set before broadcasting — so this whole bracket is invisible to the engine, and the
		// engine's own follow-up transition (SRVGraphics -> RTV for stereo) stays valid. Restoring
		// it to Present would invalidate that transition's declared previous state. Render targets
		// go back to SRVMask rather than SRVGraphics so a later compute-shader read does not trip
		// RHI validation.
		RHICmdList.Transition(FRHITransitionInfo(SourceRHI, Bracket.CopyAccess, Bracket.ExitAfter));
	}

	class FSpoutViewportSender
	{
	public:
		static FSpoutViewportSender& Get()
		{
			static FSpoutViewportSender Instance;
			return Instance;
		}

		bool QueueSend(const FSpoutStreamToken& Token)
		{
			if (!Token.IsSet() || Token.Name.IsNone())
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

			{
				FScopeLock Lock(&WindowMutex);
				// Stored as a raw pointer used only for identity comparison against the window the
				// delegate passes in. It is never dereferenced, so a destroyed window cannot be touched
				// through it.
				TargetWindow = WindowPtr;
			}

			PendingSenders.Add(Token);
			return true;
		}

		/**
		 * Drops one name from the pending set.
		 *
		 * Without it an individually closed viewport sender would stay queued, and the next
		 * present would publish a name the caller had already closed.
		 */
		bool RemovePending(const FName& SpoutName)
		{
			return PendingSenders.Remove(SpoutName);
		}

		/**
		 * Unsubscribes the back-buffer delegate and waits until no invocation of it is still running.
		 *
		 * The wait is the part that matters. Removing a multicast delegate does not interrupt a
		 * broadcast that is already in progress, so without it this function can return while the
		 * render thread is midway through the lambda and about to enqueue another command -- late
		 * work that outlives the teardown meant to precede it. Once this returns, the only
		 * commands that can exist are ones already in the queue, and the flush that follows drains
		 * them.
		 */
		void DetachProducer()
		{
			// The session has already entered Stopping by the time the ordered teardown calls this
			// (EnterStoppingState precedes DetachProducers), so a callback entered after this point
			// early-outs before touching sender state. Removal below is what stops new ones.
			if (bDelegateRegistered && FSlateApplication::IsInitialized())
			{
				if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
				{
					const FDelegateHandle HandleToRemove = BackBufferHandle;
					auto UnsubscribeFromBackBuffer = [Renderer, HandleToRemove]()
					{
						UE_LOG(LogSpoutPlugin, Log,
							TEXT("Spout: removing the viewport back-buffer delegate on the %s thread."),
							IsInRenderingThread() ? TEXT("rendering") : TEXT("game"));

						Renderer->OnBackBufferReadyToPresent().Remove(HandleToRemove);
					};

#if SPOUT_BACKBUFFER_DELEGATE_IS_THREAD_SAFE
					UnsubscribeFromBackBuffer();
#else
					// UE 5.3: remove on the broadcasting thread, and do not continue until it has
					// happened - the in-flight wait and the resource teardown below both assume no
					// further callback can start.
					RunDelegateMutationAndWait(UnsubscribeFromBackBuffer);
#endif

#if !SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER
					// The pre-resize subscription lives and dies with the producer, in
					// this same ordered detach - but on the GAME thread, on every version. It is
					// broadcast on the game thread, so it must not be marshalled to the rendering
					// thread; on 5.3 this detach is asymmetric by design (see RegisterIfNeeded).
					Renderer->OnPreResizeWindowBackBuffer().Remove(PreResizeHandle);
#endif
				}
			}

			// Cleared after the removal has completed, so the handle stays valid for as long as the
			// mutation that consumes it can still be running.
			bDelegateRegistered = false;
			BackBufferHandle.Reset();
#if !SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER
			PreResizeHandle.Reset();
#endif

			// Never wait from inside the callback itself; that would be waiting on this thread.
			if (IsInRenderingThread())
			{
				return;
			}

			// Bounded, because hanging the editor is a worse failure than a logged warning. The
			// delegate body is a handful of instructions plus one ENQUEUE_RENDER_COMMAND, so the
			// normal case exits on the first check.
			const double DeadlineSeconds = FPlatformTime::Seconds() + 2.0;
			while (InFlightBroadcasts.load(std::memory_order_acquire) > 0)
			{
				if (FPlatformTime::Seconds() > DeadlineSeconds)
				{
					UE_LOG(LogSpoutPlugin, Warning,
						TEXT("Spout: timed out waiting for the viewport back-buffer callback to finish. ")
						TEXT("Teardown is continuing; queued work is already cancelled, so any late callback is a no-op."));
					break;
				}

				FPlatformProcess::SleepNoStats(0.0f);
			}
		}

		/** Drops names that were queued but never presented. */
		void ClearPending()
		{
			PendingSenders.Reset();

			FScopeLock Lock(&WindowMutex);
			TargetWindow = nullptr;
		}

		/** Releases the cached back-buffer wraps. Must run after the render-command flush. */
		void ReleaseNativeResources()
		{
			// The cache is otherwise touched only on the render thread under this mutex.
			FSpoutD3DContext& Context = FSpoutD3DContext::Get();
			FScopeLock D3DLock(&Context.GetD3D11ContextMutex());
			BackBufferWrapCache.Reset();
		}

		/**
		 * Drops every cached back-buffer wrap and drains their destruction, so no plugin-held
		 * reference to the swap chain's buffers survives this call.
		 *
		 * IDXGISwapChain::ResizeBuffers refuses to run - fatally, DXGI_ERROR_INVALID_CALL - while
		 * ANY reference to the old back buffers is alive, and every cached wrap keeps the
		 * underlying ID3D12Resource alive. Resetting the cache releases the D3D11 wrappers, which
		 * only QUEUES their destruction inside the D3D11-on-12 device; the Flush is what actually
		 * drains it, so the D3D12 references are genuinely gone when this returns rather than
		 * merely scheduled to go. No ReleaseWrappedResources call is needed here: wraps are only
		 * ever acquired inside a copy's FScopedD3D11On12Acquire, whose destructor has already
		 * returned them (and flushed the copy) before this can run on either of its call sites.
		 */
		void InvalidateBackBufferWraps()
		{
			FSpoutD3DContext& Context = FSpoutD3DContext::Get();
			FScopeLock D3DLock(&Context.GetD3D11ContextMutex());
			BackBufferWrapCache.Reset();
			if (ID3D11DeviceContext* Immediate = Context.GetImmediateContext())
			{
				Immediate->Flush();
			}
		}

	private:
		/** Counts invocations of the back-buffer delegate that have entered but not yet returned. */
		struct FBroadcastScope
		{
			explicit FBroadcastScope(std::atomic<int32>& InCounter)
				: Counter(InCounter)
			{
				Counter.fetch_add(1, std::memory_order_acq_rel);
			}

			~FBroadcastScope()
			{
				Counter.fetch_sub(1, std::memory_order_acq_rel);
			}

			std::atomic<int32>& Counter;
		};

#if !SPOUT_BACKBUFFER_DELEGATE_IS_THREAD_SAFE
		/**
		 * Runs one back-buffer delegate mutation on the rendering thread and returns only once it
		 * has actually run (UE 5.3 only; see SPOUT_BACKBUFFER_DELEGATE_IS_THREAD_SAFE).
		 *
		 * The wait is what makes this correct rather than merely asynchronous: callers read the
		 * resulting handle, and DetachProducer must know the subscription is gone before it
		 * continues into the in-flight wait and resource teardown. FRenderCommandFence is used
		 * rather than FlushRenderingCommands because it waits for exactly this command instead of
		 * draining every pending RHI resource operation, and because the ordered teardown already
		 * owns its own flush step - adding a second global flush inside detach would blur that
		 * ordering.
		 *
		 * Renderer lifetime: the caller resolves FSlateRenderer* on the game thread and this
		 * function blocks that same thread until the command completes. FSlateApplication owns the
		 * renderer and is only torn down from the game thread, which cannot proceed while it is
		 * parked here, so the captured pointer cannot be destroyed mid-command.
		 *
		 * Already on the rendering thread: mutate directly. Waiting for the rendering thread from
		 * the rendering thread would deadlock, and a direct mutation there is safe for the same
		 * reason the whole mechanism exists - it is the thread that broadcasts, so nothing can be
		 * iterating the list concurrently.
		 */
		template <typename FunctorType>
		static void RunDelegateMutationAndWait(const FunctorType& Functor)
		{
			if (IsInRenderingThread())
			{
				Functor();
				return;
			}

			ENQUEUE_RENDER_COMMAND(SpoutViewportDelegateMutation)(
				[Functor](FRHICommandListImmediate&)
				{
					Functor();
				});

			// Bounded by the rendering thread reaching this command; with threaded rendering
			// disabled the command above already ran inline and the fence is satisfied at once.
			FRenderCommandFence Fence;
			Fence.BeginFence();
			Fence.Wait();
		}
#endif

		bool RegisterIfNeeded()
		{
			if (bDelegateRegistered)
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

			// The subscription itself is identical on every engine; only the THREAD that mutates the
			// invocation list differs (see SPOUT_BACKBUFFER_DELEGATE_IS_THREAD_SAFE). Writing it once
			// as a functor keeps the two paths from drifting apart.
			FDelegateHandle NewHandle;
			auto SubscribeToBackBuffer = [this, Renderer, &NewHandle]()
			{
				UE_LOG(LogSpoutPlugin, Log,
					TEXT("Spout: subscribing to the viewport back-buffer delegate on the %s thread."),
					IsInRenderingThread() ? TEXT("rendering") : TEXT("game"));

				NewHandle =
			Renderer->OnBackBufferReadyToPresent().AddLambda(
#if SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER
				[this](SWindow& SlateWindow, ISlateViewportProvider& ViewportProvider)
#else
				[this](SWindow& SlateWindow, const FTextureRHIRef& IncomingBackBuffer)
#endif
				{
					// Marks this callback as running for the whole of its body, so DetachProducer can
					// tell "removed the delegate" from "the delegate has actually finished".
					FBroadcastScope BroadcastScope(InFlightBroadcasts);

					// Cheapest possible early-out during teardown, and it also means the work below
					// cannot start once the session has entered Stopping.
					if (!FSpoutSession::Get().IsRunning())
					{
						return;
					}

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

					{
						// The delegate fires for every window; only consume the queue for the one the
						// game viewport last reported. Pure pointer identity, resolved on the game
						// thread in QueueSend.
						FScopeLock Lock(&WindowMutex);
						if (TargetWindow != &SlateWindow)
						{
							return;
						}
					}

					TArray<FSpoutStreamToken> TokensToSend;
					PendingSenders.Drain(TokensToSend);

					if (TokensToSend.Num() == 0)
					{
						return;
					}

					const FTextureRHIRef BackBufferCopy = BackBuffer;

					ENQUEUE_RENDER_COMMAND(SpoutViewportSend)(
						[this, TokensToSend = MoveTemp(TokensToSend), BackBufferCopy](FRHICommandListImmediate& RHICmdList)
						{
							// Every queued sender publishes the same back buffer, so they share one
							// transition, one flush and one acquire scope. Tokens that went stale
							// between the present and this command running are dropped inside.
							SendTextureOnRenderThread(
								TokensToSend,
								BackBufferCopy,
								true,
								RHICmdList,
								&BackBufferWrapCache
							);

#if SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER
							// UE 5.8: the ISlateViewportProvider refactor removed the
							// Slate pre-resize delegates, so there is no release point before
							// IDXGISwapChain::ResizeBuffers - and ResizeBuffers dies with
							// DXGI_ERROR_INVALID_CALL while any cached wrap keeps a back buffer
							// alive. The only correct design without a hook is to retain nothing
							// across frames: this present's wrap is dropped, and its destruction
							// drained, before the command returns. Costs one CreateWrappedResource
							// per present plus this flush (the copy itself was already flushed by
							// the acquire scope); the render-target send path keeps its per-sender
							// caches and is unaffected.
							InvalidateBackBufferWraps();
#endif
						}
					);
				}
			);
			}; // SubscribeToBackBuffer

#if SPOUT_BACKBUFFER_DELEGATE_IS_THREAD_SAFE
			// UE 5.4+: the delegate synchronises its own invocation list, so the game thread may
			// mutate it directly while the rendering thread broadcasts.
			SubscribeToBackBuffer();
#else
			// UE 5.3: mutate the list on the thread that broadcasts it. NewHandle is captured by
			// reference into the render command and read below only after the mutation has
			// completed, which the helper guarantees before it returns.
			RunDelegateMutationAndWait(SubscribeToBackBuffer);
#endif
			BackBufferHandle = NewHandle;
			bDelegateRegistered = BackBufferHandle.IsValid();

			if (!bDelegateRegistered)
			{
				// Nothing was subscribed, so report the attach as failed rather than leaving the
				// producer believing it is attached. The caller retries on the next send.
				return false;
			}

#if !SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER
			// UE 5.3-5.7 broadcast OnPreResizeWindowBackBuffer on the game thread from
			// FSlateRHIRenderer::ConditionalResizeViewport, after FlushRenderingCommands() and
			// immediately before RHIResizeViewport -> IDXGISwapChain::ResizeBuffers. It is the
			// engine's designed release point for externally cached back-buffer references (the
			// engine asserts the back buffer's own refcount right after the broadcast). Because
			// the render thread is drained at that moment, no back-buffer callback or queued copy
			// can be using the wraps, and the invalidation completes synchronously before the
			// broadcast returns - which is exactly what ResizeBuffers needs. The delegate fires
			// for every window; clearing on an unrelated window's resize merely costs one re-wrap
			// at the next present, so no window filtering is attempted.
			// (UE 5.8 removed these delegates in the ISlateViewportProvider refactor; its path
			// never retains wraps across frames instead - see the send command below.)
			//
			// This subscription stays on the GAME thread on every version, including 5.3, and is
			// deliberately not routed through RunDelegateMutationAndWait: OnPreResizeWindowBackBuffer
			// is broadcast on the game thread, so game-thread Add/Remove is single-threaded use of a
			// non-TS delegate - exactly what it is designed for. On 5.3 the detach path is therefore
			// asymmetric on purpose: the back-buffer delegate is mutated on the rendering thread and
			// this one on the game thread, because they are broadcast on different threads.
			if (bDelegateRegistered && !PreResizeHandle.IsValid())
			{
				PreResizeHandle = Renderer->OnPreResizeWindowBackBuffer().AddLambda(
					[this](void* /*ViewportRHIPtr*/)
					{
						InvalidateBackBufferWraps();
					});
			}
#endif

			return bDelegateRegistered;
		}

		// Names queued for the next present, each with the token it was queued under. Individually
		// removable, which is what lets CloseSender cancel a single viewport name.
		FSpoutPendingSenders PendingSenders;

		FCriticalSection WindowMutex;
		// Identity of the game viewport's window, resolved on the game thread. Compared against the
		// delegate's window and never dereferenced. Guarded by WindowMutex.
		const SWindow* TargetWindow = nullptr;

		// Non-zero while the back-buffer callback is executing. Read by DetachProducer.
		std::atomic<int32> InFlightBroadcasts{0};

		FDelegateHandle BackBufferHandle;
		// Subscription to FSlateRenderer::OnPreResizeWindowBackBuffer, which exists on
		// UE 5.3-5.7 only (5.8 removed it with the viewport-provider refactor). Registered and
		// removed alongside BackBufferHandle; unused when the provider path is compiled.
		FDelegateHandle PreResizeHandle;
		// Whether the Slate back-buffer delegate is subscribed. Distinct from
		// FSpoutSenderRegistration, which tracks whether Spout accepted a sender NAME; this is only
		// about this object's hook into the renderer.
		bool bDelegateRegistered = false;
		// Wraps of the swap chain's back buffers. Render-thread only, under the D3D11 context mutex.
		FSpoutWrapCache BackBufferWrapCache;
	};

	/**
	 * Brings the shared texture and the Spout registration for one sender into agreement with the
	 * requested geometry, and reports whether the sender is publishable afterwards.
	 *
	 * This never returns early on the strength of an existing texture. Registration is re-published
	 * (which is a comparison and nothing more once Active) on every call, so an attempt that failed
	 * on an earlier frame is retried on this one.
	 */
	bool EnsureSharedSenderTexture(FSpoutSharedSender& Sender, uint32 Width, uint32 Height, DXGI_FORMAT Format, FSpoutD3DContext& Context)
	{
		// Sender state can be accessed on the render thread and by BP calls.
		FScopeLock SenderLock(&Sender.ResourceMutex);

		if (!Sender.HasValidName())
		{
			// The constructor already reported why; nothing here can recover it.
			return false;
		}

		FSpoutSdkSenderNameBackend Backend;
		const ANSICHAR* Utf8Name = Sender.GetUtf8Name();
		const int32 FailuresBefore = Sender.Registration.GetConsecutiveFailureCount();

		const bool bTextureMatchesRequest =
			Sender.SharedTexture
			&& Sender.Width == Width
			&& Sender.Height == Height
			&& Sender.SharedFormat == Format;

		if (!bTextureMatchesRequest)
		{
			// Geometry or format changed (or there is no texture yet). The old shared texture cannot
			// receive this frame, so it goes, and with it the handle Spout is advertising.
			//
			// Invalidate() releases the name before the new texture exists. That is deliberate: for
			// the duration of the recreate the previously advertised handle refers to a texture this
			// sender has already dropped, and a receiver that opens it gets a dead resource. Briefly
			// leaving the sender list is the honest state, and it makes the recreate deterministic —
			// the Publish below always takes the clean CreateSender path.
			Sender.Registration.Invalidate(Backend);

			Sender.SharedTexture.Reset();
			Sender.SharedHandle = nullptr;
			Sender.CachedWrappedResource.Reset();
			Sender.CachedNativeResource = nullptr;
			Sender.Width = Width;
			Sender.Height = Height;
			Sender.SharedFormat = Format;

			spoutDirectX* SpoutDX = Context.GetSpoutDirectX();
			ID3D11Device* D3D11Device = Context.GetD3D11Device();
			if (!SpoutDX || !D3D11Device)
			{
				if (Sender.RegistrationLogThrottle.ShouldLog())
				{
					UE_LOG(LogSpoutPlugin, Error,
						TEXT("Spout sender '%s': the D3D11/Spout interop context is unavailable; cannot create a shared texture. State=%s."),
						*Sender.Name.ToString(),
						LexToString(Sender.Registration.GetState()));
				}
				return false;
			}

			// Create into locals so a failure cannot leave a half-assigned texture/handle pair on
			// the entry. Spout creates a DX11 shared texture and returns its shared handle.
			ComPtr<ID3D11Texture2D> NewSharedTexture;
			HANDLE NewSharedHandle = nullptr;
			const bool bCreated = SpoutDX->CreateSharedDX11Texture(
				D3D11Device,
				Width,
				Height,
				Format,
				NewSharedTexture.GetAddressOf(),
				NewSharedHandle
			);

			if (!bCreated || !NewSharedTexture || !NewSharedHandle)
			{
				// Registration is already Invalidated above, so the entry stays retryable and
				// nothing stale is advertised.
				if (Sender.RegistrationLogThrottle.ShouldLog())
				{
					UE_LOG(LogSpoutPlugin, Error,
						TEXT("Spout sender '%s': failed to create a %ux%u shared texture (%s). ")
						TEXT("The sender is not publishing; it will retry on later frames. Consecutive failures: %d."),
						*Sender.Name.ToString(),
						Width,
						Height,
						*SpoutFormatPolicy::DescribeDxgiFormat(static_cast<uint32>(Format)),
						Sender.Registration.GetConsecutiveFailureCount());
				}
				return false;
			}

			Sender.SharedTexture = MoveTemp(NewSharedTexture);
			Sender.SharedHandle = NewSharedHandle;
		}

		// Register the name, or republish it after a resize/handle change, or confirm the existing
		// advertisement still matches. Every SDK return value is checked inside Publish.
		const bool bActive = Sender.Registration.Publish(
			Backend,
			Utf8Name,
			Width,
			Height,
			Sender.SharedHandle,
			static_cast<uint32>(Format));

		if (!bActive)
		{
			if (Sender.RegistrationLogThrottle.ShouldLog())
			{
				UE_LOG(LogSpoutPlugin, Error,
					TEXT("Spout sender '%s': Spout refused the registration for %ux%u %s. ")
					TEXT("State=%s, consecutive failures=%d. The name is not advertised and no frames are published; ")
					TEXT("it will retry on later frames. Check that the sender limit (SetMaxSenders) has not been reached."),
					*Sender.Name.ToString(),
					Width,
					Height,
					*SpoutFormatPolicy::DescribeDxgiFormat(static_cast<uint32>(Format)),
					LexToString(Sender.Registration.GetState()),
					Sender.Registration.GetConsecutiveFailureCount());
			}
			return false;
		}

		// The name is now advertised, so open its access mutex and frame-count semaphore.
		// Idempotent and keyed to the name, so this is a comparison on every later frame and it
		// survives a resize untouched: the shared texture is recreated, but the mutex and the
		// semaphore belong to the NAME and stay valid across it.
		//
		// Deliberately after Publish rather than at construction. A stream whose name Spout refused
		// must not be holding that name's cross-process mutex, because the process that did get the
		// name owns it.
		if (!Sender.FrameSync.IsOpen() && !Sender.FrameSync.Open(Utf8Name))
		{
			// Not fatal: without the mutex the copy is simply unsynchronised, so frames still
			// flow. Worth saying once per interval because tearing against a well-behaved
			// receiver becomes possible again.
			if (Sender.RegistrationLogThrottle.ShouldLog())
			{
				UE_LOG(LogSpoutPlugin, Warning,
					TEXT("Spout sender '%s': could not create the shared-texture access mutex; frames are published without cross-process locking."),
					*Sender.Name.ToString());
			}
		}

		if (FailuresBefore > 0)
		{
			// Recovery is worth one line: it closes out the failure messages above.
			Sender.RegistrationLogThrottle.Reset();
			UE_LOG(LogSpoutPlugin, Log,
				TEXT("Spout sender '%s': registration recovered after %d failed attempt(s); now publishing %ux%u %s."),
				*Sender.Name.ToString(),
				FailuresBefore,
				Width,
				Height,
				*SpoutFormatPolicy::DescribeDxgiFormat(static_cast<uint32>(Format)));
		}

		return true;
	}

	ComPtr<ID3D11Texture2D> GetWrappedResource(FSpoutSharedSender& Sender, ID3D12Resource* NativeResource, SpoutRHIState::EWrapState WrapState)
	{
		// Cache the wrapped resource per native D3D12 texture to avoid re-wrapping every frame.
		FScopeLock SenderLock(&Sender.ResourceMutex);
		if (Sender.CachedNativeResource != NativeResource || !Sender.CachedWrappedResource)
		{
			Sender.CachedWrappedResource = WrapD3D12Resource(NativeResource, WrapState);
			Sender.CachedNativeResource = NativeResource;
		}

		return Sender.CachedWrappedResource;
	}
}

bool FSpoutSender::Send(FName SpoutName, ESpoutSendTextureFrom SendTextureFrom, UTextureRenderTarget2D* TextureRenderTarget2D)
{
	// Validate the name before touching the interop context or queueing anything. NAME_None, an
	// empty name, and a name whose UTF-8 encoding overflows Spout's 256-byte field are all rejected
	// here rather than reaching the SDK.
	const SpoutValidation::EResult NameResult = SpoutValidation::ValidateName(SpoutName);
	if (NameResult != SpoutValidation::EResult::Ok)
	{
		static FSpoutLogThrottle NameLogThrottle(5.0);
		if (NameLogThrottle.ShouldLog())
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("SpoutSender: rejected sender name '%s' (%s). Names must be non-empty and at most %d bytes when UTF-8 encoded."),
				*SpoutName.ToString(),
				SpoutValidation::ToString(NameResult),
				SpoutValidation::MaxSenderNameUtf8Bytes);
		}
		return false;
	}

	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	Context.InitializeIfNeeded();

	if (!Context.IsSpoutAvailable())
	{
		return false;
	}

	// Take the token before anything is queued. An unset token means the session is stopping, so the
	// request is refused rather than dropped into a queue the teardown is about to invalidate.
	const FSpoutStreamToken Token = FSpoutSession::Get().OpenStream(SpoutName, ESpoutType::Sender);
	if (!Token.IsSet())
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
		return FSpoutViewportSender::Get().QueueSend(Token);
	}

	FSpoutSendSource SendSource;
	if (!BuildSendSource(SendTextureFrom, TextureRenderTarget2D, SendSource))
	{
		return false;
	}

	// The copy must run on the render thread to safely touch RHI resources. The token travels with
	// it so the Send/Close-in-one-frame case stays correct: the command checks the token before it
	// creates a registry entry or registers the name.
	ENQUEUE_RENDER_COMMAND(SpoutSenderCmd)(
		[Token, SendSource](FRHICommandListImmediate& RHICmdList)
		{
			if (!SendSource.SourceRHI.IsValid())
			{
				return;
			}

			// Single destination; the sender's own wrap cache is keyed to this source.
			SendTextureOnRenderThread(
				MakeArrayView(&Token, 1),
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
	// Cancel first. Everything after this is cleanup of work that already exists; the cancel is what
	// stops work that is queued but has not run from re-creating any of it. Scoped to the Sender
	// role, so a Receiver with the same FName is untouched.
	FSpoutSession::Get().CancelStream(SpoutName, ESpoutType::Sender);

	// Drop the name from the viewport queue, so a closed viewport sender is not published again at
	// the next present.
	const bool bWasPending = FSpoutViewportSender::Get().RemovePending(SpoutName);

	// Registry is keyed by (Name, Type); Close must scope to the Sender entry only,
	// leaving any Receiver entry with the same FName untouched.
	const bool bHadEntry = FSpoutSenderRegistry::Get().Find(SpoutName, ESpoutType::Sender).IsValid();
	if (bHadEntry)
	{
		FSpoutSenderRegistry::Get().Remove(SpoutName, ESpoutType::Sender);
	}

	if (bHadEntry || bWasPending)
	{
		UE_LOG(LogSpoutPlugin, Log, TEXT("Closed Spout Sender: %s"), *SpoutName.ToString());
	}
}

void FSpoutSender::DetachProducers()
{
	FSpoutViewportSender::Get().DetachProducer();
}

void FSpoutSender::ClearPendingWork()
{
	FSpoutViewportSender::Get().ClearPending();
}

void FSpoutSender::ReleaseNativeResources()
{
	FSpoutViewportSender::Get().ReleaseNativeResources();
}
