// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// SpoutHostTest.AsyncBetaCheck <report.json> <seconds> <spout name>
//
// The Async Receive (Beta) check of a packaged host (Scripts/AsyncReceiveBeta/README.md). The level it runs in holds
// shipped receiver examples (BP_SpoutReceiverExample) whose Async Receive (Beta) setting was SAVED with the level; the
// check sets nothing on them. It publishes a moving pattern under the given name (the example receives "Spout") with the
// plugin's own sender, for the given time, then records what each example's saved setting is, what GetSpoutStreamStatus
// reports as requested and effective, and whether the example's received texture holds exactly one of the published
// patterns. The report is JSON; the process then exits normally (exit code 0 when every example's effective mode
// equals its saved request and its image arrived, 1 otherwise).
#include "SpoutBPFunctionLibrary.h"
#include "Containers/Ticker.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace SpoutHostTestAsyncBeta
{
	constexpr int32 Width = 64, Height = 32, Kept = 16;

	static TArray<FColor> Pattern(int32 Sequence)
	{
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y) for (int32 X = 0; X < Width; ++X)
			Pixels[Y * Width + X] = FColor(uint8((X * 17 + Sequence) & 255), uint8((Y * 29 + Sequence * 3) & 255), uint8((X + Y + Sequence * 13) & 255), 255);
		return Pixels;
	}

	static TArray<FColor> Read(UTexture* Texture)
	{
		TArray<FColor> Pixels;
		FTextureResource* Resource = Texture ? Texture->GetResource() : nullptr;
		ENQUEUE_RENDER_COMMAND(SpoutAsyncBetaCheckRead)([Resource, &Pixels](FRHICommandListImmediate& Cmd)
		{
			FRHITexture* RHI = Resource ? Resource->GetTextureRHI() : nullptr;
			if (!RHI) return;
			FReadSurfaceDataFlags Flags(RCM_UNorm);
			Flags.SetLinearToGamma(false);
			Cmd.Transition(FRHITransitionInfo(RHI, ERHIAccess::Unknown, ERHIAccess::CopySrc));
			Cmd.ReadSurfaceData(RHI, FIntRect(0, 0, RHI->GetDesc().Extent.X, RHI->GetDesc().Extent.Y), Pixels, Flags);
			Cmd.Transition(FRHITransitionInfo(RHI, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		});
		FlushRenderingCommands();
		return Pixels;
	}

	struct FExample
	{
		TWeakObjectPtr<AActor> Actor;
		FString Label;
		bool bSavedAsync = false;
		bool bHasSetting = false;
	};

	struct FRun
	{
		FString ReportPath;
		FName Name;
		double End = 0.0;
		int32 Sequence = 0;
		TStrongObjectPtr<UTextureRenderTarget2D> Source;
		TArray<int32> Sent;
		TArray<FExample> Examples;
	};
	static TUniquePtr<FRun> GRun;

	static const TCHAR* ModeName(ESpoutReceiveMode Mode) { return Mode == ESpoutReceiveMode::AsyncBeta ? TEXT("AsyncBeta") : TEXT("Synchronous"); }

	static void Finish(const FString& Refusal = FString())
	{
		const FSpoutStreamStatus Status = USpoutBPFunctionLibrary::GetSpoutStreamStatus(GRun->Name, ESpoutType::Receiver);
		FString Json = FString::Printf(TEXT("{\n  \"check\": \"SpoutHostTest.AsyncBetaCheck\",\n  \"spoutName\": \"%s\",\n  \"refusal\": \"%s\",\n")
			TEXT("  \"status\": {\"active\": %s, \"requested\": \"%s\", \"effective\": \"%s\", \"note\": \"%s\", \"framesSubmitted\": %lld},\n  \"examples\": [\n"),
			*GRun->Name.ToString(), *Refusal.ReplaceCharWithEscapedChar(), Status.bActive ? TEXT("true") : TEXT("false"), ModeName(Status.RequestedReceiveMode),
			ModeName(Status.EffectiveReceiveMode), *Status.ReceiveModeNote.ReplaceCharWithEscapedChar(), Status.FramesSubmitted);
		bool bAllPassed = Refusal.IsEmpty() && GRun->Examples.Num() > 0;
		for (int32 I = 0; I < GRun->Examples.Num(); ++I)
		{
			FExample& E = GRun->Examples[I];
			UTexture2D* Received = nullptr;
			if (AActor* A = E.Actor.Get())
			{
				if (const FObjectPropertyBase* P = FindFProperty<FObjectPropertyBase>(A->GetClass(), TEXT("SpoutReceivedTexture")))
				{
					Received = Cast<UTexture2D>(P->GetObjectPropertyValue_InContainer(A));
				}
			}
			const TArray<FColor> Got = Read(Received);
			int32 Matched = -1;
			for (int32 Sequence : GRun->Sent) if (Got.Num() == Width * Height && Got == Pattern(Sequence)) Matched = Sequence;
			const ESpoutReceiveMode Expected = E.bSavedAsync ? ESpoutReceiveMode::AsyncBeta : ESpoutReceiveMode::Synchronous;
			const bool bPassed = E.bHasSetting && Status.bActive && Status.RequestedReceiveMode == Expected && Status.EffectiveReceiveMode == Expected && Matched >= 0;
			bAllPassed &= bPassed;
			Json += FString::Printf(TEXT("    {\"actor\": \"%s\", \"hasAsyncReceiveBeta\": %s, \"savedAsyncReceiveBeta\": %s, \"receivedTexture\": \"%s\", \"matchedSequence\": %d, \"lastSent\": %d, \"passed\": %s}%s\n"),
				*E.Label, E.bHasSetting ? TEXT("true") : TEXT("false"), E.bSavedAsync ? TEXT("true") : TEXT("false"), Received ? *Received->GetName() : TEXT(""),
				Matched, GRun->Sent.Num() ? GRun->Sent.Last() : -1, bPassed ? TEXT("true") : TEXT("false"), I + 1 < GRun->Examples.Num() ? TEXT(",") : TEXT(""));
		}
		if (Refusal.IsEmpty()) USpoutBPFunctionLibrary::CloseSender(GRun->Name);
		Json += FString::Printf(TEXT("  ],\n  \"passed\": %s\n}\n"), bAllPassed ? TEXT("true") : TEXT("false"));
		FFileHelper::SaveStringToFile(Json, *GRun->ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogTemp, Display, TEXT("SpoutHostTest.AsyncBetaCheck: %s, report %s"), bAllPassed ? TEXT("PASSED") : TEXT("FAILED"), *GRun->ReportPath);
		GRun.Reset();
		FPlatformMisc::RequestExitWithStatus(false, bAllPassed ? 0 : 1);
	}

	static bool Tick(float)
	{
		if (!GRun) return false;
		++GRun->Sequence;
		FTextureRenderTargetResource* Resource = GRun->Source->GameThread_GetRenderTargetResource();
		TArray<FColor> Pixels = Pattern(GRun->Sequence);
		ENQUEUE_RENDER_COMMAND(SpoutAsyncBetaCheckUpload)([Resource, Pixels = MoveTemp(Pixels)](FRHICommandListImmediate& Cmd)
		{
			Cmd.UpdateTexture2D(Resource->GetRenderTargetTexture(), 0, FUpdateTextureRegion2D(0, 0, 0, 0, Width, Height), Width * 4, reinterpret_cast<const uint8*>(Pixels.GetData()));
		});
		if (USpoutBPFunctionLibrary::SpoutSender(GRun->Name, ESpoutSendTextureFrom::TextureRenderTarget2D, GRun->Source.Get()))
		{
			GRun->Sent.Add(GRun->Sequence);
			if (GRun->Sent.Num() > Kept) GRun->Sent.RemoveAt(0);
		}
		if (FPlatformTime::Seconds() < GRun->End) return true;
		Finish();
		return false;
	}

	static void Start(const TArray<FString>& Args, UWorld* World)
	{
		if (GRun || Args.Num() < 3 || !World)
		{
			UE_LOG(LogTemp, Error, TEXT("SpoutHostTest.AsyncBetaCheck <report.json> <seconds> <spout name> (one at a time, in a game world)"));
			return;
		}
		GRun = MakeUnique<FRun>();
		GRun->ReportPath = Args[0];
		GRun->End = FPlatformTime::Seconds() + FMath::Clamp(FCString::Atod(*Args[1]), 1.0, 120.0);
		GRun->Name = FName(*Args[2]);
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (!A->GetClass()->GetName().StartsWith(TEXT("BP_SpoutReceiverExample"))) continue;
			const FBoolProperty* AsyncProperty = FindFProperty<FBoolProperty>(A->GetClass(), TEXT("AsyncReceiveBeta"));
			FExample E;
			E.Actor = A;
			E.Label = A->GetName();
			E.bHasSetting = AsyncProperty != nullptr;
			E.bSavedAsync = AsyncProperty && AsyncProperty->GetPropertyValue_InContainer(A);
			UE_LOG(LogTemp, Display, TEXT("SpoutHostTest.AsyncBetaCheck: example %s, saved Async Receive (Beta) = %s%s"),
				*E.Label, E.bSavedAsync ? TEXT("true") : TEXT("false"), E.bHasSetting ? TEXT("") : TEXT(" (the example has no such setting)"));
			GRun->Examples.Add(MoveTemp(E));
		}
		// Another process's sender under this name would make the check read someone else's image: refuse instead.
		FSenderStruct Existing;
		if (USpoutBPFunctionLibrary::GetSenderInfo(GRun->Name, Existing))
		{
			Finish(TEXT("another sender already holds this name"));
			return;
		}
		GRun->Source.Reset(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
		GRun->Source->InitCustomFormat(Width, Height, PF_B8G8R8A8, true);
		GRun->Source->UpdateResourceImmediate(true);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
	}
	static FAutoConsoleCommandWithWorldAndArgs Command(TEXT("SpoutHostTest.AsyncBetaCheck"),
		TEXT("SpoutHostTest.AsyncBetaCheck <report.json> <seconds> <spout name>: publishes under that name to the placed receiver examples and reports their saved, requested and effective receive mode and image, then exits."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Start));
}
