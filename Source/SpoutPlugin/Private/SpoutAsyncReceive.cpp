// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the asynchronous receive path, Async Receive (Beta): off by default and requested per receiver session.
 * See SpoutAsyncReceive.h for the design and the ownership rules, and SpoutSlotPool.h for the
 * bookkeeping.
 */
#include "SpoutAsyncReceive.h"

#if PLATFORM_WINDOWS

#include "SpoutFrameSync.h"
#include "SpoutInstrument.h"
#include "SpoutLimits.h"
#include "SpoutModule.h"
#include "HAL/RunnableThread.h"
#include "ID3D12DynamicRHI.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "RenderingThread.h"
#include "Runtime/Launch/Resources/Version.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <dxgi1_4.h>
#include "Windows/HideWindowsPlatformTypes.h"

using Microsoft::WRL::ComPtr;

namespace
{
	/** CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, for SDK headers that do not define it. */
	constexpr uint32 HighResolutionTimerFlag = 0x00000002;

	/** The next stream object's generation. Never 0, and never the same twice in a process. */
	std::atomic<uint64> GNextGeneration{1};

	/** Stream objects alive in the process. Each owns its worker thread, joined before the object is gone. */
	std::atomic<int32> GLiveAsyncStreams{0};

	/**
	 * Waits until Fence reached Value, for at most TimeoutMs. False on a timeout and on device
	 * removal (a removed device reports the largest value). Event is an auto-reset event used by one
	 * thread; an earlier wait that timed out may still set it, so the value is read again after
	 * every wake-up.
	 */
	bool WaitForFence(ID3D12Fence* Fence, uint64 Value, uint32 TimeoutMs, HANDLE Event)
	{
		uint64 Done = Fence->GetCompletedValue();
		if (Done == MAX_uint64) return false;
		if (Done >= Value) return true;
		if (TimeoutMs == 0 || !Event) return false;
		::ResetEvent(Event);
		if (FAILED(Fence->SetEventOnCompletion(Value, Event))) return false;
		const double End = FPlatformTime::Seconds() + double(TimeoutMs) / 1000.0;
		for (;;)
		{
			Done = Fence->GetCompletedValue();
			if (Done == MAX_uint64) return false;
			if (Done >= Value) return true;
			const double Left = End - FPlatformTime::Seconds();
			if (Left <= 0.0) return false;
			::WaitForSingleObject(Event, FMath::Max<uint32>(1, static_cast<uint32>(Left * 1000.0 + 0.999)));
		}
	}

	/** Queues a wait of Unreal's graphics queue on a fence another device signals. RHI thread. */
	void WaitManualFence(FRHICommandListImmediate& Cmd, ID3D12Fence* Fence, uint64 Value)
	{
#if ENGINE_MINOR_VERSION >= 8
		// No CPU-side "has signalled" flag: the copy this value stands for completed before the
		// slot was published, so the queue waits on the fence itself and finds it reached.
		GetID3D12DynamicRHI()->RHIWaitManualFence(Cmd, Fence, Value, TSharedPtr<std::atomic<bool>>());
#else
		GetID3D12DynamicRHI()->RHIWaitManualFence(Cmd, Fence, Value);
#endif
	}
}

// ---------------------------------------------------------------------------------------------
// The worker's device
// ---------------------------------------------------------------------------------------------

FSpoutAsyncDevice& FSpoutAsyncDevice::Get()
{
	static FSpoutAsyncDevice Instance;
	return Instance;
}

bool FSpoutAsyncDevice::EnsureCreated(FString& OutWhy)
{
	FScopeLock Lock(&InitMutex);
	if (bDisabled.load())
	{
		OutWhy = DisabledWhy;
		return false;
	}
	if (Device && Context)
	{
		return true;
	}
	auto Refuse = [this, &OutWhy](const FString& Why)
	{
		DisabledWhy = Why;
		OutWhy = Why;
		bDisabled = true;
		return false;
	};
	if (!GDynamicRHI || GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::D3D12)
	{
		return Refuse(TEXT("the RHI is not D3D12"));
	}
	ID3D12Device* Device12 = static_cast<ID3D12Device*>(GDynamicRHI->RHIGetNativeDevice());
	if (!Device12)
	{
		return Refuse(TEXT("Unreal's D3D12 device is not available"));
	}
	const LUID Wanted = Device12->GetAdapterLuid();
	ComPtr<IDXGIFactory4> Factory;
	HRESULT Result = CreateDXGIFactory1(IID_PPV_ARGS(Factory.GetAddressOf()));
	if (FAILED(Result))
	{
		return Refuse(FString::Printf(TEXT("the DXGI factory could not be created (hr=0x%08x)"), static_cast<uint32>(Result)));
	}
	ComPtr<IDXGIAdapter1> Adapter;
	Result = Factory->EnumAdapterByLuid(Wanted, IID_PPV_ARGS(Adapter.GetAddressOf()));
	if (FAILED(Result) || !Adapter)
	{
		return Refuse(FString::Printf(TEXT("the adapter Unreal renders with was not found (hr=0x%08x)"), static_cast<uint32>(Result)));
	}
	const D3D_FEATURE_LEVEL FeatureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	ComPtr<ID3D11Device> BaseDevice;
	ComPtr<ID3D11DeviceContext> BaseContext;
	// With Unreal's debug layer on, this device is a debug device as well, so both sides of the
	// slots and fences are checked. Without the SDK layers installed it is made as usual.
	bool bDebugLayer = GRHIGlobals.IsDebugLayerEnabled;
	Result = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | (bDebugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0),
		FeatureLevels, 2, D3D11_SDK_VERSION, BaseDevice.GetAddressOf(), nullptr, BaseContext.GetAddressOf());
	if (FAILED(Result) && bDebugLayer)
	{
		UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout asynchronous receive: the worker device could not be made with the D3D11 debug layer (hr=0x%08x); made without it."), static_cast<uint32>(Result));
		bDebugLayer = false;
		Result = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, FeatureLevels, 2, D3D11_SDK_VERSION,
			BaseDevice.GetAddressOf(), nullptr, BaseContext.GetAddressOf());
	}
	if (FAILED(Result) || !BaseDevice || !BaseContext)
	{
		return Refuse(FString::Printf(TEXT("the worker's D3D11 device could not be created (hr=0x%08x)"), static_cast<uint32>(Result)));
	}
	// The adapter of the device that was actually made, compared again: a mismatch is refused.
	ComPtr<IDXGIDevice> DxgiDevice;
	ComPtr<IDXGIAdapter> MadeOn;
	DXGI_ADAPTER_DESC MadeDesc = {};
	if (FAILED(BaseDevice.As(&DxgiDevice)) || FAILED(DxgiDevice->GetAdapter(MadeOn.GetAddressOf())) || FAILED(MadeOn->GetDesc(&MadeDesc))
		|| MadeDesc.AdapterLuid.LowPart != Wanted.LowPart || MadeDesc.AdapterLuid.HighPart != Wanted.HighPart)
	{
		return Refuse(TEXT("the worker's device is not on the adapter Unreal renders with"));
	}
	ComPtr<ID3D11Device5> Device5;
	ComPtr<ID3D11DeviceContext4> Context4;
	if (FAILED(BaseDevice.As(&Device5)) || FAILED(BaseContext.As(&Context4)))
	{
		return Refuse(TEXT("the D3D11 device has no shared fences (ID3D11Device5 and ID3D11DeviceContext4 are needed)"));
	}
	// A diagnostic, off by default: with 0 the device's GPU scheduling priority is left alone.
	const int32 GpuPriority = SpoutLimits::AsyncReceiveGpuPriority();
	if (GpuPriority != 0)
	{
		const HRESULT PriorityResult = DxgiDevice->SetGPUThreadPriority(GpuPriority);
		UE_LOG(LogSpoutPlugin, Log, TEXT("Spout asynchronous receive: GPU priority %d asked for the worker device (hr=0x%08x)."),
			GpuPriority, static_cast<uint32>(PriorityResult));
	}
	// The priority in force is read back from the device in every case, asked for or not, so that
	// the log records what the device actually runs with.
	int GpuPriorityNow = 0;
	const HRESULT PriorityReadResult = DxgiDevice->GetGPUThreadPriority(&GpuPriorityNow);
	const FString EffectivePriority = SUCCEEDED(PriorityReadResult) ? FString::Printf(TEXT("%d"), static_cast<int32>(GpuPriorityNow)) : FString(TEXT("unreadable"));
	if (bDebugLayer && SUCCEEDED(BaseDevice.As(&InfoQueue)) && InfoQueue)
	{
		// Corruption, errors and warnings are kept. Informational messages (every object made or
		// released produces one) are not.
		D3D11_MESSAGE_SEVERITY Denied[] = { D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE };
		D3D11_INFO_QUEUE_FILTER Filter = {};
		Filter.DenyList.NumSeverities = 2;
		Filter.DenyList.pSeverityList = Denied;
		// This queue belongs to the new device. Replace its default filter rather than
		// append to it (the SDK layer can already deny INFO). Do not clear messages.
		InfoQueue->ClearStorageFilter();
		InfoQueue->ClearRetrievalFilter();
		const HRESULT FilterResult = InfoQueue->AddStorageFilterEntries(&Filter);
		if (FAILED(FilterResult))
		{
			InfoQueue.Reset();
			return Refuse(FString::Printf(TEXT("the worker debug filter could not be configured (hr=0x%08x)"), static_cast<uint32>(FilterResult)));
		}
	}
	DebugLayerMessages = 0;
	Device = MoveTemp(Device5);
	Context = MoveTemp(Context4);
	UE_LOG(LogSpoutPlugin, Log, TEXT("Spout asynchronous receive: worker device created on '%s' (D3D11 debug layer %s, GPU priority setting %d, effective %s)."),
		MadeDesc.Description, InfoQueue ? TEXT("on") : TEXT("off"), GpuPriority, *EffectivePriority);
	return true;
}

void FSpoutAsyncDevice::ReportDebugLayer()
{
	FScopeLock Lock(&InitMutex);
	ReportDebugLayerLocked();
}

void FSpoutAsyncDevice::ReportDebugLayerLocked()
{
	if (!InfoQueue)
	{
		return;
	}
	const uint64 Stored = InfoQueue->GetNumStoredMessages();
	for (uint64 I = 0; I < Stored; ++I)
	{
		SIZE_T Length = 0;
		if (FAILED(InfoQueue->GetMessage(I, nullptr, &Length)) || Length < sizeof(D3D11_MESSAGE))
		{
			continue;
		}
		TArray<uint8> Buffer;
		Buffer.SetNumZeroed(static_cast<int32>(Length));
		D3D11_MESSAGE* Message = reinterpret_cast<D3D11_MESSAGE*>(Buffer.GetData());
		if (SUCCEEDED(InfoQueue->GetMessage(I, Message, &Length)) && Message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
		{
			++DebugLayerMessages;
			UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout asynchronous receive: D3D11 debug layer message (severity %d, id %d): %s"),
				static_cast<int32>(Message->Severity), static_cast<int32>(Message->ID), ANSI_TO_TCHAR(Message->pDescription));
		}
	}
	InfoQueue->ClearStoredMessages();
}

void FSpoutAsyncDevice::Shutdown()
{
	FScopeLock Lock(&InitMutex);
	{
		FScopeLock ContextLock(&ContextMutex);
		if (Context)
		{
			Context->ClearState();
			Context->Flush();
		}
		Context.Reset();
	}
	if (InfoQueue)
	{
		ReportDebugLayerLocked();
		UE_LOG(LogSpoutPlugin, Log, TEXT("Spout asynchronous receive: the worker device's D3D11 debug layer reported %lld messages in this process (corruption, errors and warnings; %llu more were dropped by its storage limit)."),
			DebugLayerMessages, static_cast<uint64>(InfoQueue->GetNumMessagesDiscardedByMessageCountLimit()));
		InfoQueue.Reset();
	}
	Device.Reset();
	bDisabled = false;
	DisabledWhy.Reset();
}

void FSpoutAsyncDevice::Disable(const TCHAR* Why, HRESULT Result)
{
	FScopeLock Lock(&InitMutex);
	if (bDisabled.load())
	{
		return;
	}
	DisabledWhy = FString::Printf(TEXT("%s (hr=0x%08x)"), Why, static_cast<uint32>(Result));
	bDisabled = true;
	UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout asynchronous receive is off for this process: %s. Streams use the synchronous path."), *DisabledWhy);
}

// ---------------------------------------------------------------------------------------------
// One stream
// ---------------------------------------------------------------------------------------------

FSpoutAsyncStream::FSpoutAsyncStream(const FName& InName, int32 InInstrumentStream, FSpoutFrameSync& InFrameSync, const FIdentity& InIdentity)
	: Name(InName)
	, InstrumentStream(InInstrumentStream)
	, FrameSync(InFrameSync)
	, Identity(InIdentity)
	, Generation(GNextGeneration.fetch_add(1))
{
	GLiveAsyncStreams.fetch_add(1);
}

int32 FSpoutAsyncStream::GetLiveStreams()
{
	return GLiveAsyncStreams.load();
}

TUniquePtr<FSpoutAsyncStream> FSpoutAsyncStream::Create(const FName& Name, int32 InstrumentStream, FSpoutFrameSync& FrameSync, const FIdentity& Identity, EPixelFormat PixelFormat, FString& OutWhy)
{
	check(IsInRenderingThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AsyncStreamCreate);
	TUniquePtr<FSpoutAsyncStream> Stream(new FSpoutAsyncStream(Name, InstrumentStream, FrameSync, Identity));
	if (!Stream->Initialize(PixelFormat, OutWhy))
	{
		return nullptr;
	}
	return Stream;
}

bool FSpoutAsyncStream::Initialize(EPixelFormat PixelFormat, FString& OutWhy)
{
	FSpoutAsyncDevice& Async = FSpoutAsyncDevice::Get();
	if (!Async.EnsureCreated(OutWhy))
	{
		return false;
	}
	if (!FrameSync.IsOpen())
	{
		OutWhy = TEXT("the stream's access mutex is not open");
		return false;
	}
	if (Identity.Format != DXGI_FORMAT_B8G8R8A8_UNORM && Identity.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
	{
		OutWhy = TEXT("the prototype handles BGRA8 and RGBA8 senders only");
		return false;
	}
	// No texture can be opened from a null handle: refused before any slot memory is reserved and before the open.
	if (Identity.SharedHandle == nullptr)
	{
		OutWhy = TEXT("the sender's shared texture handle is null");
		return false;
	}
	ID3D12Device* Device12 = static_cast<ID3D12Device*>(GDynamicRHI->RHIGetNativeDevice());
	ID3D11Device5* Device11 = Async.GetDevice();
	if (!Device12 || !Device11)
	{
		OutWhy = TEXT("a device is not available");
		return false;
	}

	// Three frames of the sender's size, charged to the retained-memory budget before anything is made.
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

	// The sender's texture, opened on the worker's device, and checked against what was advertised.
	HRESULT Result = Device11->OpenSharedResource(Identity.SharedHandle, IID_PPV_ARGS(WorkerSource.GetAddressOf()));
	if (FAILED(Result) || !WorkerSource)
	{
		return Refuse(TEXT("the sender's texture could not be opened on the worker's device"), Result);
	}
	D3D11_TEXTURE2D_DESC SourceDesc = {};
	WorkerSource->GetDesc(&SourceDesc);
	if (SourceDesc.Width != Identity.Width || SourceDesc.Height != Identity.Height || SourceDesc.Format != Identity.Format
		|| SourceDesc.MipLevels != 1 || SourceDesc.ArraySize != 1 || SourceDesc.SampleDesc.Count != 1)
	{
		OutWhy = TEXT("the sender's texture does not match its advertised size, format or single-mip, single-sample layout");
		return false;
	}
	bKeyedSource = (SourceDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0;

	// The two fences, on Unreal's device. The worker signals the written fence from its own device.
	Result = Device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(WrittenFence.GetAddressOf()));
	if (FAILED(Result)) return Refuse(TEXT("the written fence could not be created"), Result);
	Result = Device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(ReadFence.GetAddressOf()));
	if (FAILED(Result)) return Refuse(TEXT("the read fence could not be created"), Result);
	{
		HANDLE FenceHandle = nullptr;
		Result = Device12->CreateSharedHandle(WrittenFence.Get(), nullptr, GENERIC_ALL, nullptr, &FenceHandle);
		if (FAILED(Result) || !FenceHandle) return Refuse(TEXT("the written fence could not be shared"), Result);
		Result = Device11->OpenSharedFence(FenceHandle, IID_PPV_ARGS(WorkerWrittenFence.GetAddressOf()));
		::CloseHandle(FenceHandle);
		if (FAILED(Result) || !WorkerWrittenFence) return Refuse(TEXT("the written fence could not be opened on the worker's device"), Result);
	}

	// The slots: shared textures on Unreal's device, wrapped for Unreal and opened on the worker's
	// device. Created in the common state and wrapped as shared, so the state Unreal assumes for
	// them is the state they are in.
	D3D12_HEAP_PROPERTIES Heap = {};
	Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	Heap.CreationNodeMask = 1;
	Heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC SlotDesc = {};
	SlotDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	SlotDesc.Width = Identity.Width;
	SlotDesc.Height = Identity.Height;
	SlotDesc.DepthOrArraySize = 1;
	SlotDesc.MipLevels = 1;
	SlotDesc.Format = Identity.Format;
	SlotDesc.SampleDesc.Count = 1;
	SlotDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
	for (int32 I = 0; I < NumSlots; ++I)
	{
		Result = Device12->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_SHARED, &SlotDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(SlotResources[I].GetAddressOf()));
		if (FAILED(Result) || !SlotResources[I]) return Refuse(TEXT("a slot texture could not be created"), Result);
		HANDLE SlotHandle = nullptr;
		Result = Device12->CreateSharedHandle(SlotResources[I].Get(), nullptr, GENERIC_ALL, nullptr, &SlotHandle);
		if (FAILED(Result) || !SlotHandle) return Refuse(TEXT("a slot texture could not be shared"), Result);
		Result = Device11->OpenSharedResource1(SlotHandle, IID_PPV_ARGS(WorkerSlots[I].GetAddressOf()));
		::CloseHandle(SlotHandle);
		if (FAILED(Result) || !WorkerSlots[I]) return Refuse(TEXT("a slot texture could not be opened on the worker's device"), Result);
		SlotTextures[I] = GetID3D12DynamicRHI()->RHICreateTexture2DFromResource(PixelFormat, ETextureCreateFlags::Shared | ETextureCreateFlags::DisableSRVCreation,
			FClearValueBinding::None, SlotResources[I].Get());
		if (!SlotTextures[I].IsValid())
		{
			OutWhy = TEXT("a slot texture could not be wrapped for Unreal");
			return false;
		}
	}

	StopEvent = ::CreateEventW(nullptr, true, false, nullptr);
	FenceEvent = ::CreateEventW(nullptr, false, false, nullptr);
	if (!StopEvent || !FenceEvent)
	{
		OutWhy = TEXT("an event could not be created");
		return false;
	}
	// Absent on systems without high-resolution timers; the poll then waits a millisecond.
	PollTimer = ::CreateWaitableTimerExW(nullptr, nullptr, HighResolutionTimerFlag, TIMER_ALL_ACCESS);

	{
		FScopeLock Lock(&StateMutex);
		Gate.Reset(); // the stream opens: the current image is owed, whatever the notifications say
	}
	Thread = FRunnableThread::Create(this, *FString::Printf(TEXT("SpoutAsyncReceive %s"), *Name.ToString()), 0, TPri_Normal);
	if (!Thread)
	{
		OutWhy = TEXT("the worker thread could not be started");
		return false;
	}
#if SPOUT_WITH_INSTRUMENT
	if (SpoutInstrument::IsEnabled())
	{
		ReadWatch = MakeUnique<SpoutInstrument::FDestinationFenceWatch>(ReadFence.Get(), InstrumentStream, Generation, 2);
	}
#endif
	return true;
}

FSpoutAsyncStream::~FSpoutAsyncStream()
{
	Stop();
	if (Thread)
	{
		// The worker sees the request between transfers. One in progress runs to its end: it gives
		// access back first. That is bounded by the access wait and the completion bound.
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
#if SPOUT_WITH_INSTRUMENT
	ReadWatch.Reset();
#endif
	if (WorkerSeconds.load() > 0.0)
	{
		// One line per stream object, from the bookkeeping's own counters.
		const FSpoutAsyncStats Stats = GetStats();
		UE_LOG(LogSpoutPlugin, Log, TEXT("Spout receiver '%s': asynchronous receive ended (stream object %llu, %.1f s): %lld polls, %lld images published, %lld consumed, ")
			TEXT("%lld replaced unread, %lld transfers skipped for no free slot, %lld accesses refused, %lld copies failed, %lld completions timed out, %lld rule violations; ")
			TEXT("worker thread CPU time %.1f ms."),
			*Name.ToString(), Generation, WorkerSeconds.load(), Stats.Polls, Stats.Published, Stats.Consumed, Stats.ReplacedUnread, Stats.NoFreeSlot, Stats.AccessRefused,
			Stats.CopyFailed, Stats.CompletionTimeouts, Stats.RuleViolations, double(WorkerCpu100ns.load()) / 10000.0);
	}
	{
		FScopeLock Lock(&StateMutex);
		Pool.Retire();
	}
	// Nothing writes the slots any more. Copies Unreal has queued out of them keep the slots' RHI
	// textures alive on Unreal's side until its queue has passed them, so releasing here is safe.
	for (int32 I = 0; I < NumSlots; ++I)
	{
		WorkerSlots[I].Reset();
		SlotTextures[I].SafeRelease();
		SlotResources[I].Reset();
	}
	WorkerSource.Reset();
	WorkerWrittenFence.Reset();
	WrittenFence.Reset();
	ReadFence.Reset();
	if (PollTimer) ::CloseHandle(PollTimer);
	if (FenceEvent) ::CloseHandle(FenceEvent);
	if (StopEvent) ::CloseHandle(StopEvent);
	Reservation.Reset();
	// With the debug layer on: what the worker's device had to say about this stream's resources.
	FSpoutAsyncDevice::Get().ReportDebugLayer();
	GLiveAsyncStreams.fetch_sub(1);
}

void FSpoutAsyncStream::Stop()
{
	bStop = true;
	if (StopEvent)
	{
		::SetEvent(StopEvent);
	}
}

void FSpoutAsyncStream::Fail(const TCHAR* Why, HRESULT Result)
{
	if (bFailed.load())
	{
		return;
	}
	Failure = FString::Printf(TEXT("%s (hr=0x%08x)"), Why, static_cast<uint32>(Result));
	bFailed = true;
}

void FSpoutAsyncStream::WaitForNextPoll()
{
	if (PollTimer)
	{
		LARGE_INTEGER Due;
		Due.QuadPart = -static_cast<int64>(PollIntervalMicroseconds) * 10; // relative, in 100 ns
		if (::SetWaitableTimer(PollTimer, &Due, 0, nullptr, nullptr, false))
		{
			const HANDLE Handles[2] = { StopEvent, PollTimer };
			::WaitForMultipleObjects(2, Handles, false, 2);
			return;
		}
	}
	::WaitForSingleObject(StopEvent, 1);
}

uint32 FSpoutAsyncStream::Run()
{
	const double Started = FPlatformTime::Seconds();
	while (!bStop.load() && !bFailed.load())
	{
		// The poll: the SDK's public notification read, and nothing else of the frame sync's state.
		bool bCountUsable = false;
		const bool bNew = FrameSync.PollNotification(bCountUsable);
		++Polls;
		bool bOwed = false;
		{
			FScopeLock Lock(&StateMutex);
			if (bCountUsable)
			{
				Gate.ObserveCounted(bNew);
			}
			else
			{
				Gate.ObserveUncounted();
			}
			bOwed = Gate.IsOwed();
		}
		// At most one transfer in flight and at most one per poll. A transfer that was skipped or
		// given up stays owed and is tried again after the poll interval, not at once.
		if (!bOwed || !TransferOnce())
		{
			WaitForNextPoll();
		}
	}
	// What this thread cost, for the line the stream logs when it ends.
	FILETIME Created, Exited, Kernel, User;
	if (::GetThreadTimes(::GetCurrentThread(), &Created, &Exited, &Kernel, &User))
	{
		const uint64 KernelTime = (static_cast<uint64>(Kernel.dwHighDateTime) << 32) | Kernel.dwLowDateTime;
		const uint64 UserTime = (static_cast<uint64>(User.dwHighDateTime) << 32) | User.dwLowDateTime;
		WorkerCpu100ns = static_cast<int64>(KernelTime + UserTime);
	}
	WorkerSeconds = FPlatformTime::Seconds() - Started;
	return 0;
}

bool FSpoutAsyncStream::TransferOnce()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AsyncTransfer);
	FSpoutAsyncDevice& Async = FSpoutAsyncDevice::Get();
	SPOUT_INSTRUMENT(
		const bool bInstrument = SpoutInstrument::IsEnabled();
		SpoutInstrument::FRecord Rec;
		if (bInstrument)
		{
			Rec.Kind = TEXT('W');
			Rec.Stream = InstrumentStream;
			Rec.FrameRT = GFrameCounterRenderThread;
			Rec.TDetect = SpoutInstrument::Now();
			Rec.Generation = static_cast<int64>(Generation);
		}
	)

	// 1. A slot. None free: wait, bounded, for the oldest read still pending, and ask once more.
	uint64 ReadDone = ReadFence->GetCompletedValue();
	if (ReadDone == MAX_uint64)
	{
		Fail(TEXT("Unreal's device was removed"), DXGI_ERROR_DEVICE_REMOVED);
		return false;
	}
	int32 Slot = -1;
	uint64 SlotReadValue = 0;
	uint64 Oldest = 0;
	{
		FScopeLock Lock(&StateMutex);
		Slot = Pool.BeginWrite(ReadDone);
		if (Slot >= 0) SlotReadValue = Pool.GetSlot(Slot).ReadValue;
		else Oldest = Pool.OldestPendingReadValue();
	}
	if (Slot < 0)
	{
		if (Oldest != 0)
		{
			WaitForFence(ReadFence.Get(), Oldest, NoSlotWaitMs, FenceEvent);
		}
		ReadDone = ReadFence->GetCompletedValue();
		if (ReadDone != MAX_uint64)
		{
			FScopeLock Lock(&StateMutex);
			Slot = Pool.BeginWrite(ReadDone);
			if (Slot >= 0) SlotReadValue = Pool.GetSlot(Slot).ReadValue;
		}
		if (Slot < 0)
		{
			// Skipped. The transfer stays owed; the sender is never held for it.
			++NoSlotSkips;
			SPOUT_INSTRUMENT(if (bInstrument) { Rec.AsyncOutcome = 4; Rec.ReadValue = static_cast<int64>(ReadDone); Rec.TRestore = SpoutInstrument::Now(); SpoutInstrument::Push(Rec); })
			return false;
		}
	}
	SPOUT_INSTRUMENT(if (bInstrument) { Rec.Slot = Slot; Rec.SlotReadValue = static_cast<int64>(SlotReadValue); Rec.ReadValue = static_cast<int64>(ReadDone); })
	if (SlotReadValue > ReadDone)
	{
		// The bookkeeping never offers a slot whose last read has not completed. Checked against the
		// fence itself all the same: writing such a slot could corrupt an image Unreal is reading.
		{
			FScopeLock Lock(&StateMutex);
			Pool.AbandonWrite(Slot, SpoutAsync::EAbandon::Cancelled);
		}
		Fail(TEXT("a slot was offered before its last read completed"), E_UNEXPECTED);
		SPOUT_INSTRUMENT(if (bInstrument) { Rec.AsyncOutcome = 5; Rec.TRestore = SpoutInstrument::Now(); SpoutInstrument::Push(Rec); })
		return false;
	}

	// 2. Access, with the same bounded wait as the synchronous path, taken and given back by this
	// thread. 3. The notification read again under access. 4. The copy and the written-fence signal.
	// 5. The wait for that copy, bounded, while access is still held: the sender may overwrite its
	// texture as soon as access is released.
	bool bAcquired = false;
	bool bDone = false;
	HRESULT CopyResult = S_OK;
	uint64 Value = 0;
	uint64 AcquiredAt = 0;
	{
		SPOUT_INSTRUMENT(if (bInstrument) Rec.TAccess0 = SpoutInstrument::Now();)
		FSpoutTextureAccessScope Access(FrameSync, WorkerSource.Get());
		bAcquired = Access.IsAcquired();
		AcquiredAt = FPlatformTime::Cycles64();
		SPOUT_INSTRUMENT(if (bInstrument) { Rec.TAcquire = AcquiredAt; Rec.Acquired = bAcquired ? 1 : 0; })
		if (bAcquired)
		{
			// A sender publishes before it releases access, so what is published now is what this
			// copy takes. Reading the notification here keeps that publication from being reported
			// new again after its image was copied. It clears nothing: only a completed copy does.
			bool bUnused = false;
			FrameSync.PollNotification(bUnused);
			{
				FScopeLock ContextLock(&Async.GetContextMutex());
				ID3D11DeviceContext4* Context = Async.GetContext();
				Context->CopyResource(WorkerSlots[Slot].Get(), WorkerSource.Get());
				Value = ++WrittenValue;
				CopyResult = Context->Signal(WorkerWrittenFence.Get(), Value);
				Context->Flush();
			}
			SPOUT_INSTRUMENT(if (bInstrument) { Rec.TCopyRec = SpoutInstrument::Now(); Rec.WrittenValue = static_cast<int64>(Value); })
			bDone = SUCCEEDED(CopyResult) && WaitForFence(WrittenFence.Get(), Value, CompletionTimeoutMs, FenceEvent);
			SPOUT_INSTRUMENT(if (bInstrument) { Rec.TGpuDone = SpoutInstrument::Now(); Rec.Hr = static_cast<int32>(CopyResult); })
		}
	}
	// Access has been given back. A keyed texture's release is recorded on this device's context and
	// reaches the sender when the context is flushed.
	if (bAcquired && bKeyedSource)
	{
		FScopeLock ContextLock(&Async.GetContextMutex());
		Async.GetContext()->Flush();
	}
	SPOUT_INSTRUMENT(if (bInstrument) Rec.TRelease = SpoutInstrument::Now();)

	// 6. Publish the slot, or give the write up. A write that was given up is never published.
	SpoutAsync::EAbandon Why = SpoutAsync::EAbandon::Cancelled;
	uint64 Sequence = 0;
	{
		FScopeLock Lock(&StateMutex);
		if (bDone)
		{
			Sequence = Pool.Publish(Slot, Generation, Value, static_cast<int64>(AcquiredAt));
			if (Sequence != 0)
			{
				Gate.OnPublished();
			}
		}
		else
		{
			Why = !bAcquired ? SpoutAsync::EAbandon::AccessRefused : (FAILED(CopyResult) ? SpoutAsync::EAbandon::CopyFailed : SpoutAsync::EAbandon::CompletionTimeout);
			Pool.AbandonWrite(Slot, Why);
		}
	}
	if (bDone)
	{
		ConsecutiveTimeouts = 0;
	}
	else if (bAcquired)
	{
		// A removed device or a failed fence call ends the asynchronous path for the process; a copy
		// that keeps missing its bound ends it for this stream. Either way the caller falls back.
		const HRESULT Removed = Async.GetDevice()->GetDeviceRemovedReason();
		if (Removed != S_OK)
		{
			Async.Disable(TEXT("the worker's device was removed"), Removed);
			Fail(TEXT("the worker's device was removed"), Removed);
		}
		else if (FAILED(CopyResult))
		{
			Async.Disable(TEXT("a written-fence signal failed"), CopyResult);
			Fail(TEXT("a written-fence signal failed"), CopyResult);
		}
		else if (++ConsecutiveTimeouts >= MaxConsecutiveTimeouts)
		{
			Fail(TEXT("slot copies repeatedly did not complete in time"), HRESULT_FROM_WIN32(ERROR_TIMEOUT));
		}
	}
	SPOUT_INSTRUMENT(
		if (bInstrument)
		{
			Rec.TRestore = SpoutInstrument::Now();
			Rec.Sequence = static_cast<int64>(Sequence);
			Rec.Completed = Sequence != 0 ? 1 : 0;
			Rec.AsyncOutcome = Sequence != 0 ? 0 : (bDone ? 5 : 1 + static_cast<int32>(Why));
			SpoutInstrument::Push(Rec);
		}
	)
	return Sequence != 0;
}

bool FSpoutAsyncStream::ConsumeOnRenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture* Destination, uint64 ReceivedBefore)
{
	check(IsInRenderingThread());
	if (!Destination || bFailed.load())
	{
		return false;
	}
	SPOUT_INSTRUMENT(
		const bool bInstrument = SpoutInstrument::IsEnabled();
		SpoutInstrument::FRecord Rec;
		if (bInstrument)
		{
			Rec.Kind = TEXT('A');
			Rec.CompletionPath = 2;
			Rec.CompletionStage = 1;
			Rec.Stream = InstrumentStream;
			Rec.FrameRT = GFrameCounterRenderThread;
			Rec.TDetect = SpoutInstrument::Now();
			Rec.Generation = static_cast<int64>(Generation);
			Rec.ReceivedBefore = ReceivedBefore;
			Rec.ReceivedAfter = ReceivedBefore;
		}
	)
	SpoutAsync::FConsumed Taken;
	SpoutAsync::EConsume Result = SpoutAsync::EConsume::Nothing;
	{
		FScopeLock Lock(&StateMutex);
		Result = Pool.Consume(Generation, LastReadValue + 1, Taken);
		if (Result == SpoutAsync::EConsume::Consumed)
		{
			++LastReadValue;
			Gate.OnConsumed();
		}
	}
	const bool bConsumed = Result == SpoutAsync::EConsume::Consumed;
	SPOUT_INSTRUMENT(
		if (bInstrument)
		{
			Rec.AsyncOutcome = bConsumed ? 0 : (Result == SpoutAsync::EConsume::Nothing ? 1 : 2);
			if (bConsumed)
			{
				Rec.Slot = Taken.Slot;
				Rec.Sequence = static_cast<int64>(Taken.Sequence);
				Rec.WrittenValue = static_cast<int64>(Taken.WrittenValue);
				Rec.ReadValue = static_cast<int64>(Taken.ReadValue);
				Rec.TAcquire = static_cast<uint64>(Taken.Tag); // the worker's access time of the transfer this frame shows
				Rec.ReceivedAfter = ReceivedBefore + 1;
				Rec.Completed = 1;
				Rec.Delivered = 1;
			}
		}
	)
	if (!bConsumed)
	{
		SPOUT_INSTRUMENT(if (bInstrument) { Rec.TRestore = SpoutInstrument::Now(); SpoutInstrument::Push(Rec); })
		return false;
	}

	// Three queue operations, in order: wait until the slot's copy has completed on the worker's
	// device, copy the slot into Unreal's texture, signal the read fence. The render thread waits
	// for none of them. The fences are kept alive by the commands that use them.
	FRHITexture* Slot = SlotTextures[Taken.Slot].GetReference();
	const ComPtr<ID3D12Fence> Written = WrittenFence;
	const ComPtr<ID3D12Fence> Read = ReadFence;
	const uint64 WrittenValueToWaitFor = Taken.WrittenValue;
	const uint64 ReadValueToSignal = Taken.ReadValue;
	RHICmdList.EnqueueLambda([Written, WrittenValueToWaitFor](FRHICommandListImmediate& Cmd)
	{
		WaitManualFence(Cmd, Written.Get(), WrittenValueToWaitFor);
	});
	RHICmdList.Transition(FRHITransitionInfo(Slot, ERHIAccess::Unknown, ERHIAccess::CopySrc));
	RHICmdList.Transition(FRHITransitionInfo(Destination, ERHIAccess::Unknown, ERHIAccess::CopyDest));
	RHICmdList.CopyTexture(Slot, Destination, FRHICopyTextureInfo());
	RHICmdList.Transition(FRHITransitionInfo(Slot, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
	RHICmdList.Transition(FRHITransitionInfo(Destination, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
	RHICmdList.EnqueueLambda([Read, ReadValueToSignal](FRHICommandListImmediate& Cmd)
	{
		GetID3D12DynamicRHI()->RHISignalManualFence(Cmd, Read.Get(), ReadValueToSignal);
	});
	// The row's end is the end of the call: the three operations are queued, none was waited for.
	SPOUT_INSTRUMENT(if (bInstrument) { Rec.TRestore = SpoutInstrument::Now(); SpoutInstrument::Push(Rec); })
	return true;
}

FSpoutAsyncStats FSpoutAsyncStream::GetStats() const
{
	FSpoutAsyncStats Out;
	{
		FScopeLock Lock(&StateMutex);
		const SpoutAsync::FSlotCounters& N = Pool.GetCounters();
		Out.Published = N.Published;
		Out.Consumed = N.Consumed;
		Out.ReplacedUnread = N.ReplacedUnread;
		Out.AccessRefused = N.Abandoned[static_cast<int32>(SpoutAsync::EAbandon::AccessRefused)];
		Out.CopyFailed = N.Abandoned[static_cast<int32>(SpoutAsync::EAbandon::CopyFailed)];
		Out.CompletionTimeouts = N.Abandoned[static_cast<int32>(SpoutAsync::EAbandon::CompletionTimeout)];
		Out.PublishedAfterReuse = N.PublishedAfterReuse;
		Out.RuleViolations = N.RuleViolations;
	}
	Out.NoFreeSlot = NoSlotSkips.load();
	Out.Polls = Polls.load();
	return Out;
}

#endif // PLATFORM_WINDOWS
