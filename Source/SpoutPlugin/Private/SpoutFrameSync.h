// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Declares the Spout cross-process synchronisation model.
 *
 * Copying into and out of a shared texture with a bare CopyResource is not enough on its own:
 * the peer process may be writing the same surface at the same time. Spout defines a protocol
 * for this, which the bundled SDK provides and the bundled Spout.dll exports. It is made of
 * four calls:
 *
 *   - CheckTextureAccess / AllowTextureAccess : the named, cross-process texture-access mutex.
 *   - SetNewFrame / GetNewFrame               : the frame-count semaphore that tells a receiver
 *                                               whether the sender has published anything new.
 *
 * Note that the plugin's own SpoutMutex is an FCriticalSection and is not a substitute. It
 * serialises threads inside this process and says nothing at all to a Spout peer in another
 * one.
 *
 * Two things are deliberately NOT done the obvious way:
 *
 * 1. The SDK calls go through ISpoutFrameSyncBackend rather than being called inline. The cases
 *    that have to be covered are successful lock, failed lock, repeated lock/unlock, shutdown
 *    while waiting, restart and resize. If the SDK is called directly, none of that can be
 *    exercised without a second process holding a real named mutex. Behind an interface, every
 *    one of those cases is a few lines of substitute backend. ISpoutSenderNameBackend /
 *    FSpoutSenderRegistration are shaped the same way, for the same reason.
 *
 * 2. Access is taken through FSpoutTextureAccessScope, never by calling Acquire/Release in a
 *    pair by hand. "Every successful lock released exactly once" and "no forgotten unlocks" are
 *    then properties of the type rather than of the reviewer's attention -- including on the
 *    early-return and exception paths, and including the case the SDK gets wrong if you follow
 *    its own examples too literally: spoutFrameCount::AllowAccess() calls ReleaseMutex()
 *    whenever a handle exists, even when the matching CheckAccess() timed out and this process
 *    never owned the mutex.
 *
 * State is guarded by a critical section. Acquired Windows mutex ownership must be
 * released on the same thread; production access scopes run entirely on the render thread.
 * The resource/copy lock order, outermost first, is:
 *
 *      FSpoutD3DContext::D3D11ContextMutex      (D3D11 immediate context and the wrap caches)
 *   -> FSpoutSharedSender::ResourceMutex        (one entry's shared texture, handle and wraps)
 *   -> FSpoutD3DContext::SpoutMutex             (Spout's process-global sender-name list)
 *   -> FSpoutFrameSync::Mutex                   (this object)
 *   -> the Spout named access mutex             (cross-process, nonblocking try-acquire)
 *
 * A path may enter at any level and skip levels; it may never take an outer lock while holding
 * an inner one. Both the send and the receive path take the named access mutex from inside the
 * D3D11 context mutex, never the other way round, so the two cannot invert on each other. The
 * named-mutex attempt does not wait on a busy peer. Once access is acquired, the caller
 * waits for GPU completion before releasing it. This GPU wait can stall the render thread.
 * Keyed resources also use nonblocking acquisition and key zero, matching the Spout protocol.
 * Sender installation additionally holds Session::Mutex before the registry/resource locks;
 * it never flushes RHI work or waits for the game thread while holding that mutex.
 *
 * FSpoutD3DContext::InitMutex is outside this order entirely: it guards only device creation and
 * teardown and is never held while any of the above is taken.
 */
#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Templates/UniquePtr.h"

/**
 * The Spout synchronisation operations this plugin needs.
 *
 * The shared texture is passed as void* for the same reason ISpoutSenderNameBackend takes the
 * share handle as void*: this layer forwards an opaque resource identity to the SDK and has no
 * use for the D3D11 type, and keeping <d3d11.h> out of the header lets the protocol be driven
 * through this interface without a device.
 *
 * Names arrive as null-terminated UTF-8, which is what the SDK consumes and what
 * SpoutValidation produces.
 */
class ISpoutFrameSyncBackend
{
public:
	virtual ~ISpoutFrameSyncBackend() = default;

	/** Creates or opens this sender name's cross-process access mutex. */
	virtual bool CreateAccessMutex(const ANSICHAR* Utf8Name) = 0;

	/** Closes the access mutex. Must not be called while access is held. */
	virtual void CloseAccessMutex() = 0;

	/** Creates or opens this sender name's frame-count semaphore. */
	virtual void EnableFrameCount(const ANSICHAR* Utf8Name) = 0;

	/** Closes the frame-count semaphore. */
	virtual void CleanupFrameCount() = 0;

	/**
	 * Waits for exclusive access to the shared texture.
	 *
	 * False means the wait timed out or failed and this process owns nothing, so the matching
	 * release must NOT be issued.
	 */
	virtual bool CheckTextureAccess(void* SharedTexture) = 0;

	/** Gives back access previously granted by CheckTextureAccess. */
	virtual bool AllowTextureAccess(void* SharedTexture) = 0;

	/** Sender: announces that one frame has been written into the shared texture. */
	virtual void SetNewFrame() = 0;

	/**
	 * Receiver: whether the peer has published a frame since the last call.
	 *
	 * True when the peer does not publish a frame count at all, which is the SDK's own
	 * behaviour and the only safe answer: without a count there is no way to tell a repeated
	 * frame from a new one, and copying is the option that cannot drop data.
	 */
	virtual bool GetNewFrame() = 0;

	/**
	 * True when a closed backend may be handed to a later stream instead of being destroyed.
	 * Only the SDK backend says yes: constructing spoutFrameCount queries the display mode and
	 * timer resolution, which is comparatively expensive, and a stream restart constructs two of
	 * them. Every other backend keeps this default and is destroyed rather than pooled.
	 */
	virtual bool IsRecyclable() const { return false; }
};

/**
 * Builds the real SDK-backed implementation, or null when Spout is unavailable in this process.
 *
 * Prefers a recycled backend: a backend returned by RecycleSpoutSdkFrameSyncBackend holds no
 * mutex, semaphore or name state that Open does not reinitialise, so reuse is invisible to the
 * protocol and skips the expensive SDK construction on every stream restart.
 */
TUniquePtr<ISpoutFrameSyncBackend> MakeSpoutSdkFrameSyncBackend();

/**
 * Returns a closed backend to the process-wide pool, or destroys it when the pool is full or the
 * backend is not recyclable. Called by FSpoutFrameSync after Close; never with an open backend.
 */
void RecycleSpoutSdkFrameSyncBackend(TUniquePtr<ISpoutFrameSyncBackend> Backend);

/**
 * Destroys every pooled backend. Must run before Spout.dll is unloaded, because the SDK
 * destructor is in that DLL; FSpoutD3DContext::Shutdown calls it.
 */
void DrainSpoutSdkFrameSyncBackendPool();

/** Number of pooled backends. Diagnostics and tests. */
int32 NumPooledSpoutSdkFrameSyncBackends();

/** Upper bound on pooled backends; anything beyond it is destroyed on recycle. */
int32 MaxPooledSpoutSdkFrameSyncBackends();

/**
 * One stream's half of the Spout access and frame-count protocol.
 *
 * Owns the backend, so a stream that is torn down closes its mutex and semaphore with it. All
 * of the protocol's state lives here rather than being spread across the send and receive
 * paths:
 *
 *   - whether the access mutex and frame semaphore are open at all;
 *   - whether this object currently holds cross-process access (so a second acquire is refused
 *     instead of producing an unbalanced release);
 *   - whether a frame reported new by the peer has actually been consumed by a copy yet.
 *
 * That last one is the reason ConsumeNewFrame/MarkFrameConsumed are two calls rather than one.
 * GetNewFrame() decrements a semaphore: asking is destructive. If the copy that the answer was
 * for then does not happen -- because the access wait timed out, or a destination went away --
 * the frame would be lost with no way to notice. Holding the answer until a copy actually
 * lands makes a failed access attempt cost latency instead of a frame, so skipped frames stay
 * consistent.
 *
 * With no backend, or after a failed Open, access and frame publication are refused.
 */
class FSpoutFrameSync
{
public:
	explicit FSpoutFrameSync(TUniquePtr<ISpoutFrameSyncBackend> InBackend);
	~FSpoutFrameSync();

	FSpoutFrameSync(const FSpoutFrameSync&) = delete;
	FSpoutFrameSync& operator=(const FSpoutFrameSync&) = delete;

	/**
	 * Opens the access mutex and frame-count semaphore for one sender name.
	 *
	 * Idempotent: a second call for the same name does nothing, which is what lets the send and
	 * receive paths call it on the frame the stream becomes publishable without tracking whether
	 * they already have. A name change closes the previous pair first, so a stream can never hold
	 * one name's mutex while counting another name's frames.
	 *
	 * Returns whether the access mutex is open afterwards. Frame counting can be disabled
	 * machine-wide in SpoutSettings, so its absence is not a failure and is not reported as one.
	 */
	bool Open(const ANSICHAR* Utf8Name);

	/**
	 * Closes both.
	 *
	 * Idempotent: a second call does nothing. That matters because the ordered teardown closes
	 * explicitly and the destructor closes again, and handing the SDK a second close for a handle
	 * it has already released is not something to rely on being harmless.
	 */
	void Close();

	bool IsOpen() const;

	/** Sender: call after GPU copy completion, before releasing access. */
	void PublishFrame();

	/**
	 * Receiver: whether there is a frame worth copying.
	 *
	 * True when the peer has published since the last consumed frame, or when a previously
	 * reported frame has still not been copied. False means the shared texture holds the same
	 * frame that was already copied, so the whole receive -- transitions, RHI flush and GPU copy
	 * -- can be skipped.
	 */
	bool ConsumeNewFrame();

	/** Receiver: records that the pending frame was actually copied. */
	void MarkFrameConsumed();

	/** Request the current image again after a native source generation changes. */
	void RequestCurrentFrame();

	/** True while a frame reported new by the peer has not yet been copied. Diagnostics and tests. */
	bool HasPendingFrame() const;

	/** True while this object holds cross-process access. Diagnostics and tests. */
	bool IsAccessHeld() const;

	/** Successful acquires, matching releases, timed-out acquires, and published frames. Tests. */
	int32 GetAcquireCount() const;
	int32 GetReleaseCount() const;
	int32 GetFailedAcquireCount() const;
	int32 GetPublishedFrameCount() const;
	void GetStatus(int64& OutFrames, int64& OutAccessMisses) const;

private:
	friend class FSpoutTextureAccessScope;

	/** Only FSpoutTextureAccessScope may take and give back access, so the two cannot be separated. */
	bool AcquireTextureAccess(void* SharedTexture);
	void ReleaseTextureAccess(void* SharedTexture);

	mutable FCriticalSection Mutex;

	TUniquePtr<ISpoutFrameSyncBackend> Backend;

	/** Null-terminated copy of the name Open() succeeded with. Empty when nothing is open. */
	TArray<ANSICHAR> OpenUtf8Name;

	bool bOpen = false;
	bool bAccessHeld = false;
	bool bFramePending = false;

	int64 AcquireCount = 0;
	int64 ReleaseCount = 0;
	int64 FailedAcquireCount = 0;
	int64 PublishedFrameCount = 0;
	int64 ConsumedFrameCount = 0;
};

/**
 * Scoped cross-process access to one shared texture.
 *
 * The only way to take the Spout access mutex. Construction attempts the wait; destruction gives
 * access back if and only if the wait succeeded. A caller that finds IsAcquired() false must skip
 * the copy: it owns nothing, and releasing anyway is the SDK's own bug.
 *
 * Deliberately not copyable or movable -- an access token that can be duplicated is an access
 * token that can be released twice.
 */
class FSpoutTextureAccessScope
{
public:
	FSpoutTextureAccessScope(FSpoutFrameSync& InFrameSync, void* InSharedTexture)
		: FrameSync(InFrameSync)
		, SharedTexture(InSharedTexture)
		, bAcquired(InFrameSync.AcquireTextureAccess(InSharedTexture))
	{
	}

	~FSpoutTextureAccessScope()
	{
		if (bAcquired)
		{
			FrameSync.ReleaseTextureAccess(SharedTexture);
		}
	}

	FSpoutTextureAccessScope(const FSpoutTextureAccessScope&) = delete;
	FSpoutTextureAccessScope& operator=(const FSpoutTextureAccessScope&) = delete;

	/** Whether this process owns the shared texture for the lifetime of the scope. */
	bool IsAcquired() const { return bAcquired; }

private:
	FSpoutFrameSync& FrameSync;
	void* SharedTexture;
	bool bAcquired;
};
