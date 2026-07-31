/**
 * Implements the Spout access-mutex and frame-count protocol.
 *
 * The state machine here is deliberately free of D3D, Spout and Unreal render threads: everything
 * the SDK does arrives through ISpoutFrameSyncBackend, so the whole of the protocol's behaviour
 * -- successful and failed locks, balanced release, repeated lock/unlock, restart, resize,
 * teardown while a peer holds the mutex -- can be driven through that interface alone.
 */
#include "SpoutFrameSync.h"

#include "SpoutModule.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include <d3d11.h>
#include "SpoutDX/SpoutFrameCount.h"
#include "Windows/HideWindowsPlatformTypes.h"
#ifdef GetNextSibling
#undef GetNextSibling
#endif
#endif

namespace
{
#if PLATFORM_WINDOWS
	/**
	 * Adapts the bundled spoutFrameCount to ISpoutFrameSyncBackend.
	 *
	 * One instance per stream, not one per process: spoutFrameCount keeps the access-mutex handle,
	 * the semaphore handle and the receiver's last-seen frame count as member state, all of which
	 * are per sender NAME. Sharing one instance between two streams would make them overwrite each
	 * other's notion of which frame they last saw.
	 */
	class FSpoutSdkFrameSyncBackend final : public ISpoutFrameSyncBackend
	{
	public:
		virtual bool CreateAccessMutex(const ANSICHAR* Utf8Name) override
		{
			return FrameCount.CreateAccessMutex(Utf8Name);
		}

		virtual void CloseAccessMutex() override
		{
			FrameCount.CloseAccessMutex();
		}

		virtual void EnableFrameCount(const ANSICHAR* Utf8Name) override
		{
			FrameCount.EnableFrameCount(Utf8Name);
		}

		virtual void CleanupFrameCount() override
		{
			FrameCount.CleanupFrameCount();
		}

		virtual bool CheckTextureAccess(void* SharedTexture) override
		{
			return FrameCount.CheckTextureAccess(static_cast<ID3D11Texture2D*>(SharedTexture));
		}

		virtual bool AllowTextureAccess(void* SharedTexture) override
		{
			return FrameCount.AllowTextureAccess(static_cast<ID3D11Texture2D*>(SharedTexture));
		}

		virtual void SetNewFrame() override
		{
			FrameCount.SetNewFrame();
		}

		virtual bool GetNewFrame() override
		{
			return FrameCount.GetNewFrame();
		}

	private:
		spoutFrameCount FrameCount;
	};
#endif
}

TUniquePtr<ISpoutFrameSyncBackend> MakeSpoutSdkFrameSyncBackend()
{
#if PLATFORM_WINDOWS
	// Constructing spoutFrameCount is the first touch of a Spout symbol on this path, which is what
	// triggers the delay-load bind of Spout.dll. Binding a DLL that never loaded terminates the
	// process with 0xC06D007E, so the same guard the D3D context uses applies here. Returning null
	// leaves FSpoutFrameSync in its permissive mode, which keeps copies working unsynchronised
	// rather than stopping them.
	if (!FSpoutModule::IsSpoutDllLoaded())
	{
		return nullptr;
	}

	return MakeUnique<FSpoutSdkFrameSyncBackend>();
#else
	return nullptr;
#endif
}

FSpoutFrameSync::FSpoutFrameSync(TUniquePtr<ISpoutFrameSyncBackend> InBackend)
	: Backend(MoveTemp(InBackend))
{
}

FSpoutFrameSync::~FSpoutFrameSync()
{
	Close();
}

bool FSpoutFrameSync::Open(const ANSICHAR* Utf8Name)
{
	if (Utf8Name == nullptr || Utf8Name[0] == '\0')
	{
		return false;
	}

	FScopeLock Lock(&Mutex);

	if (!Backend)
	{
		return false;
	}

	if (bOpen)
	{
		// Already open for this name: nothing to do. This is the per-frame path, so it must stay a
		// comparison and nothing more.
		if (OpenUtf8Name.Num() > 1 && FCStringAnsi::Strcmp(OpenUtf8Name.GetData(), Utf8Name) == 0)
		{
			return true;
		}

		// A different name. Defensive -- a stream's name is fixed in practice -- but holding one
		// name's access mutex while counting another name's frames would be a silent protocol
		// error, so the previous pair is closed first.
		if (bAccessHeld)
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("Spout frame sync: refusing to reopen under a new name while texture access is held."));
			return false;
		}

		Backend->CloseAccessMutex();
		Backend->CleanupFrameCount();
		bOpen = false;
		bFramePending = false;
		OpenUtf8Name.Reset();
	}

	const bool bMutexCreated = Backend->CreateAccessMutex(Utf8Name);

	// Frame counting is a machine-wide setting a user can turn off in SpoutSettings, so its absence
	// is not an error. When it is off the peer publishes no count, GetNewFrame() answers "new" for
	// every call, and the receiver copies unconditionally, which is the only correct answer when
	// there is nothing to compare.
	Backend->EnableFrameCount(Utf8Name);

	const int32 NameLength = FCStringAnsi::Strlen(Utf8Name);
	OpenUtf8Name.Reset(NameLength + 1);
	OpenUtf8Name.Append(Utf8Name, NameLength + 1);

	bOpen = true;
	return bMutexCreated;
}

void FSpoutFrameSync::Close()
{
	FScopeLock Lock(&Mutex);

	if (!bOpen)
	{
		// Nothing to close. This runs more often than it looks: the destructor closes, and the
		// ordered teardown closes explicitly before that, so the second call must not hand the SDK
		// a second CloseAccessMutex/CleanupFrameCount pair for a handle it has already given up.
		bAccessHeld = false;
		bFramePending = false;
		OpenUtf8Name.Reset();
		return;
	}

	if (!Backend)
	{
		bOpen = false;
		bFramePending = false;
		OpenUtf8Name.Reset();
		return;
	}

	if (bAccessHeld)
	{
		// Closing the mutex handle underneath a live acquire would abandon it, which every other
		// process on that name would then see as WAIT_ABANDONED. Teardown reaches this point only
		// after the render-command flush, so an access scope can no longer be live; if one somehow
		// is, saying so is better than corrupting the name for every peer.
		UE_LOG(LogSpoutPlugin, Warning,
			TEXT("Spout frame sync: closing while texture access is still held; the access mutex will be abandoned."));
	}

	Backend->CloseAccessMutex();
	Backend->CleanupFrameCount();

	bOpen = false;
	bAccessHeld = false;
	bFramePending = false;
	OpenUtf8Name.Reset();
}

bool FSpoutFrameSync::IsOpen() const
{
	FScopeLock Lock(&Mutex);
	return bOpen;
}

void FSpoutFrameSync::PublishFrame()
{
	FScopeLock Lock(&Mutex);

	++PublishedFrameCount;

	if (Backend)
	{
		Backend->SetNewFrame();
	}
}

bool FSpoutFrameSync::ConsumeNewFrame()
{
	FScopeLock Lock(&Mutex);

	if (!Backend)
	{
		// Nothing to ask. Copy every frame.
		return true;
	}

	// Asking is destructive -- GetNewFrame() decrements the frame semaphore -- so the answer is
	// latched rather than re-asked. Once a frame is known to be pending it stays pending until a
	// copy actually lands, so a timed-out access wait costs one frame of latency instead of losing
	// the frame entirely.
	if (Backend->GetNewFrame())
	{
		bFramePending = true;
	}

	return bFramePending;
}

void FSpoutFrameSync::MarkFrameConsumed()
{
	FScopeLock Lock(&Mutex);
	bFramePending = false;
}

bool FSpoutFrameSync::HasPendingFrame() const
{
	FScopeLock Lock(&Mutex);
	return bFramePending;
}

bool FSpoutFrameSync::IsAccessHeld() const
{
	FScopeLock Lock(&Mutex);
	return bAccessHeld;
}

int32 FSpoutFrameSync::GetAcquireCount() const
{
	FScopeLock Lock(&Mutex);
	return AcquireCount;
}

int32 FSpoutFrameSync::GetReleaseCount() const
{
	FScopeLock Lock(&Mutex);
	return ReleaseCount;
}

int32 FSpoutFrameSync::GetFailedAcquireCount() const
{
	FScopeLock Lock(&Mutex);
	return FailedAcquireCount;
}

int32 FSpoutFrameSync::GetPublishedFrameCount() const
{
	FScopeLock Lock(&Mutex);
	return PublishedFrameCount;
}

bool FSpoutFrameSync::AcquireTextureAccess(void* SharedTexture)
{
	FScopeLock Lock(&Mutex);

	if (bAccessHeld)
	{
		// Nested acquire. The Windows mutex would grant it (it is recursive for its owner) and then
		// need two releases to actually let go, so a single scope exit would leave it locked
		// forever. Refusing is the only outcome that cannot deadlock a peer.
		UE_LOG(LogSpoutPlugin, Warning,
			TEXT("Spout frame sync: texture access requested while already held; the nested request was refused."));
		++FailedAcquireCount;
		return false;
	}

	if (!Backend)
	{
		// No protocol available. Permit the copy and count it, so acquires and releases stay
		// balanced in this mode too.
		bAccessHeld = true;
		++AcquireCount;
		return true;
	}

	if (!Backend->CheckTextureAccess(SharedTexture))
	{
		// The wait timed out or failed: this process owns nothing. Not calling AllowTextureAccess
		// here is the point -- the SDK's own AllowAccess() releases whenever a handle exists, so
		// pairing it with a failed check hands away a mutex this process never took.
		++FailedAcquireCount;
		return false;
	}

	bAccessHeld = true;
	++AcquireCount;
	return true;
}

void FSpoutFrameSync::ReleaseTextureAccess(void* SharedTexture)
{
	FScopeLock Lock(&Mutex);

	if (!bAccessHeld)
	{
		// Only reachable if something released outside FSpoutTextureAccessScope.
		UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout frame sync: texture access released without being held."));
		return;
	}

	bAccessHeld = false;
	++ReleaseCount;

	if (Backend)
	{
		Backend->AllowTextureAccess(SharedTexture);
	}
}
