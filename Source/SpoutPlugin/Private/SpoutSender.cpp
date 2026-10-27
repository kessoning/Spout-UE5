// Copyright (c) 2023 RockmanZuyi
// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the Spout sender flow by copying UE D3D12 textures into shared DX11 textures
 * via D3D11-on-12, coordinating with the sender registry and D3D context.
 */
#include "SpoutSender.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "SpoutAsyncSend.h"
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
#include "SpoutLimits.h"
#include "SpoutInstrument.h"

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
#include "Misc/ScopeExit.h"

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

bool SpoutAsyncSendRefusesSynchronousSend(FSpoutSharedSender& Entry)
{
	if (Entry.AsyncSendMode != 1)
	{
		return false;
	}
	++Entry.RefusedSynchronousSends;
	if (Entry.RefusedSendLogThrottle.ShouldLog())
	{
		UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout sender '%s': a viewport send to a name whose render-target sender uses the asynchronous send (r.Spout.Dev.AsyncSend) was refused; ")
			TEXT("one name is published by one source."), *Entry.Name.ToString());
	}
	return true;
}

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
		// Game-thread resource pointer; its RHI texture is resolved by the queued render command.
		FTextureRenderTargetResource* Resource = nullptr;
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
			int64 FrameBytes = 0;
			if (!SpoutLimits::FrameBytes(TextureRenderTarget2D->SizeX, TextureRenderTarget2D->SizeY, RenderTargetFormat, FrameBytes)) return false;
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

			// Only take the game-thread resource pointer here. UE queues resource deletion
			// on the render thread after this request, or fences it during GC. The queued
			// command below precedes either release and resolves the RHI after InitRHI.
			OutSource.Resource = RTResource;

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
#if SPOUT_WITH_INSTRUMENT
	/**
	 * Measurement builds: one send destination's row. Declared before the destination's access
	 * scope, so it is destroyed after it: the release time is stamped once access has been given
	 * back, on every path out of the loop body.
	 */
	struct FSendRecordScope
	{
		bool bEnabled;
		SpoutInstrument::FRecord Rec;
		TArray<SpoutInstrument::FRecord, TInlineAllocator<4>>& Records;
		FSendRecordScope(bool bInEnabled, TArray<SpoutInstrument::FRecord, TInlineAllocator<4>>& InRecords) : bEnabled(bInEnabled), Records(InRecords) {}
		~FSendRecordScope()
		{
			if (bEnabled)
			{
				Rec.TRelease = SpoutInstrument::Now();
				Records.Add(Rec);
			}
		}
	};
#endif

	/**
	 * The send on the shared-slot path (SpoutSharedSlot.h), used when the process latched it. Nothing of
	 * Unreal's is wrapped for D3D11 and the D3D11-on-12 device is not used.
	 *
	 * Unreal's queue copies the source into the source's slot and signals the filled fence; the existing submission
	 * call puts that on the GPU; the render thread waits, bounded and holding no access, until it is done. Then, one
	 * name at a time and under that name's access, the native device copies the slot into the name's shared texture
	 * and the render thread waits until that copy has finished (or a device was removed) before the frame is
	 * announced and access released: completion precedes announcement and mutex release, as on the wrap path. A
	 * timed-out fill publishes nothing this frame; its copy still runs and the next fill is ordered behind it on the
	 * same queue.
	 *
	 * SlotHolder and RefusedFor belong to the source: the viewport sender's own, or the first destination's entry for
	 * a render target (a render-target send always has exactly one name).
	 */
	void SendThroughSharedSlot(
		TConstArrayView<FSpoutSendDestination> Destinations,
		const FTextureRHIRef& SourceRHI,
		bool bIsViewport,
		FRHICommandListImmediate& RHICmdList,
		const SpoutRHIState::FCopyBracket& Bracket,
		DXGI_FORMAT SharedFormat,
		TUniquePtr<FSpoutSlotSend>* SlotHolder,
		FSpoutSlotSend::FIdentity* RefusedFor)
	{
		FSpoutGpuRetirement::Get().Poll();
		FSpoutSharedSender& First = *Destinations[0].Entry;
		if (!SlotHolder || !RefusedFor)
		{
			SlotHolder = &First.SlotSend;
			RefusedFor = &First.SlotSendRefusedFor;
		}
		FSpoutSlotSend::FIdentity Identity;
		Identity.Width = static_cast<uint32>(SourceRHI->GetDesc().Extent.X);
		Identity.Height = static_cast<uint32>(SourceRHI->GetDesc().Extent.Y);
		Identity.Format = SharedFormat;
		Identity.PixelFormat = SourceRHI->GetFormat();
		if (*SlotHolder && (*SlotHolder)->GetIdentity() != Identity)
		{
			// Retired with its outstanding work, never dropped.
			SlotHolder->Reset();
		}
		if (!*SlotHolder)
		{
			if (*RefusedFor == Identity)
			{
				return;
			}
			const FString What = bIsViewport ? FString(TEXT("viewport sender")) : FString::Printf(TEXT("sender '%s'"), *First.Name.ToString());
			FString Why;
			*SlotHolder = FSpoutSlotSend::Create(What, Identity, Why);
			if (!*SlotHolder)
			{
				*RefusedFor = Identity;
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout %s: the shared-slot send could not start for %ux%u %s (%s). Nothing is published until the source's size or format changes."),
					*What, Identity.Width, Identity.Height, *SpoutFormatPolicy::DescribeDxgiFormat(static_cast<uint32>(SharedFormat)), *Why);
				return;
			}
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout %s: publishing through the shared-slot path (%ux%u %s)."),
				*What, Identity.Width, Identity.Height, *SpoutFormatPolicy::DescribeDxgiFormat(static_cast<uint32>(SharedFormat)));
		}
		FSpoutSlotSend& Slot = **SlotHolder;
		if (Slot.HasFailed() || !Slot.CanWrite())
		{
			// Stopped for this size and format (logged once when it stopped), or a native copy out of the slot is
			// not proven finished: nothing is queued into the slot.
			return;
		}

		SPOUT_INSTRUMENT(
			const bool bInstrument = SpoutInstrument::IsEnabled();
			uint64 TDrain0 = 0, TDrain1 = 0;
			TArray<SpoutInstrument::FRecord, TInlineAllocator<4>> Records;
			if (bInstrument) SpoutInstrument::Calibrate(FSpoutD3DContext::Get().GetD3D12CommandQueue());
		)
		SpoutSlotRules::EWait Filled = SpoutSlotRules::EWait::TimedOut;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SendSlotFill);
			const uint64 FillValue = Slot.QueueFill(RHICmdList, SourceRHI, Bracket.EntryBefore, Bracket.CopyAccess, Bracket.ExitAfter);
			SPOUT_INSTRUMENT(if (bInstrument) TDrain0 = SpoutInstrument::Now();)
			SubmitSpoutInteropTransitions(RHICmdList);
			Filled = Slot.WaitFilled(FillValue);
			SPOUT_INSTRUMENT(if (bInstrument) TDrain1 = SpoutInstrument::Now();)
		}
		if (Filled != SpoutSlotRules::EWait::Reached)
		{
			if (Filled == SpoutSlotRules::EWait::TimedOut && First.AccessLogThrottle.ShouldLog())
			{
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout: the copy into the send slot did not finish within %u ms; this frame was not published. The next frame is."),
					FSpoutSlotSend::FilledTimeoutMs);
			}
			return;
		}

		for (const FSpoutSendDestination& Destination : Destinations)
		{
			SPOUT_INSTRUMENT(
				FSendRecordScope RecScope(bInstrument, Records);
				SpoutInstrument::FRecord& Rec = RecScope.Rec;
				if (bInstrument)
				{
					if (Destination.Entry->InstrumentId < 0) Destination.Entry->InstrumentId = SpoutInstrument::RegisterStream(Destination.Entry->Name, bIsViewport ? TEXT("SV") : TEXT("S"));
					Rec.Kind = TEXT('S');
					Rec.Stream = Destination.Entry->InstrumentId;
					Rec.FrameRT = GFrameCounterRenderThread;
					Rec.TDrain0 = TDrain0; Rec.TDrain1 = TDrain1;
					Rec.TAccess0 = SpoutInstrument::Now();
				}
			)
			ESpoutSlotCopy Result = ESpoutSlotCopy::NotStarted;
			{
				FSpoutTextureAccessScope Access(Destination.Entry->FrameSync, Destination.SharedTexture.GetReference());
				SPOUT_INSTRUMENT(if (bInstrument) { Rec.TAcquire = SpoutInstrument::Now(); Rec.Acquired = Access.IsAcquired() ? 1 : 0; })
				if (!Access.IsAcquired())
				{
					if (Destination.Entry->AccessLogThrottle.ShouldLog())
					{
						UE_LOG(LogSpoutPlugin, Warning,
							TEXT("Spout sender '%s': shared-texture access was unavailable because a receiver still held it after waiting up to %u ms (r.Spout.AccessWaitMs), ")
							TEXT("so this frame was skipped and not published. The next frame is published once the texture is free."),
							*Destination.Entry->Name.ToString(), SpoutLimits::AccessWaitMs());
					}
					continue;
				}
				Result = Slot.DrainInto(Destination.SharedTexture.GetReference());
				SPOUT_INSTRUMENT(if (bInstrument) { Rec.TCopyRec = SpoutInstrument::Now(); Rec.TGpuDone = Rec.TCopyRec; Rec.Hr = static_cast<int32>(Result); })
				if (Result == ESpoutSlotCopy::Completed)
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SendPublishFrame);
					Destination.Entry->FrameSync.PublishFrame();
					++Destination.Entry->SlotTransfers;
					SPOUT_INSTRUMENT(if (bInstrument) { Rec.TPublish = SpoutInstrument::Now(); Rec.SenderFrame = Destination.Entry->FrameSync.GetLastObservedSenderFrame(); })
				}
				else if (Result == ESpoutSlotCopy::SignalFailed && Slot.GetOutstanding().GetProof() != SpoutSlotRules::EProof::Proven)
				{
					// The texture may still be written: it is retired, never released, when it is replaced.
					FScopeLock SenderLock(&Destination.Entry->ResourceMutex);
					Destination.Entry->bSharedTextureWriteUnproven = true;
				}
			}
			// Access has been given back. A keyed texture's release reaches peers when the native context is flushed.
			D3D11_TEXTURE2D_DESC SharedDesc = {};
			Destination.SharedTexture->GetDesc(&SharedDesc);
			if ((SharedDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0)
			{
				Slot.FlushKeyedRelease();
			}
			if (Result != ESpoutSlotCopy::Completed)
			{
				// The slot stopped (failed signal or removal): nothing more can be drained from it this frame.
				break;
			}
		}
		SPOUT_INSTRUMENT(
			if (bInstrument)
			{
				const uint64 TRestore = SpoutInstrument::Now();
				for (SpoutInstrument::FRecord& R : Records) { R.TRestore = TRestore; SpoutInstrument::Push(R); }
			}
		)
	}

	/**
	 * The asynchronous sender's installation (FSpoutAsyncSendStream::FInstaller), on its worker thread. An identity the
	 * entry already publishes needs nothing (the synchronous send's EnsureSharedSenderTexture would only compare); a new
	 * one is installed under ExecuteIfCurrent, exactly as the render thread installs it for the synchronous send, so a
	 * close or a session stop prevents it.
	 */
	bool InstallForAsyncSend(FSpoutSharedSender& Sender, const FSpoutStreamToken& Token, const FSpoutAsyncSendStream::FIdentity& Identity, TRefCountPtr<ID3D11Texture2D>& OutTexture)
	{
		OutTexture = nullptr;
		{
			FScopeLock SenderLock(&Sender.ResourceMutex);
			if (Sender.Registration.IsActive() && Sender.SharedTexture && Sender.Width == Identity.Width && Sender.Height == Identity.Height
				&& Sender.SharedFormat == Identity.Format)
			{
				OutTexture = TRefCountPtr<ID3D11Texture2D>(Sender.SharedTexture.Get());
				return true;
			}
		}
		bool bInstalled = false;
		FSpoutSession::Get().ExecuteIfCurrent(Token, [&]()
		{
			if (!EnsureSharedSenderTexture(Sender, Identity.Width, Identity.Height, Identity.Format, FSpoutD3DContext::Get()))
			{
				return;
			}
			FScopeLock SenderLock(&Sender.ResourceMutex);
			if (Sender.Registration.IsActive() && Sender.SharedTexture)
			{
				OutTexture = TRefCountPtr<ID3D11Texture2D>(Sender.SharedTexture.Get());
				bInstalled = true;
			}
		});
		return bInstalled;
	}

	/**
	 * The asynchronous send (r.Spout.Dev.AsyncSend, 1 by default; SpoutAsyncSend.h): a render-target send on the shared-slot path goes
	 * to the entry's asynchronous sender. Returns false when the entry uses the synchronous send (the switch was off at
	 * its first render-target send, or its asynchronous sender could not start), true otherwise: the send was handed
	 * over (accepted or counted as not accepted) or its token is no longer current (nothing queued, as the synchronous
	 * path does).
	 */
	bool SendThroughAsyncStream(const FSpoutStreamToken& Token, const FTextureRHIRef& SourceRHI, FRHICommandListImmediate& RHICmdList, DXGI_FORMAT SharedFormat,
		const FSpoutSendOrigin& Origin)
	{
		TSharedPtr<FSpoutSharedSender> Sender;
		if (!FSpoutSession::Get().ExecuteIfCurrent(Token, [&]() { Sender = FSpoutSenderRegistry::Get().FindOrAdd(Token.Name, ESpoutType::Sender); }) || !Sender)
		{
			return true;
		}
		if (Sender->AsyncSendMode < 0)
		{
			Sender->AsyncSendMode = SpoutLimits::DevAsyncSend() ? 1 : 0;
		}
		if (Sender->AsyncSendMode == 0)
		{
			return false;
		}
		// The render thread is the only writer of AsyncSend while the entry lives (the destructor runs when no command
		// holds the entry), so it reads it without the entry's lock, which the worker holds while it installs.
		TSharedPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe> Stream = Sender->AsyncSend;
		if (!Stream)
		{
			FString Why;
			Stream = FSpoutAsyncSendStream::Create(*Sender, &InstallForAsyncSend, Why);
			if (!Stream)
			{
				Sender->AsyncSendMode = 0;
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout sender '%s': the asynchronous send (r.Spout.Dev.AsyncSend) could not start (%s); this sender uses the synchronous send until it is closed."),
					*Sender->Name.ToString(), *Why);
				return false;
			}
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout sender '%s': publishing through the asynchronous send."), *Sender->Name.ToString());
			FScopeLock SenderLock(&Sender->ResourceMutex);
			Sender->AsyncSend = Stream;
		}
		FSpoutAsyncSendStream::FIdentity Identity;
		Identity.Width = static_cast<uint32>(SourceRHI->GetDesc().Extent.X);
		Identity.Height = static_cast<uint32>(SourceRHI->GetDesc().Extent.Y);
		Identity.Format = SharedFormat;
		Identity.PixelFormat = SourceRHI->GetFormat();
		Stream->Send(Token, RHICmdList, SourceRHI, SpoutRHIState::GetCopyBracket(SpoutRHIState::EResourceRole::SenderRenderTarget), Identity, Origin);
		// If a close removed the entry meanwhile, this is its last reference: its destructor stops the stream here,
		// which resolves the frame just accepted (its fill is already dispatched) before the worker exits.
		return true;
	}

	void SendTextureOnRenderThread(
		TConstArrayView<FSpoutStreamToken> Tokens,
		const FTextureRHIRef& SourceRHI,
		bool bIsViewport,
		FRHICommandListImmediate& RHICmdList,
		FSpoutWrapCache* SourceWrapCache,
		TUniquePtr<FSpoutSlotSend>* SourceSlot,
		FSpoutSlotSend::FIdentity* SourceSlotRefusedFor,
		const FSpoutSendOrigin& Origin = FSpoutSendOrigin())
	{
		if (Tokens.Num() == 0 || !SourceRHI.IsValid())
		{
			return;
		}

		// Measurement builds only: r.Spout.Instrument.Bypass bit 1 returns before any Spout work,
		// so the same load can be measured with no Spout work.
		SPOUT_INSTRUMENT(if (SpoutInstrument::BypassMask() & 2) return;)

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
		if (Desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || Desc.MipLevels != 1
			|| Desc.DepthOrArraySize != 1 || Desc.SampleDesc.Count != 1 || Desc.SampleDesc.Quality != 0)
		{
			if (GSenderFormatLogThrottle.ShouldLog())
			{
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout sender requires a single-mip, single-sample 2D texture; disable Auto Generate Mips on the source."));
			}
			return;
		}

		const SpoutFormatPolicy::FSenderFormat SourceFormat = SpoutFormatPolicy::ResolveSenderFormat(
			Desc.Format, EnumHasAnyFlags(SourceRHI->GetDesc().Flags, ETextureCreateFlags::SRGB));
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

		// The asynchronous send (r.Spout.Dev.AsyncSend, 1 by default): a render-target send (always one name) may go to the entry's
		// asynchronous sender, which installs the texture and registration on its own worker. With the switch off and no
		// asynchronous sender alive, nothing of this runs.
		if (!bIsViewport && SpoutInterop::IsSharedSlot() && LiveTokens.Num() == 1
			&& (SpoutLimits::DevAsyncSend() || FSpoutAsyncSendStream::GetLiveStreams() > 0)
			&& SendThroughAsyncStream(LiveTokens[0], SourceRHI, RHICmdList, Format, Origin))
		{
			return;
		}

		// Resolve every destination BEFORE anything with a GPU cost. Shared-texture creation and
		// Spout registration are device-level (free-threaded), so they stay outside
		// D3D11ContextMutex — and doing them first means a batch with nothing publishable (a sender
		// whose registration is failing, say) costs no state transition and no RHI flush at all.
		TArray<FSpoutSendDestination, TInlineAllocator<4>> Destinations;

		for (const FSpoutStreamToken& Token : LiveTokens)
		{
			// Cancellation must wait for installation, or prevent it entirely. A separate
			// IsCurrent check leaves a close-between-check-and-insert race.
			Session.ExecuteIfCurrent(Token, [&]()
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SendResolveDestination);
				TSharedPtr<FSpoutSharedSender> Sender = FSpoutSenderRegistry::Get().FindOrAdd(Token.Name, ESpoutType::Sender);
				if (!Sender)
				{
					return;
				}

				// The asynchronous send (r.Spout.Dev.AsyncSend): an entry whose asynchronous sender exists belongs to its worker
				// (shared texture, registration, access, announcement). A synchronous send to the same name (the viewport
				// sender) is refused rather than run beside the worker. Render thread only, like AsyncSendMode.
				if (SpoutAsyncSendRefusesSynchronousSend(*Sender))
				{
					return;
				}

				// Gate on the registration state, not on the texture existing. A live shared texture
				// says nothing about whether Spout accepted the name, and publishing into a texture
				// whose name is unregistered (or whose advertised handle is stale) sends the frames
				// nowhere a receiver can find them.
				if (!EnsureSharedSenderTexture(*Sender, static_cast<uint32>(Desc.Width), static_cast<uint32>(Desc.Height), Format, Context))
				{
					return;
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
			});
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

		// Latched once per process (r.Spout.SharedSlotInterop): the shared-slot path wraps nothing of Unreal's.
		if (SpoutInterop::IsSharedSlot())
		{
			SendThroughSharedSlot(Destinations, SourceRHI, bIsViewport, RHICmdList, Bracket, Format, SourceSlot, SourceSlotRefusedFor);
			return;
		}

		SPOUT_INSTRUMENT(
			const bool bInstrument = SpoutInstrument::IsEnabled();
			uint64 TDrain0 = 0, TDrain1 = 0, TWrap0 = 0, TWrap1 = 0;
			TArray<SpoutInstrument::FRecord, TInlineAllocator<4>> Records;
			if (bInstrument) SpoutInstrument::Calibrate(Context.GetD3D12CommandQueue());
		)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SendTransitionSubmit);
			TransitionSpoutInteropTexture(RHICmdList, SourceRHI, Bracket.EntryBefore, Bracket.CopyAccess);
			SPOUT_INSTRUMENT(if (bInstrument) TDrain0 = SpoutInstrument::Now();)
			SubmitSpoutInteropTransitions(RHICmdList);
			SPOUT_INSTRUMENT(if (bInstrument) TDrain1 = SpoutInstrument::Now();)
		}

		{
			// D3D11 immediate context is not thread-safe; guard its usage (and the wrap caches it
			// shares with the receiver path).
			FScopeLock D3DLock(&Context.GetD3D11ContextMutex());

			ComPtr<ID3D11Texture2D> WrappedSource;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SendWrapSource);
				SPOUT_INSTRUMENT(if (bInstrument) TWrap0 = SpoutInstrument::Now();)
				WrappedSource = SourceWrapCache
					? SourceWrapCache->GetOrCreate(NativeRes, Bracket.WrapState)
					: GetWrappedResource(*Destinations[0].Entry, NativeRes, Bracket.WrapState);
				SPOUT_INSTRUMENT(if (bInstrument) TWrap1 = SpoutInstrument::Now();)
			}

			if (WrappedSource)
			{
				// One acquire/release scope covers every copy: the source is wrapped once and
				// transitioned once no matter how many names publish it.
				FScopedD3D11On12Acquire Acquire(WrappedSource.Get());
				if (Acquire.IsValid() && Context.PrepareGpuCompletion())
				{
					for (const FSpoutSendDestination& Destination : Destinations)
					{
						// Take this name's cross-process access mutex around its copy, one name at
						// a time. Never several at once: each destination is a different named
						// mutex, and holding two of them would make the lock order depend on the
						// order names happen to be queued in.
						//
						// The attempt waits at most r.Spout.AccessWaitMs for a peer to finish. A
						// failed wait means a peer is still reading this texture. Skipping is
						// the correct response -- overwriting it would tear the frame the peer is
						// mid-read of -- and the scope owns nothing, so nothing is released.
						SPOUT_INSTRUMENT(
							FSendRecordScope RecScope(bInstrument, Records);
							SpoutInstrument::FRecord& Rec = RecScope.Rec;
							if (bInstrument)
							{
								if (Destination.Entry->InstrumentId < 0) Destination.Entry->InstrumentId = SpoutInstrument::RegisterStream(Destination.Entry->Name, bIsViewport ? TEXT("SV") : TEXT("S"));
								Rec.Kind = TEXT('S');
								Rec.Stream = Destination.Entry->InstrumentId;
								Rec.FrameRT = GFrameCounterRenderThread;
								Rec.TDrain0 = TDrain0; Rec.TDrain1 = TDrain1; Rec.TWrap0 = TWrap0; Rec.TWrap1 = TWrap1;
								Rec.TAccess0 = SpoutInstrument::Now();
							}
						)
						FSpoutTextureAccessScope Access(
							Destination.Entry->FrameSync,
							Destination.SharedTexture.GetReference());
						SPOUT_INSTRUMENT(if (bInstrument) { Rec.TAcquire = SpoutInstrument::Now(); Rec.Acquired = Access.IsAcquired() ? 1 : 0; })

						if (!Access.IsAcquired())
						{
							if (Destination.Entry->AccessLogThrottle.ShouldLog())
							{
								UE_LOG(LogSpoutPlugin, Warning,
									TEXT("Spout sender '%s': shared-texture access was unavailable because a receiver still held it after waiting up to %u ms (r.Spout.AccessWaitMs), ")
									TEXT("so this frame was skipped and not published. The next frame is published once the texture is free."),
									*Destination.Entry->Name.ToString(), SpoutLimits::AccessWaitMs());
							}
							continue;
						}

						SPOUT_INSTRUMENT(if (bInstrument) SpoutInstrument::GetGpuBracket().Begin(Context.GetImmediateContext());)
						{
							TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SendCopyResource);
							Context.GetImmediateContext()->CopyResource(
								Destination.SharedTexture.GetReference(),
								WrappedSource.Get());
						}
						SPOUT_INSTRUMENT(if (bInstrument) { SpoutInstrument::GetGpuBracket().End(Context.GetImmediateContext()); Rec.TCopyRec = SpoutInstrument::Now(); })

						// Completion precedes announcement and mutex release. A submitted copy
						// may still be running on the GPU when a foreign device starts reading.
						const HRESULT CompletionResult = Context.WaitForGpuCompletion();
						SPOUT_INSTRUMENT(if (bInstrument) { Rec.TGpuDone = SpoutInstrument::Now(); Rec.Hr = static_cast<int32>(CompletionResult); SpoutInstrument::GetGpuBracket().Resolve(Context.GetImmediateContext(), Rec); })
						if (SUCCEEDED(CompletionResult))
						{
							TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SendPublishFrame);
							Destination.Entry->FrameSync.PublishFrame();
							SPOUT_INSTRUMENT(if (bInstrument) { Rec.TPublish = SpoutInstrument::Now(); Rec.SenderFrame = Destination.Entry->FrameSync.GetLastObservedSenderFrame(); })
						}
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
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SendRestoreTransition);
			TransitionSpoutInteropTexture(RHICmdList, SourceRHI, Bracket.CopyAccess, Bracket.ExitAfter);
		}
		SPOUT_INSTRUMENT(
			if (bInstrument)
			{
				const uint64 TRestore = SpoutInstrument::Now();
				for (SpoutInstrument::FRecord& R : Records) { R.TRestore = TRestore; SpoutInstrument::Push(R); }
			}
		)
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
			{
				// The cache is otherwise touched only on the render thread under this mutex.
				FSpoutD3DContext& Context = FSpoutD3DContext::Get();
				FScopeLock D3DLock(&Context.GetD3D11ContextMutex());
				BackBufferWrapCache.Reset();
			}
			{
				// Retired with its outstanding GPU work, never dropped (SpoutSharedSlot.h).
				FScopeLock SlotLock(&ViewportSlotMutex);
				ViewportSlot.Reset();
				ViewportSlotRefusedFor = FSpoutSlotSend::FIdentity();
			}
			FSpoutGpuRetirement::Get().Poll();
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
							SPOUT_INSTRUMENT(
								const bool bInstrumentViewport = SpoutInstrument::IsEnabled();
								const uint64 TViewport0 = bInstrumentViewport ? SpoutInstrument::Now() : 0;
							)
							{
								// The viewport's slot is used here and released by ReleaseNativeResources.
								FScopeLock SlotLock(&ViewportSlotMutex);
								SendTextureOnRenderThread(
									TokensToSend,
									BackBufferCopy,
									true,
									RHICmdList,
									&BackBufferWrapCache,
									&ViewportSlot,
									&ViewportSlotRefusedFor
								);
							}
							SPOUT_INSTRUMENT(
								const uint64 TViewport1 = bInstrumentViewport ? SpoutInstrument::Now() : 0;
								uint64 TViewport2 = TViewport1;
							)

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
							// caches and is unaffected. The shared-slot path holds no back buffer
							// at all, so it has nothing to drop.
							if (!SpoutInterop::IsSharedSlot())
							{
								InvalidateBackBufferWraps();
							}
							SPOUT_INSTRUMENT(if (bInstrumentViewport) TViewport2 = SpoutInstrument::Now();)
#endif
							SPOUT_INSTRUMENT(
								if (bInstrumentViewport)
								{
									SpoutInstrument::FRecord ViewportRecord;
									ViewportRecord.Kind = TEXT('V');
									ViewportRecord.Stream = -2;
									ViewportRecord.FrameRT = GFrameCounterRenderThread;
									ViewportRecord.TDetect = TViewport0;
									ViewportRecord.TRestore = TViewport1;
									ViewportRecord.TWrap0 = TViewport1;
									ViewportRecord.TWrap1 = TViewport2;
									SpoutInstrument::Push(ViewportRecord);
								}
							)
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
		// The shared-slot path's slot for the viewport source, and the source identity it was last refused for.
		// Used by the send command and released by ReleaseNativeResources, both under ViewportSlotMutex. Holds no
		// back buffer: the back buffer is only the source of an RHI copy into it.
		FCriticalSection ViewportSlotMutex;
		TUniquePtr<FSpoutSlotSend> ViewportSlot;
		FSpoutSlotSend::FIdentity ViewportSlotRefusedFor;
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
		if (!Sender.CanRetry(Width, Height, Format)) return false;
		TRACE_CPUPROFILER_EVENT_SCOPE(Spout_EnsureSharedSenderTexture);

		if (!Sender.HasValidName())
		{
			// The constructor already reported why; nothing here can recover it.
			return false;
		}

		FSpoutSdkSenderNameBackend Backend;
		const ANSICHAR* Utf8Name = Sender.GetUtf8Name();
		bool bFrameSyncOpen = Sender.FrameSync.IsOpen();
		if (!bFrameSyncOpen)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SenderFrameSyncOpen);
			bFrameSyncOpen = Sender.FrameSync.Open(Utf8Name);
		}
		if (!bFrameSyncOpen)
		{
			Sender.NativeFailure(E_FAIL);
			if (Sender.AccessLogThrottle.ShouldLog())
			{
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout sender '%s': access mutex unavailable; publication deferred."), *Sender.Name.ToString());
			}
			return false;
		}
		const int32 FailuresBefore = Sender.Registration.GetConsecutiveFailureCount();

		const bool bTextureMatchesRequest =
			Sender.SharedTexture
			&& Sender.Width == Width
			&& Sender.Height == Height
			&& Sender.SharedFormat == Format;

		if (!bTextureMatchesRequest)
		{
			int64 FrameBytes = 0;
			if (!SpoutLimits::FrameBytes(Width, Height, SpoutFormatPolicy::ResolveReceiverFormat(Format).PixelFormat, FrameBytes)) return false;
			const auto Reservation = FSpoutResourceReservation::TryReserve(FrameBytes * 2);
			if (!Reservation) return false;
			// Geometry or format changed (or there is no texture yet). The old shared texture cannot
			// receive this frame, so it goes, and with it the handle Spout is advertising.
			//
			// Invalidate() releases the name before the new texture exists. That is deliberate: for
			// the duration of the recreate the previously advertised handle refers to a texture this
			// sender has already dropped, and a receiver that opens it gets a dead resource. Briefly
			// leaving the sender list is the honest state, and it makes the recreate deterministic —
			// the Publish below always takes the clean CreateSender path.
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SenderInvalidateName);
				Sender.Registration.Invalidate(Backend);
			}

			if (Sender.bSharedTextureWriteUnproven)
			{
				// A native copy into it was never proven finished: only the native device's removal may release it.
				SpoutRetireUnprovenTexture(MoveTemp(Sender.SharedTexture), FString::Printf(TEXT("the shared texture of sender '%s'"), *Sender.Name.ToString()));
				Sender.bSharedTextureWriteUnproven = false;
			}
			Sender.SharedTexture.Reset();
			Sender.SharedHandle = nullptr;
			Sender.CachedWrappedResource.Reset();
			Sender.CachedNativeResource = nullptr;
			Sender.Width = Width;
			Sender.Height = Height;
			Sender.SharedFormat = Format;

			spoutDirectX* SpoutDX = Context.GetSpoutDirectX();
			// The shared-slot path creates the texture peers open on its native device, where its copies run; the wrap
			// path creates it on the D3D11-on-12 device. Either way it is an ordinary D3D11 shared texture to a peer.
			ID3D11Device* D3D11Device = SpoutInterop::IsSharedSlot()
				? static_cast<ID3D11Device*>(FSpoutAsyncDevice::Get().GetDevice())
				: Context.GetD3D11Device();
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
			bool bCreated = false;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(Spout_CreateSharedDX11Texture);
				// The SDK is given the device only, and its source is not in this repository: whether it touches the
				// device's immediate context is not known. The shared-slot path's native context is also used by the
				// asynchronous receive's worker, so the call is made under that context's lock (entry lock first, then
				// the context lock: the documented order, SpoutSharedSlot.h).
				FCriticalSection* NativeContextMutex = SpoutInterop::IsSharedSlot() ? &FSpoutAsyncDevice::Get().GetContextMutex() : nullptr;
				if (NativeContextMutex) NativeContextMutex->Lock();
				ON_SCOPE_EXIT { if (NativeContextMutex) NativeContextMutex->Unlock(); };
				bCreated = SpoutDX->CreateSharedDX11Texture(
					D3D11Device,
					Width,
					Height,
					Format,
					NewSharedTexture.GetAddressOf(),
					NewSharedHandle
				);
			}

			if (!bCreated || !NewSharedTexture || !NewSharedHandle)
			{
			Sender.NativeFailure(E_FAIL);
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
			Sender.NativeReservation = Reservation;
		}

		// Register the name, or republish it after a resize/handle change, or confirm the existing
		// advertisement still matches. Every SDK return value is checked inside Publish.
		bool bActive = false;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SenderPublishName);
			bActive = Sender.Registration.Publish(
				Backend,
				Utf8Name,
				Width,
				Height,
				Sender.SharedHandle,
				static_cast<uint32>(Format));
		}

		if (!bActive)
		{
			Sender.NativeFailure(E_FAIL);
			if (Sender.RegistrationLogThrottle.ShouldLog())
			{
				UE_LOG(LogSpoutPlugin, Error,
					TEXT("Spout sender '%s': Spout refused the registration for %ux%u %s. ")
					TEXT("State=%s, consecutive failures=%d. This request publishes no frames and will retry later. ")
					TEXT("Check for a name already in use, a busy registry, or the sender limit being reached."),
					*Sender.Name.ToString(),
					Width,
					Height,
					*SpoutFormatPolicy::DescribeDxgiFormat(static_cast<uint32>(Format)),
					LexToString(Sender.Registration.GetState()),
					Sender.Registration.GetConsecutiveFailureCount());
			}
			return false;
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

		Sender.NativeSuccess();
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
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_Send);
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
	if (!FSpoutSession::Get().IsRunning()) return false;
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
		const FSpoutStreamToken Token = FSpoutSession::Get().OpenStream(SpoutName, ESpoutType::Sender);
		return Token.IsSet() && FSpoutViewportSender::Get().QueueSend(Token);
	}

	FSpoutSendSource SendSource;
	if (!BuildSendSource(SendTextureFrom, TextureRenderTarget2D, SendSource))
	{
		return false;
	}

	const FSpoutStreamToken Token = FSpoutSession::Get().OpenStream(SpoutName, ESpoutType::Sender);
	const auto QueuedWork = FSpoutSession::Get().TryQueue(Token);
	if (!QueuedWork) return false;
	// The copy must run on the render thread to safely touch RHI resources. The token travels with
	// it so the Send/Close-in-one-frame case stays correct: the command checks the token before it
	// creates a registry entry or registers the name.
	FSpoutSession::Get().MarkQueued(Token);
	// Where this send came from, for the asynchronous sender's development outcome log: this game frame and now.
	FSpoutSendOrigin Origin;
	Origin.Tick = static_cast<int64>(GFrameCounter);
	Origin.Qpc = FPlatformTime::Cycles64();
	ENQUEUE_RENDER_COMMAND(SpoutSenderCmd)(
		[Token, SendSource, QueuedWork, Origin](FRHICommandListImmediate& RHICmdList)
		{
			if (!FSpoutSession::Get().IsCurrent(Token) || !SendSource.Resource)
			{
				return;
			}
			const FTextureRHIRef SourceRHI = SendSource.Resource->GetRenderTargetTexture();

			// Single destination; the sender's own wrap cache is keyed to this source.
			SendTextureOnRenderThread(
				MakeArrayView(&Token, 1),
				SourceRHI,
				SendSource.bIsViewport,
				RHICmdList,
				nullptr,
				nullptr,
				nullptr,
				Origin
			);
		});

	return true;
}

void FSpoutSender::Close(FName SpoutName)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_CloseSender);
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
