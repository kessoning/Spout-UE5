// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// Release smoke for the placeable actors in a packaged game (test-only). Command line:
//   -SpoutActorsSmoke -SpoutActorsReport=<file> -SpoutActorsExt=<external sender> -SpoutActorsExtSize=<W>x<H> [-SpoutActorsQuick]
// The external sender is SpoutPatternSender (quadrants red/green/blue/white, black corner marker). The game spawns a
// Spout Sender actor at its defaults (Game Viewport Only) and a Spout Receiver actor on the external sender, then checks:
//   D1 defaults, A1 sending and receiving, A2 exact pixels through the receiver's output render target, (hold for the
//   orchestrator's external Game Viewport Only receive), B disable/enable, C Blueprint Close Receiver and reopen,
//   F a refused consumer beside the shared session; then exits. Quick mode: D1, A1, A2 only.
// Each check is written as "CHECK <id> PASS|FAIL <detail>"; the current phase to "<report>.phase".
#include "SpoutHostTestActors.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/EngineVersion.h"
#include "SpoutBPFunctionLibrary.h"
#include "SpoutModule.h"
#include "SpoutReceiverActor.h"
#include "SpoutSenderActor.h"

namespace SpoutHostTest
{
	namespace
	{
		FTSTicker::FDelegateHandle TickHandle;
		FString ReportPath;
		FString Lines;
		FName ExtName;
		FIntPoint ExtSize(640, 360);
		bool bQuick = false;
		bool bAnyFail = false;
		int32 Step = 0;
		double StepStart = 0.0;
		double RunStart = 0.0;
		int64 SavedFrames = 0;
		TWeakObjectPtr<ASpoutSenderActor> Sender;
		TWeakObjectPtr<ASpoutReceiverActor> Receiver;
		TWeakObjectPtr<ASpoutReceiverActor> Refused;
		UTextureRenderTarget2D* Target = nullptr;

		void Flush()
		{
			FFileHelper::SaveStringToFile(Lines, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		void Phase(const TCHAR* Name)
		{
			FFileHelper::SaveStringToFile(FString(Name), *(ReportPath + TEXT(".phase")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			Lines += FString::Printf(TEXT("PHASE %s %.2f\n"), Name, FPlatformTime::Seconds() - RunStart);
			Flush();
		}

		void Check(const TCHAR* Id, bool bPass, const FString& Detail)
		{
			bAnyFail |= !bPass;
			Lines += FString::Printf(TEXT("CHECK %s %s %s\n"), Id, bPass ? TEXT("PASS") : TEXT("FAIL"), *Detail);
			UE_LOG(LogTemp, Display, TEXT("SpoutActorsSmoke CHECK %s %s %s"), Id, bPass ? TEXT("PASS") : TEXT("FAIL"), *Detail);
			Flush();
		}

		void Next()
		{
			++Step;
			StepStart = FPlatformTime::Seconds();
		}

		double Age()
		{
			return FPlatformTime::Seconds() - StepStart;
		}

		UWorld* GameWorld()
		{
			if (!GEngine) return nullptr;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				UWorld* World = Context.World();
				if (World && (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE) && World->HasBegunPlay()) return World;
			}
			return nullptr;
		}

		FSpoutStreamStatus ReceiveStatus()
		{
			return USpoutBPFunctionLibrary::GetSpoutStreamStatus(ExtName, ESpoutType::Receiver);
		}

		float HasSignal(const ASpoutReceiverActor* Actor)
		{
			UMaterialInstanceDynamic* Mid = Actor ? Actor->GetDisplayMaterialInstance() : nullptr;
			return Mid ? Mid->K2_GetScalarParameterValue(TEXT("HasSignal")) : -1.f;
		}

		FString Describe(const ASpoutReceiverActor* Actor)
		{
			if (!Actor) return TEXT("(no actor)");
			return FString::Printf(TEXT("state %d, '%s', size %dx%d, applied '%s', HasSignal %.0f"), static_cast<int32>(Actor->State), *Actor->Status,
				Actor->ReceivedSize.X, Actor->ReceivedSize.Y, *Actor->AppliedSettings, HasSignal(Actor));
		}

		bool PixelsMatch(FString& OutDetail)
		{
			FTextureRenderTargetResource* Resource = Target ? Target->GameThread_GetRenderTargetResource() : nullptr;
			if (!Resource) { OutDetail = TEXT("no target resource"); return false; }
			TArray<FColor> Pixels;
			if (!Resource->ReadPixels(Pixels)) { OutDetail = TEXT("ReadPixels failed"); return false; }
			const int32 W = Target->SizeX, H = Target->SizeY;
			if (Pixels.Num() != W * H || W <= 0 || H <= 0) { OutDetail = TEXT("unexpected pixel count"); return false; }
			struct FSample { float U, V; FColor Want; };
			const FSample Samples[] = { { 0.25f, 0.25f, FColor(255, 0, 0) }, { 0.75f, 0.25f, FColor(0, 255, 0) }, { 0.25f, 0.75f, FColor(0, 0, 255) },
				{ 0.75f, 0.75f, FColor(255, 255, 255) }, { 0.03f, 0.03f, FColor(0, 0, 0) } };
			bool bOk = W == ExtSize.X && H == ExtSize.Y;
			OutDetail = FString::Printf(TEXT("target %dx%d:"), W, H);
			for (const FSample& S : Samples)
			{
				const FColor C = Pixels[FMath::Clamp(int32(S.V * H), 0, H - 1) * W + FMath::Clamp(int32(S.U * W), 0, W - 1)];
				const bool bNear = FMath::Abs(C.R - S.Want.R) <= 1 && FMath::Abs(C.G - S.Want.G) <= 1 && FMath::Abs(C.B - S.Want.B) <= 1;
				bOk &= bNear;
				OutDetail += FString::Printf(TEXT(" (%d,%d,%d)"), C.R, C.G, C.B);
			}
			return bOk;
		}

		void Finish()
		{
			Lines += FString::Printf(TEXT("RESULT %s\n"), bAnyFail ? TEXT("FAIL") : TEXT("PASS"));
			Flush();
			Phase(TEXT("done"));
			if (Target) { Target->RemoveFromRoot(); Target = nullptr; }
			FPlatformMisc::RequestExit(false);
		}

		bool Tick(float)
		{
			UWorld* World = GameWorld();
			const double Total = FPlatformTime::Seconds() - RunStart;
			if (Total > 90.0 && Step < 100)
			{
				Check(TEXT("TIMEOUT"), false, FString::Printf(TEXT("step %d did not finish within 90 s"), Step));
				Step = 100;
				Finish();
				return true;
			}
			if (!World || Step >= 100) return true;
			ASpoutSenderActor* S = Sender.Get();
			ASpoutReceiverActor* R = Receiver.Get();
			switch (Step)
			{
			case 0:
			{
				const ASpoutSenderActor* SD = GetDefault<ASpoutSenderActor>();
				const ASpoutReceiverActor* RD = GetDefault<ASpoutReceiverActor>();
				Check(TEXT("D1"), SD->SendFrom == ESpoutSendTextureFrom::GameViewportOnly && SD->bEnabled && RD->bAsyncReceive && RD->ColorSpace == ESpoutColorSpace::Auto
					&& RD->SenderName.IsNone() && RD->bEnabled && RD->bShowDisplay && RD->DisplayMaterial != nullptr,
					FString::Printf(TEXT("sender SendFrom %d enabled %d; receiver AsyncReceive %d ColorSpace %d SenderName '%s' display material %s; plugin build %s"),
						static_cast<int32>(SD->SendFrom), SD->bEnabled, RD->bAsyncReceive, static_cast<int32>(RD->ColorSpace), *RD->SenderName.ToString(),
						RD->DisplayMaterial ? *RD->DisplayMaterial->GetPathName() : TEXT("none"), *FSpoutModule::GetBuildSourceIdentity()));
				Target = NewObject<UTextureRenderTarget2D>();
				Target->AddToRoot();
				Target->RenderTargetFormat = RTF_RGBA8;
				Target->bAutoGenerateMips = false;
				Target->InitCustomFormat(64, 64, PF_B8G8R8A8, true);
				Target->UpdateResourceImmediate(true);
				FActorSpawnParameters Params;
				Sender = World->SpawnActor<ASpoutSenderActor>(ASpoutSenderActor::StaticClass(), FTransform::Identity, Params);
				Params.bDeferConstruction = true;
				ASpoutReceiverActor* NewReceiver = World->SpawnActor<ASpoutReceiverActor>(ASpoutReceiverActor::StaticClass(), FTransform(FVector(600, 0, 150)), Params);
				if (NewReceiver)
				{
					NewReceiver->SenderName = ExtName;
					NewReceiver->OutputRenderTarget = Target;
					NewReceiver->FinishSpawning(FTransform(FVector(600, 0, 150)));
				}
				Receiver = NewReceiver;
				Phase(TEXT("A"));
				Next();
				break;
			}
			case 1:
				if (S && R && S->bPublishing && R->State == ESpoutReceiverActorState::Receiving && R->ReceivedSize == ExtSize && Age() > 1.0)
				{
					Check(TEXT("A1"), true, FString::Printf(TEXT("sender '%s'; receiver %s"), *S->Status, *Describe(R)));
					FString Detail;
					Check(TEXT("A2"), PixelsMatch(Detail), Detail);
					Phase(bQuick ? TEXT("quick-hold") : TEXT("A-hold"));
					Next();
				}
				else if (Age() > 10.0)
				{
					Check(TEXT("A1"), false, FString::Printf(TEXT("after 10 s: sender %s / %s; receiver %s"), S ? (S->bPublishing ? TEXT("publishing") : TEXT("not publishing")) : TEXT("none"),
						S ? *S->Status : TEXT(""), *Describe(R)));
					Step = 100;
					Finish();
				}
				break;
			case 2: // the orchestrator receives the Game Viewport Only stream externally during this hold
				if (Age() > 4.0)
				{
					if (bQuick) { Step = 100; Finish(); break; }
					S->SetEnabled(false);
					R->SetEnabled(false);
					Phase(TEXT("B"));
					Next();
				}
				break;
			case 3:
				if (Age() > 1.5)
				{
					const bool bSenderGone = !USpoutBPFunctionLibrary::GetSpoutStreamStatus(S->SpoutName, ESpoutType::Sender).bActive;
					const bool bReceiverGone = !ReceiveStatus().bActive;
					Check(TEXT("B1"), bSenderGone && bReceiverGone && R->State == ESpoutReceiverActorState::Disabled,
						FString::Printf(TEXT("disabled: sender stream gone %d, receiver session gone %d, receiver %s"), bSenderGone, bReceiverGone, *Describe(R)));
					S->SetEnabled(true);
					R->SetEnabled(true);
					Next();
				}
				break;
			case 4:
				if (S->bPublishing && R->State == ESpoutReceiverActorState::Receiving)
				{
					Check(TEXT("B2"), true, FString::Printf(TEXT("re-enabled after %.2f s: %s"), Age(), *Describe(R)));
					Phase(TEXT("C"));
					Next();
				}
				else if (Age() > 4.0)
				{
					Check(TEXT("B2"), false, FString::Printf(TEXT("not back within 4 s: sender %d, %s"), S->bPublishing, *Describe(R)));
					Next();
				}
				break;
			case 5: // let the session count up, then close it the way a Blueprint does
				if (ReceiveStatus().FramesSubmitted >= 120 || Age() > 5.0)
				{
					SavedFrames = ReceiveStatus().FramesSubmitted;
					UTexture2D* Texture = nullptr;
					UMaterialInstanceDynamic* Material = nullptr;
					USpoutBPFunctionLibrary::CloseReceiver(ExtName, Texture, Material);
					Next();
				}
				break;
			case 6:
				if (R->State == ESpoutReceiverActorState::Receiving && HasSignal(R) == 1.f && ReceiveStatus().bActive && Age() > 0.05)
				{
					const int64 Now = ReceiveStatus().FramesSubmitted;
					Check(TEXT("C1"), Age() <= 2.0 && Now < SavedFrames, FString::Printf(TEXT("Receiving again after %.2f s; session frames %lld (before the close %lld)"), Age(), Now, SavedFrames));
					Phase(TEXT("F"));
					Next();
				}
				else if (Age() > 2.0)
				{
					Check(TEXT("C1"), false, FString::Printf(TEXT("not Receiving within 2 s of Close Receiver: %s; session frames %lld (before %lld)"), *Describe(R), ReceiveStatus().FramesSubmitted, SavedFrames));
					Phase(TEXT("F"));
					Next();
				}
				break;
			case 7:
			{
				IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.MaxConsumersPerReceiver"))->Set(1, ECVF_SetByCode);
				FActorSpawnParameters Params;
				Params.bDeferConstruction = true;
				ASpoutReceiverActor* F = World->SpawnActor<ASpoutReceiverActor>(ASpoutReceiverActor::StaticClass(), FTransform(FVector(600, 400, 150)), Params);
				if (F)
				{
					F->SenderName = ExtName;
					F->ColorSpace = ESpoutColorSpace::Linear;
					F->FinishSpawning(FTransform(FVector(600, 400, 150)));
				}
				Refused = F;
				Next();
				break;
			}
			case 8:
			{
				ASpoutReceiverActor* F = Refused.Get();
				if (F && F->State == ESpoutReceiverActorState::Refused && Age() > 1.0)
				{
					const int64 F1 = ReceiveStatus().FramesSubmitted;
					SavedFrames = F1;
					Check(TEXT("F1"), F->Status.Contains(TEXT("consumers")) && F->AppliedSettings.Contains(TEXT("Auto")) && R->State == ESpoutReceiverActorState::Receiving,
						FString::Printf(TEXT("refused actor %s; main receiver %s"), *Describe(F), *Describe(R)));
					IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.MaxConsumersPerReceiver"))->Set(32, ECVF_SetByCode);
					Next();
				}
				else if (Age() > 3.0)
				{
					Check(TEXT("F1"), false, FString::Printf(TEXT("not Refused within 3 s: %s"), *Describe(F)));
					IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.MaxConsumersPerReceiver"))->Set(32, ECVF_SetByCode);
					Next();
				}
				break;
			}
			case 9:
			{
				ASpoutReceiverActor* F = Refused.Get();
				if (F && F->State == ESpoutReceiverActorState::Receiving && F->Status.Contains(TEXT("Linear not applied")))
				{
					Check(TEXT("F2"), R->State == ESpoutReceiverActorState::Receiving && ReceiveStatus().FramesSubmitted > SavedFrames,
						FString::Printf(TEXT("joined after the limit was restored: %s; main %s"), *Describe(F), *Describe(R)));
					F->Destroy();
					Next();
				}
				else if (Age() > 4.0)
				{
					Check(TEXT("F2"), false, FString::Printf(TEXT("did not join within 4 s: %s"), *Describe(F)));
					if (F) F->Destroy();
					Next();
				}
				break;
			}
			case 10:
				if (Age() > 1.0)
				{
					const FSpoutStreamStatus Status = ReceiveStatus();
					Check(TEXT("F3"), Status.bActive && R->State == ESpoutReceiverActorState::Receiving,
						FString::Printf(TEXT("after the refused actor left: session active %d, main %s"), Status.bActive, *Describe(R)));
					Step = 100;
					Finish();
				}
				break;
			default:
				break;
			}
			return true;
		}
	}

	bool FActorsSmoke::IsRequested()
	{
		return FParse::Param(FCommandLine::Get(), TEXT("SpoutActorsSmoke"));
	}

	void FActorsSmoke::Start()
	{
		if (!IsRequested()) return;
		FString Ext = TEXT("ExtPattern"), Size;
		FParse::Value(FCommandLine::Get(), TEXT("SpoutActorsReport="), ReportPath);
		FParse::Value(FCommandLine::Get(), TEXT("SpoutActorsExt="), Ext);
		if (FParse::Value(FCommandLine::Get(), TEXT("SpoutActorsExtSize="), Size))
		{
			FString W, H;
			if (Size.Split(TEXT("x"), &W, &H)) ExtSize = FIntPoint(FCString::Atoi(*W), FCString::Atoi(*H));
		}
		bQuick = FParse::Param(FCommandLine::Get(), TEXT("SpoutActorsQuick"));
		ExtName = FName(*Ext);
		RunStart = FPlatformTime::Seconds();
		Lines = FString::Printf(TEXT("START engine %s quick %d ext %s %dx%d\n"), *FEngineVersion::Current().ToString(), bQuick, *Ext, ExtSize.X, ExtSize.Y);
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.0f);
	}

	void FActorsSmoke::Stop()
	{
		if (TickHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
			TickHandle.Reset();
			Lines += TEXT("SHUTDOWN\n");
			Flush();
		}
	}
}
