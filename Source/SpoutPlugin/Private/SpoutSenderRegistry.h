// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Declares the shared sender/receiver registry and state objects that track Spout handles,
 * textures, and cached wrappers across the plugin.
 */
#pragma once

#include "CoreMinimal.h"
#include "SpoutBPFunctionLibrary.h"
#include "SpoutFormatPolicy.h"
#include "SpoutFrameSync.h"
#include "SpoutAsyncReceive.h"
#include "SpoutSharedSlot.h"
#include "SpoutGpuCompletion.h"
#include "SpoutLogThrottle.h"
#include "SpoutSenderRegistration.h"
#include "SpoutLimits.h"
#include <atomic>
#include "RHI.h"
#include "UObject/WeakObjectPtr.h"

class FSpoutAsyncSendStream;

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d11.h>
#include <wrl/client.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

/**
 * Forwards registration calls to the process-global Spout sender-name list.
 *
 * Each call takes the shared Spout mutex internally, so callers must already hold the owning
 * entry's ResourceMutex and must not hold the Spout mutex themselves. That ordering (entry
 * resource state, then the process-local Spout mutex) is the one the rest of the plugin uses.
 */
class FSpoutSdkSenderNameBackend final : public ISpoutSenderNameBackend
{
public:
	virtual bool CreateSender(const ANSICHAR* Utf8Name, uint32 Width, uint32 Height, void* SharedHandle, uint32 Format) override;
	virtual bool UpdateSender(const ANSICHAR* Utf8Name, uint32 Width, uint32 Height, void* SharedHandle, uint32 Format) override;
	virtual void ReleaseSenderName(const ANSICHAR* Utf8Name) override;
};

class FSpoutSharedSender
{
public:
	FSpoutSharedSender(FName InName, ESpoutType InType);
	~FSpoutSharedSender();

	// Releases shared handles and unregisters sender names when applicable.
	void ReleaseResources();

	// Spout sender/receiver identifier (shared across the registry).
	FName Name;

	/**
	 * Null-terminated UTF-8 encoding of Name, produced once at construction by
	 * SpoutValidation::ValidateName. Empty when the name failed validation, which is what keeps an
	 * unusable name from ever reaching the Spout SDK. Read-only after construction.
	 */
	TArray<ANSICHAR> Utf8Name;

	/** True when Utf8Name is a usable, correctly terminated encoding. */
	bool HasValidName() const { return Utf8Name.Num() > 1; }

	/** Utf8Name as a C string, or an empty string when the name is invalid. */
	const ANSICHAR* GetUtf8Name() const { return HasValidName() ? Utf8Name.GetData() : ""; }

	uint32 Width = 0;
	uint32 Height = 0;
	// DXGI format of the shared texture. Part of the recreate identity together
	// with Width/Height: a source format change at the same resolution must recreate the shared
	// texture, otherwise CopyResource is fed mismatched formats and silently drops frames.
	DXGI_FORMAT SharedFormat = DXGI_FORMAT_UNKNOWN;
	// Distinguishes sender vs receiver ownership semantics.
	ESpoutType Type = ESpoutType::Sender;

	/**
	 * Sender role only: explicit registration state.
	 *
	 * Registration is tracked here rather than inferred from SharedTexture being non-null, because
	 * a live texture says nothing about whether Spout accepted the name. Guarded by ResourceMutex.
	 */
	FSpoutSenderRegistration Registration;

	/**
	 * This stream's half of the Spout access-mutex and frame-count protocol.
	 *
	 * One per entry, not one per process: the access mutex, the frame semaphore and the receiver's
	 * last-seen frame count are all per sender NAME. Opened before sender publication or
	 * receiver installation, and closed by ReleaseResources.
	 *
	 * Self-synchronising, so it is deliberately NOT guarded by ResourceMutex; see the lock order in
	 * SpoutFrameSync.h.
	 */
	FSpoutFrameSync FrameSync;

	/**
	 * Rate limiter for the "another process is holding this texture" diagnostic.
	 *
	 * Separate from the registration limiter: a contended texture is a per-frame condition on a
	 * stream that is otherwise completely healthy, and folding it into the registration limiter
	 * would let contention hide a registration failure or the other way round.
	 */
	FSpoutLogThrottle AccessLogThrottle;

	/** Measurement builds: the stream id registered with SpoutInstrument, or -1 until first used. */
	int32 InstrumentId = -1;

	/**
	 * Receiver role only, Async Receive (Beta), off by default: the asynchronous receive of this stream at its
	 * current sender identity, or null while the stream uses the synchronous path. Created only for a session
	 * opened with Async Receive (Beta); created, used and replaced on the render thread; destroyed there or by
	 * ReleaseResources. While it exists it is the only user of FrameSync's access and notification calls, and its
	 * worker thread is the only thread that takes Spout access for this stream. AsyncRefusedFor is the identity
	 * the asynchronous path was last refused for, so a refusal is logged once and not retried on every frame.
	 */
	TUniquePtr<FSpoutAsyncStream> Async;
	FSpoutAsyncStream::FIdentity AsyncRefusedFor;

	/**
	 * Receiver role only: the receive mode of this session. One entry is one session per name, so the mode is
	 * the session's, decided by the first ACCEPTED receive on this entry and never changed for its life; a
	 * different mode needs Close (which removes the entry) and a new receive. Game thread only: the render
	 * commands carry the value they were queued with, which is the same for every command of one entry.
	 */
	bool bReceiveModeEstablished = false;
	bool bAsyncReceiveRequested = false;
	/** Rate limiter for refused receives that asked for the other mode. */
	FSpoutLogThrottle ReceiveModeLogThrottle;

	/**
	 * Receiver role only: what the most recent render-thread receive actually used, written there and read on the
	 * game thread by GetSpoutStreamStatus. bAsyncReceiveActive is true only while this session's worker exists and
	 * has not failed. AsyncReceiveNote says why a session that asked for Async Receive (Beta) runs synchronously.
	 */
	std::atomic<bool> bAsyncReceiveActive{false};
	/**
	 * Receiver role only: frames Async Receive (Beta) delivered (its render-thread copy queued), for the status's
	 * FramesSubmitted. The asynchronous path does not complete transfers through FrameSync, so they are counted here;
	 * cumulative for the entry like FrameSync's own count. Render thread writes, any thread reads. Status only.
	 */
	std::atomic<int64> AsyncDeliveredFrames{0};
	void SetAsyncReceiveNote(const FString& Note) { FScopeLock Lock(&ReceiveModeNoteMutex); AsyncReceiveNote = Note; }
	FString GetAsyncReceiveNote() const { FScopeLock Lock(&ReceiveModeNoteMutex); return AsyncReceiveNote; }

	/**
	 * The shared-slot path (SpoutSharedSlot.h), when the process latched it. Render thread only, apart from
	 * ReleaseResources, which runs when no render command holds the entry any more. Replacing or resetting one of these
	 * hands its objects to FSpoutGpuRetirement with their outstanding work: they are never released directly.
	 *   SlotReceive  receiver role: the slots at the current sender identity
	 *   SlotSend     sender role, render-target source: the slot the source is copied into (the viewport's slot is
	 *                held by the viewport sender)
	 * The RefusedFor identities keep a refusal from being retried, and logged, on every frame.
	 */
	TUniquePtr<FSpoutSlotReceive> SlotReceive;
	FSpoutSlotReceive::FIdentity SlotReceiveRefusedFor;
	TUniquePtr<FSpoutSlotSend> SlotSend;
	FSpoutSlotSend::FIdentity SlotSendRefusedFor;
	/**
	 * Sender role, shared-slot path: a native copy into SharedTexture could not be proven finished (its signal and the
	 * event query after it both failed). The texture is then retired, never released, when it is replaced.
	 */
	bool bSharedTextureWriteUnproven = false;
	/** Transfers this entry completed through the shared-slot path. Diagnostics and tests. */
	std::atomic<int64> SlotTransfers{0};

	/**
	 * Sender role, render-target source, development switch r.Spout.Dev.AsyncSend (SpoutAsyncSend.h): whether this
	 * entry uses the asynchronous sender (-1 undecided, 0 synchronous, 1 asynchronous). Decided on the render thread at
	 * the first send that looks (the switch on, or an asynchronous sender alive) and kept for the entry's life; an entry
	 * that sent synchronously before may move to 1, never back. Render thread only.
	 */
	int8 AsyncSendMode = -1;
	/** Synchronous sends refused because this entry belongs to its asynchronous sender (one name, one source). */
	std::atomic<int64> RefusedSynchronousSends{0};
	/**
	 * The entry's asynchronous sender while it exists. Created on the render thread; assigned and read elsewhere under
	 * ResourceMutex. Its worker is the only thread that uses SharedTexture, the registration, FrameSync's access and
	 * the announcement of this entry. Stopped (owed frames resolved, worker joined) by the destructor before
	 * ReleaseResources, outside ResourceMutex, which the worker takes to install.
	 */
	TSharedPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe> AsyncSend;

	// Shared DX11 texture exposed to other Spout clients (senders only).
	Microsoft::WRL::ComPtr<ID3D11Texture2D> SharedTexture;
	HANDLE SharedHandle = nullptr;

	// CPU-readable staging texture for receiver readback.
	Microsoft::WRL::ComPtr<ID3D11Texture2D> StagingTexture;
	// CPU readback completion belongs to the latched receiver device, not the legacy bridge.
	FSpoutGpuCompletion CpuCompletion;

	// Render-thread-owned completed image and per-destination delivery generations.
	// Weak UObject keys do not keep optional targets alive; the last resource is
	// retained only until that live target is refreshed, pruned, or the session closes.
	struct FDestinationDelivery
	{
		FTextureRHIRef Resource;
		uint64 Frame = 0;
	};
	FTextureRHIRef ReceivedFrame;
	uint64 ReceivedFrameNumber = 0;
	TMap<TWeakObjectPtr<UObject>, FDestinationDelivery> DestinationDeliveries;

	/**
	 * Receiver role only: the last reported compatibility status of the optional output render
	 * target, so a persistent mismatch is logged when the situation changes rather than every
	 * frame. Game thread only.
	 */
	SpoutFormatPolicy::EOutputTargetStatus LastOutputTargetStatus = SpoutFormatPolicy::EOutputTargetStatus::Compatible;

	/** Rate limiters for the per-frame failure diagnostics on this stream. */
	FSpoutLogThrottle OutputTargetLogThrottle;
	FSpoutLogThrottle RegistrationLogThrottle;
	FSpoutLogThrottle FormatLogThrottle;
	FSpoutLogThrottle OpenLogThrottle;
	/** A synchronous send refused because the entry has an asynchronous sender (its own, so it never hides the installer's warnings). */
	FSpoutLogThrottle RefusedSendLogThrottle;

	// Cached D3D11 wrapper for a D3D12 resource.
	// - Sender role: wraps the source UE RHI texture (state = COPY_SOURCE).
	// - Receiver role: wraps the primary destination UE RHI texture (state = COPY_DEST).
	// The role is fixed per FSpoutSharedSender instance because the registry is keyed by (Name, Type).
	Microsoft::WRL::ComPtr<ID3D11Texture2D> CachedWrappedResource;
	void* CachedNativeResource = nullptr;

	// Receiver role only: wraps the optional secondary render-target destination (state = COPY_DEST).
	Microsoft::WRL::ComPtr<ID3D11Texture2D> CachedRTWrappedResource;
	void* CachedRTNativeResource = nullptr;

	// Protects SharedTexture/handle updates across game and render threads.
	FCriticalSection ResourceMutex;
	TSharedPtr<FSpoutResourceReservation, ESPMode::ThreadSafe> NativeReservation;
	// Game-thread admission records; render delivery records are separately owned above.
	TMap<TWeakObjectPtr<UObject>, TSharedPtr<FSpoutResourceReservation, ESPMode::ThreadSafe>> OutputReservations;
	std::atomic<int64> NativeFailures{0};
	std::atomic<int64> LastNativeError{0};
	// Render-thread retry state. A new native identity resets the backoff.
	double NextNativeRetry = 0;
	double RetryDelay = 0.01;
	uint32 RetryWidth = 0, RetryHeight = 0;
	DXGI_FORMAT RetryFormat = DXGI_FORMAT_UNKNOWN;
	HANDLE RetryHandle = nullptr;
	bool CanRetry(uint32 W, uint32 H, DXGI_FORMAT Format, HANDLE Handle = nullptr)
	{
		if (RetryWidth != W || RetryHeight != H || RetryFormat != Format || RetryHandle != Handle)
		{
			RetryWidth = W; RetryHeight = H; RetryFormat = Format; RetryHandle = Handle;
			NextNativeRetry = 0; RetryDelay = 0.01;
		}
		return FPlatformTime::Seconds() >= NextNativeRetry;
	}
	void NativeFailure(HRESULT Error)
	{
		++NativeFailures; LastNativeError.store(static_cast<int64>(Error));
		NextNativeRetry = FPlatformTime::Seconds() + RetryDelay;
		RetryDelay = FMath::Min(1.0, RetryDelay * 2.0);
	}
	void NativeSuccess() { LastNativeError.store(0); NextNativeRetry = 0; RetryDelay = 0.01; }

private:
	/** Guards AsyncReceiveNote only (render thread writes, game thread reads); never held with another lock. */
	mutable FCriticalSection ReceiveModeNoteMutex;
	FString AsyncReceiveNote;
};

class FSpoutSenderRegistry
{
public:
	// Global registry of sender/receiver shared state.
	static FSpoutSenderRegistry& Get();

	// Finds an existing entry keyed by (Name, Type) or creates a new one.
	// Keying on Type prevents a sender and receiver with the same FName from
	// sharing (and corrupting) the same FSpoutSharedSender instance.
	TSharedPtr<FSpoutSharedSender> FindOrAdd(const FName& Name, ESpoutType Type);
	TSharedPtr<FSpoutSharedSender> Find(const FName& Name, ESpoutType Type);

	/**
	 * Drops one entry.
	 *
	 * The entry destructs after the registry mutex is released, so ~FSpoutSharedSender takes the
	 * entry's resource mutex and the process-local Spout mutex without the registry mutex still
	 * held.
	 */
	void Remove(const FName& Name, ESpoutType Type);

	/** Drops every entry, with the same deferred-destruction property as Remove. */
	void Clear();

	/** Live entry count. Diagnostics and teardown assertions. */
	int32 Num() const;

private:
	// Keyed by (Name, Type) so senders and receivers with the same FName coexist.
	TMap<TPair<FName, ESpoutType>, TSharedPtr<FSpoutSharedSender>> Senders;
	// Protects registry mutations across threads.
	mutable FCriticalSection Mutex;
};
