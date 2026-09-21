// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
#include "SpoutBPFunctionLibrary.h"
#include "SpoutD3DContext.h"
#include "SpoutSenderRegistry.h"
#include "SpoutSession.h"
#include "Async/Async.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/Event.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"
#include <atomic>

#define SPOUT_P0_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

namespace
{
	void ExpectWindowsWrapProbe(FAutomationTestBase& Test)
	{
		if (FParse::Param(FCommandLine::Get(), TEXT("d3ddebug")))
		{
			// Windows emits ID 916 while probing optional D3D11 metadata even when
			// CreateWrappedResource returns S_OK. Tests/Native/D3D11On12WrapProbe.cpp
			// reproduces it without Unreal/Spout. Leave the production debug queue
			// untouched; accept only this exact diagnostic in these pixel tests.
			Test.AddExpectedError(TEXT("ID3D12CompatibilityDevice::ReflectSharedProperties: Resource provided was not shared by D3D11, or with a D3D11 desc."), EAutomationExpectedErrorFlags::Contains, 0);
		}
	}
	TArray<FColor> MakePattern(int32 W, int32 H, int32 Sequence)
	{
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(W * H);
		for (int32 Y = 0; Y < H; ++Y)
			for (int32 X = 0; X < W; ++X)
				Pixels[Y * W + X] = FColor((X * 17 + Sequence) & 255, (Y * 29 + Sequence) & 255, (X + Y + Sequence * 13) & 255, (X == 0 && Y == 0) ? 255 : ((X * 3 + Y * 7 + Sequence) & 255));
		return Pixels;
	}
	void QueuePattern(UTextureRenderTarget2D* Target, int32 Sequence)
	{
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		const int32 W = Target->SizeX, H = Target->SizeY;
		TArray<FColor> Pixels = MakePattern(W, H, Sequence);
		ENQUEUE_RENDER_COMMAND(SpoutP0Pattern)([Resource, W, H, Pixels = MoveTemp(Pixels)](FRHICommandListImmediate& Cmd)
		{
			Cmd.UpdateTexture2D(Resource->GetRenderTargetTexture(), 0, FUpdateTextureRegion2D(0, 0, 0, 0, W, H), W * sizeof(FColor), reinterpret_cast<const uint8*>(Pixels.GetData()));
		});
	}
	void QueueRead(UTexture2D* Texture, int32 W, int32 H, TArray<FColor>& Pixels)
	{
		FTextureResource* Resource = Texture ? Texture->GetResource() : nullptr;
		ENQUEUE_RENDER_COMMAND(SpoutP0Read)([Resource, W, H, &Pixels](FRHICommandListImmediate& Cmd)
		{
			if (!Resource || !Resource->GetTextureRHI()) return;
			FReadSurfaceDataFlags Flags(RCM_UNorm);
			Flags.SetLinearToGamma(false);
			Cmd.Transition(FRHITransitionInfo(Resource->GetTextureRHI(), ERHIAccess::Unknown, ERHIAccess::CopySrc));
			Cmd.ReadSurfaceData(Resource->GetTextureRHI(), FIntRect(0, 0, W, H), Pixels, Flags);
			Cmd.Transition(FRHITransitionInfo(Resource->GetTextureRHI(), ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP0Pixels, "SpoutPlugin.P0.RealPixelsFirstFrameResizeAndResourceRelease", SPOUT_P0_FLAGS)
bool FSpoutP0Pixels::RunTest(const FString&)
{
	ExpectWindowsWrapProbe(*this);
	FSpoutD3DContext::Get().InitializeIfNeeded();
	if (!TestTrue(TEXT("real interop device available"), FSpoutD3DContext::Get().IsSpoutAvailable())) return false;
	IConsoleVariable* Path = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.GPUReceiver"));
	const int32 PreviousPath = Path->GetInt();
	const EConsoleVariableFlags PreviousPriority = static_cast<EConsoleVariableFlags>(Path->GetFlags() & ECVF_SetByMask);
	for (int32 GPU : {1, 0})
	{
		Path->Set(GPU, ECVF_SetByCode);
		const FName Name(*FString::Printf(TEXT("SpoutP0_%s_%d"), *FGuid::NewGuid().ToString(EGuidFormats::Digits), GPU));
		TStrongObjectPtr<UTextureRenderTarget2D> Source(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
		UMaterialInstanceDynamic* Material = nullptr;
		UTexture2D* Texture = nullptr;
		for (int32 Frame = 0; Frame < 3; ++Frame)
		{
			const int32 W = 32 + Frame * 16, H = 16 + Frame * 8;
			// Force the game-thread Send to run BEFORE resource initialization. A
			// game-thread RHI dereference would reject this first-frame request.
			FEvent* Gate = FPlatformProcess::GetSynchEventFromPool(true);
			ENQUEUE_RENDER_COMMAND(SpoutP0DelayInit)([Gate](FRHICommandListImmediate&) { Gate->Wait(10000); });
			Source->InitCustomFormat(W, H, PF_B8G8R8A8, true);
			QueuePattern(Source.Get(), Frame + 1);
			TestTrue(TEXT("send accepted before InitRHI"), USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source.Get()));
			Gate->Trigger();
			FlushRenderingCommands();
			FPlatformProcess::ReturnSynchEventToPool(Gate);
			Gate = FPlatformProcess::GetSynchEventFromPool(true);
			ENQUEUE_RENDER_COMMAND(SpoutP0DelayReceive)([Gate](FRHICommandListImmediate&) { Gate->Wait(10000); });
			const bool Accepted = USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, Material, Texture, nullptr, ESpoutColorSpace::Linear);
			TestTrue(TEXT("first receive accepted before InitRHI"), Accepted);
			TArray<FColor> Actual;
			QueueRead(Texture, W, H, Actual);
			Gate->Trigger();
			FlushRenderingCommands();
			FPlatformProcess::ReturnSynchEventToPool(Gate);
			const TArray<FColor> Expected = MakePattern(W, H, Frame + 1);
			TestEqual(TEXT("readback dimensions"), Actual.Num(), Expected.Num());
			TestTrue(*FString::Printf(TEXT("%s complete sequence %d matches every pixel"), GPU ? TEXT("GPU") : TEXT("CPU"), Frame + 1), Actual == Expected);

			// A later UObject resource replacement must not invalidate an already
			// queued receive/read. Capture and read the old resource before its release.
			QueuePattern(Source.Get(), Frame + 11);
			USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source.Get());
			FlushRenderingCommands();
			USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, Material, Texture, nullptr, ESpoutColorSpace::Linear);
			Actual.Reset(); QueueRead(Texture, W, H, Actual);
			if (Texture) Texture->UpdateResource();
			FlushRenderingCommands();
			TestTrue(TEXT("queued read survives later resource replacement"), Actual == MakePattern(W, H, Frame + 11));
			QueuePattern(Source.Get(), Frame + 21);
			USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source.Get());
			FlushRenderingCommands();
			USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, Material, Texture, nullptr, ESpoutColorSpace::Linear);
			Actual.Reset(); QueueRead(Texture, W, H, Actual); FlushRenderingCommands();
			TestTrue(TEXT("same-size receive restores writable resource after UpdateResource"), Actual == MakePattern(W, H, Frame + 21));
		}
		USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material);
		USpoutBPFunctionLibrary::CloseSender(Name);
		FlushRenderingCommands();
		TestFalse(TEXT("sender stays absent after close"), FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Sender).IsValid());
	}
	Path->Set(PreviousPath, ECVF_SetByCode);
	Path->ClearFlags(ECVF_SetByMask);
	Path->SetFlags(PreviousPriority);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP0CancelInstall, "SpoutPlugin.P0.CancellationOrdersAfterInFlightInstallation", SPOUT_P0_FLAGS)
bool FSpoutP0CancelInstall::RunTest(const FString&)
{
	FSpoutSession Session;
	const FSpoutStreamToken Token = Session.OpenStream(FName(TEXT("InstallationRace")), ESpoutType::Sender);
	FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* Release = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* CancelStarted = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* CancelFinished = FPlatformProcess::GetSynchEventFromPool(true);
	std::atomic<bool> Installed{false};
	TFuture<void> Installer = Async(EAsyncExecution::Thread, [&] {
		Session.ExecuteIfCurrent(Token, [&] { Entered->Trigger(); Release->Wait(5000); Installed.store(true); });
	});
	TestTrue(TEXT("installation reached controlled barrier"), Entered->Wait(5000));
	TFuture<void> Canceller = Async(EAsyncExecution::Thread, [&] {
		CancelStarted->Trigger(); Session.CancelStream(Token.Name, Token.Type);
		Installed.store(false); CancelFinished->Trigger();
	});
	CancelStarted->Wait(5000);
	TestFalse(TEXT("close cannot pass in-flight installation"), CancelFinished->Wait(50));
	Release->Trigger(); Installer.Wait(); Canceller.Wait();
	TestFalse(TEXT("no entry resurrected after close"), Installed.load());
	TestFalse(TEXT("old token cannot install again"), Session.ExecuteIfCurrent(Token, [&] { Installed.store(true); }));
	FPlatformProcess::ReturnSynchEventToPool(Entered); FPlatformProcess::ReturnSynchEventToPool(Release);
	FPlatformProcess::ReturnSynchEventToPool(CancelStarted); FPlatformProcess::ReturnSynchEventToPool(CancelFinished);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP0ExternalPixels, "SpoutPlugin.P0.ExternalProcessPixelsBothDirections", SPOUT_P0_FLAGS)
bool FSpoutP0ExternalPixels::RunTest(const FString&)
{
	ExpectWindowsWrapProbe(*this);
	FString PeerExe;
	if (!FParse::Value(FCommandLine::Get(), TEXT("SpoutNativePeer="), PeerExe))
	{
		AddWarning(TEXT("External pixel qualification not run: supply -SpoutNativePeer=<built SpoutPixelPeer.exe>. See Tests/Native."));
		return true;
	}
	if (!TestTrue(TEXT("native pixel oracle exists"), IFileManager::Get().FileExists(*PeerExe))) return false;
	FSpoutD3DContext::Get().InitializeIfNeeded();
	if (!TestTrue(TEXT("interop available"), FSpoutD3DContext::Get().IsSpoutAvailable())) return false;
	const FString Prefix = TEXT("SpoutExternalP0_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FName SendName(*(Prefix + TEXT("_UE")));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
	Source->InitCustomFormat(32, 16, PF_B8G8R8A8, true);
	QueuePattern(Source.Get(), 1);
	USpoutBPFunctionLibrary::SpoutSender(SendName, ESpoutSendTextureFrom::TextureRenderTarget2D, Source.Get());
	FlushRenderingCommands();
	const FString ResultPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / (Prefix + TEXT(".json")));
	const FString Args = FString::Printf(TEXT("receive %s \"%s\""), *SendName.ToString(), *ResultPath);
	FProcHandle Peer = FPlatformProcess::CreateProc(*PeerExe, *Args, false, true, true, nullptr, 0, nullptr, nullptr);
	TestTrue(TEXT("foreign receiver process launched"), Peer.IsValid());
	const double Deadline = FPlatformTime::Seconds() + 15.0;
	for (int32 Frame = 2; Peer.IsValid() && FPlatformProcess::IsProcRunning(Peer) && FPlatformTime::Seconds() < Deadline; ++Frame)
	{
		QueuePattern(Source.Get(), Frame);
		USpoutBPFunctionLibrary::SpoutSender(SendName, ESpoutSendTextureFrom::TextureRenderTarget2D, Source.Get());
		FlushRenderingCommands(); FPlatformProcess::Sleep(0.005f);
	}
	if (Peer.IsValid())
	{
		if (FPlatformProcess::IsProcRunning(Peer)) { AddError(TEXT("foreign receiver timed out")); FPlatformProcess::TerminateProc(Peer); }
		int32 Exit = -1;
		TestTrue(TEXT("foreign receiver exited"), FPlatformProcess::GetProcReturnCode(Peer, &Exit));
		TestEqual(TEXT("foreign receiver verified eight complete frames"), Exit, 0);
		FPlatformProcess::CloseProc(Peer);
	}
	USpoutBPFunctionLibrary::CloseSender(SendName); FlushRenderingCommands();

	IConsoleVariable* Path = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.GPUReceiver"));
	const int32 PreviousPath = Path->GetInt();
	const EConsoleVariableFlags PreviousPriority = static_cast<EConsoleVariableFlags>(Path->GetFlags() & ECVF_SetByMask);
	for (int32 GPU : {1, 0})
	{
		Path->Set(GPU, ECVF_SetByCode);
		const FString ForeignName = Prefix + FString::Printf(TEXT("_Native%d"), GPU);
		const FString StopName = ForeignName + TEXT("_Stop"), ReadyName = ForeignName + TEXT("_Ready");
		HANDLE Stop = CreateEventW(nullptr, true, false, *StopName);
		HANDLE Ready = CreateEventW(nullptr, true, false, *ReadyName);
		const FString SenderArgs = FString::Printf(TEXT("send %s %s %s"), *ForeignName, *StopName, *ReadyName);
		Peer = FPlatformProcess::CreateProc(*PeerExe, *SenderArgs, false, true, true, nullptr, 0, nullptr, nullptr);
		const bool IsReady = Peer.IsValid() && WaitForSingleObject(Ready, 10000) == WAIT_OBJECT_0;
		TestTrue(TEXT("foreign sender published first frame"), IsReady);
		UMaterialInstanceDynamic* Material = nullptr; UTexture2D* Texture = nullptr;
		int32 Verified = 0, LastSequence = -1;
		TSet<int32> Widths;
		const double ReceiveDeadline = FPlatformTime::Seconds() + 10.0;
		while (IsReady && (Verified < 8 || Widths.Num() < 2) && FPlatformTime::Seconds() < ReceiveDeadline)
		{
			if (USpoutBPFunctionLibrary::SpoutReceiver(FName(*ForeignName), nullptr, NAME_None, Material, Texture, nullptr, ESpoutColorSpace::Linear) && Texture)
			{
				const int32 W = Texture->GetSizeX(), H = Texture->GetSizeY();
				TArray<FColor> Actual; QueueRead(Texture, W, H, Actual); FlushRenderingCommands();
				// A source can disappear between metadata lookup and opening its handle.
				// A newly allocated, not-yet-filled target is allowed to remain transparent.
				if (Actual.Num() && Actual[0].A == 255)
				{
					const int32 Sequence = Actual[0].R;
					if (!TestTrue(TEXT("foreign frame matches every pixel without tearing"), Actual == MakePattern(W, H, Sequence))) break;
					Widths.Add(W);
					if (Sequence != LastSequence) { ++Verified; LastSequence = Sequence; }
				}
			}
			FPlatformProcess::Sleep(0.01f);
		}
		TestTrue(TEXT("at least eight distinct foreign frames verified"), Verified >= 8);
		TestTrue(TEXT("foreign sender resize observed"), Widths.Num() >= 2);
		SetEvent(Stop);
		if (Peer.IsValid())
		{
			const double StopDeadline = FPlatformTime::Seconds() + 3.0;
			while (FPlatformProcess::IsProcRunning(Peer) && FPlatformTime::Seconds() < StopDeadline) FPlatformProcess::Sleep(0.01f);
			if (FPlatformProcess::IsProcRunning(Peer)) FPlatformProcess::TerminateProc(Peer);
			int32 Exit = -1; FPlatformProcess::GetProcReturnCode(Peer, &Exit);
			TestEqual(TEXT("foreign sender stopped cleanly"), Exit, 0);
			FPlatformProcess::CloseProc(Peer);
		}
		CloseHandle(Ready); CloseHandle(Stop);
		USpoutBPFunctionLibrary::CloseReceiver(FName(*ForeignName), Texture, Material); FlushRenderingCommands();
	}
	Path->Set(PreviousPath, ECVF_SetByCode);
	Path->ClearFlags(ECVF_SetByMask);
	Path->SetFlags(PreviousPriority);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP0Layouts, "SpoutPlugin.P0.NativeDescriptorMismatchAndMipRejection", SPOUT_P0_FLAGS)
bool FSpoutP0Layouts::RunTest(const FString&)
{
	ExpectWindowsWrapProbe(*this);
	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	Context.InitializeIfNeeded();
	if (!TestTrue(TEXT("interop device available"), Context.IsSpoutAvailable())) return false;
	const FString Unique = TEXT("SpoutP0_Layout_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FName Name(*Unique);
	FTCHARToUTF8 Utf8(*Unique);
	D3D11_TEXTURE2D_DESC Desc = {};
	Desc.Width = 32; Desc.Height = 16; Desc.MipLevels = Desc.ArraySize = Desc.SampleDesc.Count = 1;
	Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; Desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> Shared;
	if (!TestTrue(TEXT("shared texture created"), SUCCEEDED(Context.GetD3D11Device()->CreateTexture2D(&Desc, nullptr, &Shared)))) return false;
	Microsoft::WRL::ComPtr<IDXGIResource> DXGI; Shared.As(&DXGI);
	HANDLE Handle = nullptr; DXGI->GetSharedHandle(&Handle);
	FSpoutSafeSenderNames Foreign;
	if (!TestTrue(TEXT("malformed advertisement created"), Foreign.CreateSenderExact(Utf8.Get(), 64, 16, Handle, Desc.Format))) return false;
	// A nonempty registry with a name longer than STL's small-string buffer
	// exposes a cross-DLL allocator mismatch. Exercise this inside an Unreal
	// module, whose operator delete differs from the bundled SDK's CRT allocator.
	FSpoutSafeSenderNames Second;
	FTCHARToUTF8 SecondName(*(Unique + TEXT("_Second")));
	TestTrue(TEXT("register beside a long foreign name without allocator crossing"), Second.CreateSenderExact(SecondName.Get(), 32, 16, Handle, Desc.Format));
	Second.ReleaseSenderName(SecondName.Get());
	TestFalse(TEXT("long foreign-name collision refused"), Second.CreateSenderExact(Utf8.Get(), 32, 16, Handle, Desc.Format));
	unsigned int ForeignW = 0, ForeignH = 0; HANDLE ForeignHandle = nullptr; DWORD ForeignFormat = 0;
	TestTrue(TEXT("foreign metadata remains intact after collision"), Foreign.GetSenderInfo(Utf8.Get(), ForeignW, ForeignH, ForeignHandle, ForeignFormat) && ForeignW == 64 && ForeignH == 16 && ForeignFormat == Desc.Format);
	UMaterialInstanceDynamic* Material = nullptr; UTexture2D* Texture = nullptr;
	USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, Material, Texture, nullptr, ESpoutColorSpace::Linear);
	FlushRenderingCommands();
	TSharedPtr<FSpoutSharedSender> Receiver = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Receiver);
	TestTrue(TEXT("native size mismatch refused before installation"), Receiver && !Receiver->SharedTexture);
	Foreign.UpdateSender(Utf8.Get(), 32, 16, Handle, DXGI_FORMAT_R8G8B8A8_UNORM);
	USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, Material, Texture, nullptr, ESpoutColorSpace::Linear);
	FlushRenderingCommands();
	TestTrue(TEXT("native format mismatch refused before installation"), Receiver && !Receiver->SharedTexture);
	Foreign.UpdateSender(Utf8.Get(), 32, 16, Handle, Desc.Format);
	USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, Material, Texture, nullptr, ESpoutColorSpace::Linear);
	FlushRenderingCommands();
	TestTrue(TEXT("corrected advertisement can reconnect"), Receiver && !!Receiver->SharedTexture);
	USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material);
	Receiver.Reset(); Foreign.ReleaseSenderName(Utf8.Get()); FlushRenderingCommands();

	TStrongObjectPtr<UTextureRenderTarget2D> Mipped(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
	Mipped->bAutoGenerateMips = true;
	Mipped->InitCustomFormat(32, 16, PF_B8G8R8A8, true);
	USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Mipped.Get());
	FlushRenderingCommands();
	TestFalse(TEXT("multi-mip source does not publish a sender"), Foreign.FindSenderName(Utf8.Get()));
	USpoutBPFunctionLibrary::CloseSender(Name); FlushRenderingCommands();
	return true;
}
#undef SPOUT_P0_FLAGS
#endif
