// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the shared-slot interop path. See SpoutSharedSlot.h for the design, the ownership rules and the lock
 * order, and SpoutSlotRules.h for when retired objects may be released.
 *
 * The slot and fence construction follows the asynchronous receive path's (SpoutAsyncReceive.cpp); the code is
 * duplicated there rather than shared.
 */
#include "SpoutSharedSlot.h"

#if PLATFORM_WINDOWS

#include "SpoutAsyncReceive.h"
#include "SpoutDebugCapture.h"
#include "SpoutLimits.h"
#include "SpoutModule.h"
#include "HAL/PlatformProcess.h"
#include "ID3D12DynamicRHI.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "Runtime/Launch/Resources/Version.h"

using Microsoft::WRL::ComPtr;

namespace
{
	/** Slot and fence generations of the shared-slot path, never 0 and never repeated in a process. */
	std::atomic<uint64> GNextSlotGeneration{1};

	ID3D11Device5* NativeDevice()
	{
		return FSpoutAsyncDevice::Get().GetDevice();
	}

	bool IsNativeRemoved()
	{
		ID3D11Device5* Device = NativeDevice();
		return !Device || Device->GetDeviceRemovedReason() != S_OK;
	}

	ID3D12Device* UnrealDevice()
	{
		return GDynamicRHI ? static_cast<ID3D12Device*>(GDynamicRHI->RHIGetNativeDevice()) : nullptr;
	}

	bool IsUnrealRemoved()
	{
		ID3D12Device* Device = UnrealDevice();
		return !Device || Device->GetDeviceRemovedReason() != S_OK;
	}

	FString FormatLuid(const LUID& Luid)
	{
		return FString::Printf(TEXT("%08x:%08x"), static_cast<uint32>(Luid.HighPart), static_cast<uint32>(Luid.LowPart));
	}

	/** The adapter the native device was actually made on, read back from the device itself. */
	bool ReadNativeAdapterLuid(LUID& Out)
	{
		ID3D11Device5* Device = NativeDevice();
		ComPtr<IDXGIDevice> Dxgi;
		ComPtr<IDXGIAdapter> Adapter;
		DXGI_ADAPTER_DESC Desc = {};
		if (!Device || FAILED(Device->QueryInterface(IID_PPV_ARGS(Dxgi.GetAddressOf()))) || FAILED(Dxgi->GetAdapter(Adapter.GetAddressOf())) || FAILED(Adapter->GetDesc(&Desc)))
		{
			return false;
		}
		Out = Desc.AdapterLuid;
		return true;
	}

	/**
	 * Waits, at most TimeoutMs, until Fence reached Value. Used only for the send's fill, a fence Unreal's queue signals,
	 * while no access is held. A removal of either device ends it: nothing can be sent then. Waits for native work go
	 * through WaitForNativeWork instead, which never gives up while a peer's access is held. When the completion event
	 * cannot be set up the fence is polled instead.
	 */
	SpoutSlotRules::EWait WaitForFenceValue(ID3D12Fence* Fence, uint64 Value, uint32 TimeoutMs, HANDLE Event)
	{
		using SpoutSlotRules::EWait;
		uint64 Done = Fence->GetCompletedValue();
		if (Done == SpoutSlotRules::RemovedValue) return EWait::Removed;
		if (Done >= Value) return EWait::Reached;
		if (TimeoutMs == 0) return EWait::TimedOut;
		bool bEventSet = false;
		if (Event)
		{
			::ResetEvent(Event);
			bEventSet = SUCCEEDED(Fence->SetEventOnCompletion(Value, Event));
		}
		const bool bBounded = TimeoutMs != MAX_uint32;
		const double End = FPlatformTime::Seconds() + double(TimeoutMs) / 1000.0;
		for (;;)
		{
			Done = Fence->GetCompletedValue();
			if (Done == SpoutSlotRules::RemovedValue) return EWait::Removed;
			if (Done >= Value) return EWait::Reached;
			if (IsNativeRemoved() || IsUnrealRemoved()) return EWait::Removed;
			uint32 SliceMs = 100;
			if (bBounded)
			{
				const double Left = End - FPlatformTime::Seconds();
				if (Left <= 0.0) return EWait::TimedOut;
				SliceMs = FMath::Clamp<uint32>(static_cast<uint32>(Left * 1000.0 + 0.999), 1u, 100u);
			}
			if (bEventSet)
			{
				// The event may also have been set by an earlier wait that timed out: the value is read again.
				::WaitForSingleObject(Event, SliceMs);
			}
			else
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
		}
	}

	/** Queues a wait of Unreal's graphics queue on a fence another device signals. RHI thread. */
	void QueueWaitManualFence(FRHICommandListImmediate& Cmd, ID3D12Fence* Fence, uint64 Value)
	{
#if ENGINE_MINOR_VERSION >= 8
		// No CPU-side "has signalled" flag: the value was observed reached on the CPU before this was queued.
		GetID3D12DynamicRHI()->RHIWaitManualFence(Cmd, Fence, Value, TSharedPtr<std::atomic<bool>>());
#else
		GetID3D12DynamicRHI()->RHIWaitManualFence(Cmd, Fence, Value);
#endif
	}

	/** A D3D12 fence on Unreal's device. bShared also opens it on the native device (OutNative). */
	HRESULT CreateCrossFence(bool bShared, ComPtr<ID3D12Fence>& Out12, ComPtr<ID3D11Fence>* OutNative)
	{
		ID3D12Device* Device12 = UnrealDevice();
		ID3D11Device5* Device11 = NativeDevice();
		if (!Device12 || (bShared && !Device11)) return E_UNEXPECTED;
		HRESULT Result = Device12->CreateFence(0, bShared ? D3D12_FENCE_FLAG_SHARED : D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(Out12.GetAddressOf()));
		if (FAILED(Result) || !bShared) return Result;
		HANDLE Handle = nullptr;
		Result = Device12->CreateSharedHandle(Out12.Get(), nullptr, GENERIC_ALL, nullptr, &Handle);
		if (FAILED(Result) || !Handle) return FAILED(Result) ? Result : E_FAIL;
		Result = Device11->OpenSharedFence(Handle, IID_PPV_ARGS(OutNative->GetAddressOf()));
		::CloseHandle(Handle);
		return Result;
	}

	/**
	 * A slot: a committed texture on Unreal's device in a shared heap with simultaneous access, opened on the native
	 * device, and an RHI texture of it. Created in the common state and wrapped as shared, like the asynchronous
	 * receive's slots.
	 */
	HRESULT CreateSlot(uint32 Width, uint32 Height, DXGI_FORMAT Format, EPixelFormat PixelFormat,
		ComPtr<ID3D12Resource>& OutResource, ComPtr<ID3D11Texture2D>& OutNative, FTextureRHIRef& OutTexture)
	{
		ID3D12Device* Device12 = UnrealDevice();
		ID3D11Device5* Device11 = NativeDevice();
		if (!Device12 || !Device11) return E_UNEXPECTED;
		D3D12_HEAP_PROPERTIES Heap = {};
		Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		Heap.CreationNodeMask = 1;
		Heap.VisibleNodeMask = 1;
		D3D12_RESOURCE_DESC Desc = {};
		Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		Desc.Width = Width;
		Desc.Height = Height;
		Desc.DepthOrArraySize = 1;
		Desc.MipLevels = 1;
		Desc.Format = Format;
		Desc.SampleDesc.Count = 1;
		Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
		HRESULT Result = Device12->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_SHARED, &Desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(OutResource.GetAddressOf()));
		if (FAILED(Result) || !OutResource) return FAILED(Result) ? Result : E_FAIL;
		HANDLE Handle = nullptr;
		Result = Device12->CreateSharedHandle(OutResource.Get(), nullptr, GENERIC_ALL, nullptr, &Handle);
		if (FAILED(Result) || !Handle) return FAILED(Result) ? Result : E_FAIL;
		Result = Device11->OpenSharedResource1(Handle, IID_PPV_ARGS(OutNative.GetAddressOf()));
		::CloseHandle(Handle);
		if (FAILED(Result) || !OutNative) return FAILED(Result) ? Result : E_FAIL;
		OutTexture = GetID3D12DynamicRHI()->RHICreateTexture2DFromResource(PixelFormat, ETextureCreateFlags::Shared | ETextureCreateFlags::DisableSRVCreation,
			FClearValueBinding::None, OutResource.Get());
		return OutTexture.IsValid() ? S_OK : E_FAIL;
	}

	HRESULT CreateProofQuery(ComPtr<ID3D11Query>& Out)
	{
		ID3D11Device5* Device11 = NativeDevice();
		if (!Device11) return E_UNEXPECTED;
		D3D11_QUERY_DESC Desc = { D3D11_QUERY_EVENT, 0 };
		return Device11->CreateQuery(&Desc, Out.GetAddressOf());
	}

	void AddObject(TArray<TRefCountPtr<IUnknown>>& Objects, IUnknown* Object)
	{
		if (Object)
		{
			Objects.Add(TRefCountPtr<IUnknown>(Object));
		}
	}

	/**
	 * The native signal of a copy, with the failure injection of the tests. Context lock held by the caller.
	 */
	HRESULT SignalNative(ID3D11DeviceContext4* Context, ID3D11Fence* Fence, uint64 Value)
	{
#if WITH_DEV_AUTOMATION_TESTS
		int32 Left = SpoutSlotTestHooks::FailNativeSignals.load();
		while (Left > 0)
		{
			if (SpoutSlotTestHooks::FailNativeSignals.compare_exchange_weak(Left, Left - 1))
			{
				return E_FAIL;
			}
		}
#endif
		return Context->Signal(Fence, Value);
	}

#if WITH_DEV_AUTOMATION_TESTS
	bool TakeHook(std::atomic<int32>& Counter)
	{
		int32 Left = Counter.load();
		while (Left > 0)
		{
			if (Counter.compare_exchange_weak(Left, Left - 1))
			{
				return true;
			}
		}
		return false;
	}
#endif

	/** How a wait for native work proven finished ended. */
	enum class ENativeProof : uint8
	{
		InTime,   // proven finished within the bound
		Late,     // proven finished, after the bound
		Removed   // the native device was removed: nothing it was given will run any more
	};

	/**
	 * Waits until the native work submitted before Value's signal is proven finished, or the native device is removed.
	 * It never returns earlier: the caller holds a peer's access, and the peer may write the texture (receive) or read
	 * it (send) as soon as that access is released. BoundMs only classifies the result as in time or late.
	 *
	 * The proof is the fence when the signal succeeded (bSignalled), else the event query Proof the caller ended
	 * right after the copy. The fence is read through its D3D12 view. When Unreal's device is removed, that view reports
	 * the largest value, which says nothing about the native device; the proof then moves to an event query on the
	 * native context. A query that reports an error other than the native device's removal proves nothing, and it is
	 * issued again. While it waits, a warning says every 5 s that the peer's access is still held.
	 */
	ENativeProof WaitForNativeWork(ID3D12Fence* View, uint64 Value, bool bSignalled, ID3D11Query* Proof, uint32 BoundMs, HANDLE Event, const FString& What)
	{
		FSpoutAsyncDevice& Async = FSpoutAsyncDevice::Get();
		const double Start = FPlatformTime::Seconds();
		double NotBefore = Start;
#if WITH_DEV_AUTOMATION_TESTS
		// Tests: a GPU operation that takes this long. Completion is not accepted earlier; the real proof is still needed.
		const int32 DelayMs = SpoutSlotTestHooks::DelayNativeCompletionMs.exchange(0);
		NotBefore += double(FMath::Max(DelayMs, 0)) / 1000.0;
#endif
		auto IssueQuery = [&Async, Proof]()
		{
			FScopeLock ContextLock(&Async.GetContextMutex());
			ID3D11DeviceContext4* Context = Async.GetContext();
			if (!Context)
			{
				return false;
			}
			Context->End(Proof);
			Context->Flush();
			return true;
		};
		bool bOnQuery = !bSignalled;
		bool bEventSet = false;
		bool bQueryErrorLogged = false;
		double NextWarning = Start + 5.0;
		for (;;)
		{
			if (IsNativeRemoved())
			{
				return ENativeProof::Removed;
			}
			if (FPlatformTime::Seconds() >= NextWarning)
			{
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout %s: still waiting after %.1f s for a native copy to be proven finished; the peer's access stays held until it is, or until the native device is removed."),
					*What, FPlatformTime::Seconds() - Start);
				NextWarning = FPlatformTime::Seconds() + 5.0;
			}
			bool bDone = false;
			if (!bOnQuery)
			{
				const uint64 Done = View->GetCompletedValue();
				if (Done == SpoutSlotRules::RemovedValue)
				{
					// Unreal's device was removed: this view no longer reports the native device's progress.
					if (!IssueQuery())
					{
						return ENativeProof::Removed;
					}
					bOnQuery = true;
					continue;
				}
				bDone = Done >= Value;
				if (!bDone && !bEventSet && Event)
				{
					::ResetEvent(Event);
					bEventSet = SUCCEEDED(View->SetEventOnCompletion(Value, Event));
				}
			}
			else
			{
				HRESULT QueryResult = S_FALSE;
				{
					FScopeLock ContextLock(&Async.GetContextMutex());
					ID3D11DeviceContext4* Context = Async.GetContext();
					QueryResult = Context ? Context->GetData(Proof, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) : DXGI_ERROR_DEVICE_REMOVED;
				}
#if WITH_DEV_AUTOMATION_TESTS
				if (QueryResult == S_OK && TakeHook(SpoutSlotTestHooks::FailProofQueries))
				{
					QueryResult = E_FAIL;
				}
#endif
				if (QueryResult == S_OK)
				{
					bDone = true;
				}
				else if (QueryResult != S_FALSE)
				{
					if (IsNativeRemoved())
					{
						return ENativeProof::Removed;
					}
					// Not a removal, and not a proof: ask again. Access stays held meanwhile.
					if (!bQueryErrorLogged)
					{
						bQueryErrorLogged = true;
						UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout %s: the event query that proves a native copy finished reported 0x%08x; it is issued again and the peer's access stays held."),
							*What, static_cast<uint32>(QueryResult));
					}
					if (!IssueQuery())
					{
						return ENativeProof::Removed;
					}
					FPlatformProcess::SleepNoStats(0.001f);
					continue;
				}
			}
			const double Now = FPlatformTime::Seconds();
			if (bDone && Now >= NotBefore)
			{
				return (Now - Start) * 1000.0 <= double(BoundMs) ? ENativeProof::InTime : ENativeProof::Late;
			}
			if (bEventSet && !bDone)
			{
				// The event may also have been set by an earlier wait: the value is read again on every pass.
				::WaitForSingleObject(Event, 100);
			}
			else
			{
				FPlatformProcess::SleepNoStats(bDone ? 0.001f : 0.0f);
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// The selection
// ---------------------------------------------------------------------------------------------

namespace SpoutInterop
{
	namespace
	{
		FCriticalSection GLatchMutex;
		std::atomic<uint8> GMode{static_cast<uint8>(EMode::Unlatched)};
		FString GReason;
		bool GLatchedRequest = true;
		int32 GLastReportedRequest = -1;
		std::atomic<int64> GWrapCalls{0};
	}

	void LatchOnce()
	{
		FScopeLock Lock(&GLatchMutex);
		if (static_cast<EMode>(GMode.load()) != EMode::Unlatched)
		{
			return;
		}
		GLatchedRequest = SpoutLimits::SharedSlotInteropRequested();
		EMode Chosen = EMode::Wrap;
		if (!GLatchedRequest)
		{
			GReason = TEXT("r.Spout.SharedSlotInterop is 0");
		}
		else
		{
			FString Why;
			if (FSpoutAsyncDevice::Get().EnsureCreated(Why))
			{
				Chosen = EMode::SharedSlot;
				GReason = TEXT("r.Spout.SharedSlotInterop is 1 and the native D3D11 device exists");
			}
			else
			{
				GReason = FString::Printf(TEXT("r.Spout.SharedSlotInterop is 1, but the native D3D11 device could not be created (%s); this choice was made before any slot object existed"), *Why);
			}
		}

		// The adapters actually in use, read back from the devices, and their match enforced: slots are shared between
		// the two devices only when both are on the same adapter.
		ID3D12Device* Device12 = UnrealDevice();
		const LUID UnrealLuid = Device12 ? Device12->GetAdapterLuid() : LUID{};
		if (Chosen == EMode::SharedSlot)
		{
			LUID NativeLuid = {};
			const bool bRead = ReadNativeAdapterLuid(NativeLuid);
			const bool bMatch = bRead && Device12 && NativeLuid.LowPart == UnrealLuid.LowPart && NativeLuid.HighPart == UnrealLuid.HighPart;
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout interop adapters: unreal=%s native=%s match=%d"),
				*FormatLuid(UnrealLuid), bRead ? *FormatLuid(NativeLuid) : TEXT("unreadable"), bMatch ? 1 : 0);
			if (!bMatch)
			{
				Chosen = EMode::Wrap;
				GReason = TEXT("r.Spout.SharedSlotInterop is 1, but the native D3D11 device is not proven to be on Unreal's adapter; this choice was made before any slot object existed");
			}
		}
		else
		{
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout interop adapters: unreal=%s native=none"), *FormatLuid(UnrealLuid));
		}
		GMode = static_cast<uint8>(Chosen);
		if (Chosen == EMode::SharedSlot)
		{
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout interop path latched for this process: %s (%s). It does not change until the process restarts."), ToString(Chosen), *GReason);
		}
		else
		{
			UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout interop path latched for this process: %s (%s). It does not change until the process restarts."), ToString(Chosen), *GReason);
		}
	}

	EMode GetMode()
	{
		return static_cast<EMode>(GMode.load());
	}

	bool IsSharedSlot()
	{
		return GetMode() == EMode::SharedSlot;
	}

	const TCHAR* ToString(EMode Mode)
	{
		switch (Mode)
		{
		case EMode::SharedSlot: return TEXT("shared-slot");
		case EMode::Wrap:       return TEXT("wrap");
		default:                return TEXT("unlatched");
		}
	}

	FString GetLatchReason()
	{
		FScopeLock Lock(&GLatchMutex);
		return GReason;
	}

	void NoteSettingChange()
	{
		FScopeLock Lock(&GLatchMutex);
		if (static_cast<EMode>(GMode.load()) == EMode::Unlatched)
		{
			return;
		}
		const bool bNow = SpoutLimits::SharedSlotInteropRequested();
		const int32 NowValue = bNow ? 1 : 0;
		if (bNow != GLatchedRequest && NowValue != GLastReportedRequest)
		{
			GLastReportedRequest = NowValue;
			UE_LOG(LogSpoutPlugin, Warning, TEXT("r.Spout.SharedSlotInterop is now %d, but this process latched the %s path at startup. The setting takes effect at the next process start."),
				NowValue, ToString(static_cast<EMode>(GMode.load())));
		}
	}

	int64 GetWrapCallCount()
	{
		return GWrapCalls.load();
	}

	void CountWrapCall()
	{
		++GWrapCalls;
	}
}

// ---------------------------------------------------------------------------------------------
// Retirement
// ---------------------------------------------------------------------------------------------

FSpoutGpuRetirement& FSpoutGpuRetirement::Get()
{
	static FSpoutGpuRetirement Instance;
	return Instance;
}

FSpoutGpuRetirement::~FSpoutGpuRetirement()
{
	// Static destruction, or an instance other than Get()'s going away. Nothing here may release an object whose work
	// is not proven finished, and the devices may already be gone: what is left is kept for the rest of the process.
	FScopeLock Lock(&Mutex);
	for (TUniquePtr<FRetiredSet>& Record : Records)
	{
		static_cast<void>(Record.Release()); // deliberately never deleted
	}
	Records.Reset();
}

void FSpoutGpuRetirement::Retire(TUniquePtr<FRetiredSet> Record)
{
	if (!Record)
	{
		return;
	}
	Record->RetiredAt = FPlatformTime::Seconds();
	FScopeLock Lock(&Mutex);
	++RetiredCount;
	Records.Add(MoveTemp(Record));
}

bool FSpoutGpuRetirement::IsIdle(FRetiredSet& Record)
{
	const bool bNativeRemoved = IsNativeRemoved();
	if (Record.Outstanding.NeedsProofQuery() && Record.Proof && !bNativeRemoved)
	{
		FSpoutAsyncDevice& Async = FSpoutAsyncDevice::Get();
		FScopeLock ContextLock(&Async.GetContextMutex());
		if (ID3D11DeviceContext4* Context = Async.GetContext())
		{
			// The query was ended and flushed when it was issued: no flush is needed to make progress.
			if (Context->GetData(Record.Proof.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
			{
				Record.Outstanding.ProofCompleted();
			}
		}
	}
	const uint64 NativeView = Record.NativeFence ? Record.NativeFence->GetCompletedValue() : 0;
	bool bViewLostProven = false;
	if (NativeView == SpoutSlotRules::RemovedValue && !bNativeRemoved && Record.Outstanding.GetNativeRequired() > 0)
	{
		// Unreal's device was removed, and the D3D12 view of the native fence no longer reports the native device's
		// progress. An event query on the native context proves its earlier work instead.
		FSpoutAsyncDevice& Async = FSpoutAsyncDevice::Get();
		FScopeLock ContextLock(&Async.GetContextMutex());
		ID3D11DeviceContext4* Context = Async.GetContext();
		if (Context && !Record.ViewLostProof && SUCCEEDED(CreateProofQuery(Record.ViewLostProof)) && Record.ViewLostProof)
		{
			Context->End(Record.ViewLostProof.Get());
			Context->Flush();
		}
		else if (Context && Record.ViewLostProof)
		{
			bViewLostProven = Context->GetData(Record.ViewLostProof.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
		}
	}
	const uint64 NativeDone = SpoutSlotRules::NativeProgress(NativeView, bNativeRemoved, bViewLostProven);
	const uint64 QueueDone = Record.QueueFence ? Record.QueueFence->GetCompletedValue() : 0;
	return Record.Outstanding.IsIdle(NativeDone, QueueDone, bNativeRemoved);
}

void FSpoutGpuRetirement::Release(FRetiredSet& Record)
{
	// The order does not matter once the work is proven finished; RHI textures go first so that their last
	// reference is dropped while the objects under them are still referenced here.
	for (FTextureRHIRef& Texture : Record.Textures)
	{
		Texture.SafeRelease();
	}
	Record.Textures.Reset();
	Record.Objects.Reset();
	Record.Proof.Reset();
	Record.ViewLostProof.Reset();
	Record.NativeFence.Reset();
	Record.QueueFence.Reset();
	if (Record.Event)
	{
		::CloseHandle(Record.Event);
		Record.Event = nullptr;
	}
	Record.Reservation.Reset();
}

int32 FSpoutGpuRetirement::Poll()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SlotRetirementPoll);
	FScopeLock Lock(&Mutex);
	int32 ReleasedNow = 0;
	for (int32 I = Records.Num() - 1; I >= 0; --I)
	{
		if (IsIdle(*Records[I]))
		{
			Release(*Records[I]);
			Records.RemoveAt(I);
			++ReleasedNow;
		}
	}
	ReleasedCount += ReleasedNow;
	return ReleasedNow;
}

void FSpoutGpuRetirement::DrainForShutdown(uint32 TimeoutMs)
{
	const double End = FPlatformTime::Seconds() + double(TimeoutMs) / 1000.0;
	for (;;)
	{
		Poll();
		{
			FScopeLock Lock(&Mutex);
			if (Records.Num() == 0)
			{
				break;
			}
		}
		if (FPlatformTime::Seconds() > End)
		{
			break;
		}
		FPlatformProcess::SleepNoStats(0.001f);
	}
	FScopeLock Lock(&Mutex);
	for (TUniquePtr<FRetiredSet>& Record : Records)
	{
		const SpoutSlotRules::FOutstanding& O = Record->Outstanding;
		UE_LOG(LogSpoutPlugin, Warning,
			TEXT("Spout shared-slot retirement: '%s' is kept for the rest of the process, not released: its GPU work is not proven finished ")
			TEXT("(native fence %llu of %llu, queue fence %llu of %llu, proof %d, native device removed %d)."),
			*Record->What,
			Record->NativeFence ? Record->NativeFence->GetCompletedValue() : 0ull, O.GetNativeRequired(),
			Record->QueueFence ? Record->QueueFence->GetCompletedValue() : 0ull, O.GetQueueRequired(),
			static_cast<int32>(O.GetProof()), IsNativeRemoved() ? 1 : 0);
		++LeakedCount;
		static_cast<void>(Record.Release()); // deliberately never deleted
	}
	Records.Reset();
	UE_LOG(LogSpoutPlugin, Log, TEXT("Spout shared-slot retirement at shutdown: retired=%lld released=%lld kept=%lld pending=0"),
		RetiredCount, ReleasedCount, LeakedCount);
}

FSpoutGpuRetirement::FStats FSpoutGpuRetirement::GetStats() const
{
	FScopeLock Lock(&Mutex);
	FStats Stats;
	Stats.Retired = RetiredCount;
	Stats.Released = ReleasedCount;
	Stats.Leaked = LeakedCount;
	Stats.Pending = Records.Num();
	return Stats;
}

#if WITH_DEV_AUTOMATION_TESTS
void FSpoutGpuRetirement::DiscardForTest()
{
	FScopeLock Lock(&Mutex);
	for (TUniquePtr<FRetiredSet>& Record : Records)
	{
		Release(*Record);
	}
	Records.Reset();
}
#endif

void SpoutRetireUnprovenTexture(ComPtr<ID3D11Texture2D>&& Texture, const FString& What)
{
	if (!Texture)
	{
		return;
	}
	TUniquePtr<FSpoutGpuRetirement::FRetiredSet> Record = MakeUnique<FSpoutGpuRetirement::FRetiredSet>();
	Record->What = What;
	// No query was possible after the copy whose signal failed: only the removal of the native device proves it.
	Record->Outstanding.NativeSignalFailed(false);
	AddObject(Record->Objects, Texture.Get());
	Texture.Reset();
	FSpoutGpuRetirement::Get().Retire(MoveTemp(Record));
}

// ---------------------------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------------------------

FSpoutSlotReceive::FSpoutSlotReceive(const FName& InName, const FIdentity& InIdentity)
	: Name(InName)
	, Identity(InIdentity)
	, Generation(GNextSlotGeneration.fetch_add(1))
{
}

TUniquePtr<FSpoutSlotReceive> FSpoutSlotReceive::Create(const FName& Name, const FIdentity& Identity, EPixelFormat PixelFormat, FString& OutWhy)
{
	check(IsInRenderingThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SlotReceiveCreate);
	TUniquePtr<FSpoutSlotReceive> Set(new FSpoutSlotReceive(Name, Identity));
	if (!Set->Initialize(PixelFormat, OutWhy))
	{
		// Nothing was submitted with any of its objects: its destructor retires them, and the first poll releases them.
		return nullptr;
	}
	return Set;
}

bool FSpoutSlotReceive::Initialize(EPixelFormat PixelFormat, FString& OutWhy)
{
	if (!SpoutInterop::IsSharedSlot() || !NativeDevice() || !UnrealDevice())
	{
		OutWhy = TEXT("the shared-slot path or one of its devices is not available");
		return false;
	}
	int64 FrameBytes = 0;
	if (!SpoutLimits::FrameBytes(Identity.Width, Identity.Height, PixelFormat, FrameBytes))
	{
		OutWhy = TEXT("the frame size is outside the plugin's limits");
		return false;
	}
	Reservation = FSpoutResourceReservation::TryReserve(FrameBytes * NumSlots);
	if (!Reservation)
	{
		OutWhy = TEXT("the retained-memory budget has no room for the slots");
		return false;
	}
	auto Refuse = [&OutWhy](const TCHAR* What, HRESULT Result)
	{
		OutWhy = FString::Printf(TEXT("%s (hr=0x%08x)"), What, static_cast<uint32>(Result));
		return false;
	};

	HRESULT Result = NativeDevice()->OpenSharedResource(Identity.SharedHandle, IID_PPV_ARGS(NativeSource.GetAddressOf()));
	if (FAILED(Result) || !NativeSource)
	{
		return Refuse(TEXT("the sender's texture could not be opened on the native device"), Result);
	}
	D3D11_TEXTURE2D_DESC SourceDesc = {};
	NativeSource->GetDesc(&SourceDesc);
	if (SourceDesc.Width != Identity.Width || SourceDesc.Height != Identity.Height || SourceDesc.Format != Identity.Format
		|| SourceDesc.MipLevels != 1 || SourceDesc.ArraySize != 1 || SourceDesc.SampleDesc.Count != 1)
	{
		OutWhy = TEXT("the sender's texture does not match its advertised size, format or single-mip, single-sample layout");
		return false;
	}
	bKeyedSource = (SourceDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0;

	Result = CreateCrossFence(true, WrittenFence, &NativeWrittenFence);
	if (FAILED(Result) || !WrittenFence || !NativeWrittenFence) return Refuse(TEXT("the written fence could not be created and shared"), Result);
	Result = CreateCrossFence(false, ReadFence, nullptr);
	if (FAILED(Result) || !ReadFence) return Refuse(TEXT("the read fence could not be created"), Result);
	Result = CreateProofQuery(Proof);
	if (FAILED(Result) || !Proof) return Refuse(TEXT("the event query could not be created"), Result);
	for (int32 I = 0; I < NumSlots; ++I)
	{
		Result = CreateSlot(Identity.Width, Identity.Height, Identity.Format, PixelFormat, SlotResources[I], NativeSlots[I], SlotTextures[I]);
		if (FAILED(Result)) return Refuse(TEXT("a slot could not be created, shared, opened or made an RHI texture"), Result);
	}
	Event = ::CreateEventW(nullptr, false, false, nullptr);
	if (!Event)
	{
		OutWhy = TEXT("an event could not be created");
		return false;
	}
	return true;
}

FSpoutSlotReceive::~FSpoutSlotReceive()
{
	Pool.Retire();
	TUniquePtr<FSpoutGpuRetirement::FRetiredSet> Record = MakeUnique<FSpoutGpuRetirement::FRetiredSet>();
	Record->What = FString::Printf(TEXT("receive slots of '%s' (generation %llu)"), *Name.ToString(), Generation);
	Record->NativeFence = WrittenFence;
	Record->QueueFence = ReadFence;
	Record->Proof = Proof;
	Record->Outstanding = Outstanding;
	for (int32 I = 0; I < NumSlots; ++I)
	{
		if (SlotTextures[I].IsValid())
		{
			Record->Textures.Add(SlotTextures[I]);
		}
		AddObject(Record->Objects, NativeSlots[I].Get());
		AddObject(Record->Objects, SlotResources[I].Get());
	}
	AddObject(Record->Objects, NativeSource.Get());
	AddObject(Record->Objects, NativeWrittenFence.Get());
	Record->Event = Event;
	Event = nullptr;
	Record->Reservation = MoveTemp(Reservation);
	FSpoutGpuRetirement::Get().Retire(MoveTemp(Record));
	// With the native debug layer on: what it stored about these objects so far.
	FSpoutAsyncDevice::Get().ReportDebugLayer();
}

void FSpoutSlotReceive::Fail(const TCHAR* Why, HRESULT Result)
{
	if (bFailed)
	{
		return;
	}
	bFailed = true;
	Failure = FString::Printf(TEXT("%s (hr=0x%08x)"), Why, static_cast<uint32>(Result));
	UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout receiver '%s': the shared-slot receive stopped for this sender identity: %s. It stays stopped until the sender's size, format or handle changes; its objects are retired."),
		*Name.ToString(), *Failure);
}

int32 FSpoutSlotReceive::BeginWrite()
{
	if (bFailed)
	{
		return -1;
	}
	const uint64 ReadDone = ReadFence->GetCompletedValue();
	if (ReadDone == SpoutSlotRules::RemovedValue)
	{
		Fail(TEXT("Unreal's device was removed"), DXGI_ERROR_DEVICE_REMOVED);
		return -1;
	}
	return Pool.BeginWrite(ReadDone);
}

ESpoutSlotCopy FSpoutSlotReceive::CopyAndWait(int32 Slot, uint64& OutValue)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SlotReceiveCopy);
	OutValue = 0;
	FSpoutAsyncDevice& Async = FSpoutAsyncDevice::Get();
	if (bFailed || Slot < 0 || Slot >= NumSlots)
	{
		return ESpoutSlotCopy::NotStarted;
	}
	HRESULT SignalResult = S_OK;
	uint64 Value = 0;
	{
		FScopeLock ContextLock(&Async.GetContextMutex());
		ID3D11DeviceContext4* Context = Async.GetContext();
		if (!Context)
		{
			Fail(TEXT("the native device is gone"), E_UNEXPECTED);
			return ESpoutSlotCopy::NotStarted;
		}
		Context->CopyResource(NativeSlots[Slot].Get(), NativeSource.Get());
		Value = ++WrittenValue;
		SignalResult = SignalNative(Context, NativeWrittenFence.Get(), Value);
		if (FAILED(SignalResult))
		{
			// The copy is submitted and no fence value will prove it: an event query issued after it will.
			Context->End(Proof.Get());
		}
		Context->Flush();
	}
	const bool bSignalled = SUCCEEDED(SignalResult);
	if (bSignalled)
	{
		Outstanding.NativeSignalled(Value);
	}
	else
	{
		// The copy is submitted and no fence value will prove it: the event query ended after it will.
		Outstanding.NativeSignalFailed(true);
	}

	// The copy reads the peer's texture, and the caller holds the peer's access. Access is given back only once the
	// read is proven finished, or the native device is removed: a copy that misses its bound is discarded, but the
	// peer may not write while it still runs.
	ENativeProof Proven = WaitForNativeWork(WrittenFence.Get(), Value, bSignalled, Proof.Get(), CompletionTimeoutMs, Event,
		FString::Printf(TEXT("receiver '%s'"), *Name.ToString()));
	if (Proven == ENativeProof::Removed)
	{
		Fail(TEXT("the native device was removed"), DXGI_ERROR_DEVICE_REMOVED);
		return ESpoutSlotCopy::Removed;
	}
	if (!bSignalled)
	{
		Outstanding.ProofCompleted();
		Fail(TEXT("a fence signal on the native device failed"), SignalResult);
		return ESpoutSlotCopy::SignalFailed;
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (TakeHook(SpoutSlotTestHooks::TimeoutReceiveWaits))
	{
		Proven = ENativeProof::Late;
	}
#endif
	if (Proven == ENativeProof::Late)
	{
		// Finished, but after the bound: discarded, never published; the frame stays pending.
		if (Timeouts.OnTimeout())
		{
			Fail(TEXT("slot copies repeatedly finished later than their bound"), HRESULT_FROM_WIN32(ERROR_TIMEOUT));
		}
		return ESpoutSlotCopy::Late;
	}
	Timeouts.OnSuccess();
	OutValue = Value;
	return ESpoutSlotCopy::Completed;
}

void FSpoutSlotReceive::Abandon(int32 Slot, SpoutAsync::EAbandon Why)
{
	Pool.AbandonWrite(Slot, Why);
}

bool FSpoutSlotReceive::PublishAndConsume(int32 Slot, uint64 Value, FRHICommandListImmediate& RHICmdList, FRHITexture* Destination,
	ERHIAccess DestinationBefore, ERHIAccess DestinationAfter)
{
	if (!Destination)
	{
		Pool.AbandonWrite(Slot, SpoutAsync::EAbandon::Cancelled);
		return false;
	}
	if (Pool.Publish(Slot, Generation, Value, 0) == 0)
	{
		return false;
	}
	SpoutAsync::FConsumed Taken;
	const uint64 NextRead = ReadValue + 1;
	if (Pool.Consume(Generation, NextRead, Taken) != SpoutAsync::EConsume::Consumed)
	{
		return false;
	}
	ReadValue = NextRead;
	Outstanding.QueueSignalQueued(NextRead);

	// Three queue operations, in order. The wait is already satisfied (the CPU observed the value) and orders the
	// two devices' memory; the read signal is what proves, later, that the copy out of the slot finished.
	FRHITexture* SlotTexture = SlotTextures[Taken.Slot].GetReference();
	const ComPtr<ID3D12Fence> Written = WrittenFence;
	const ComPtr<ID3D12Fence> Read = ReadFence;
	const uint64 WaitValue = Taken.WrittenValue;
	const uint64 SignalValue = Taken.ReadValue;
	RHICmdList.EnqueueLambda([Written, WaitValue](FRHICommandListImmediate& Cmd)
	{
		QueueWaitManualFence(Cmd, Written.Get(), WaitValue);
	});
	SPOUT_D3D_DIAG_TRANSITION("receive.slot.to-copysrc", SlotTexture, ERHIAccess::Unknown, ERHIAccess::CopySrc);
	RHICmdList.Transition(FRHITransitionInfo(SlotTexture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
	SPOUT_D3D_DIAG_TRANSITION("receive.destination.to-copydest", Destination, DestinationBefore, ERHIAccess::CopyDest);
	RHICmdList.Transition(FRHITransitionInfo(Destination, DestinationBefore, ERHIAccess::CopyDest));
	RHICmdList.CopyTexture(SlotTexture, Destination, FRHICopyTextureInfo());
	SPOUT_D3D_DIAG_TRANSITION("receive.slot.to-srv", SlotTexture, ERHIAccess::CopySrc, ERHIAccess::SRVMask);
	RHICmdList.Transition(FRHITransitionInfo(SlotTexture, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
	SPOUT_D3D_DIAG_TRANSITION("receive.destination.to-after", Destination, ERHIAccess::CopyDest, DestinationAfter);
	RHICmdList.Transition(FRHITransitionInfo(Destination, ERHIAccess::CopyDest, DestinationAfter));
	RHICmdList.EnqueueLambda([Read, SignalValue](FRHICommandListImmediate& Cmd)
	{
		GetID3D12DynamicRHI()->RHISignalManualFence(Cmd, Read.Get(), SignalValue);
	});
	return true;
}

void FSpoutSlotReceive::FlushKeyedRelease()
{
	FSpoutAsyncDevice& Async = FSpoutAsyncDevice::Get();
	FScopeLock ContextLock(&Async.GetContextMutex());
	if (ID3D11DeviceContext4* Context = Async.GetContext())
	{
		Context->Flush();
	}
}

// ---------------------------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------------------------

FSpoutSlotSend::FSpoutSlotSend(const FString& InWhat, const FIdentity& InIdentity)
	: What(InWhat)
	, Identity(InIdentity)
{
}

TUniquePtr<FSpoutSlotSend> FSpoutSlotSend::Create(const FString& What, const FIdentity& Identity, FString& OutWhy)
{
	check(IsInRenderingThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SlotSendCreate);
	TUniquePtr<FSpoutSlotSend> Set(new FSpoutSlotSend(What, Identity));
	if (!Set->Initialize(OutWhy))
	{
		return nullptr;
	}
	return Set;
}

bool FSpoutSlotSend::Initialize(FString& OutWhy)
{
	if (!SpoutInterop::IsSharedSlot() || !NativeDevice() || !UnrealDevice())
	{
		OutWhy = TEXT("the shared-slot path or one of its devices is not available");
		return false;
	}
	int64 FrameBytes = 0;
	if (!SpoutLimits::FrameBytes(Identity.Width, Identity.Height, Identity.PixelFormat, FrameBytes))
	{
		OutWhy = TEXT("the frame size is outside the plugin's limits");
		return false;
	}
	Reservation = FSpoutResourceReservation::TryReserve(FrameBytes);
	if (!Reservation)
	{
		OutWhy = TEXT("the retained-memory budget has no room for the slot");
		return false;
	}
	auto Refuse = [&OutWhy](const TCHAR* Text, HRESULT Result)
	{
		OutWhy = FString::Printf(TEXT("%s (hr=0x%08x)"), Text, static_cast<uint32>(Result));
		return false;
	};
	HRESULT Result = CreateCrossFence(false, FilledFence, nullptr);
	if (FAILED(Result) || !FilledFence) return Refuse(TEXT("the filled fence could not be created"), Result);
	Result = CreateCrossFence(true, DrainedFence, &NativeDrainedFence);
	if (FAILED(Result) || !DrainedFence || !NativeDrainedFence) return Refuse(TEXT("the drained fence could not be created and shared"), Result);
	Result = CreateProofQuery(Proof);
	if (FAILED(Result) || !Proof) return Refuse(TEXT("the event query could not be created"), Result);
	Result = CreateSlot(Identity.Width, Identity.Height, Identity.Format, Identity.PixelFormat, SlotResource, NativeSlot, SlotTexture);
	if (FAILED(Result)) return Refuse(TEXT("the slot could not be created, shared, opened or made an RHI texture"), Result);
	Event = ::CreateEventW(nullptr, false, false, nullptr);
	if (!Event)
	{
		OutWhy = TEXT("an event could not be created");
		return false;
	}
	return true;
}

FSpoutSlotSend::~FSpoutSlotSend()
{
	TUniquePtr<FSpoutGpuRetirement::FRetiredSet> Record = MakeUnique<FSpoutGpuRetirement::FRetiredSet>();
	Record->What = FString::Printf(TEXT("send slot of %s"), *What);
	Record->NativeFence = DrainedFence;
	Record->QueueFence = FilledFence;
	Record->Proof = Proof;
	Record->Outstanding = Outstanding;
	if (SlotTexture.IsValid())
	{
		Record->Textures.Add(SlotTexture);
	}
	AddObject(Record->Objects, NativeSlot.Get());
	AddObject(Record->Objects, SlotResource.Get());
	AddObject(Record->Objects, NativeDrainedFence.Get());
	Record->Event = Event;
	Event = nullptr;
	Record->Reservation = MoveTemp(Reservation);
	FSpoutGpuRetirement::Get().Retire(MoveTemp(Record));
	FSpoutAsyncDevice::Get().ReportDebugLayer();
}

void FSpoutSlotSend::Fail(const TCHAR* Why, HRESULT Result)
{
	if (bFailed)
	{
		return;
	}
	bFailed = true;
	Failure = FString::Printf(TEXT("%s (hr=0x%08x)"), Why, static_cast<uint32>(Result));
	UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout %s: the shared-slot send stopped for this source size and format: %s. It stays stopped until they change; its objects are retired."),
		*What, *Failure);
}

bool FSpoutSlotSend::CanWrite() const
{
	if (bFailed || !DrainedFence)
	{
		return false;
	}
	return Outstanding.CanQueueWrite(DrainedFence->GetCompletedValue(), IsNativeRemoved());
}

uint64 FSpoutSlotSend::QueueFill(FRHICommandListImmediate& RHICmdList, FRHITexture* Source, ERHIAccess SourceBefore, ERHIAccess SourceCopy, ERHIAccess SourceAfter)
{
	FRHITexture* Slot = SlotTexture.GetReference();
	SPOUT_D3D_DIAG_TRANSITION("send.source.to-copy", Source, SourceBefore, SourceCopy);
	RHICmdList.Transition(FRHITransitionInfo(Source, SourceBefore, SourceCopy));
	SPOUT_D3D_DIAG_TRANSITION("send.slot.to-copydest", Slot, ERHIAccess::Unknown, ERHIAccess::CopyDest);
	RHICmdList.Transition(FRHITransitionInfo(Slot, ERHIAccess::Unknown, ERHIAccess::CopyDest));
	RHICmdList.CopyTexture(Source, Slot, FRHICopyTextureInfo());
#if WITH_DEV_AUTOMATION_TESTS
	SpoutSlotTestHooks::OnSourceCopied(RHICmdList, Source);
#endif
	SPOUT_D3D_DIAG_TRANSITION("send.source.to-after", Source, SourceCopy, SourceAfter);
	RHICmdList.Transition(FRHITransitionInfo(Source, SourceCopy, SourceAfter));
	SPOUT_D3D_DIAG_TRANSITION("send.slot.to-srv", Slot, ERHIAccess::CopyDest, ERHIAccess::SRVMask);
	RHICmdList.Transition(FRHITransitionInfo(Slot, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
	const uint64 Value = ++FilledValue;
	Outstanding.QueueSignalQueued(Value);
	const ComPtr<ID3D12Fence> Filled = FilledFence;
	RHICmdList.EnqueueLambda([Filled, Value](FRHICommandListImmediate& Cmd)
	{
		GetID3D12DynamicRHI()->RHISignalManualFence(Cmd, Filled.Get(), Value);
	});
	return Value;
}

SpoutSlotRules::EWait FSpoutSlotSend::WaitFilled(uint64 Value)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SlotSendWaitFilled);
#if WITH_DEV_AUTOMATION_TESTS
	if (TakeHook(SpoutSlotTestHooks::TimeoutFilledWaits))
	{
		return SpoutSlotRules::EWait::TimedOut;
	}
#endif
	const SpoutSlotRules::EWait Wait = WaitForFenceValue(FilledFence.Get(), Value, FilledTimeoutMs, Event);
	if (Wait == SpoutSlotRules::EWait::Removed)
	{
		Fail(TEXT("a device was removed"), DXGI_ERROR_DEVICE_REMOVED);
	}
	return Wait;
}

ESpoutSlotCopy FSpoutSlotSend::DrainInto(ID3D11Texture2D* Destination)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SlotSendDrain);
	FSpoutAsyncDevice& Async = FSpoutAsyncDevice::Get();
	if (bFailed || !Destination)
	{
		return ESpoutSlotCopy::NotStarted;
	}
	HRESULT SignalResult = S_OK;
	uint64 Value = 0;
	{
		FScopeLock ContextLock(&Async.GetContextMutex());
		ID3D11DeviceContext4* Context = Async.GetContext();
		if (!Context)
		{
			Fail(TEXT("the native device is gone"), E_UNEXPECTED);
			return ESpoutSlotCopy::NotStarted;
		}
		Context->CopyResource(Destination, NativeSlot.Get());
		Value = ++DrainedValue;
		SignalResult = SignalNative(Context, NativeDrainedFence.Get(), Value);
		if (FAILED(SignalResult))
		{
			Context->End(Proof.Get());
		}
		Context->Flush();
	}
	const bool bSignalled = SUCCEEDED(SignalResult);
	if (bSignalled)
	{
		Outstanding.NativeSignalled(Value);
	}
	else
	{
		Outstanding.NativeSignalFailed(true);
	}

	// The copy writes a texture a peer reads, and the caller holds that name's access. Access is given back only once
	// the write is proven finished, or the native device is removed: never on a wall-clock bound (the rule of
	// SpoutGpuCompletion.h), and never because a failed proof leaves the texture retained.
	const ENativeProof Proven = WaitForNativeWork(DrainedFence.Get(), Value, bSignalled, Proof.Get(), MAX_uint32, Event, What);
	if (Proven == ENativeProof::Removed)
	{
		Fail(TEXT("the native device was removed"), DXGI_ERROR_DEVICE_REMOVED);
		return ESpoutSlotCopy::Removed;
	}
	if (!bSignalled)
	{
		// Proven finished, but nothing is announced: the slot stops for this source size and format.
		Outstanding.ProofCompleted();
		Fail(TEXT("a fence signal on the native device failed"), SignalResult);
		return ESpoutSlotCopy::SignalFailed;
	}
	return ESpoutSlotCopy::Completed;
}

void FSpoutSlotSend::FlushKeyedRelease()
{
	FSpoutAsyncDevice& Async = FSpoutAsyncDevice::Get();
	FScopeLock ContextLock(&Async.GetContextMutex());
	if (ID3D11DeviceContext4* Context = Async.GetContext())
	{
		Context->Flush();
	}
}

// ---------------------------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS
namespace SpoutSlotTestHooks
{
	std::atomic<int32> FailNativeSignals{0};
	std::atomic<int32> TimeoutReceiveWaits{0};
	std::atomic<int32> TimeoutFilledWaits{0};
	std::atomic<int32> DelayNativeCompletionMs{0};
	std::atomic<int32> FailProofQueries{0};

	namespace
	{
		FCriticalSection GCaptureMutex;
		bool GCaptureArmed = false;
		bool GCaptureTaken = false;
		TArray<FLinearColor> GCapturePixels;
		FIntPoint GCaptureSize = FIntPoint::ZeroValue;
		EPixelFormat GCaptureFormat = PF_Unknown;
	}

	void ArmSourceCapture()
	{
		FScopeLock Lock(&GCaptureMutex);
		GCaptureArmed = true;
		GCaptureTaken = false;
		GCapturePixels.Reset();
		GCaptureSize = FIntPoint::ZeroValue;
		GCaptureFormat = PF_Unknown;
	}

	bool TakeSourceCapture(TArray<FLinearColor>& OutPixels, FIntPoint& OutSize, EPixelFormat& OutFormat)
	{
		FScopeLock Lock(&GCaptureMutex);
		if (!GCaptureTaken)
		{
			return false;
		}
		OutPixels = MoveTemp(GCapturePixels);
		OutSize = GCaptureSize;
		OutFormat = GCaptureFormat;
		GCaptureTaken = false;
		return true;
	}

	void OnSourceCopied(FRHICommandListImmediate& RHICmdList, FRHITexture* Source)
	{
		{
			FScopeLock Lock(&GCaptureMutex);
			if (!GCaptureArmed || !Source)
			{
				return;
			}
			GCaptureArmed = false;
		}
		// The source is in its copy state here. Reading it back waits for the GPU: tests only.
		const FIntPoint Size(Source->GetDesc().Extent.X, Source->GetDesc().Extent.Y);
		TArray<FLinearColor> Pixels;
		// The same flags as the read-back of the received texture it is compared with: equal inputs give equal
		// outputs, and the conversion of 8- and 10-bit values to floats loses nothing.
		FReadSurfaceDataFlags Flags(RCM_MinMax);
		Flags.SetLinearToGamma(false);
		RHICmdList.ReadSurfaceData(Source, FIntRect(0, 0, Size.X, Size.Y), Pixels, Flags);
		FScopeLock Lock(&GCaptureMutex);
		GCapturePixels = MoveTemp(Pixels);
		GCaptureSize = Size;
		GCaptureFormat = Source->GetFormat();
		GCaptureTaken = true;
	}
}
#endif

#endif // PLATFORM_WINDOWS
