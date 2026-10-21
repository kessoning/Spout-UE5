// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the debug-layer capture. See SpoutDebugCapture.h.
 */
#include "SpoutDebugCapture.h"

#if PLATFORM_WINDOWS && !UE_BUILD_SHIPPING

#include "SpoutAsyncReceive.h"
#include "SpoutModule.h"
#include "HAL/PlatformProcess.h"
#include "RHI.h"
#include "RHIGlobals.h"
#include <atomic>

#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include "Windows/HideWindowsPlatformTypes.h"

using Microsoft::WRL::ComPtr;

#if SPOUT_WITH_D3D_DIAG
#include "SpoutD3DContext.h"
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTLS.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "RHIResources.h"
#include "Runtime/Launch/Resources/Version.h"

// The D3D diagnostic (SpoutDebugCapture.h). A named namespace, not an anonymous one: this file may share a unity file.
namespace SpoutD3DDiagDetail
{
	constexpr int32 Ids[] = { 1008, 1424, 1328, 202, 210 };
	constexpr int64 StacksPerId = 8;
	constexpr uint32 Frames = 62;
	constexpr int64 TransitionsPerSite = 256;
	constexpr uint32 MaxModules = 4096;

	std::atomic<int64> StacksTaken[UE_ARRAY_COUNT(Ids)];
	std::atomic<int64> TransitionSequence{0};
	FCriticalSection SiteMutex;
	TMap<FString, int64> SiteCounts;

	bool On()
	{
		static const bool bOn = FParse::Param(FCommandLine::Get(), TEXT("SpoutD3DDiag"));
		return bOn;
	}
}

#include "Windows/AllowWindowsPlatformTypes.h"
#include <psapi.h>
namespace SpoutD3DDiagDetail
{
	/**
	 * Called from the D3D12 message callback, on the thread that made the D3D12 call: its raw return addresses, for the
	 * first StacksPerId occurrences of each id in Ids. RtlCaptureStackBackTrace only walks the stack: no
	 * symbol is loaded and no D3D call is made here.
	 */
	void LogStack(int32 Severity, int32 Id, int64 Occurrence)
	{
		int32 Index = -1;
		for (int32 I = 0; I < UE_ARRAY_COUNT(Ids); ++I)
		{
			if (Ids[I] == Id)
			{
				Index = I;
				break;
			}
		}
		if (Index < 0 || StacksTaken[Index].fetch_add(1) >= StacksPerId)
		{
			return;
		}
		PVOID Trace[Frames] = {};
		const uint32 Depth = ::RtlCaptureStackBackTrace(0, Frames, Trace, nullptr);
		FString List;
		for (uint32 I = 0; I < Depth; ++I)
		{
			List += FString::Printf(TEXT("%s0x%llx"), I ? TEXT(",") : TEXT(""), static_cast<uint64>(reinterpret_cast<UPTRINT>(Trace[I])));
		}
		UE_LOG(LogSpoutPlugin, Log, TEXT("Spout D3D diag stack: id=%d severity=%d occurrence=%lld thread=%u time=%.6f depth=%u frames=%s"),
			Id, Severity, Occurrence, FPlatformTLS::GetCurrentThreadId(), FPlatformTime::Seconds(), Depth, *List);
	}

	/** The PDB identity of a loaded image, read from its own mapped CodeView record (every offset bounds-checked). */
	void ReadCodeView(const uint8* Base, uint64 Size, uint32& OutStamp, FString& OutGuid, uint32& OutAge, FString& OutPdb)
	{
		if (!Base || Size < sizeof(IMAGE_DOS_HEADER))
		{
			return;
		}
		const IMAGE_DOS_HEADER* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
		if (Dos->e_magic != IMAGE_DOS_SIGNATURE || Dos->e_lfanew <= 0 || static_cast<uint64>(Dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > Size)
		{
			return;
		}
		const IMAGE_NT_HEADERS64* Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base + Dos->e_lfanew);
		if (Nt->Signature != IMAGE_NT_SIGNATURE || Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		{
			return;
		}
		OutStamp = Nt->FileHeader.TimeDateStamp;
		const IMAGE_DATA_DIRECTORY& Directory = Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
		if (!Directory.VirtualAddress || static_cast<uint64>(Directory.VirtualAddress) + Directory.Size > Size)
		{
			return;
		}
		const IMAGE_DEBUG_DIRECTORY* Entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(Base + Directory.VirtualAddress);
		for (uint32 I = 0; I < Directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++I)
		{
			const IMAGE_DEBUG_DIRECTORY& Entry = Entries[I];
			if (Entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || !Entry.AddressOfRawData || Entry.SizeOfData < 25
				|| static_cast<uint64>(Entry.AddressOfRawData) + Entry.SizeOfData > Size)
			{
				continue;
			}
			const uint8* Record = Base + Entry.AddressOfRawData;
			if (FMemory::Memcmp(Record, "RSDS", 4) != 0)
			{
				continue;
			}
			GUID Guid;
			FMemory::Memcpy(&Guid, Record + 4, sizeof(Guid));
			uint32 Age = 0;
			FMemory::Memcpy(&Age, Record + 20, sizeof(Age));
			OutGuid = FString::Printf(TEXT("%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X"), static_cast<uint32>(Guid.Data1), static_cast<uint32>(Guid.Data2),
				static_cast<uint32>(Guid.Data3), Guid.Data4[0], Guid.Data4[1], Guid.Data4[2], Guid.Data4[3], Guid.Data4[4], Guid.Data4[5], Guid.Data4[6], Guid.Data4[7]);
			OutAge = Age;
			TArray<ANSICHAR> Name;
			for (uint32 J = 24; J < Entry.SizeOfData && Record[J]; ++J)
			{
				Name.Add(static_cast<ANSICHAR>(Record[J]));
			}
			Name.Add('\0');
			OutPdb = UTF8_TO_TCHAR(Name.GetData());
			return;
		}
	}

	/** At shutdown: every loaded module, so that the logged addresses can be mapped to module and offset, and symbolized. */
	void LogModules()
	{
		HMODULE Modules[MaxModules] = {};
		DWORD Needed = 0;
		const HANDLE Process = ::GetCurrentProcess();
		if (!::EnumProcessModules(Process, Modules, sizeof(Modules), &Needed))
		{
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout D3D diag modules: NOT readable (error %u)"), static_cast<uint32>(::GetLastError()));
			return;
		}
		const uint32 Count = FMath::Min<uint32>(Needed / sizeof(HMODULE), MaxModules);
		UE_LOG(LogSpoutPlugin, Log, TEXT("Spout D3D diag modules: count=%u listed=%u"), static_cast<uint32>(Needed / sizeof(HMODULE)), Count);
		for (uint32 I = 0; I < Count; ++I)
		{
			MODULEINFO Info = {};
			WCHAR Path[MAX_PATH] = {};
			if (!::GetModuleInformation(Process, Modules[I], &Info, sizeof(Info)))
			{
				continue;
			}
			::GetModuleFileNameExW(Process, Modules[I], Path, MAX_PATH);
			uint32 Stamp = 0, Age = 0;
			FString Guid = TEXT("-"), Pdb = TEXT("-");
			ReadCodeView(static_cast<const uint8*>(Info.lpBaseOfDll), Info.SizeOfImage, Stamp, Guid, Age, Pdb);
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout D3D diag module: base=0x%llx size=0x%x stamp=0x%08x guid=%s age=%u pdb=%s path=%s"),
				static_cast<uint64>(reinterpret_cast<UPTRINT>(Info.lpBaseOfDll)), static_cast<uint32>(Info.SizeOfImage), Stamp, *Guid, Age, *Pdb, Path);
		}
	}
}
#include "Windows/HideWindowsPlatformTypes.h"

namespace SpoutD3DDiag
{
	bool IsOn()
	{
		return SpoutD3DDiagDetail::On();
	}

	void NoteTransition(const TCHAR* Site, FRHITexture* Texture, ERHIAccess Before, ERHIAccess After)
	{
		using namespace SpoutD3DDiagDetail;
		if (!On() || !Texture)
		{
			return;
		}
		{
			FScopeLock Lock(&SiteMutex);
			int64& Count = SiteCounts.FindOrAdd(FString(Site));
			if (++Count > TransitionsPerSite)
			{
				if (Count == TransitionsPerSite + 1)
				{
					UE_LOG(LogSpoutPlugin, Log, TEXT("Spout D3D diag transition limit: site=%s recorded=%lld; later transitions are not recorded"), Site, TransitionsPerSite);
				}
				return;
			}
		}
		UE_LOG(LogSpoutPlugin, Log, TEXT("Spout D3D diag transition: site=%s n=%lld thread=%u time=%.6f name=%s native=0x%llx before=0x%x after=0x%x"),
			Site, TransitionSequence.fetch_add(1) + 1, FPlatformTLS::GetCurrentThreadId(), FPlatformTime::Seconds(), *Texture->GetName().ToString(),
			static_cast<uint64>(reinterpret_cast<UPTRINT>(Texture->GetNativeResource())), static_cast<uint32>(Before), static_cast<uint32>(After));
	}

	// An engine-only reference: observe the editor for five seconds without creating
	// either Spout interop device or running a transfer. It does not change rendering
	// settings or filter any message, and is absent from ordinary plugin builds.
	void RunEngineOnly()
	{
		if (!IsOn() || FSpoutD3DContext::Get().IsInitialized() || FSpoutAsyncDevice::Get().GetDevice())
		{
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout D3D engine-only: refused (diagnostic off or interop already initialized)"));
			RequestEngineExit(TEXT("Spout engine-only diagnostic refused"));
			return;
		}
		SpoutDebugCapture::Start();
		const auto Counts = SpoutDebugCapture::GetCounts();
		UE_LOG(LogSpoutPlugin, Log, TEXT("Spout D3D engine-only: begin registered=%d canary=%lld interop=0 native=0"), Counts.bRegistered ? 1 : 0, Counts.Canary);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			const bool bInterop = FSpoutD3DContext::Get().IsInitialized();
			const bool bNative = FSpoutAsyncDevice::Get().GetDevice() != nullptr;
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout D3D engine-only: end interop=%d native=%d"), bInterop ? 1 : 0, bNative ? 1 : 0);
			RequestEngineExit(TEXT("Spout engine-only diagnostic finished"));
			return false;
		}), 5.0f);
	}

	FAutoConsoleCommand GEngineOnlyCommand(TEXT("Spout.Diag.EngineOnly"),
		TEXT("Diagnostic build only: capture five seconds of editor rendering without initializing Spout interop, then exit normally."),
		FConsoleCommandDelegate::CreateStatic(&RunEngineOnly));
}
#endif // SPOUT_WITH_D3D_DIAG

namespace
{
	/** Messages of one severity and id logged with their text; all of them are counted. */
	constexpr int64 TextsPerId = 20;

	FCriticalSection GMutex;
	ComPtr<ID3D12InfoQueue1> GQueue;
	DWORD GCookie = 0;
	bool GRegistered = false;
	double GStarted = 0.0;
	int64 GCanary = 0;
	std::atomic<int64> GBySeverity[5];
	std::atomic<int64> GWrapReflection{0};
	/** Callbacks that have entered and not yet returned; the summary waits for 0 after unregistering. */
	std::atomic<int32> GInFlight{0};
	FCriticalSection GIdMutex;
	TMap<uint64, int64> GById;

	struct FInFlight
	{
		FInFlight() { GInFlight.fetch_add(1); }
		~FInFlight() { GInFlight.fetch_sub(1); }
	};

	void __stdcall OnMessage(D3D12_MESSAGE_CATEGORY Category, D3D12_MESSAGE_SEVERITY Severity, D3D12_MESSAGE_ID Id, LPCSTR Description, void*)
	{
		// No D3D call in here: it could add a message, or re-enter the debug layer.
		const FInFlight Scope;
		const int32 S = FMath::Clamp(static_cast<int32>(Severity), 0, 4);
		GBySeverity[S].fetch_add(1);
		if (static_cast<int32>(Id) == SpoutDebugCapture::WrapReflectionMessageId)
		{
			GWrapReflection.fetch_add(1);
		}
		int64 Seen = 0;
		{
			FScopeLock Lock(&GIdMutex);
			int64& Count = GById.FindOrAdd((static_cast<uint64>(S) << 32) | static_cast<uint32>(Id));
			Seen = ++Count;
		}
		if (S <= static_cast<int32>(D3D12_MESSAGE_SEVERITY_WARNING) && Seen <= TextsPerId)
		{
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout debug capture: D3D12 message severity=%d id=%d category=%d occurrence=%lld: %s"),
				S, static_cast<int32>(Id), static_cast<int32>(Category), Seen, Description ? ANSI_TO_TCHAR(Description) : TEXT(""));
		}
#if SPOUT_WITH_D3D_DIAG
		if (SpoutD3DDiagDetail::On())
		{
			SpoutD3DDiagDetail::LogStack(S, static_cast<int32>(Id), Seen);
		}
#endif
	}

	FString SeverityList(const D3D11_INFO_QUEUE_FILTER_DESC& Desc)
	{
		TArray<FString> Parts;
		for (UINT I = 0; I < Desc.NumSeverities; ++I)
		{
			Parts.Add(FString::FromInt(static_cast<int32>(Desc.pSeverityList[I])));
		}
		return FString::Join(Parts, TEXT(","));
	}

	/** Reads one filter of a D3D11 info queue. bStorage chooses the storage or the retrieval filter. */
	bool ReadFilter(ID3D11InfoQueue* Queue, bool bStorage, TArray<uint8>& Buffer, D3D11_INFO_QUEUE_FILTER*& OutFilter)
	{
		SIZE_T Length = 0;
		const HRESULT SizeResult = bStorage ? Queue->GetStorageFilter(nullptr, &Length) : Queue->GetRetrievalFilter(nullptr, &Length);
		if (FAILED(SizeResult) || Length < sizeof(D3D11_INFO_QUEUE_FILTER))
		{
			return false;
		}
		Buffer.SetNumZeroed(static_cast<int32>(Length));
		OutFilter = reinterpret_cast<D3D11_INFO_QUEUE_FILTER*>(Buffer.GetData());
		return SUCCEEDED(bStorage ? Queue->GetStorageFilter(OutFilter, &Length) : Queue->GetRetrievalFilter(OutFilter, &Length));
	}
}

namespace SpoutDebugCapture
{
	void Start()
	{
		FScopeLock Lock(&GMutex);
		if (GRegistered || !GRHIGlobals.IsDebugLayerEnabled || !GDynamicRHI || GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::D3D12)
		{
			return;
		}
		ID3D12Device* Device = static_cast<ID3D12Device*>(GDynamicRHI->RHIGetNativeDevice());
		if (!Device)
		{
			return;
		}
		HRESULT Result = Device->QueryInterface(IID_PPV_ARGS(GQueue.GetAddressOf()));
		if (FAILED(Result) || !GQueue)
		{
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout debug capture: D3D12 callback NOT registered: ID3D12InfoQueue1 is not available (hr=0x%08x). The D3D12 capture is missing."), static_cast<uint32>(Result));
			return;
		}
		for (std::atomic<int64>& Count : GBySeverity)
		{
			Count = 0;
		}
		GWrapReflection = 0;
		{
			FScopeLock IdLock(&GIdMutex);
			GById.Reset();
		}
		GCookie = 0;
		Result = GQueue->RegisterMessageCallback(&OnMessage, D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS, nullptr, &GCookie);
		GRegistered = SUCCEEDED(Result) && GCookie != 0;
		if (!GRegistered)
		{
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout debug capture: D3D12 callback NOT registered (hr=0x%08x, cookie %u). The D3D12 capture is missing."), static_cast<uint32>(Result), static_cast<uint32>(GCookie));
			GQueue.Reset();
			return;
		}
		GStarted = FPlatformTime::Seconds();

		// The canary: one fence made and released here must produce messages in the callback.
		const int64 Before = GBySeverity[D3D12_MESSAGE_SEVERITY_INFO].load() + GBySeverity[D3D12_MESSAGE_SEVERITY_MESSAGE].load();
		{
			ComPtr<ID3D12Fence> Canary;
			Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(Canary.GetAddressOf()));
		}
		const int64 After = GBySeverity[D3D12_MESSAGE_SEVERITY_INFO].load() + GBySeverity[D3D12_MESSAGE_SEVERITY_MESSAGE].load();
		GCanary = After - Before;
		UE_LOG(LogSpoutPlugin, Log, TEXT("Spout debug capture: D3D12 callback registered=1 cookie=%u filters=ignored canary=%lld%s"),
			static_cast<uint32>(GCookie), GCanary, GCanary > 0 ? TEXT("") : TEXT(" (NOT PROVEN LIVE: the canary fence produced no message)"));
#if SPOUT_WITH_D3D_DIAG
		if (SpoutD3DDiagDetail::On())
		{
			const LUID Adapter = Device->GetAdapterLuid();
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout D3D diag adapter: unreal=%08x:%08x"),
				static_cast<uint32>(Adapter.HighPart), static_cast<uint32>(Adapter.LowPart));
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout D3D diag: on; ids=1008,1424,1328,202,210; stacks_per_id=%lld; frames=%u; transitions_per_site=%lld; engine=%d.%d; process=%u"),
				SpoutD3DDiagDetail::StacksPerId, SpoutD3DDiagDetail::Frames, SpoutD3DDiagDetail::TransitionsPerSite, ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION,
				FPlatformProcess::GetCurrentProcessId());
		}
#endif
	}

	void Stop()
	{
		FScopeLock Lock(&GMutex);
		if (!GRegistered)
		{
			return;
		}
		// Unregister first, then wait until no callback is still running, and only then count: the summary includes
		// every message the callback received, up to its unregistration.
		const HRESULT Unregistered = GQueue->UnregisterMessageCallback(GCookie);
		const double WaitEnd = FPlatformTime::Seconds() + 2.0;
		while (GInFlight.load() > 0 && FPlatformTime::Seconds() < WaitEnd)
		{
			FPlatformProcess::SleepNoStats(0.0f);
		}
		const int32 Pending = GInFlight.load();
		GQueue.Reset();
		GCookie = 0;
		GRegistered = false;
		const double Window = FPlatformTime::Seconds() - GStarted;
		UE_LOG(LogSpoutPlugin, Log, TEXT("Spout debug capture: D3D12 callback unregistered hr=0x%08x pending_callbacks=%d"), static_cast<uint32>(Unregistered), Pending);
		UE_LOG(LogSpoutPlugin, Log, TEXT("Spout debug capture: D3D12 summary registered=1 canary=%lld corruption=%lld error=%lld warning=%lld info=%lld message=%lld id916=%lld window_s=%.1f"),
			GCanary, GBySeverity[0].load(), GBySeverity[1].load(), GBySeverity[2].load(), GBySeverity[3].load(), GBySeverity[4].load(), GWrapReflection.load(), Window);
		{
			FScopeLock IdLock(&GIdMutex);
			for (const TPair<uint64, int64>& Pair : GById)
			{
				UE_LOG(LogSpoutPlugin, Log, TEXT("Spout debug capture: D3D12 count severity=%d id=%d count=%lld"),
					static_cast<int32>(Pair.Key >> 32), static_cast<int32>(Pair.Key & 0xffffffffu), Pair.Value);
			}
		}
		UE_LOG(LogSpoutPlugin, Log, TEXT("Spout debug capture: D3D12 capture finalized"));
#if SPOUT_WITH_D3D_DIAG
		if (SpoutD3DDiagDetail::On())
		{
			SpoutD3DDiagDetail::LogModules();
		}
#endif
	}

	void ReportNative(const TCHAR* Where)
	{
		if (!GRHIGlobals.IsDebugLayerEnabled)
		{
			return;
		}
		ID3D11Device5* Device = FSpoutAsyncDevice::Get().GetDevice();
		if (!Device)
		{
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout debug capture: native D3D11 at %s: no native device"), Where);
			return;
		}
		ComPtr<ID3D11InfoQueue> Queue;
		if (FAILED(Device->QueryInterface(IID_PPV_ARGS(Queue.GetAddressOf()))) || !Queue)
		{
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout debug capture: native D3D11 at %s: debug layer ABSENT (no info queue). The native capture is missing."), Where);
			return;
		}
		TArray<uint8> StorageBuffer, RetrievalBuffer;
		D3D11_INFO_QUEUE_FILTER* Storage = nullptr;
		D3D11_INFO_QUEUE_FILTER* Retrieval = nullptr;
		const bool bStorage = ReadFilter(Queue.Get(), true, StorageBuffer, Storage);
		const bool bRetrieval = ReadFilter(Queue.Get(), false, RetrievalBuffer, Retrieval);
		UE_LOG(LogSpoutPlugin, Log,
			TEXT("Spout debug capture: native D3D11 at %s: layer=on storage_read=%d deny_severities=[%s] deny_ids=%u deny_categories=%u allow_severities=%u allow_ids=%u allow_categories=%u ")
			TEXT("retrieval_read=%d retrieval_entries=%u limit=%llu stored=%llu allowed_by_retrieval=%llu discarded_by_limit=%llu denied_by_storage=%llu"),
			Where,
			bStorage ? 1 : 0,
			bStorage ? *SeverityList(Storage->DenyList) : TEXT("?"),
			bStorage ? Storage->DenyList.NumIDs : 0u,
			bStorage ? Storage->DenyList.NumCategories : 0u,
			bStorage ? Storage->AllowList.NumSeverities : 0u,
			bStorage ? Storage->AllowList.NumIDs : 0u,
			bStorage ? Storage->AllowList.NumCategories : 0u,
			bRetrieval ? 1 : 0,
			bRetrieval ? (Retrieval->DenyList.NumSeverities + Retrieval->DenyList.NumIDs + Retrieval->DenyList.NumCategories
				+ Retrieval->AllowList.NumSeverities + Retrieval->AllowList.NumIDs + Retrieval->AllowList.NumCategories) : 0u,
			static_cast<uint64>(Queue->GetMessageCountLimit()),
			static_cast<uint64>(Queue->GetNumStoredMessages()),
			static_cast<uint64>(Queue->GetNumStoredMessagesAllowedByRetrievalFilter()),
			static_cast<uint64>(Queue->GetNumMessagesDiscardedByMessageCountLimit()),
			static_cast<uint64>(Queue->GetNumMessagesDeniedByStorageFilter()));
	}

	FCounts GetCounts()
	{
		FScopeLock Lock(&GMutex);
		FCounts Out;
		Out.bRegistered = GRegistered;
		Out.Canary = GCanary;
		for (int32 I = 0; I < 5; ++I)
		{
			Out.BySeverity[I] = GBySeverity[I].load();
		}
		Out.WrapReflection = GWrapReflection.load();
		return Out;
	}
}

#else

namespace SpoutDebugCapture
{
	void Start() {}
	void Stop() {}
	void ReportNative(const TCHAR*) {}
	FCounts GetCounts() { return FCounts(); }
}

#endif
