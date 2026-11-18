// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// The sender list (List Spout Senders, Get Max Spout Senders), the receive outcomes (Get Spout Receiver Session Info)
// and the graphics-card check, with real senders: this game's own render-target sender, and the native pixel peer
// (-SpoutNativePeer) sending from another hardware adapter when the machine has one. On a machine with a single GPU,
// or without the peer, the cross-GPU test says so and checks nothing.
#include "Misc/AutomationTest.h"
#if SPOUT_WITH_TESTS && PLATFORM_WINDOWS
#include "SpoutBPFunctionLibrary.h"
#include "SpoutD3DContext.h"
#include "SpoutGpu.h"
#include "SpoutReceiver.h"
#include "DynamicRHI.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <dxgi1_4.h>
#include "Windows/HideWindowsPlatformTypes.h"

#define SenderListTestFlags (EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	FName UniqueName(const TCHAR* What)
	{
		return FName(*FString::Printf(TEXT("SpoutListTest_%s_%s"), What, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8)));
	}

	/** Sends Target under Name until Spout lists it (registration completes on the render thread). Bounded. */
	bool SendUntilListed(FName Name, UTextureRenderTarget2D* Target, FSpoutSenderInfo& OutInfo)
	{
		for (int32 Attempt = 0; Attempt < 200; ++Attempt)
		{
			USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Target);
			FlushRenderingCommands();
			TArray<FSpoutSenderInfo> Senders;
			if (USpoutBPFunctionLibrary::ListSpoutSenders(Senders))
			{
				if (const FSpoutSenderInfo* Found = Senders.FindByPredicate([Name](const FSpoutSenderInfo& S) { return S.Name == Name; }))
				{
					OutInfo = *Found;
					return true;
				}
			}
			FPlatformProcess::Sleep(0.005f);
		}
		return false;
	}

	FString LuidText(const LUID& Luid)
	{
		return FString::Printf(TEXT("%08x:%08x"), static_cast<uint32>(Luid.HighPart), static_cast<uint32>(Luid.LowPart));
	}

	/** A hardware adapter other than Unreal's, if the machine has one. */
	bool FindOtherHardwareAdapter(FString& OutLuid, FString& OutName)
	{
		ID3D12Device* Device12 = GDynamicRHI ? static_cast<ID3D12Device*>(GDynamicRHI->RHIGetNativeDevice()) : nullptr;
		if (!Device12)
		{
			return false;
		}
		const LUID Unreal = Device12->GetAdapterLuid();
		TRefCountPtr<IDXGIFactory1> Factory;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(Factory.GetInitReference()))))
		{
			return false;
		}
		for (UINT Index = 0;; ++Index)
		{
			TRefCountPtr<IDXGIAdapter1> Adapter;
			if (Factory->EnumAdapters1(Index, Adapter.GetInitReference()) == DXGI_ERROR_NOT_FOUND)
			{
				return false;
			}
			DXGI_ADAPTER_DESC1 Desc = {};
			if (!Adapter.IsValid() || FAILED(Adapter->GetDesc1(&Desc)) || (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0
				|| (Desc.AdapterLuid.LowPart == Unreal.LowPart && Desc.AdapterLuid.HighPart == Unreal.HighPart))
			{
				continue;
			}
			// A device must be possible on it, as the peer will make one.
			TRefCountPtr<ID3D11Device> Probe;
			if (SUCCEEDED(D3D11CreateDevice(Adapter.GetReference(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
				Probe.GetInitReference(), nullptr, nullptr)))
			{
				OutLuid = LuidText(Desc.AdapterLuid);
				OutName = Desc.Description;
				return true;
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSenderListThisGame, "SpoutPlugin.Senders.ListShowsThisGamesSender", SenderListTestFlags)
bool FSpoutSenderListThisGame::RunTest(const FString&)
{
	FSpoutD3DContext::Get().InitializeIfNeeded();
	if (!TestTrue(TEXT("interop available"), FSpoutD3DContext::Get().IsSpoutAvailable())) return false;
	TStrongObjectPtr<UTextureRenderTarget2D> Target(USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(64, 32, PF_B8G8R8A8, true));
	const FName Name = UniqueName(TEXT("Own"));
	FSpoutSenderInfo Info;
	if (TestTrue(TEXT("this game's sender is listed"), SendUntilListed(Name, Target.Get(), Info)))
	{
		TestEqual(TEXT("its width"), Info.Width, 64);
		TestEqual(TEXT("its height"), Info.Height, 32);
		TestEqual(TEXT("its format"), Info.Format, FString(TEXT("BGRA8")));
		TestEqual(TEXT("its bit depth"), Info.BitsPerChannel, 8);
		TestTrue(TEXT("it is this game's"), Info.bFromThisGame);
		TestTrue(*FString::Printf(TEXT("its application is this one (%s)"), *Info.SourceApplication),
			Info.SourceApplication.Equals(FPaths::GetCleanFilename(FPlatformProcess::ExecutablePath()), ESearchCase::IgnoreCase));
		TestTrue(TEXT("its texture is on Unreal's graphics card"), Info.Gpu == ESpoutSenderGpu::SameAsUnreal);
	}
	USpoutBPFunctionLibrary::CloseSender(Name);
	FlushRenderingCommands();
	TestTrue(TEXT("without a handle the graphics card is Unknown"), SpoutGpu::Locate(UniqueName(TEXT("NoHandle")), nullptr).Where == ESpoutSenderGpu::Unknown);

	int32 Max = 0;
	USpoutBPFunctionLibrary::GetMaxSenders(Max);
	TestEqual(TEXT("Get Max Spout Senders reads the same limit as Get Max Senders"), USpoutBPFunctionLibrary::GetMaxSpoutSenders(), Max);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSessionInfoResults, "SpoutPlugin.Receivers.SessionInfoExplainsTheLastReceive", SenderListTestFlags)
bool FSpoutSessionInfoResults::RunTest(const FString&)
{
	FSpoutD3DContext::Get().InitializeIfNeeded();
	if (!TestTrue(TEXT("interop available"), FSpoutD3DContext::Get().IsSpoutAvailable())) return false;

	// No sender of that name.
	const FName Missing = UniqueName(TEXT("Missing"));
	UTexture2D* Texture = nullptr;
	UMaterialInstanceDynamic* Material = nullptr;
	TestFalse(TEXT("a name nobody sends is not received"), USpoutBPFunctionLibrary::SpoutReceiver(Missing, nullptr, NAME_None, Material, Texture));
	FSpoutReceiverSessionInfo Info = USpoutBPFunctionLibrary::GetSpoutReceiverSessionInfo(Missing);
	TestTrue(TEXT("the outcome is No Sender"), Info.bHasLastResult && Info.LastResult == ESpoutReceiveResult::NoSender);
	TestFalse(TEXT("no session"), Info.bSessionOpen);
	TestFalse(TEXT("explained in words"), Info.LastResultText.IsEmpty());

	// A sender of this game, received.
	TStrongObjectPtr<UTextureRenderTarget2D> Target(USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(64, 32, PF_B8G8R8A8, true));
	const FName Name = UniqueName(TEXT("Session"));
	FSpoutSenderInfo Listed;
	if (!TestTrue(TEXT("sender running"), SendUntilListed(Name, Target.Get(), Listed))) return false;
	Material = nullptr;
	Texture = nullptr;
	TestTrue(TEXT("received"), USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, Material, Texture));
	Info = USpoutBPFunctionLibrary::GetSpoutReceiverSessionInfo(Name);
	TestTrue(TEXT("the outcome is Accepted"), Info.LastResult == ESpoutReceiveResult::Accepted);
	TestTrue(TEXT("the session is open"), Info.bSessionOpen);
	TestTrue(TEXT("in Async Receive, the default"), Info.ReceiveMode == ESpoutReceiveMode::AsyncBeta);
	TestTrue(TEXT("used by Blueprint or code"), Info.bUsedByBlueprintOrCode);
	FlushRenderingCommands();

	// More receive calls on one stream within a frame than r.Spout.MaxPendingPerStream allows: the stream queue is full.
	// The queue empties as the render thread runs the calls; during a real frame it is busy rendering, here it would
	// keep up, so it is held at a gate while the calls are made (released at the latest after 5 s).
	IConsoleVariable* PerStream = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.MaxPendingPerStream"));
	if (!GIsThreadedRendering)
	{
		AddInfo(TEXT("Stream Queue Full not checked: rendering is not threaded in this run."));
	}
	else if (TestNotNull(TEXT("r.Spout.MaxPendingPerStream"), PerStream))
	{
		const FString Before = PerStream->GetString();
		const EConsoleVariableFlags BeforeSource = static_cast<EConsoleVariableFlags>(PerStream->GetFlags() & ECVF_SetByMask);
		PerStream->Set(2, ECVF_SetByConsole);
		FEvent* Gate = FPlatformProcess::GetSynchEventFromPool(true);
		ENQUEUE_RENDER_COMMAND(SpoutTestHoldRenderThread)([Gate](FRHICommandListImmediate&) { Gate->Wait(5000); });
		bool bRefused = false;
		for (int32 Call = 0; Call < 4 && !bRefused; ++Call)
		{
			UMaterialInstanceDynamic* Consumer = nullptr;
			UTexture2D* ConsumerTexture = nullptr;
			bRefused = !USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, Consumer, ConsumerTexture);
		}
		Gate->Trigger();
		FlushRenderingCommands();
		FPlatformProcess::ReturnSynchEventToPool(Gate);
		TestTrue(TEXT("a call beyond the per-stream limit is refused"), bRefused);
		TestTrue(TEXT("as Stream Queue Full"), USpoutBPFunctionLibrary::GetSpoutReceiverSessionInfo(Name).LastResult == ESpoutReceiveResult::StreamQueueFull);
		PerStream->Set(*Before, ECVF_SetByConsole);
		PerStream->ClearFlags(ECVF_SetByMask);
		PerStream->SetFlags(BeforeSource);
	}

	UTexture2D* ToClose = Texture;
	UMaterialInstanceDynamic* MaterialToClose = Material;
	USpoutBPFunctionLibrary::CloseReceiver(Name, ToClose, MaterialToClose);
	USpoutBPFunctionLibrary::CloseSender(Name);
	FlushRenderingCommands();

	// Every outcome can be told in words.
	const UEnum* Results = StaticEnum<ESpoutReceiveResult>();
	for (int32 Index = 0; Index < Results->NumEnums() - 1; ++Index)
	{
		const ESpoutReceiveResult Result = static_cast<ESpoutReceiveResult>(Results->GetValueByIndex(Index));
		TestFalse(*FString::Printf(TEXT("%s has a description"), *Results->GetNameStringByIndex(Index)), FSpoutReceiver::DescribeResult(Result).IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSenderOnAnotherGpu, "SpoutPlugin.Gpu.SenderOnAnotherGpuIsRefusedAndNamed", SenderListTestFlags)
bool FSpoutSenderOnAnotherGpu::RunTest(const FString&)
{
	FString PeerExe;
	if (!FParse::Value(FCommandLine::Get(), TEXT("SpoutNativePeer="), PeerExe) || !IFileManager::Get().FileExists(*PeerExe))
	{
		AddInfo(TEXT("Not checked: this run has no native pixel peer (-SpoutNativePeer)."));
		return true;
	}
	FSpoutD3DContext::Get().InitializeIfNeeded();
	if (!TestTrue(TEXT("interop available"), FSpoutD3DContext::Get().IsSpoutAvailable())) return false;
	FString OtherLuid, OtherName;
	if (!FindOtherHardwareAdapter(OtherLuid, OtherName))
	{
		AddInfo(TEXT("Not checked: this machine has no second hardware graphics adapter."));
		return true;
	}
	AddInfo(FString::Printf(TEXT("Sending from %s (%s); Unreal renders on %s."), *OtherName, *OtherLuid, *SpoutGpu::UnrealAdapterName()));
	AddExpectedMessage(TEXT("is on another graphics card"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	const FString Name = FString::Printf(TEXT("SpoutListTest_OtherGpu_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString StopName = Name + TEXT("_Stop"), ReadyName = Name + TEXT("_Ready");
	HANDLE Stop = CreateEventW(nullptr, true, false, *StopName);
	HANDLE Ready = CreateEventW(nullptr, true, false, *ReadyName);
	const FString Args = FString::Printf(TEXT("send %s %s %s \"--luid=%s\""), *Name, *StopName, *ReadyName, *OtherLuid);
	FProcHandle Peer = FPlatformProcess::CreateProc(*PeerExe, *Args, false, true, true, nullptr, 0, nullptr, nullptr);
	const bool bReady = Peer.IsValid() && WaitForSingleObject(Ready, 10000) == WAIT_OBJECT_0;
	if (TestTrue(TEXT("the peer sends from the other graphics card"), bReady))
	{
		UTexture2D* Texture = nullptr;
		UMaterialInstanceDynamic* Material = nullptr;
		TestFalse(TEXT("its texture is not received"), USpoutBPFunctionLibrary::SpoutReceiver(FName(*Name), nullptr, NAME_None, Material, Texture));
		const FSpoutReceiverSessionInfo Info = USpoutBPFunctionLibrary::GetSpoutReceiverSessionInfo(FName(*Name));
		TestTrue(*FString::Printf(TEXT("the outcome is Sender On Another GPU (%s)"), *Info.LastResultText), Info.LastResult == ESpoutReceiveResult::OtherGpu);
		TArray<FSpoutSenderInfo> Senders;
		USpoutBPFunctionLibrary::ListSpoutSenders(Senders);
		const FSpoutSenderInfo* Listed = Senders.FindByPredicate([&Name](const FSpoutSenderInfo& S) { return S.Name == FName(*Name); });
		if (TestNotNull(TEXT("the sender is listed"), Listed))
		{
			TestTrue(TEXT("as on another graphics card"), Listed->Gpu == ESpoutSenderGpu::Other);
			TestEqual(TEXT("named as Windows names it"), Listed->OtherGpuName, OtherName);
			TestFalse(TEXT("not this game's"), Listed->bFromThisGame);
		}
	}
	SetEvent(Stop);
	if (Peer.IsValid())
	{
		const double Deadline = FPlatformTime::Seconds() + 3.0;
		while (FPlatformProcess::IsProcRunning(Peer) && FPlatformTime::Seconds() < Deadline) FPlatformProcess::Sleep(0.01f);
		if (FPlatformProcess::IsProcRunning(Peer)) FPlatformProcess::TerminateProc(Peer);
		FPlatformProcess::CloseProc(Peer);
	}
	CloseHandle(Stop);
	CloseHandle(Ready);
	return true;
}
#endif
