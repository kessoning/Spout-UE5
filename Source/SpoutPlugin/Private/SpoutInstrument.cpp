// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// Measurement instrumentation. This file compiles to nothing unless the build is a measurement
// build (SPOUT_WITH_INSTRUMENT, see SpoutInstrument.h).
#include "SpoutInstrument.h"

#if SPOUT_WITH_INSTRUMENT

#include "HAL/IConsoleManager.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/FileManager.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeLock.h"
#include "Serialization/Archive.h"
#include "Runtime/Launch/Resources/Version.h"
#include <atomic>

#if PLATFORM_WINDOWS
SpoutInstrument::FDestinationFenceWatch::FDestinationFenceWatch(ID3D12Fence* InFence, int32 InStream, uint64 InGeneration, int32 InPath)
	: Fence(InFence), Stream(InStream), Path(InPath), Generation(InGeneration)
{
	StopEvent = ::CreateEventW(nullptr, true, false, nullptr);
	WakeEvent = ::CreateEventW(nullptr, false, false, nullptr);
	if (Fence && StopEvent && WakeEvent)
	{
		Thread = FThread(TEXT("SpoutDestinationWatch"), [this]() { Run(); });
	}
	// If creation fails no F rows are written, and completion cannot be established without them.
}

SpoutInstrument::FDestinationFenceWatch::~FDestinationFenceWatch()
{
	if (StopEvent) ::SetEvent(StopEvent);
	if (Thread.IsJoinable()) Thread.Join();
	if (WakeEvent) ::CloseHandle(WakeEvent);
	if (StopEvent) ::CloseHandle(StopEvent);
}

void SpoutInstrument::FDestinationFenceWatch::Run()
{
	const HANDLE Handles[2] = { StopEvent, WakeEvent };
	uint64 Seen = 0, Registered = 0;
	while (::WaitForSingleObject(StopEvent, 0) == WAIT_TIMEOUT)
	{
		const uint64 Done = Fence->GetCompletedValue();
		if (Done == MAX_uint64) return; // device removed: never manufacture completion
		if (Done != Seen)
		{
			Seen = Done;
			FRecord Rec;
			Rec.Kind = TEXT('F');
			Rec.Stream = Stream;
			Rec.TDetect = Now();
			Rec.Generation = static_cast<int64>(Generation);
			Rec.ReadValue = static_cast<int64>(Done);
			Rec.CompletionPath = Path;
			Rec.CompletionStage = 1;
			Push(Rec);
		}
		if (Registered != Seen + 1)
		{
			if (FAILED(Fence->SetEventOnCompletion(Seen + 1, WakeEvent))) return;
			Registered = Seen + 1;
		}
		if (::WaitForMultipleObjects(2, Handles, false, 100) == WAIT_FAILED) return;
	}
}
#endif

namespace
{
	TAutoConsoleVariable<int32> GSpoutInstrument(TEXT("r.Spout.Instrument"), 0,
		TEXT("Measurement builds only. 0 (default): off, nothing is recorded. ")
		TEXT("1: every receive call, send destination and viewport send on the render thread writes one CSV row of QPC and GPU timestamps to r.Spout.InstrumentPath."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> GSpoutInstrumentBypass(TEXT("r.Spout.Instrument.Bypass"), 0,
		TEXT("Measurement builds only. Bit 0: receive render commands return before any Spout work. ")
		TEXT("Bit 1: send render commands do the same. 0 (default): normal operation."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<FString> GSpoutInstrumentPath(TEXT("r.Spout.InstrumentPath"), TEXT(""),
		TEXT("CSV file written by r.Spout.Instrument. Empty: <ProjectSaved>/Spout/instrument-<pid>.csv."),
		ECVF_Default);

	// The column line; FormatRecord writes one field per name, in this order. The first 47 columns
	// are those of instrumentVersion 5; version 6 appends two, version 7 seven more.
	const TCHAR* const GColumns =
		TEXT("kind,stream,frameRT,tDetect,newFrame,senderFrame,pending,tDrain0,tDrain1,tWrap0,tWrap1,tAccess0,tAcquire,acquired,tCopyRec,tGpuDone,tPublish,tRelease,tRestore,gpuT0,gpuT1,gpuFreq,gpuDisjoint,hr,")
		TEXT("waitDecision,waitOutcome,tWait0,tWait1,waitPolls,waitBoundUs,predLoUs,predHiUs,periodUs,countAfterWait,countUnderAccess,budgetBeforeUs,budgetAfterUs,flags,predSuspensions,predWastedWaits,predJitterUs,")
		TEXT("ackBefore,delivered,receivedBefore,receivedAfter,frameRequests,syncOpens,")
		TEXT("completed,newUnderAccess,")
		TEXT("slot,sequence,generation,writtenValue,readValue,slotReadValue,asyncOutcome,completionPath,completionStage");

	FString FormatRecord(const SpoutInstrument::FRecord& R)
	{
		return FString::Printf(TEXT("%c,%d,%llu,%llu,%d,%lld,%d,%llu,%llu,%llu,%llu,%llu,%llu,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d,0x%08x,%d,%d,%llu,%llu,%d,%.1f,%.1f,%.1f,%.1f,%lld,%lld,%.1f,%.1f,%d,%lld,%lld,%.1f,%lld,%d,%llu,%llu,%lld,%lld,%d,%d,%d,%lld,%lld,%lld,%lld,%lld,%d,%d,%d\n"),
			R.Kind, R.Stream, R.FrameRT, R.TDetect, R.NewFrame, R.SenderFrame, R.Pending,
			R.TDrain0, R.TDrain1, R.TWrap0, R.TWrap1, R.TAccess0, R.TAcquire, R.Acquired,
			R.TCopyRec, R.TGpuDone, R.TPublish, R.TRelease, R.TRestore,
			R.GpuT0, R.GpuT1, R.GpuFreq, R.GpuDisjoint, static_cast<uint32>(R.Hr),
			R.WaitDecision, R.WaitOutcome, R.TWait0, R.TWait1, R.WaitPolls, R.WaitBoundUs, R.PredLoUs, R.PredHiUs, R.PeriodUs,
			R.CountAfterWait, R.CountUnderAccess, R.BudgetBeforeUs, R.BudgetAfterUs, R.Flags, R.PredSuspensions, R.PredWastedWaits, R.PredJitterUs,
			R.AckBefore, R.Delivered, R.ReceivedBefore, R.ReceivedAfter, R.FrameRequests, R.SyncOpens,
			R.Completed, R.NewUnderAccess,
			R.Slot, R.Sequence, R.Generation, R.WrittenValue, R.ReadValue, R.SlotReadValue, R.AsyncOutcome, R.CompletionPath, R.CompletionStage);
	}

	/** Buffers rows on the producing thread and writes them from its own thread every 250 ms. */
	class FInstrumentWriter final : public FRunnable
	{
	public:
		static FInstrumentWriter& Get()
		{
			static FInstrumentWriter* Instance = new FInstrumentWriter();
			return *Instance;
		}

		void Enqueue(const SpoutInstrument::FRecord& R)
		{
			{
				FScopeLock L(&Mutex);
				Records.Add(R);
			}
			EnsureStarted();
		}

		void EnqueueLine(FString&& Line)
		{
			{
				FScopeLock L(&Mutex);
				Lines.Add(MoveTemp(Line));
			}
			EnsureStarted();
		}

		virtual uint32 Run() override
		{
			while (!bStop.load())
			{
				WakeEvent->Wait(250);
				Drain();
			}
			Drain();
			if (Writer)
			{
				Writer->Close();
				Writer.Reset();
			}
			return 0;
		}

		virtual void Stop() override
		{
			bStop = true;
			if (WakeEvent) WakeEvent->Trigger();
		}

	private:
		void EnsureStarted()
		{
			if (bStarted.load()) return;
			FScopeLock L(&StartMutex);
			if (bStarted.load()) return;
			WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
			Thread = FRunnableThread::Create(this, TEXT("SpoutInstrumentWriter"), 0, TPri_BelowNormal);
			FCoreDelegates::OnEnginePreExit.AddRaw(this, &FInstrumentWriter::OnPreExit);
			bStarted = true;
		}

		void OnPreExit()
		{
			Stop();
			if (Thread)
			{
				Thread->WaitForCompletion();
				delete Thread;
				Thread = nullptr;
			}
		}

		void Open()
		{
			FString Path = GSpoutInstrumentPath.GetValueOnAnyThread();
			if (Path.IsEmpty())
			{
				Path = FPaths::ProjectSavedDir() / TEXT("Spout") / FString::Printf(TEXT("instrument-%u.csv"), FPlatformProcess::GetCurrentProcessId());
			}
			Writer = MakeShareable(IFileManager::Get().CreateFileWriter(*Path, FILEWRITE_AllowRead));
			if (!Writer)
			{
				bOpenFailed = true;
				return;
			}
			const FString Header = FString::Printf(TEXT("#meta,instrumentVersion=%d,qpcFrequency=%.0f,pid=%u,engine=%d.%d.%d,started=%s\n"),
					SpoutInstrument::Version, 1.0 / FPlatformTime::GetSecondsPerCycle64(), FPlatformProcess::GetCurrentProcessId(),
					ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION, ENGINE_PATCH_VERSION, *FDateTime::UtcNow().ToIso8601())
				+ GColumns + TEXT("\n");
			FTCHARToUTF8 U(*Header);
			Writer->Serialize(const_cast<ANSICHAR*>(U.Get()), U.Length());
		}

		void Drain()
		{
			TArray<SpoutInstrument::FRecord> R;
			TArray<FString> L;
			{
				FScopeLock Lock(&Mutex);
				R = MoveTemp(Records);
				L = MoveTemp(Lines);
				Records.Reset();
				Lines.Reset();
			}
			if (R.Num() == 0 && L.Num() == 0) return;
			if (!Writer && !bOpenFailed) Open();
			if (!Writer) return;
			FString Out;
			for (const FString& S : L) Out += S;
			for (const SpoutInstrument::FRecord& Rec : R) Out += FormatRecord(Rec);
			FTCHARToUTF8 U(*Out);
			Writer->Serialize(const_cast<ANSICHAR*>(U.Get()), U.Length());
			Writer->Flush();
		}

		FCriticalSection Mutex, StartMutex;
		TArray<SpoutInstrument::FRecord> Records;
		TArray<FString> Lines;
		std::atomic<bool> bStarted{false};
		std::atomic<bool> bStop{false};
		bool bOpenFailed = false;
		FEvent* WakeEvent = nullptr;
		FRunnableThread* Thread = nullptr;
		TSharedPtr<FArchive> Writer;
	};
}

const TCHAR* SpoutInstrument::ColumnHeader()
{
	return GColumns;
}

FString SpoutInstrument::FormatRecordLine(const FRecord& Record)
{
	return FormatRecord(Record);
}

bool SpoutInstrument::IsEnabled()
{
	return GSpoutInstrument.GetValueOnAnyThread() != 0;
}

int32 SpoutInstrument::BypassMask()
{
	return GSpoutInstrumentBypass.GetValueOnAnyThread();
}

int32 SpoutInstrument::RegisterStream(const FName& Name, const TCHAR* Role)
{
	static FCriticalSection RegisterMutex;
	static int32 NextId = 0;
	FScopeLock L(&RegisterMutex);
	const int32 Id = NextId++;
	FInstrumentWriter::Get().EnqueueLine(FString::Printf(TEXT("#stream,%d,%s,%s\n"), Id, Role, *Name.ToString()));
	return Id;
}

void SpoutInstrument::Push(const FRecord& Record)
{
	FInstrumentWriter::Get().Enqueue(Record);
}

#if PLATFORM_WINDOWS

void SpoutInstrument::Calibrate(ID3D12CommandQueue* Queue)
{
	static uint64 LastCalibration = 0;
	const uint64 T = Now();
	if (!Queue) return;
	if (LastCalibration != 0 && double(T - LastCalibration) * FPlatformTime::GetSecondsPerCycle64() < 1.0) return;
	LastCalibration = T;
	UINT64 GpuTimestamp = 0, CpuTimestamp = 0, Frequency = 0;
	const HRESULT HrCalibration = Queue->GetClockCalibration(&GpuTimestamp, &CpuTimestamp);
	const HRESULT HrFrequency = Queue->GetTimestampFrequency(&Frequency);
	FInstrumentWriter::Get().EnqueueLine(FString::Printf(TEXT("#calib,%llu,%llu,%llu,%llu,0x%08x,0x%08x\n"),
		T, static_cast<uint64>(CpuTimestamp), static_cast<uint64>(GpuTimestamp), static_cast<uint64>(Frequency),
		static_cast<uint32>(HrCalibration), static_cast<uint32>(HrFrequency)));
}

void SpoutInstrument::FGpuBracket::Begin(ID3D11DeviceContext* Context)
{
	if (!Context) return;
	if (!Disjoint)
	{
		Microsoft::WRL::ComPtr<ID3D11Device> Device;
		Context->GetDevice(&Device);
		if (!Device) return;
		D3D11_QUERY_DESC DisjointDesc = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
		D3D11_QUERY_DESC TimestampDesc = { D3D11_QUERY_TIMESTAMP, 0 };
		if (FAILED(Device->CreateQuery(&DisjointDesc, &Disjoint)) || FAILED(Device->CreateQuery(&TimestampDesc, &T0)) || FAILED(Device->CreateQuery(&TimestampDesc, &T1)))
		{
			Disjoint.Reset();
			T0.Reset();
			T1.Reset();
			return;
		}
	}
	Context->Begin(Disjoint.Get());
	Context->End(T0.Get());
	bActive = true;
	bEnded = false;
}

void SpoutInstrument::FGpuBracket::End(ID3D11DeviceContext* Context)
{
	if (!bActive || bEnded || !Context) return;
	Context->End(T1.Get());
	Context->End(Disjoint.Get());
	bEnded = true;
}

void SpoutInstrument::FGpuBracket::Resolve(ID3D11DeviceContext* Context, FRecord& Record)
{
	if (!bActive) return;
	bActive = false;
	if (!bEnded || !Context)
	{
		Record.GpuDisjoint = -3;
		return;
	}
	// The caller's event query, recorded after these, has already completed, so these are complete
	// too; the bound is a guard against a driver that reports them late, not an expected wait.
	const uint64 Deadline = Now() + static_cast<uint64>(0.002 / FPlatformTime::GetSecondsPerCycle64());
	auto Fetch = [&](ID3D11Query* Query, void* Data, UINT Size) -> bool
	{
		HRESULT Hr;
		while ((Hr = Context->GetData(Query, Data, Size, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE && Now() < Deadline)
		{
			SwitchToThread();
		}
		return Hr == S_OK;
	};
	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT DisjointData = {};
	UINT64 A = 0, B = 0;
	const bool bOk = Fetch(Disjoint.Get(), &DisjointData, sizeof(DisjointData)) && Fetch(T0.Get(), &A, sizeof(A)) && Fetch(T1.Get(), &B, sizeof(B));
	if (!bOk)
	{
		Record.GpuDisjoint = -2;
		return;
	}
	Record.GpuT0 = A;
	Record.GpuT1 = B;
	Record.GpuFreq = DisjointData.Frequency;
	Record.GpuDisjoint = DisjointData.Disjoint ? 1 : 0;
}

SpoutInstrument::FGpuBracket& SpoutInstrument::GetGpuBracket()
{
	static FGpuBracket Bracket;
	return Bracket;
}

#endif

#endif // SPOUT_WITH_INSTRUMENT
