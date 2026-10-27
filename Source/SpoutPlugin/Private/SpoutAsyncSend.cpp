// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the asynchronous render-target sender (the default; r.Spout.Dev.AsyncSend 0 turns it off). See SpoutAsyncSend.h
 * for the design, the ownership rules and who waits, SpoutSendRing.h for the bookkeeping, and SpoutSharedSlot.h for
 * the slot, fence and retirement rules it reuses.
 */
#include "SpoutAsyncSend.h"

#if PLATFORM_WINDOWS

#include "SpoutAsyncReceive.h"
#include "SpoutDebugCapture.h"
#include "SpoutFrameSync.h"
#include "SpoutLimits.h"
#include "SpoutLogThrottle.h"
#include "SpoutModule.h"
#include "SpoutNativeWork.h"
#include "SpoutSenderRegistry.h"
#include "SpoutSharedSlot.h"
#include "HAL/Event.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "Misc/DateTime.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "ID3D12DynamicRHI.h"
#include "Misc/CoreDelegates.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RenderingThread.h"
#include "RHICommandList.h"

using Microsoft::WRL::ComPtr;
using SpoutSendRing::EOutcome;
using SpoutSendRing::ENotAccepted;
using SpoutSendRing::EStep;
// Not "FFrame": CoreUObject declares a global type of that name.
using FRingFrame = SpoutSendRing::FFrame;

/** One source identity's slots and fences. Its objects are retired, never released, once no frame uses it. */
struct FSpoutAsyncSendStream::FSet
{
	uint64 Generation = 0;
	FIdentity Identity;
	ComPtr<ID3D12Fence> FilledFence;          // Unreal's queue signals it after each fill
	ComPtr<ID3D12Fence> DrainedFence;         // D3D12 view of the fence the native device signals after each drain
	ComPtr<ID3D11Fence> NativeDrainedFence;
	ComPtr<ID3D11Query> Proof;                // ended after a drain whose signal failed
	ComPtr<ID3D12Resource> SlotResources[SlotsPerSet];
	ComPtr<ID3D11Texture2D> NativeSlots[SlotsPerSet];
	FTextureRHIRef SlotTextures[SlotsPerSet];
	HANDLE Event = nullptr;                   // the worker's waits on this set's fences
	TSharedPtr<FSpoutResourceReservation, ESPMode::ThreadSafe> Reservation;
	uint64 FilledValue = 0;                   // render thread
	uint64 DrainedValue = 0;                  // worker
	SpoutSlotRules::FOutstanding Outstanding; // state lock
	bool bFailed = false;                     // state lock: a drain's signal failed; nothing more is accepted into it

	~FSet()
	{
		// Only a set whose creation failed gets here with its objects: no GPU work ever used them. A used set's objects
		// were moved to the retirement list.
		if (Event)
		{
			::CloseHandle(Event);
		}
	}
};

/** What the worker works on: a copy of the oldest frame, its set and the stream's token, taken under the state lock. */
struct FSpoutAsyncSendStream::FWork
{
	FRingFrame Frame;
	TSharedPtr<FSet, ESPMode::ThreadSafe> Set;
	FSpoutStreamToken Token;
};

namespace
{
	std::atomic<int32> GLiveSendStreams{0};
	std::atomic<uint64> GNextSendSetGeneration{1};

	/** The process's streams, for the flush hook. A leaf lock. */
	FCriticalSection GSendStreamsMutex;
	TArray<TWeakPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe>> GSendStreams;

	FDelegateHandle GFlushEndHandle;
	FDelegateHandle GEndFrameHandle;

	/**
	 * r.Spout.Dev.AsyncSend.DrainAfter 1: a fence on Unreal's device that Unreal's queue signals behind each frame's own
	 * work. Created by the first stream, on the render thread; its values are counted on the render thread in the order
	 * the frame-end commands run, so a send's "next frame end" is GFrameEndSignalled + 1. Render thread only, apart from
	 * the module's shutdown.
	 */
	ComPtr<ID3D12Fence> GFrameEndFence;
	uint64 GFrameEndSignalled = 0;

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

	double Ms(double Seconds) { return Seconds * 1000.0; }

	std::atomic<uint64> GNextStreamObject{1};

	const TCHAR* OutcomeName(EOutcome O)
	{
		switch (O)
		{
		case EOutcome::Published: return TEXT("Published");
		case EOutcome::Superseded: return TEXT("Superseded");
		case EOutcome::AccessAbandoned: return TEXT("AccessAbandoned");
		case EOutcome::FailedFill: return TEXT("FailedFill");
		case EOutcome::FailedInstall: return TEXT("FailedInstall");
		case EOutcome::FailedDrain: return TEXT("FailedDrain");
		case EOutcome::FailedRemoved: return TEXT("FailedRemoved");
		default: return TEXT("None");
		}
	}

	const TCHAR* NotAcceptedName(ENotAccepted W)
	{
		switch (W)
		{
		case ENotAccepted::NoSlot: return TEXT("NoSlot");
		case ENotAccepted::Stopping: return TEXT("Stopping");
		case ENotAccepted::Budget: return TEXT("Budget");
		case ENotAccepted::Unpublishable: return TEXT("Unpublishable");
		default: return TEXT("Unknown");
		}
	}

#if WITH_DEV_AUTOMATION_TESTS
	/** Set by SpoutAsyncSendTestHooks::RequestOutcomeLogFlushHolding for its own request: how long it holds Mutex. */
	thread_local int32 GHoldThisFlushRequestMs = 0;
#endif

	/**
	 * The development outcome log (r.Spout.Dev.AsyncSend.OutcomeLog). The render thread and the workers queue rows under
	 * Mutex, a leaf held only to append one row or to swap the queue out (no other allocation under it); the log's own
	 * thread appends the queue to the file every 200 ms, as do Flush and the module's shutdown. FileMutex serializes the
	 * appends and every change of the path: rows queued before a change go to the old file, rows queued after it to the
	 * new one, and none is dropped while a path is set. Each process writes its own meta line to a path before its first
	 * rows there; the column header is written only to an empty or new file. A stream that started before the log was
	 * turned on has no start row, and one that ran while the log was off has no rows for that time: neither can be
	 * reconciled with the counters. The module's shutdown turns the log off and
	 * closes it until the next startup (a change of the variable in between starts no writer); the startup sets the
	 * path again and starts a writer.
	 *
	 * The writer's wake event: RequestFlush runs on any thread but a worker (Stream::Stop), while the game thread takes
	 * the event (SetPath) and gives it back (Shutdown); giving it back deletes the handle Wake points to (Unreal's event
	 * pool, 5.3 and 5.8, deletes the FSafeRecyclableEvent wrapper and keeps the inner event for reuse). So Wake is set and
	 * cleared only under Mutex, RequestFlush triggers it only under Mutex, and Shutdown gives it back only after clearing
	 * it: no request can hold a handle that is given back. Trigger takes no lock of the plugin (FEventWin: its stats
	 * message, then SetEvent), so Mutex stays a leaf. The writer reads Wake without the lock: it is set before the writer
	 * starts and cleared only after the writer is joined.
	 */
	class FSpoutSendOutcomeLog final : public FRunnable
	{
	public:
		static FSpoutSendOutcomeLog& Get()
		{
			static FSpoutSendOutcomeLog Instance;
			return Instance;
		}

		bool IsOn() const { return bOn.load(); }

		/** Module startup, before its SetPath: the log may run again after a shutdown. */
		void Open()
		{
			FScopeLock FileLock(&FileMutex);
			bClosed = false;
		}

		/** Game thread (the variable's change, module startup). */
		void SetPath(const FString& NewPath)
		{
			{
				FScopeLock FileLock(&FileMutex);
				// After the module's shutdown nothing would join a writer: the variable is read again at the next startup.
				if (bClosed || NewPath == Path)
				{
					return;
				}
				// What was queued for the old path is written there first; from the swap on, rows queue for the new one.
				TArray<FString> Lines;
				{
					FScopeLock Lock(&Mutex);
					Swap(Lines, Pending);
					bOn = !NewPath.IsEmpty();
				}
				WriteLocked(Lines);
				Path = NewPath;
				bHeaderWritten = false;
			}
			if (bOn.load() && !Thread)
			{
				bExit = false;
				if (!Wake)
				{
					FEvent* Event = FPlatformProcess::GetSynchEventFromPool(false);
#if WITH_DEV_AUTOMATION_TESTS
					SpoutAsyncSendTestHooks::OutcomeLogEventsTaken.fetch_add(1);
#endif
					FScopeLock Lock(&Mutex);
					Wake = Event;
				}
				Thread = FRunnableThread::Create(this, TEXT("SpoutAsyncSendOutcomeLog"), 0, TPri_Normal);
			}
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout: asynchronous send outcome log (development): %s"), NewPath.IsEmpty() ? TEXT("off") : *NewPath);
		}

		void Push(FString&& Line)
		{
			FScopeLock Lock(&Mutex);
			if (bOn.load())
			{
				Pending.Add(MoveTemp(Line));
			}
		}

		/** Writes what is queued now, on the calling thread (serialized with the writer and with path changes). */
		void Flush()
		{
			FScopeLock FileLock(&FileMutex);
			TArray<FString> Lines;
			{
				FScopeLock Lock(&Mutex);
				Swap(Lines, Pending);
			}
			WriteLocked(Lines);
		}

		/** Module shutdown: writes what is queued, stops the writer and turns the log off (a restart sets the path again). */
		void Shutdown()
		{
			bExit = true;
			if (Thread)
			{
				Wake->Trigger();   // the game thread is the only one that sets or clears it
				Thread->WaitForCompletion();
				delete Thread;
				Thread = nullptr;
			}
#if WITH_DEV_AUTOMATION_TESTS
			if (SpoutAsyncSendTestHooks::OnOutcomeLogWriterJoined)
			{
				SpoutAsyncSendTestHooks::OnOutcomeLogWriterJoined();
			}
#endif
			FEvent* Retired = nullptr;
			{
				FScopeLock FileLock(&FileMutex);
				TArray<FString> Lines;
				{
					FScopeLock Lock(&Mutex);
					Swap(Lines, Pending);
					bOn = false;
					// Cleared where RequestFlush triggers it: from here on no request holds it, and it can be given back.
					Retired = Wake;
					Wake = nullptr;
				}
				WriteLocked(Lines);
				Path.Empty();
				bHeaderWritten = false;
				bClosed = true;
			}
			if (Retired)
			{
				FPlatformProcess::ReturnSynchEventToPool(Retired);
#if WITH_DEV_AUTOMATION_TESTS
				SpoutAsyncSendTestHooks::OutcomeLogEventsReturned.fetch_add(1);
#endif
			}
		}

		/** Any thread but a worker (a stream's Stop). Wakes the writer, if one runs. */
		void RequestFlush()
		{
			FScopeLock Lock(&Mutex);
			if (!Wake)
			{
				return;
			}
#if WITH_DEV_AUTOMATION_TESTS
			if (GHoldThisFlushRequestMs > 0)
			{
				SpoutAsyncSendTestHooks::FlushRequestsHolding.fetch_add(1);
				FPlatformProcess::SleepNoStats(GHoldThisFlushRequestMs / 1000.0f);
				SpoutAsyncSendTestHooks::FlushRequestsHolding.fetch_sub(1);
			}
#endif
			Wake->Trigger();
#if WITH_DEV_AUTOMATION_TESTS
			SpoutAsyncSendTestHooks::FlushRequestsTriggered.fetch_add(1);
#endif
		}

#if WITH_DEV_AUTOMATION_TESTS
		SpoutAsyncSendTestHooks::FOutcomeLogState GetStateForTest()
		{
			check(IsInGameThread());
			SpoutAsyncSendTestHooks::FOutcomeLogState State;
			State.bWriter = Thread != nullptr;
			{
				FScopeLock FileLock(&FileMutex);
				State.bClosed = bClosed;
			}
			FScopeLock Lock(&Mutex);
			State.bOn = bOn.load();
			State.bWake = Wake != nullptr;
			return State;
		}
#endif

	private:
		/**
		 * FileMutex held: appends Lines to Path. Before this process's first rows in the file, its meta line (pid, clock,
		 * time), and the column header too when the file is new or empty; both are tried again after a failed write.
		 */
		void WriteLocked(const TArray<FString>& Lines)
		{
			if (Path.IsEmpty() || Lines.Num() == 0)
			{
				return;
			}
			FString Text;
			if (!bHeaderWritten)
			{
				Text += FString::Printf(TEXT("# SpoutAsyncSendOutcomes/1 pid=%u qpcFrequency=%.0f openedQpc=%llu openedUtc=%s\n"),
					FPlatformProcess::GetCurrentProcessId(), 1.0 / FPlatformTime::GetSecondsPerCycle64(), FPlatformTime::Cycles64(),
					*FDateTime::UtcNow().ToIso8601());
				if (IFileManager::Get().FileSize(*Path) <= 0)
				{
					Text += TEXT("kind,stream,streamObject,set,frame,sendTick,sendQpc,renderFrame,acceptQpc,readyQpc,firstAttemptQpc,acquiredQpc,publishQpc,resolvedQpc,")
						TEXT("outcome,attempts,failedAttempts,frameEndValue,frameEndFallback,detail\n");
				}
			}
			for (const FString& L : Lines)
			{
				Text += L;
				Text += TEXT("\n");
			}
			if (FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append))
			{
				bHeaderWritten = true;
			}
			else
			{
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout: the asynchronous send outcome log could not be written to %s; %d rows are lost (the log is then incomplete and is not evidence)."),
					*Path, Lines.Num());
			}
		}

		virtual uint32 Run() override
		{
			while (!bExit.load())
			{
				Wake->Wait(200);
				Flush();
			}
			return 0;
		}

		FCriticalSection Mutex;       // leaf: Pending, bOn's changes, Wake (set, cleared and triggered under it)
		FCriticalSection FileMutex;   // the appends and Path; never taken under Mutex
		TArray<FString> Pending;
		FString Path;                 // FileMutex
		bool bHeaderWritten = false;  // FileMutex
		bool bClosed = false;         // FileMutex: from the module's shutdown to its next startup
		std::atomic<bool> bOn{false};
		std::atomic<bool> bExit{false};
		FRunnableThread* Thread = nullptr;  // game thread
		FEvent* Wake = nullptr;             // Mutex for other threads; the game thread changes it, the writer reads it
	};

	void OnOutcomeLogChanged(IConsoleVariable* Variable)
	{
		if (IsInGameThread())
		{
			FSpoutSendOutcomeLog::Get().SetPath(Variable ? Variable->GetString() : FString());
		}
	}

	TAutoConsoleVariable<FString> GSpoutDevAsyncSendOutcomeLog(TEXT("r.Spout.Dev.AsyncSend.OutcomeLog"), TEXT(""),
		TEXT("Development switch, not a supported setting. A file path: the asynchronous sender appends one CSV row per frame (and per send it did not accept, ")
		TEXT("per stream start and stop) to it. Empty (default): off."),
		FConsoleVariableDelegate::CreateStatic(&OnOutcomeLogChanged), ECVF_Default);

	/** Quoted for a CSV field. */
	FString Quoted(const FString& Text)
	{
		return TEXT("\"") + Text.Replace(TEXT("\""), TEXT("\"\"")) + TEXT("\"");
	}
}

// ---------------------------------------------------------------------------------------------
// Creation and the render thread's side
// ---------------------------------------------------------------------------------------------

FSpoutAsyncSendStream::FSpoutAsyncSendStream(FSpoutSharedSender& InEntry, FInstaller InInstaller)
	: Entry(InEntry)
	, Installer(MoveTemp(InInstaller))
	, What(FString::Printf(TEXT("sender '%s'"), *InEntry.Name.ToString()))
	, StreamObject(GNextStreamObject.fetch_add(1))
	, LogName(Quoted(InEntry.Name.ToString()))
{
	GLiveSendStreams.fetch_add(1);
	CreatedAt = FPlatformTime::Seconds();
	if (FSpoutSendOutcomeLog::Get().IsOn())
	{
		FSpoutSendOutcomeLog::Get().Push(FString::Printf(TEXT("C,%s,%llu,0,0,-1,0,%llu,%llu,0,0,0,0,0,Created,0,0,0,0,"),
			*LogName, StreamObject, static_cast<uint64>(GFrameCounterRenderThread), FPlatformTime::Cycles64()));
	}
}

FSpoutAsyncSendStream::FFrameTrace* FSpoutAsyncSendStream::FindTraceLocked(uint64 Id)
{
	for (FFrameTrace& T : Traces)
	{
		if (T.Id == Id)
		{
			return &T;
		}
	}
	return nullptr;
}

bool FSpoutAsyncSendStream::ResolveLocked(uint64 Id, EOutcome Outcome)
{
	const FRingFrame* F = Ring.Find(Id);
	const FRingFrame Frame = F ? *F : FRingFrame();
	if (!Ring.Resolve(Id, Outcome))
	{
		return false;
	}
	const uint64 ResolvedQpc = FPlatformTime::Cycles64();
	const int32 Index = Traces.IndexOfByPredicate([Id](const FFrameTrace& T) { return T.Id == Id; });
	if (Index != INDEX_NONE)
	{
		const FFrameTrace& T = Traces[Index];
		if (FSpoutSendOutcomeLog::Get().IsOn())
		{
			// The detail column says when a flush or the stream's stop allowed the frame one attempt only.
			const TCHAR* Limit = Frame.Limit == SpoutSendRing::ELimit::Stop ? TEXT("limit=stop") : Frame.Limit == SpoutSendRing::ELimit::Flush ? TEXT("limit=flush") : TEXT("");
			FSpoutSendOutcomeLog::Get().Push(FString::Printf(TEXT("F,%s,%llu,%llu,%llu,%lld,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%s,%d,%d,%llu,%d,%s"),
				*LogName, StreamObject, Frame.SetGeneration, Id, T.Origin.Tick, T.Origin.Qpc, T.RenderFrame, T.AcceptQpc, T.ReadyQpc, T.FirstAttemptQpc,
				T.AcquiredQpc, T.PublishQpc, ResolvedQpc, OutcomeName(Outcome), T.Attempts, Frame.FailedAttempts, Frame.FrameEndValue, T.bFrameEndFallback ? 1 : 0, Limit));
		}
		Traces.RemoveAt(Index);
	}
	return true;
}

void FSpoutAsyncSendStream::NotAcceptedLocked(ENotAccepted Why, const FSpoutSendOrigin& Origin)
{
	Ring.NotAccepted(Why);
	if (FSpoutSendOutcomeLog::Get().IsOn())
	{
		FSpoutSendOutcomeLog::Get().Push(FString::Printf(TEXT("N,%s,%llu,0,0,%lld,%llu,%llu,%llu,0,0,0,0,0,NotAccepted%s,0,0,0,0,"),
			*LogName, StreamObject, Origin.Tick, Origin.Qpc, static_cast<uint64>(GFrameCounterRenderThread), FPlatformTime::Cycles64(), NotAcceptedName(Why)));
	}
}

int32 FSpoutAsyncSendStream::GetLiveStreams()
{
	return GLiveSendStreams.load();
}

TSharedPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe> FSpoutAsyncSendStream::Create(FSpoutSharedSender& Entry, FInstaller Installer, FString& OutWhy)
{
	check(IsInRenderingThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AsyncSendCreate);
	TSharedPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe> Stream(new FSpoutAsyncSendStream(Entry, MoveTemp(Installer)));
	if (!Stream->Initialize(OutWhy))
	{
		Stream->Stop();
		return nullptr;
	}
	FScopeLock Lock(&GSendStreamsMutex);
	GSendStreams.RemoveAll([](const TWeakPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe>& S) { return !S.IsValid(); });
	GSendStreams.Add(Stream);
	return Stream;
}

bool FSpoutAsyncSendStream::Initialize(FString& OutWhy)
{
	if (!SpoutInterop::IsSharedSlot() || !FSpoutAsyncDevice::Get().GetDevice() || !GDynamicRHI)
	{
		OutWhy = TEXT("the shared-slot path or one of its devices is not available");
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (TakeHook(SpoutAsyncSendTestHooks::FailCreations))
	{
		OutWhy = TEXT("refused by a test");
		return false;
	}
#endif
	if (!GFrameEndFence)
	{
		const HRESULT Result = SpoutNativeWork::CreateFencePair(false, GFrameEndFence, nullptr);
		if (FAILED(Result) || !GFrameEndFence)
		{
			GFrameEndFence.Reset();
			OutWhy = FString::Printf(TEXT("the frame-end fence could not be created (hr=0x%08x)"), static_cast<uint32>(Result));
			return false;
		}
	}
	FrameEndFence = GFrameEndFence;
	WorkEvent = FPlatformProcess::GetSynchEventFromPool(false);
	Thread = FRunnableThread::Create(this, *FString::Printf(TEXT("SpoutAsyncSend %s"), *Entry.Name.ToString()), 0, TPri_Normal);
	if (!Thread)
	{
		OutWhy = TEXT("the worker thread could not be started");
		return false;
	}
	return true;
}

FSpoutAsyncSendStream::~FSpoutAsyncSendStream()
{
	// The entry calls Stop before it goes; this covers a stream whose creation failed.
	Stop();
	if (WorkEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(WorkEvent);
		WorkEvent = nullptr;
	}
	GLiveSendStreams.fetch_sub(1);
}

TSharedPtr<FSpoutAsyncSendStream::FSet, ESPMode::ThreadSafe> FSpoutAsyncSendStream::FindSetLocked(uint64 Generation) const
{
	for (const TSharedPtr<FSet, ESPMode::ThreadSafe>& Set : Sets)
	{
		if (Set->Generation == Generation)
		{
			return Set;
		}
	}
	return nullptr;
}

TSharedPtr<FSpoutAsyncSendStream::FSet, ESPMode::ThreadSafe> FSpoutAsyncSendStream::CreateSet(const FIdentity& Identity, FString& OutWhy, bool& bOutBudget)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AsyncSendCreateSet);
	bOutBudget = false;
	int64 FrameBytes = 0;
	if (!SpoutLimits::FrameBytes(Identity.Width, Identity.Height, Identity.PixelFormat, FrameBytes))
	{
		OutWhy = TEXT("the frame size is outside the plugin's limits");
		return nullptr;
	}
	TSharedPtr<FSet, ESPMode::ThreadSafe> Set = MakeShared<FSet, ESPMode::ThreadSafe>();
	Set->Identity = Identity;
	Set->Reservation = FSpoutResourceReservation::TryReserve(FrameBytes * SlotsPerSet);
	if (!Set->Reservation)
	{
		bOutBudget = true;
		OutWhy = TEXT("the retained-memory budget has no room for the slots");
		return nullptr;
	}
	auto Refuse = [&OutWhy](const TCHAR* Text, HRESULT Result)
	{
		OutWhy = FString::Printf(TEXT("%s (hr=0x%08x)"), Text, static_cast<uint32>(Result));
		return nullptr;
	};
	HRESULT Result = SpoutNativeWork::CreateFencePair(false, Set->FilledFence, nullptr);
	if (FAILED(Result) || !Set->FilledFence) return Refuse(TEXT("the filled fence could not be created"), Result);
	Result = SpoutNativeWork::CreateFencePair(true, Set->DrainedFence, &Set->NativeDrainedFence);
	if (FAILED(Result) || !Set->DrainedFence || !Set->NativeDrainedFence) return Refuse(TEXT("the drained fence could not be created and shared"), Result);
	Result = SpoutNativeWork::CreateProof(Set->Proof);
	if (FAILED(Result) || !Set->Proof) return Refuse(TEXT("the event query could not be created"), Result);
	for (int32 S = 0; S < SlotsPerSet; ++S)
	{
		Result = SpoutNativeWork::CreateSlotTexture(Identity.Width, Identity.Height, Identity.Format, Identity.PixelFormat, Set->SlotResources[S], Set->NativeSlots[S], Set->SlotTextures[S]);
		if (FAILED(Result)) return Refuse(TEXT("a slot could not be created, shared, opened or made an RHI texture"), Result);
	}
	Set->Event = ::CreateEventW(nullptr, false, false, nullptr);
	if (!Set->Event) return Refuse(TEXT("an event could not be created"), E_FAIL);
	Set->Generation = GNextSendSetGeneration.fetch_add(1);
	return Set;
}

void FSpoutAsyncSendStream::Send(const FSpoutStreamToken& InToken, FRHICommandListImmediate& RHICmdList, FRHITexture* Source, const SpoutRHIState::FCopyBracket& Bracket, const FIdentity& Identity,
	const FSpoutSendOrigin& Origin)
{
	check(IsInRenderingThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AsyncSendAccept);
	FSpoutGpuRetirement::Get().Poll();
	// The frame's acceptance time, for the bounds and the counters and for the outcome log's acceptQpc alike.
	const double Now = FPlatformTime::Seconds();
	const uint64 NowQpc = FPlatformTime::Cycles64();

	// No Stop can run while this does: Stop runs only once the entry's last reference is dropped, and the command that
	// calls this holds one. So nothing below races a stop of this stream.
	TArray<TSharedPtr<FSet, ESPMode::ThreadSafe>> Unused;
	TSharedPtr<FSet, ESPMode::ThreadSafe> Current;
	ENotAccepted Refusal = ENotAccepted::Count;
	{
		FScopeLock Lock(&StateMutex);
		Token = InToken;
		Current = FindSetLocked(Ring.GetCurrentSet());
		if (Current && Current->Identity != Identity)
		{
			// A new size or format: the set is given up; its owed frames still end in order, and it is retired with its
			// reservation once none uses it.
			Ring.EndSet();
			Current.Reset();
			NextInstallProbe = 0.0;
			InstallBackoff = 0.01;
		}
		TakeUnusedSetsLocked(Unused);
		if (Ring.IsStopping()) Refusal = ENotAccepted::Stopping;
		else if (bFailed.load()) Refusal = ENotAccepted::Unpublishable;
		else if (Current && Current->bFailed) Refusal = ENotAccepted::Unpublishable;
		else if (Now < NextInstallProbe) Refusal = ENotAccepted::Unpublishable;
		if (Refusal != ENotAccepted::Count) NotAcceptedLocked(Refusal, Origin);
	}
	RetireSets(Unused);
	if (Refusal != ENotAccepted::Count)
	{
		return;
	}

	if (!Current)
	{
		if (RefusedFor == Identity)
		{
			FScopeLock Lock(&StateMutex);
			NotAcceptedLocked(ENotAccepted::Unpublishable, Origin);
			return;
		}
		FString Why;
		bool bBudget = false;
		Current = CreateSet(Identity, Why, bBudget);
		if (!Current)
		{
			if (!bBudget)
			{
				RefusedFor = Identity;
				UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout %s: the asynchronous send's slots could not be created for %ux%u (%s). Nothing is published until the source's size or format changes."),
					*What, Identity.Width, Identity.Height, *Why);
			}
			FScopeLock Lock(&StateMutex);
			// A budget refusal is counted and tried again on the next send, never latched.
			NotAcceptedLocked(bBudget ? ENotAccepted::Budget : ENotAccepted::Unpublishable, Origin);
			return;
		}
		RefusedFor = FIdentity();
		FScopeLock Lock(&StateMutex);
		Ring.BeginSet(Current->Generation);
		Sets.Add(Current);
		++SetsCreated;
	}

	int32 Slot = -1;
	{
		FScopeLock Lock(&StateMutex);
		ENotAccepted Why = ENotAccepted::NoSlot;
		Slot = Ring.FindFreeSlot(Why);
		if (Slot < 0)
		{
			// Every slot holds a frame that is not resolved yet: the render thread does not wait.
			NotAcceptedLocked(Why, Origin);
			return;
		}
	}

	// Unreal's copy of the source into the slot and the filled signal, on this thread's command list only.
	FRHITexture* SlotTexture = Current->SlotTextures[Slot].GetReference();
	SPOUT_D3D_DIAG_TRANSITION("asyncsend.source.to-copy", Source, Bracket.EntryBefore, Bracket.CopyAccess);
	RHICmdList.Transition(FRHITransitionInfo(Source, Bracket.EntryBefore, Bracket.CopyAccess));
	SPOUT_D3D_DIAG_TRANSITION("asyncsend.slot.to-copydest", SlotTexture, ERHIAccess::Unknown, ERHIAccess::CopyDest);
	RHICmdList.Transition(FRHITransitionInfo(SlotTexture, ERHIAccess::Unknown, ERHIAccess::CopyDest));
	RHICmdList.CopyTexture(Source, SlotTexture, FRHICopyTextureInfo());
#if WITH_DEV_AUTOMATION_TESTS
	SpoutSlotTestHooks::OnSourceCopied(RHICmdList, Source);
#endif
	SPOUT_D3D_DIAG_TRANSITION("asyncsend.source.to-after", Source, Bracket.CopyAccess, Bracket.ExitAfter);
	RHICmdList.Transition(FRHITransitionInfo(Source, Bracket.CopyAccess, Bracket.ExitAfter));
	SPOUT_D3D_DIAG_TRANSITION("asyncsend.slot.to-srv", SlotTexture, ERHIAccess::CopyDest, ERHIAccess::SRVMask);
	RHICmdList.Transition(FRHITransitionInfo(SlotTexture, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
	const uint64 FillValue = ++Current->FilledValue;
	const ComPtr<ID3D12Fence> Filled = Current->FilledFence;
	RHICmdList.EnqueueLambda([Filled, FillValue](FRHICommandListImmediate& Cmd)
	{
		GetID3D12DynamicRHI()->RHISignalManualFence(Cmd, Filled.Get(), FillValue);
	});
	// The next frame-end signal after this send's command (DrainAfter 1); 0: drain after the fill.
	const uint64 FrameEndValue = SpoutLimits::DevAsyncSendDrainAfter() == 1 ? GFrameEndSignalled + 1 : 0;
	{
		FScopeLock Lock(&StateMutex);
		Current->Outstanding.QueueSignalQueued(FillValue);
		const uint64 Id = Ring.Accept(Slot, FillValue, FrameEndValue, Now);
		ensureMsgf(Id != 0, TEXT("Spout %s: a fill was queued into a slot the asynchronous send's bookkeeping refused"), *What);
		if (Id != 0)
		{
			FFrameTrace& T = Traces.AddDefaulted_GetRef();
			T.Id = Id;
			T.Origin = Origin;
			T.RenderFrame = GFrameCounterRenderThread;
			T.AcceptQpc = NowQpc;
		}
	}
	// Hand the fill to the RHI thread now, without waiting for it: the worker waits for the fence on the CPU, and a
	// close on this thread right after this command must find the fill on its way to the GPU.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AsyncSendDispatch);
		RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
	}
	WorkEvent->Trigger();
}

// ---------------------------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------------------------

uint32 FSpoutAsyncSendStream::Run()
{
	for (;;)
	{
		if (Step())
		{
			continue;
		}
		{
			FScopeLock Lock(&StateMutex);
			if (bStopRequested && Ring.IsEmpty())
			{
				break;
			}
		}
		WorkEvent->Wait(100);
	}
	return 0;
}

bool FSpoutAsyncSendStream::Step()
{
	if (bFailed.load())
	{
		// A frame the render thread accepted while the stream failed ends at once, with the stream's failure.
		FScopeLock Lock(&StateMutex);
		while (const FRingFrame* F = Ring.Oldest())
		{
			if (!ResolveLocked(F->Id, static_cast<EOutcome>(FailedOutcome.load())))
			{
				break;
			}
		}
		return false;
	}
	FWork W;
	{
		FScopeLock Lock(&StateMutex);
		const FRingFrame* Oldest = Ring.Oldest();
		if (!Oldest)
		{
			return false;
		}
		W.Frame = *Oldest;
		W.Set = FindSetLocked(Oldest->SetGeneration);
		W.Token = Token;
	}
	if (!ensureMsgf(W.Set.IsValid(), TEXT("Spout %s: a frame's slot set is missing"), *What))
	{
		ResolveOldest(W.Frame.Id, EOutcome::FailedFill);
		return true;
	}
	if (W.Frame.ReadyAt < 0.0)
	{
		WaitForFill(W);
	}
	else
	{
		WorkReady(W);
	}
	return true;
}

void FSpoutAsyncSendStream::WaitForFill(const FWork& W)
{
	using SpoutSlotRules::EWait;
	EWait Wait = EWait::TimedOut;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AsyncSendFillWait);
		const double Bound = double(FilledTimeoutMs) / 1000.0;
		const double Elapsed = FPlatformTime::Seconds() - W.Frame.AcceptedAt;
		const uint32 LeftMs = Elapsed >= Bound ? 0u : static_cast<uint32>(Ms(Bound - Elapsed) + 0.999);
		Wait = SpoutNativeWork::WaitForQueueFence(W.Set->FilledFence.Get(), W.Frame.FillValue, LeftMs, W.Set->Event);
#if WITH_DEV_AUTOMATION_TESTS
		if (Wait == EWait::Reached && TakeHook(SpoutAsyncSendTestHooks::TimeoutFills))
		{
			Wait = EWait::TimedOut;
		}
		if (Wait == EWait::Reached)
		{
			const int32 DelayMs = SpoutAsyncSendTestHooks::DelayFillMs.exchange(0);
			if (DelayMs > 0)
			{
				FPlatformProcess::SleepNoStats(float(DelayMs) / 1000.0f);
			}
		}
#endif
	}
	if (Wait == EWait::Removed)
	{
		if (SpoutNativeWork::NativeRemoved())
		{
			FailStream(TEXT("the native device was removed"), EOutcome::FailedRemoved);
		}
		else
		{
			FailStream(TEXT("Unreal's device was removed"), EOutcome::FailedFill);
		}
		return;
	}
	if (Wait == EWait::TimedOut)
	{
		if (FillLogThrottle.ShouldLog())
		{
			UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout %s: the copy into the asynchronous send's slot did not finish within %u ms; that frame was not published."),
				*What, FilledTimeoutMs);
		}
		ResolveOldest(W.Frame.Id, EOutcome::FailedFill);
		return;
	}

	// r.Spout.Dev.AsyncSend.DrainAfter 1: drain behind the frame's own work, or after the fallback. A flush or stop
	// ends the wait: their frames get one attempt at once.
	if (W.Frame.FrameEndValue > 0 && FrameEndFence)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AsyncSendFrameEndWait);
		const double Until = FPlatformTime::Seconds() + FrameEndFallbackSeconds;
		bool bReached = false;
		for (;;)
		{
			const uint64 Done = FrameEndFence->GetCompletedValue();
			if (SpoutSlotRules::Reached(Done, W.Frame.FrameEndValue))
			{
				bReached = true;
				break;
			}
			{
				FScopeLock Lock(&StateMutex);
				const FRingFrame* F = Ring.Find(W.Frame.Id);
				if (!F || F->bOneAttempt)
				{
					break;
				}
			}
			if (FPlatformTime::Seconds() >= Until)
			{
				FScopeLock Lock(&StateMutex);
				++FrameEndFallbacks;
				if (FFrameTrace* T = FindTraceLocked(W.Frame.Id))
				{
					T->bFrameEndFallback = true;
				}
				break;
			}
			// A whole millisecond: shorter sleeps become SwitchToThread on Windows, a spin.
			FPlatformProcess::SleepNoStats(0.001f);
		}
		static_cast<void>(bReached);
	}
	FScopeLock Lock(&StateMutex);
	Ring.MarkReady(W.Frame.Id, FPlatformTime::Seconds());
	if (FFrameTrace* T = FindTraceLocked(W.Frame.Id))
	{
		T->ReadyQpc = FPlatformTime::Cycles64();
	}
}

void FSpoutAsyncSendStream::WorkReady(const FWork& W)
{
	const uint64 Id = W.Frame.Id;

	// The supersession check: is a newer frame ready, that is its fill observed finished and, when it drains after the
	// frame end, that frame end reached too? Fence reads outside the lock. Newer frames are not marked ready here: each
	// goes through its own fill and frame-end wait when it becomes the oldest. The set (not a ComPtr: ComPtr breaks UE
	// containers on 5.3) keeps the fence alive while it is read.
	struct FNewer { TSharedPtr<FSet, ESPMode::ThreadSafe> Set; uint64 FillValue; uint64 FrameEndValue; };
	TArray<FNewer, TInlineAllocator<8>> Newer;
	{
		FScopeLock Lock(&StateMutex);
		for (const FRingFrame& F : Ring.GetFrames())
		{
			if (F.Id <= Id) continue;
			Newer.Add({FindSetLocked(F.SetGeneration), F.FillValue, F.FrameEndValue});
		}
	}
	bool bNewerReady = false;
	for (const FNewer& N : Newer)
	{
		ID3D12Fence* Fence = N.Set ? N.Set->FilledFence.Get() : nullptr;
		const bool bFilled = Fence && SpoutSlotRules::Reached(Fence->GetCompletedValue(), N.FillValue);
		const bool bFrameEnded = N.FrameEndValue == 0 || !FrameEndFence || SpoutSlotRules::Reached(FrameEndFence->GetCompletedValue(), N.FrameEndValue);
		bNewerReady |= bFilled && bFrameEnded;
	}
	EStep Next = EStep::Attempt;
	{
		FScopeLock Lock(&StateMutex);
		const double Now = FPlatformTime::Seconds();
		const FRingFrame* F = Ring.Oldest();
		if (!F || F->Id != Id)
		{
			return;
		}
		double Deadline = AccessDeadlineSeconds;
#if WITH_DEV_AUTOMATION_TESTS
		// Tests that must reach a flush or a stop before a blocked frame's deadline lengthen it (no other effect).
		if (const int32 TestMs = SpoutAsyncSendTestHooks::AccessDeadlineMs.load(); TestMs > 0) Deadline = TestMs / 1000.0;
#endif
		Next = SpoutSendRing::NextStep(*F, Now, bNewerReady, Deadline);
	}
	if (Next == EStep::Supersede)
	{
		ResolveOldest(Id, EOutcome::Superseded);
		return;
	}
	if (Next == EStep::Abandon)
	{
		ResolveOldest(Id, EOutcome::AccessAbandoned);
		return;
	}

	// The entry's shared texture and registration for this frame's identity, in frame order. An identity already
	// installed needs no session check; a new one is installed only while the stream's token is current, so nothing
	// is registered after a close or during a session stop.
	TRefCountPtr<ID3D11Texture2D> Shared;
	bool bInstalled = false;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AsyncSendInstall);
#if WITH_DEV_AUTOMATION_TESTS
		if (!TakeHook(SpoutAsyncSendTestHooks::FailInstalls))
#endif
		{
			bInstalled = Installer(Entry, W.Token, W.Set->Identity, Shared) && Shared.IsValid();
		}
	}
	if (!bInstalled)
	{
		{
			FScopeLock Lock(&StateMutex);
			NextInstallProbe = FPlatformTime::Seconds() + InstallBackoff;
			InstallBackoff = FMath::Min(1.0, InstallBackoff * 2.0);
		}
		ResolveOldest(Id, EOutcome::FailedInstall);
		return;
	}

	// One access attempt of r.Spout.AccessWaitMs, on this thread (Windows mutexes are thread-affine).
	const double AttemptAt = FPlatformTime::Seconds();
	const uint64 AttemptQpc = FPlatformTime::Cycles64();
	uint64 AcquiredQpc = 0;
	uint64 PublishQpc = 0;
	if (FirstAttemptFor != Id)
	{
		FirstAttemptFor = Id;
		FirstAttemptAt = AttemptAt;
	}
	bool bAcquired = false;
	bool bSubmitted = false;
	bool bSignalled = false;
	uint64 Value = 0;
	ESpoutNativeProof Proven = ESpoutNativeProof::InTime;
	double AcquiredAt = 0.0;
	{
		FSpoutTextureAccessScope Access(Entry.FrameSync, Shared.GetReference());
		bAcquired = Access.IsAcquired();
		if (bAcquired)
		{
			AcquiredAt = FPlatformTime::Seconds();
			AcquiredQpc = FPlatformTime::Cycles64();
			TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AsyncSendDrain);
			FSpoutAsyncDevice& Async = FSpoutAsyncDevice::Get();
			HRESULT SignalResult = S_OK;
			{
				FScopeLock ContextLock(&Async.GetContextMutex());
				if (ID3D11DeviceContext4* Context = Async.GetContext())
				{
					Context->CopyResource(Shared.GetReference(), W.Set->NativeSlots[W.Frame.Slot].Get());
					Value = ++W.Set->DrainedValue;
#if WITH_DEV_AUTOMATION_TESTS
					SignalResult = TakeHook(SpoutAsyncSendTestHooks::FailSignals) ? E_FAIL : SpoutNativeWork::Signal(Context, W.Set->NativeDrainedFence.Get(), Value);
#else
					SignalResult = SpoutNativeWork::Signal(Context, W.Set->NativeDrainedFence.Get(), Value);
#endif
					if (FAILED(SignalResult))
					{
						Context->End(W.Set->Proof.Get());
					}
					Context->Flush();
					bSubmitted = true;
				}
			}
			bSignalled = bSubmitted && SUCCEEDED(SignalResult);
			if (bSubmitted)
			{
				// The copy writes the texture peers read, under their access: access is given back only once the copy is
				// proven finished, or the native device was removed. Never on a wall-clock bound.
				TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AsyncSendDrainProof);
#if WITH_DEV_AUTOMATION_TESTS
				const int32 NotBeforeMs = SpoutAsyncSendTestHooks::DelayDrainMs.exchange(0);
#else
				const int32 NotBeforeMs = 0;
#endif
				Proven = SpoutNativeWork::WaitForProof(W.Set->DrainedFence.Get(), Value, bSignalled, W.Set->Proof.Get(), MAX_uint32, W.Set->Event, What, NotBeforeMs);
				if (bSignalled && Proven != ESpoutNativeProof::Removed)
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(Spout_SendPublishFrame);
					Entry.FrameSync.PublishFrame();
					PublishQpc = FPlatformTime::Cycles64();
					++Entry.SlotTransfers;
				}
			}
		}
	}
	// Access has been given back. A keyed texture's release reaches peers when the native context is flushed.
	if (bAcquired)
	{
		D3D11_TEXTURE2D_DESC SharedDesc = {};
		Shared->GetDesc(&SharedDesc);
		if ((SharedDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0)
		{
			FSpoutAsyncDevice& Async = FSpoutAsyncDevice::Get();
			FScopeLock ContextLock(&Async.GetContextMutex());
			if (ID3D11DeviceContext4* Context = Async.GetContext())
			{
				Context->Flush();
			}
		}
	}
	const double Done = FPlatformTime::Seconds();

	if (!bAcquired)
	{
		{
			FScopeLock Lock(&StateMutex);
			Ring.NoteAttempt(Id, false);
			if (FFrameTrace* T = FindTraceLocked(Id))
			{
				T->FirstAttemptQpc = T->FirstAttemptQpc ? T->FirstAttemptQpc : AttemptQpc;
				++T->Attempts;
			}
		}
		if (AccessLogThrottle.ShouldLog())
		{
			// The same words as every other access-unavailable warning of the plugin, so one log search finds them all.
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("Spout %s: shared-texture access was unavailable because a receiver still held it after waiting up to %u ms (r.Spout.AccessWaitMs), ")
				TEXT("so this frame was not published yet. It waits for another attempt, until a newer frame is ready or %.0f ms after it was ready (asynchronous send)."),
				*What, SpoutLimits::AccessWaitMs(), Ms(AccessDeadlineSeconds));
		}
		if (SpoutLimits::AccessWaitMs() == 0)
		{
			// Attempts that do not wait would otherwise spin.
			FPlatformProcess::SleepNoStats(0.001f);
		}
		return;
	}

	EOutcome Outcome = EOutcome::Published;
	if (!bSubmitted || Proven == ESpoutNativeProof::Removed)
	{
		Outcome = EOutcome::FailedRemoved;
	}
	else if (!bSignalled)
	{
		Outcome = EOutcome::FailedDrain;
	}
	TArray<TSharedPtr<FSet, ESPMode::ThreadSafe>> Unused;
	{
		FScopeLock Lock(&StateMutex);
		Ring.NoteAttempt(Id, true);
		if (FFrameTrace* T = FindTraceLocked(Id))
		{
			T->FirstAttemptQpc = T->FirstAttemptQpc ? T->FirstAttemptQpc : AttemptQpc;
			T->AcquiredQpc = AcquiredQpc;
			T->PublishQpc = PublishQpc;
			++T->Attempts;
		}
		if (bSubmitted)
		{
			Ring.BeginDrain(Id);
			if (bSignalled)
			{
				W.Set->Outstanding.NativeSignalled(Value);
			}
			else
			{
				W.Set->Outstanding.NativeSignalFailed(true);
				if (Proven != ESpoutNativeProof::Removed)
				{
					W.Set->Outstanding.ProofCompleted();
				}
				// Proven finished, but nothing announced: the set stops for this source size and format.
				W.Set->bFailed = true;
			}
		}
		if (Outcome == EOutcome::Published)
		{
			const double Latency = Ms(Done - W.Frame.AcceptedAt);
			const double AccessWait = Ms(AcquiredAt - FirstAttemptAt);
			PublishLatencyMsSum += Latency;
			PublishLatencyMsMax = FMath::Max(PublishLatencyMsMax, Latency);
			AccessWaitMsSum += AccessWait;
			AccessWaitMsMax = FMath::Max(AccessWaitMsMax, AccessWait);
		}
		ResolveLocked(Id, Outcome);
		TakeUnusedSetsLocked(Unused);
	}
	RetireSets(Unused);
	if (Outcome == EOutcome::FailedDrain)
	{
		UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout %s: a fence signal on the native device failed; the copy was proven finished by an event query before access was given back. ")
			TEXT("The asynchronous send stopped for this source size and format; a new size or format starts again."), *What);
	}
	else if (Outcome == EOutcome::FailedRemoved)
	{
		FailStream(bSubmitted ? TEXT("the native device was removed") : TEXT("the native device is gone"), EOutcome::FailedRemoved);
	}
}

void FSpoutAsyncSendStream::ResolveOldest(uint64 Id, EOutcome Outcome)
{
	TArray<TSharedPtr<FSet, ESPMode::ThreadSafe>> Unused;
	{
		FScopeLock Lock(&StateMutex);
		ResolveLocked(Id, Outcome);
		TakeUnusedSetsLocked(Unused);
	}
	RetireSets(Unused);
}

void FSpoutAsyncSendStream::FailStream(const FString& Why, EOutcome OwedOutcome)
{
	if (!bFailed.load())
	{
		{
			FScopeLock Lock(&FailureMutex);
			Failure = Why;
		}
		FailedOutcome = static_cast<uint8>(OwedOutcome);
		bFailed = true;
		UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout %s: the asynchronous send stopped (%s). Nothing more is published by this sender until it is closed and sent again."),
			*What, *Why);
	}
	// Every frame still owed ends now, in order. Fills still on Unreal's queue are recorded in their sets, which are
	// retired with those values: nothing is released before they finish or a device was removed. The current set is
	// left as it is: a send on the render thread may be between its checks and its acceptance and still use it (a frame
	// it accepts then ends at the worker's next step); Stop retires it.
	TArray<TSharedPtr<FSet, ESPMode::ThreadSafe>> Unused;
	{
		FScopeLock Lock(&StateMutex);
		while (const FRingFrame* F = Ring.Oldest())
		{
			if (!ResolveLocked(F->Id, OwedOutcome))
			{
				break;
			}
		}
		TakeUnusedSetsLocked(Unused);
	}
	RetireSets(Unused);
}

void FSpoutAsyncSendStream::TakeUnusedSetsLocked(TArray<TSharedPtr<FSet, ESPMode::ThreadSafe>>& Out)
{
	for (int32 I = Sets.Num() - 1; I >= 0; --I)
	{
		const uint64 G = Sets[I]->Generation;
		if (G != Ring.GetCurrentSet() && !Ring.SetInUse(G))
		{
			Out.Add(Sets[I]);
			Sets.RemoveAt(I);
		}
	}
}

void FSpoutAsyncSendStream::RetireSets(TArray<TSharedPtr<FSet, ESPMode::ThreadSafe>>& Retired)
{
	if (Retired.Num() == 0)
	{
		return;
	}
	for (TSharedPtr<FSet, ESPMode::ThreadSafe>& Set : Retired)
	{
		// No thread fills or drains a set once it is out of the list: neither its fences' values nor its
		// Outstanding change any more.
		TUniquePtr<FSpoutGpuRetirement::FRetiredSet> Record = MakeUnique<FSpoutGpuRetirement::FRetiredSet>();
		Record->What = FString::Printf(TEXT("asynchronous send slots of %s (%ux%u, set %llu)"), *What, Set->Identity.Width, Set->Identity.Height, Set->Generation);
		Record->NativeFence = Set->DrainedFence;
		Record->QueueFence = Set->FilledFence;
		Record->Proof = Set->Proof;
		Record->Outstanding = Set->Outstanding;
		for (int32 S = 0; S < SlotsPerSet; ++S)
		{
			if (Set->SlotTextures[S].IsValid())
			{
				Record->Textures.Add(Set->SlotTextures[S]);
			}
			SpoutNativeWork::AddRetiredObject(Record->Objects, Set->NativeSlots[S].Get());
			SpoutNativeWork::AddRetiredObject(Record->Objects, Set->SlotResources[S].Get());
			Set->SlotTextures[S].SafeRelease();
			Set->NativeSlots[S].Reset();
			Set->SlotResources[S].Reset();
		}
		SpoutNativeWork::AddRetiredObject(Record->Objects, Set->NativeDrainedFence.Get());
		Record->Event = Set->Event;
		Set->Event = nullptr;
		Record->Reservation = MoveTemp(Set->Reservation);
		Set->NativeDrainedFence.Reset();
		Set->DrainedFence.Reset();
		Set->FilledFence.Reset();
		Set->Proof.Reset();
		FSpoutGpuRetirement::Get().Retire(MoveTemp(Record));
	}
	FScopeLock Lock(&StateMutex);
	SetsRetired += Retired.Num();
	Retired.Reset();
}

// ---------------------------------------------------------------------------------------------
// Flush, stop and counters
// ---------------------------------------------------------------------------------------------

void FSpoutAsyncSendStream::ResolveOwed()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AsyncSendResolveOwed);
	uint64 UpTo = 0;
	{
		// A stream with frames owed has a running worker: it exits only once nothing is owed after a stop.
		FScopeLock Lock(&StateMutex);
		if (Ring.IsEmpty())
		{
			return;
		}
		UpTo = Ring.LimitToOneAttempt();
	}
	WorkEvent->Trigger();
	// Bounded by the classes the worker waits for (the fill, one access attempt, the drain's proof): the drain's proof
	// is the only unbounded one, as on the synchronous send.
	for (;;)
	{
		{
			FScopeLock Lock(&StateMutex);
			if (Ring.ResolvedUpTo(UpTo))
			{
				return;
			}
		}
		FPlatformProcess::SleepNoStats(0.001f);
	}
}

void FSpoutAsyncSendStream::Stop()
{
	if (bStopped.exchange(true))
	{
		return;
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(Spout_AsyncSendStop);
	{
		FScopeLock Lock(&StateMutex);
		Ring.BeginStop();
		bStopRequested = true;
	}
	if (Thread)
	{
		// The worker resolves every frame owed (one access attempt each; the fill and the drain's proof as always) and
		// exits once none is left. On a GPU that stalls without a removal this waits as long (warnings every 5 s).
		WorkEvent->Trigger();
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	TArray<TSharedPtr<FSet, ESPMode::ThreadSafe>> All;
	{
		FScopeLock Lock(&StateMutex);
		ensureMsgf(Ring.IsEmpty(), TEXT("Spout %s: the asynchronous send stopped with frames owed"), *What);
		Ring.EndSet();
		All = MoveTemp(Sets);
	}
	RetireSets(All);
	{
		FScopeLock Lock(&GSendStreamsMutex);
		GSendStreams.RemoveAll([this](const TWeakPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe>& S)
		{
			const TSharedPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe> P = S.Pin();
			return !P.IsValid() || P.Get() == this;
		});
	}
	if (CreatedAt > 0.0 && GetStats().Accepted > 0)
	{
		LogSummary();
	}
	if (FSpoutSendOutcomeLog::Get().IsOn())
	{
		// The counters the rows must reconcile with, and the frames still traced (always none after the join).
		const FSpoutAsyncSendStats S = GetStats();
		int32 Untraced = 0;
		{
			FScopeLock Lock(&StateMutex);
			Untraced = Traces.Num();
		}
		FSpoutSendOutcomeLog::Get().Push(FString::Printf(TEXT("S,%s,%llu,0,0,-1,0,0,0,0,0,0,0,%llu,Summary,%lld,%lld,0,0,")
			TEXT("accepted=%lld;published=%lld;superseded=%lld;accessAbandoned=%lld;failedFill=%lld;failedInstall=%lld;failedDrain=%lld;failedRemoved=%lld;inFlight=%lld;")
			TEXT("notAcceptedNoSlot=%lld;notAcceptedStopping=%lld;notAcceptedBudget=%lld;notAcceptedUnpublishable=%lld;frameEndFallbacks=%lld;setsCreated=%lld;setsRetired=%lld;")
			TEXT("ruleViolations=%lld;untracedFrames=%d;")
			TEXT("flushPublished=%lld;flushSuperseded=%lld;flushAccessAbandoned=%lld;flushFailed=%lld;stopPublished=%lld;stopSuperseded=%lld;stopAccessAbandoned=%lld;stopFailed=%lld;failure=%s"),
			*LogName, StreamObject, FPlatformTime::Cycles64(), S.Attempts, S.FailedAttempts,
			S.Accepted, S.Published(), S.Superseded(), S.AccessAbandoned(), S.Outcomes[static_cast<int32>(EOutcome::FailedFill)],
			S.Outcomes[static_cast<int32>(EOutcome::FailedInstall)], S.Outcomes[static_cast<int32>(EOutcome::FailedDrain)],
			S.Outcomes[static_cast<int32>(EOutcome::FailedRemoved)], S.InFlight, S.NotAccepted[0], S.NotAccepted[1], S.NotAccepted[2], S.NotAccepted[3],
			S.FrameEndFallbacks, S.SetsCreated, S.SetsRetired, S.RuleViolations, Untraced,
			S.FlushOutcomes[static_cast<int32>(EOutcome::Published)], S.FlushOutcomes[static_cast<int32>(EOutcome::Superseded)],
			S.FlushOutcomes[static_cast<int32>(EOutcome::AccessAbandoned)], FSpoutAsyncSendStats::FailedIn(S.FlushOutcomes),
			S.StopOutcomes[static_cast<int32>(EOutcome::Published)], S.StopOutcomes[static_cast<int32>(EOutcome::Superseded)],
			S.StopOutcomes[static_cast<int32>(EOutcome::AccessAbandoned)], FSpoutAsyncSendStats::FailedIn(S.StopOutcomes),
			*GetFailure().Replace(TEXT(","), TEXT(" ")).Replace(TEXT(";"), TEXT(" "))));
		FSpoutSendOutcomeLog::Get().RequestFlush();
	}
	FSpoutAsyncDevice::Get().ReportDebugLayer();
}

FSpoutAsyncSendStats FSpoutAsyncSendStream::GetStats() const
{
	FSpoutAsyncSendStats S;
	FScopeLock Lock(&StateMutex);
	const SpoutSendRing::FCounters& C = Ring.GetCounters();
	S.Accepted = C.Accepted;
	for (int32 I = 0; I < static_cast<int32>(ENotAccepted::Count); ++I) S.NotAccepted[I] = C.NotAccepted[I];
	for (int32 I = 0; I < static_cast<int32>(EOutcome::Count); ++I)
	{
		S.Outcomes[I] = C.Outcomes[I];
		S.FlushOutcomes[I] = C.FlushOutcomes[I];
		S.StopOutcomes[I] = C.StopOutcomes[I];
	}
	S.InFlight = C.InFlight();
	S.Attempts = C.Attempts;
	S.FailedAttempts = C.FailedAttempts;
	S.RuleViolations = C.RuleViolations;
	S.FrameEndFallbacks = FrameEndFallbacks;
	S.SetsCreated = SetsCreated;
	S.SetsRetired = SetsRetired;
	S.PublishLatencyMsSum = PublishLatencyMsSum;
	S.PublishLatencyMsMax = PublishLatencyMsMax;
	S.AccessWaitMsSum = AccessWaitMsSum;
	S.AccessWaitMsMax = AccessWaitMsMax;
	return S;
}

FString FSpoutAsyncSendStream::GetFailure() const
{
	FScopeLock Lock(&FailureMutex);
	return Failure;
}

void FSpoutAsyncSendStream::LogSummary() const
{
	const FSpoutAsyncSendStats S = GetStats();
	const int64 P = S.Published();
	UE_LOG(LogSpoutPlugin, Log,
		TEXT("Spout %s: asynchronous send ended (%.1f s): accepted %lld = published %lld + superseded %lld + access abandoned %lld + failed %lld (fill %lld, install %lld, drain %lld, removed %lld) + in flight %lld; ")
		TEXT("not accepted %lld (no slot %lld, stopping %lld, budget %lld, unpublishable %lld); access attempts %lld (failed %lld); frame-end fallbacks %lld; slot sets created %lld, retired %lld; ")
		TEXT("publication latency mean %.2f ms, max %.2f ms; access wait mean %.2f ms, max %.2f ms; rule violations %lld. ")
		TEXT("Of these, resolved while a flush allowed one attempt only: published %lld, superseded %lld, access abandoned %lld, failed %lld; while the stop did: published %lld, superseded %lld, access abandoned %lld, failed %lld."),
		*What, FPlatformTime::Seconds() - CreatedAt, S.Accepted, P, S.Superseded(), S.AccessAbandoned(), S.Failed(),
		S.Outcomes[static_cast<int32>(EOutcome::FailedFill)], S.Outcomes[static_cast<int32>(EOutcome::FailedInstall)],
		S.Outcomes[static_cast<int32>(EOutcome::FailedDrain)], S.Outcomes[static_cast<int32>(EOutcome::FailedRemoved)], S.InFlight,
		S.NotAccepted[0] + S.NotAccepted[1] + S.NotAccepted[2] + S.NotAccepted[3], S.NotAccepted[0], S.NotAccepted[1], S.NotAccepted[2], S.NotAccepted[3],
		S.Attempts, S.FailedAttempts, S.FrameEndFallbacks, S.SetsCreated, S.SetsRetired,
		P > 0 ? S.PublishLatencyMsSum / double(P) : 0.0, S.PublishLatencyMsMax, P > 0 ? S.AccessWaitMsSum / double(P) : 0.0, S.AccessWaitMsMax, S.RuleViolations,
		S.FlushOutcomes[static_cast<int32>(EOutcome::Published)], S.FlushOutcomes[static_cast<int32>(EOutcome::Superseded)],
		S.FlushOutcomes[static_cast<int32>(EOutcome::AccessAbandoned)], FSpoutAsyncSendStats::FailedIn(S.FlushOutcomes),
		S.StopOutcomes[static_cast<int32>(EOutcome::Published)], S.StopOutcomes[static_cast<int32>(EOutcome::Superseded)],
		S.StopOutcomes[static_cast<int32>(EOutcome::AccessAbandoned)], FSpoutAsyncSendStats::FailedIn(S.StopOutcomes));
}

// ---------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------

void FSpoutAsyncSendStream::ResolveAllOwed()
{
	TArray<TSharedPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe>> Live;
	{
		FScopeLock Lock(&GSendStreamsMutex);
		for (const TWeakPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe>& S : GSendStreams)
		{
			if (TSharedPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe> P = S.Pin())
			{
				Live.Add(MoveTemp(P));
			}
		}
	}
	for (const TSharedPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe>& S : Live)
	{
		S->ResolveOwed();
	}
}

void FSpoutAsyncSendStream::QueueFrameEndSignal()
{
	if (GLiveSendStreams.load() == 0)
	{
		return;
	}
	// Behind this frame's render commands (the scene and Slate were queued before OnEndFrame), ahead of the next.
	ENQUEUE_RENDER_COMMAND(SpoutAsyncSendFrameEnd)([](FRHICommandListImmediate& RHICmdList)
	{
		if (!GFrameEndFence)
		{
			return;
		}
		const uint64 Value = ++GFrameEndSignalled;
		const ComPtr<ID3D12Fence> Fence = GFrameEndFence;
		RHICmdList.EnqueueLambda([Fence, Value](FRHICommandListImmediate& Cmd)
		{
			GetID3D12DynamicRHI()->RHISignalManualFence(Cmd, Fence.Get(), Value);
		});
	});
}

void FSpoutAsyncSendStream::RegisterHooks()
{
	check(IsInGameThread());
	if (!GFlushEndHandle.IsValid())
	{
		GFlushEndHandle = FCoreRenderDelegates::OnFlushRenderingCommandsEnd.AddStatic(&FSpoutAsyncSendStream::ResolveAllOwed);
	}
	if (!GEndFrameHandle.IsValid())
	{
		GEndFrameHandle = FCoreDelegates::OnEndFrame.AddStatic(&FSpoutAsyncSendStream::QueueFrameEndSignal);
	}
	// A path set before the module registered the variable (an ini file, the command line) does not call its delegate.
	FSpoutSendOutcomeLog::Get().Open();
	FSpoutSendOutcomeLog::Get().SetPath(GSpoutDevAsyncSendOutcomeLog.GetValueOnGameThread());
}

void FSpoutAsyncSendStream::FlushOutcomeLog()
{
	FSpoutSendOutcomeLog::Get().Flush();
}

void FSpoutAsyncSendStream::UnregisterHooks()
{
	check(IsInGameThread());
	FCoreRenderDelegates::OnFlushRenderingCommandsEnd.Remove(GFlushEndHandle);
	FCoreDelegates::OnEndFrame.Remove(GEndFrameHandle);
	GFlushEndHandle.Reset();
	GEndFrameHandle.Reset();
	// Every stream stopped during the module's shutdown and queued its summary: write everything and stop the writer.
	FSpoutSendOutcomeLog::Get().Shutdown();
	// Every stream is gone by now (the module's shutdown closed them and flushed rendering). The fence may still be
	// signalled by Unreal's queue: it is released only once its last value is observed reached; otherwise it is kept
	// for the rest of the process, never released early.
	if (!GFrameEndFence)
	{
		return;
	}
	const uint64 Last = GFrameEndSignalled;
	const double Until = FPlatformTime::Seconds() + 0.2;
	while (!SpoutSlotRules::Reached(GFrameEndFence->GetCompletedValue(), Last) && FPlatformTime::Seconds() < Until)
	{
		FPlatformProcess::SleepNoStats(0.001f);
	}
	if (SpoutSlotRules::Reached(GFrameEndFence->GetCompletedValue(), Last))
	{
		GFrameEndFence.Reset();
	}
	else
	{
		UE_LOG(LogSpoutPlugin, Log, TEXT("Spout: the asynchronous sender's frame-end fence is kept for the rest of the process: its last signal (%llu) is not observed reached."), Last);
		static_cast<void>(GFrameEndFence.Detach()); // deliberately never released
	}
}

bool SpoutGetAsyncSendStats(const FName& Name, FSpoutAsyncSendStats& Out)
{
	const TSharedPtr<FSpoutSharedSender> Entry = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Sender);
	if (!Entry)
	{
		return false;
	}
	TSharedPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe> Stream;
	{
		FScopeLock Lock(&Entry->ResourceMutex);
		Stream = Entry->AsyncSend;
	}
	if (!Stream)
	{
		return false;
	}
	Out = Stream->GetStats();
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
namespace SpoutAsyncSendTestHooks
{
	std::atomic<int32> TimeoutFills{0};
	std::atomic<int32> DelayFillMs{0};
	std::atomic<int32> FailSignals{0};
	std::atomic<int32> DelayDrainMs{0};
	std::atomic<int32> FailInstalls{0};
	std::atomic<int32> FailCreations{0};
	std::atomic<int32> AccessDeadlineMs{0};
	std::atomic<int32> FlushRequestsHolding{0};
	std::atomic<int32> FlushRequestsTriggered{0};
	std::atomic<int32> OutcomeLogEventsTaken{0};
	std::atomic<int32> OutcomeLogEventsReturned{0};
	TFunction<void()> OnOutcomeLogWriterJoined;

	void CloseOutcomeLog()
	{
		check(IsInGameThread());
		FSpoutSendOutcomeLog::Get().Shutdown();
	}

	void ReopenOutcomeLog()
	{
		check(IsInGameThread());
		FSpoutSendOutcomeLog::Get().Open();
		FSpoutSendOutcomeLog::Get().SetPath(GSpoutDevAsyncSendOutcomeLog.GetValueOnGameThread());
	}

	void RequestOutcomeLogFlush()
	{
		FSpoutSendOutcomeLog::Get().RequestFlush();
	}

	void RequestOutcomeLogFlushHolding(int32 HoldMs)
	{
		GHoldThisFlushRequestMs = HoldMs;
		FSpoutSendOutcomeLog::Get().RequestFlush();
		GHoldThisFlushRequestMs = 0;
	}

	void PushOutcomeLogRow(const FString& Row)
	{
		FSpoutSendOutcomeLog::Get().Push(CopyTemp(Row));
	}

	FOutcomeLogState GetOutcomeLogState()
	{
		return FSpoutSendOutcomeLog::Get().GetStateForTest();
	}
}
#endif

#endif // PLATFORM_WINDOWS
