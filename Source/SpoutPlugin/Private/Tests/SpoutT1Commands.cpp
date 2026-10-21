// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
//
// Two correctness checks, run as console commands, each in a process of its own. Neither is registered as an
// automation test, so a test pass that runs every registered SpoutPlugin test (in an editor process latched to the
// shared-slot path) neither runs nor expects them.
//
//   Spout.T1.Viewport        in a -game process latched to the shared-slot path: sends the game viewport, reads the
//                            exact back buffer that was sent (taken right after its copy into the slot), receives the
//                            stream in the same process and compares every texel; then resizes the window and does it
//                            again. Its phases are SpoutT1ViewportSequence.h: the resize is requested once, in the tick
//                            that enters the resize phase. Ends the process when done or when its own 90 s bound passes.
//   Spout.T1.LegacyControl   in the separate positive-control process latched to the legacy wrap path
//                            (r.Spout.SharedSlotInterop 0, -d3ddebug): one render-target round trip that must wrap,
//                            and the debug capture must count the wrap message (id 916). Shows the capture can see it.
//
// Each logs exactly one line "Spout T1 result: check=<name> outcome=<PASS|FAIL> ..." and one "Spout T1 failure:" line
// per failed condition, for a log analyzer to read; a missing result line is missing evidence.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
#include "SpoutBPFunctionLibrary.h"
#include "SpoutD3DContext.h"
#include "SpoutDebugCapture.h"
#include "SpoutModule.h"
#include "SpoutSenderRegistry.h"
#include "SpoutSharedSlot.h"
#include "SpoutT1ViewportSequence.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "UnrealClient.h"
#include "UnrealEngine.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	void LogResult(const TCHAR* Check, const TArray<FString>& Failures, const FString& Detail)
	{
		for (const FString& Failure : Failures)
		{
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout T1 failure: check=%s: %s"), Check, *Failure);
		}
		UE_LOG(LogSpoutPlugin, Log, TEXT("Spout T1 result: check=%s outcome=%s %s"), Check, Failures.Num() == 0 ? TEXT("PASS") : TEXT("FAIL"), *Detail);
	}

	/** Every texel of a texture as floats, with the flags the source capture uses (SpoutSharedSlot.cpp). */
	TArray<FLinearColor> ReadLinear(FRHITexture* Texture)
	{
		TArray<FLinearColor> Pixels;
		if (!Texture) return Pixels;
		const FIntPoint Size(Texture->GetDesc().Extent.X, Texture->GetDesc().Extent.Y);
		TArray<FLinearColor>* Target = &Pixels;
		ENQUEUE_RENDER_COMMAND(SpoutT1Read)([Texture, Size, Target](FRHICommandListImmediate& Cmd)
		{
			FReadSurfaceDataFlags Flags(RCM_MinMax);
			Flags.SetLinearToGamma(false);
			Cmd.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
			Cmd.ReadSurfaceData(Texture, FIntRect(0, 0, Size.X, Size.Y), *Target, Flags);
			Cmd.Transition(FRHITransitionInfo(Texture, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		});
		FlushRenderingCommands();
		return Pixels;
	}

	int32 CountDistinct(const TArray<FLinearColor>& Pixels, int32 Limit)
	{
		TArray<FLinearColor> Seen;
		for (const FLinearColor& P : Pixels)
		{
			if (!Seen.Contains(P))
			{
				Seen.Add(P);
				if (Seen.Num() >= Limit) break;
			}
		}
		return Seen.Num();
	}

	FRHITexture* RHIOf(UTexture2D* Texture)
	{
		FTextureResource* Resource = Texture ? Texture->GetResource() : nullptr;
		return Resource ? Resource->GetTextureRHI() : nullptr;
	}

	// ---------------------------------------------------------------------------------------------
	// Spout.T1.Viewport
	// ---------------------------------------------------------------------------------------------

	struct FViewportCheck
	{
		FName Name;
		TUniquePtr<SpoutT1::FViewportSequence> Sequence;
		uint64 ReceivedAtSend = 0;
		double Deadline = 0.0;
		int64 WrapsAtStart = 0;
		bool bCaptureTaken = false;
		TArray<FLinearColor> Captured;
		FIntPoint CapturedSize = FIntPoint::ZeroValue;
		EPixelFormat CapturedFormat = PF_Unknown;
		UTexture2D* Texture = nullptr;
		UMaterialInstanceDynamic* Material = nullptr;
		TArray<FString> Failures;
		TArray<FString> Verified;
		TArray<FString> Requests;
		FTSTicker::FDelegateHandle Ticker;

		uint64 ReceivedNow() const
		{
			const TSharedPtr<FSpoutSharedSender> Entry = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Receiver);
			return Entry ? Entry->ReceivedFrameNumber : 0;
		}

		/**
		 * One step per engine tick: what the sequence decides (SpoutT1ViewportSequence.h), carried out. The resize request
		 * is the sequence's entry action of its Resize phase: emitted once, in the tick that enters the phase, and the
		 * RequestResize case below is the only place that asks for a resolution. False ends the ticker.
		 */
		bool Tick()
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				Failures.Add(FString::Printf(TEXT("the check's 90 s bound passed in phase %d at size %d"), static_cast<int32>(Sequence->GetPhase()), Sequence->GetSizeIndex()));
				Finish();
				return false;
			}
			UGameViewportClient* Client = GEngine ? GEngine->GameViewport : nullptr;
			if (!Client || !Client->Viewport)
			{
				Failures.Add(TEXT("the game viewport is gone"));
				Finish();
				return false;
			}
			if (!bCaptureTaken && Sequence->GetPhase() == SpoutT1::EPhase::AwaitCapture)
			{
				bCaptureTaken = SpoutSlotTestHooks::TakeSourceCapture(Captured, CapturedSize, CapturedFormat);
			}
			SpoutT1::FInput In;
			In.Viewport = SpoutT1::FSize{ Client->Viewport->GetSizeXY().X, Client->Viewport->GetSizeXY().Y };
			In.Frame = GFrameCounter;
			In.Now = FPlatformTime::Seconds();
			In.bCaptureTaken = bCaptureTaken;
			In.bDelivered = bCaptureTaken && Texture && Texture->GetSizeX() == CapturedSize.X && Texture->GetSizeY() == CapturedSize.Y
				&& ReceivedNow() > ReceivedAtSend;
			std::vector<SpoutT1::EAction> Actions;
			const SpoutT1::FSize Wanted = Sequence->GetWanted();
			Sequence->Tick(In, Actions);
			for (const SpoutT1::EAction Action : Actions)
			{
				switch (Action)
				{
				case SpoutT1::EAction::RequestResize:
					Requests.Add(FString::Printf(TEXT("%dx%d at frame %llu"), Sequence->GetWanted().X, Sequence->GetWanted().Y, GFrameCounter));
					FSystemResolution::RequestResolutionChange(Sequence->GetWanted().X, Sequence->GetWanted().Y, EWindowMode::Windowed);
					break;
				case SpoutT1::EAction::SendViewport:
					bCaptureTaken = false;
					SpoutSlotTestHooks::ArmSourceCapture();
					ReceivedAtSend = ReceivedNow();
					if (!USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::GameViewport, nullptr))
					{
						Failures.Add(TEXT("the viewport send was refused"));
						Finish();
						return false;
					}
					break;
				case SpoutT1::EAction::Receive:
					USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, Material, Texture, nullptr, ESpoutColorSpace::Linear);
					FlushRenderingCommands();
					break;
				case SpoutT1::EAction::Verify:
					VerifyOne(FIntPoint(Wanted.X, Wanted.Y));
					break;
				case SpoutT1::EAction::Fail:
					Failures.Add(FString::Printf(TEXT("size %d: %s"), Sequence->GetSizeIndex(), ANSI_TO_TCHAR(Sequence->GetFailure())));
					Finish();
					return false;
				case SpoutT1::EAction::Finish:
					Finish();
					return false;
				default:
					break;
				}
			}
			return true;
		}

		void VerifyOne(const FIntPoint& Want)
		{
			const TArray<FLinearColor> Got = ReadLinear(RHIOf(Texture));
			const int32 Distinct = CountDistinct(Captured, 4);
			const FString Step = FString::Printf(TEXT("%dx%d %s"), CapturedSize.X, CapturedSize.Y, GetPixelFormatString(CapturedFormat));
			bool bOk = true;
			if (CapturedSize != Want)
			{
				bOk = false;
				Failures.Add(FString::Printf(TEXT("%s: the sent back buffer is not the viewport's size %dx%d"), *Step, Want.X, Want.Y));
			}
			if (Distinct < 2)
			{
				bOk = false;
				Failures.Add(FString::Printf(TEXT("%s: the sent image is uniform, so it cannot show a copy"), *Step));
			}
			if (Got.Num() != Captured.Num() || Got != Captured)
			{
				bOk = false;
				int32 Differing = 0;
				for (int32 I = 0; I < FMath::Min(Got.Num(), Captured.Num()); ++I) if (!(Got[I] == Captured[I])) ++Differing;
				Failures.Add(FString::Printf(TEXT("%s: the received texture differs from the sent back buffer in %d of %d texels (sizes %d and %d)"),
					*Step, Differing, Captured.Num(), Got.Num(), Captured.Num()));
			}
			if (bOk)
			{
				Verified.Add(Step);
			}
			bCaptureTaken = false;
		}

		void Finish()
		{
			USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material);
			USpoutBPFunctionLibrary::CloseSender(Name);
			FlushRenderingCommands();
			const int64 Wraps = SpoutInterop::GetWrapCallCount() - WrapsAtStart;
			if (Wraps != 0)
			{
				Failures.Add(FString::Printf(TEXT("%lld D3D11-on-12 wraps were made"), Wraps));
			}
			// No size was asked for more than once (a size the viewport already had is not asked for at all).
			for (int32 I = 0; I < 2; ++I)
			{
				if (Sequence->GetResizeRequests(I) > 1)
				{
					Failures.Add(FString::Printf(TEXT("size %d was requested %d times"), I, Sequence->GetResizeRequests(I)));
				}
			}
			LogResult(TEXT("Viewport"), Failures, FString::Printf(TEXT("path=%s verified=[%s] wraps=%lld requests=[%s]"),
				SpoutInterop::ToString(SpoutInterop::GetMode()), *FString::Join(Verified, TEXT("; ")), Wraps, *FString::Join(Requests, TEXT("; "))));
			RequestEngineExit(TEXT("Spout.T1.Viewport finished"));
		}
	};
	TSharedPtr<FViewportCheck> GViewportCheck;

	void StartViewportCheck()
	{
		if (GViewportCheck.IsValid())
		{
			return;
		}
		TArray<FString> Failures;
		FSpoutD3DContext::Get().InitializeIfNeeded();
		if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
		{
			Failures.Add(TEXT("no game viewport: run this command in a -game process"));
		}
		if (!SpoutInterop::IsSharedSlot())
		{
			Failures.Add(FString::Printf(TEXT("this process latched the %s path, not the shared-slot path"), SpoutInterop::ToString(SpoutInterop::GetMode())));
		}
		if (Failures.Num())
		{
			LogResult(TEXT("Viewport"), Failures, TEXT("started=0"));
			RequestEngineExit(TEXT("Spout.T1.Viewport could not start"));
			return;
		}
		GViewportCheck = MakeShared<FViewportCheck>();
		FViewportCheck& Check = *GViewportCheck;
		Check.Name = FName(*(TEXT("SpoutT1Viewport_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		Check.Sequence = MakeUnique<SpoutT1::FViewportSequence>(std::vector<SpoutT1::FSize>{ SpoutT1::FSize{1280, 720}, SpoutT1::FSize{640, 360} });
		Check.Deadline = FPlatformTime::Seconds() + 90.0;
		Check.WrapsAtStart = SpoutInterop::GetWrapCallCount();
		// Something other than a cleared frame in the back buffer: text drawn into the viewport for the whole check.
		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(0x5370741), 600.0f, FColor::Yellow, TEXT("SPOUT T1 VIEWPORT CHECK: the back buffer is sent, received and compared texel by texel"));
		Check.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			return GViewportCheck.IsValid() && GViewportCheck->Tick();
		}));
	}

	FAutoConsoleCommand GSpoutT1Viewport(
		TEXT("Spout.T1.Viewport"),
		TEXT("T1 correctness run only: sends the game viewport through the shared-slot path, compares the received texture with the exact back buffer sent, resizes and repeats, then exits. -game processes only."),
		FConsoleCommandDelegate::CreateStatic(&StartViewportCheck));

	// ---------------------------------------------------------------------------------------------
	// Spout.T1.LegacyControl
	// ---------------------------------------------------------------------------------------------

	void RunLegacyControl()
	{
		TArray<FString> Failures;
		FSpoutD3DContext::Get().InitializeIfNeeded();
		if (SpoutInterop::GetMode() != SpoutInterop::EMode::Wrap)
		{
			Failures.Add(FString::Printf(TEXT("this process latched the %s path; the control needs r.Spout.SharedSlotInterop 0 before the first Spout initialization"),
				SpoutInterop::ToString(SpoutInterop::GetMode())));
		}
		if (!FParse::Param(FCommandLine::Get(), TEXT("d3ddebug")))
		{
			Failures.Add(TEXT("the control needs -d3ddebug"));
		}
		const SpoutDebugCapture::FCounts Before = SpoutDebugCapture::GetCounts();
		if (!Before.bRegistered || Before.Canary <= 0)
		{
			Failures.Add(FString::Printf(TEXT("the D3D12 capture is not proven live (registered %d, canary %lld)"), Before.bRegistered ? 1 : 0, Before.Canary));
		}
		const int64 WrapsBefore = SpoutInterop::GetWrapCallCount();
		bool bPixels = false;
		if (Failures.Num() == 0)
		{
			const FName Name(*(TEXT("SpoutT1Control_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			TStrongObjectPtr<UTextureRenderTarget2D> Source(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
			const int32 W = 64, H = 16;
			Source->InitCustomFormat(W, H, PF_B8G8R8A8, true);
			TArray<FColor> Pattern;
			Pattern.SetNumUninitialized(W * H);
			for (int32 Y = 0; Y < H; ++Y) for (int32 X = 0; X < W; ++X) Pattern[Y * W + X] = FColor(uint8(X * 4), uint8(Y * 16), uint8(X + Y), 255);
			FTextureRenderTargetResource* Resource = Source->GameThread_GetRenderTargetResource();
			ENQUEUE_RENDER_COMMAND(SpoutT1ControlUpload)([Resource, W, H, Pattern](FRHICommandListImmediate& Cmd)
			{
				Cmd.UpdateTexture2D(Resource->GetRenderTargetTexture(), 0, FUpdateTextureRegion2D(0, 0, 0, 0, W, H), W * sizeof(FColor), reinterpret_cast<const uint8*>(Pattern.GetData()));
			});
			FlushRenderingCommands();
			USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source.Get());
			FlushRenderingCommands();
			UTexture2D* Texture = nullptr;
			UMaterialInstanceDynamic* Material = nullptr;
			USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, Material, Texture, nullptr, ESpoutColorSpace::Linear);
			FlushRenderingCommands();
			bPixels = Texture && ReadLinear(RHIOf(Texture)) == ReadLinear(Resource->GetRenderTargetTexture());
			USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material);
			USpoutBPFunctionLibrary::CloseSender(Name);
			FlushRenderingCommands();
			if (!bPixels)
			{
				Failures.Add(TEXT("the round trip through the legacy wrap path did not deliver the source's texels"));
			}
		}
		const SpoutDebugCapture::FCounts After = SpoutDebugCapture::GetCounts();
		const int64 Wraps = SpoutInterop::GetWrapCallCount() - WrapsBefore;
		const int64 Seen916 = After.WrapReflection - Before.WrapReflection;
		if (Failures.Num() == 0 && Wraps < 1)
		{
			Failures.Add(TEXT("the legacy path made no D3D11-on-12 wrap"));
		}
		if (Failures.Num() == 0 && Seen916 < 1)
		{
			Failures.Add(TEXT("the D3D12 capture did not count the wrap message (id 916): it cannot be shown to see it"));
		}
		LogResult(TEXT("LegacyControl"), Failures, FString::Printf(TEXT("path=%s wraps=%lld id916=%lld pixels=%d"),
			SpoutInterop::ToString(SpoutInterop::GetMode()), Wraps, Seen916, bPixels ? 1 : 0));
	}

	FAutoConsoleCommand GSpoutT1LegacyControl(
		TEXT("Spout.T1.LegacyControl"),
		TEXT("T1 correctness run only: in a process latched to the legacy wrap path with -d3ddebug, makes one wrap round trip and checks that the debug capture counted the wrap message."),
		FConsoleCommandDelegate::CreateStatic(&RunLegacyControl));
}
#endif
