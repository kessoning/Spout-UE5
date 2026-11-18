// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#include "SpoutSenderActor.h"

#include "Components/BillboardComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "SpoutActorSupport.h"
#include "SpoutModule.h"
#include "SpoutSender.h"
#include "SpoutSettings.h"
#include "UObject/ConstructorHelpers.h"

#if WITH_EDITOR
#include "Logging/MessageLog.h"
#include "Misc/UObjectToken.h"
#endif

#define LOCTEXT_NAMESPACE "SpoutSenderActor"

namespace
{
	// How long a name this actor just closed may still be listed by Spout (unregistration completes on the render
	// thread), during which it is not mistaken for another application's sender.
	constexpr double RecentlyClosedGraceSeconds = 2.0;
	// How often a refused ownership check is retried.
	constexpr double OwnershipRetrySeconds = 1.0;
	// How long an accepted request may go without a published frame before Status says so.
	constexpr double NoFrameNoticeSeconds = 2.0;
}

ASpoutSenderActor::ASpoutSenderActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	// After the frame's game work, like the shipped example Blueprints.
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;
#if WITH_EDITORONLY_DATA
	Sprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
	if (Sprite)
	{
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> SpriteTexture(TEXT("/Engine/EditorResources/S_Emitter"));
		Sprite->Sprite = SpriteTexture.Get();
		Sprite->SetupAttachment(SceneRoot);
		Sprite->bIsScreenSizeScaled = true;
	}
#endif
}

void ASpoutSenderActor::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() == NM_DedicatedServer)
	{
		Status = TEXT("Not sending: dedicated server");
		SetActorTickEnabled(false);
		return;
	}
	Status = bEnabled ? TEXT("Starting") : TEXT("Disabled");
}

void ASpoutSenderActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopSending(TEXT("Stopped"));
	Super::EndPlay(EndPlayReason);
}

void ASpoutSenderActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateSending();
}

bool ASpoutSenderActor::ShouldTickIfViewportsOnly() const
{
	// In the level editor: while Send In Editor is on, and until a stream it still holds is closed.
	return bSendInEditor || bClaimed;
}

void ASpoutSenderActor::Destroyed()
{
	// The level editor's actors get no EndPlay: deleting one must close its stream too.
	StopSending(TEXT("Stopped"));
	Super::Destroyed();
}

bool ASpoutSenderActor::MaySendInEditor(FString& Reason) const
{
	if (!bSendInEditor)
	{
		Reason = TEXT("Not sending in the editor: Send In Editor is off");
		return false;
	}
	if (USpoutSettings::Get().StartupPolicy != ESpoutStartupPolicy::EditorAndPlay)
	{
		Reason = TEXT("Not sending in the editor: Project Settings > Plugins > Spout > Startup Policy is Play only");
		return false;
	}
	return true;
}

void ASpoutSenderActor::YieldToPlay()
{
	StopSending(TEXT("Paused while Play runs: Play's copy of this sender sends the name"));
	bYieldedToPlay = true;
}

void ASpoutSenderActor::SetEnabled(bool bNewEnabled)
{
	bEnabled = bNewEnabled;
	if (HasActorBegunPlay()) UpdateSending();
}

void ASpoutSenderActor::SetSpoutName(FName NewName)
{
	SpoutName = NewName;
	if (HasActorBegunPlay()) UpdateSending();
}

void ASpoutSenderActor::SetSource(ESpoutSendTextureFrom NewSendFrom, UTextureRenderTarget2D* NewTextureRenderTarget)
{
	SendFrom = NewSendFrom;
	TextureRenderTarget = NewTextureRenderTarget;
	if (HasActorBegunPlay()) UpdateSending();
}

void ASpoutSenderActor::SetProblem(const FString& NewStatus)
{
	bPublishing = false;
	Status = NewStatus;
	// Actionable problems are logged once when they appear, never per frame.
	if (LastProblem != NewStatus)
	{
		LastProblem = NewStatus;
		UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout Sender actor '%s': %s"), *GetActorNameOrLabel(), *NewStatus);
	}
}

void ASpoutSenderActor::StopSending(const TCHAR* Reason)
{
	if (bClaimed)
	{
		// Only a name this actor claimed and checked is ever closed by it.
		USpoutBPFunctionLibrary::CloseSender(AppliedName);
		SpoutActorSupport::ReleaseSenderName(AppliedName, this);
		RecentlyClosedName = AppliedName;
		RecentlyClosedTime = FPlatformTime::Seconds();
		bClaimed = false;
	}
	bPublishing = false;
	AppliedName = NAME_None;
	AppliedTarget.Reset();
	LastFrames = 0;
	Status = Reason;
}

void ASpoutSenderActor::UpdateSending()
{
	UWorld* World = GetWorld();
	const bool bInEditor = SpoutActorSupport::IsInLevelEditorWorld(this);
	if (!World || (!World->IsGameWorld() && !bInEditor) || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	if (!bEnabled)
	{
		if (bClaimed || Status != TEXT("Disabled")) StopSending(TEXT("Disabled"));
		LastProblem.Reset();
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (bInEditor)
	{
		FString Reason;
		if (!MaySendInEditor(Reason))
		{
			if (bClaimed || Status != Reason) StopSending(*Reason);
			bYieldedToPlay = false;
			return;
		}
		if (bYieldedToPlay)
		{
			if (SpoutActorSupport::IsPlaySessionRunning())
			{
				return;
			}
			// Play has ended and closed the name. It may still be listed for a moment, while its unregistration
			// completes on the render thread: treated as just closed by this actor, not as another sender.
			bYieldedToPlay = false;
			RecentlyClosedName = SpoutName;
			RecentlyClosedTime = Now;
			NextOwnershipCheckTime = 0.0;
		}
	}
	UTextureRenderTarget2D* Target = SendFrom == ESpoutSendTextureFrom::TextureRenderTarget2D ? TextureRenderTarget.Get() : nullptr;

	// A different name releases the old one first; a different source under the same name restarts the stream so two
	// sources never feed one name.
	if (bClaimed && AppliedName != SpoutName)
	{
		StopSending(TEXT("Renaming"));
	}
	else if (bClaimed && (AppliedSendFrom != SendFrom || AppliedTarget.Get() != Target))
	{
		USpoutBPFunctionLibrary::CloseSender(AppliedName);
		AppliedSendFrom = SendFrom;
		AppliedTarget = Target;
		LastFrames = 0;
		LastFrameAdvanceTime = Now;
	}

	if (SendFrom == ESpoutSendTextureFrom::TextureRenderTarget2D && !Target)
	{
		if (bClaimed) StopSending(TEXT("Stopped"));
		SetProblem(TEXT("Not sending: Send From is Texture Render Target 2D but no Texture Render Target is set."));
		return;
	}
	if (SpoutName.IsNone() || SpoutName.ToString().TrimStartAndEnd().IsEmpty())
	{
		SetProblem(TEXT("Not sending: Spout Name is empty."));
		return;
	}

	if (!bClaimed)
	{
		if (Now < NextOwnershipCheckTime) return;
		NextOwnershipCheckTime = Now + OwnershipRetrySeconds;
		const AActor* Holder = nullptr;
		bool bTakenFromEditor = false;
		if (!SpoutActorSupport::ClaimSenderName(SpoutName, this, Holder))
		{
			// While Play runs, Play's sender takes a name the level editor's sender holds; that one resumes after Play.
			ASpoutSenderActor* EditorHolder = const_cast<ASpoutSenderActor*>(Cast<ASpoutSenderActor>(Holder));
			if (!bInEditor && EditorHolder && SpoutActorSupport::IsInLevelEditorWorld(EditorHolder))
			{
				EditorHolder->YieldToPlay();
				bTakenFromEditor = SpoutActorSupport::ClaimSenderName(SpoutName, this, Holder);
			}
			else if (bInEditor && Holder && !SpoutActorSupport::IsInLevelEditorWorld(Holder))
			{
				// The other way round: Play already sends this name; this one waits for Play to end.
				YieldToPlay();
				return;
			}
			if (!bTakenFromEditor)
			{
				SetProblem(FString::Printf(TEXT("Not sending: the name '%s' is already used by the Spout Sender actor '%s'."), *SpoutName.ToString(), Holder ? *Holder->GetActorNameOrLabel() : TEXT("?")));
				return;
			}
		}
		// A name just closed by this actor, or by the level editor's sender that handed it over, may still be listed
		// while its unregistration completes: it is not another sender.
		const bool bJustClosedByUs = bTakenFromEditor || (SpoutName == RecentlyClosedName && Now - RecentlyClosedTime < RecentlyClosedGraceSeconds);
		if (!bJustClosedByUs)
		{
			if (SpoutActorSupport::IsPublishedByThisProcess(SpoutName))
			{
				SpoutActorSupport::ReleaseSenderName(SpoutName, this);
				SetProblem(FString::Printf(TEXT("Not sending: the name '%s' is already published in this game by a Blueprint or C++ sender."), *SpoutName.ToString()));
				return;
			}
			if (SpoutActorSupport::IsAdvertised(SpoutName))
			{
				SpoutActorSupport::ReleaseSenderName(SpoutName, this);
				SetProblem(FString::Printf(TEXT("Not sending: another application already publishes a Spout sender named '%s'."), *SpoutName.ToString()));
				return;
			}
		}
		bClaimed = true;
		AppliedName = SpoutName;
		AppliedSendFrom = SendFrom;
		AppliedTarget = Target;
		LastFrames = 0;
		LastFrameAdvanceTime = Now;
		LastProblem.Reset();
	}

	// In the level editor the viewport sources send the level editor's viewport or window. The stream belongs to this
	// actor's world: the end of Play closes Play's senders and leaves the level editor's running.
	const bool bAccepted = FSpoutSender::Send(AppliedName, AppliedSendFrom, Target,
		bInEditor ? ESpoutViewportTarget::LevelEditor : ESpoutViewportTarget::Game, this);
	if (!bAccepted)
	{
		bPublishing = false;
		if (AppliedSendFrom == ESpoutSendTextureFrom::TextureRenderTarget2D)
		{
			SetProblem(TEXT("Not sending: the Texture Render Target was refused (size, format or mips; see the Output Log)."));
		}
		else
		{
			// Ordinary and quiet: for example the first frames of Play, before the game viewport exists.
			Status = bInEditor ? TEXT("Waiting for a level viewport") : TEXT("Waiting for the game viewport");
		}
		return;
	}
	const int64 Frames = USpoutBPFunctionLibrary::GetSpoutStreamStatus(AppliedName, ESpoutType::Sender).FramesSubmitted;
	if (Frames > LastFrames)
	{
		LastFrames = Frames;
		LastFrameAdvanceTime = Now;
		bPublishing = true;
		LastProblem.Reset();
		Status = FString::Printf(TEXT("Sending '%s'"), *AppliedName.ToString());
		bool bRealtime = true;
		if (bInEditor && AppliedSendFrom != ESpoutSendTextureFrom::TextureRenderTarget2D && FSpoutModule::GetLevelViewport(bRealtime).IsValid() && !bRealtime)
		{
			Status += TEXT(" (the level viewport is not Realtime: the image changes only when the viewport redraws)");
		}
	}
	else if (Now - LastFrameAdvanceTime > NoFrameNoticeSeconds)
	{
		bPublishing = false;
		Status = FString::Printf(TEXT("Requested, but no frame published for %.0f s (see the Output Log)"), Now - LastFrameAdvanceTime);
	}
	else if (!bPublishing)
	{
		Status = TEXT("Request accepted; waiting for the first published frame");
	}
}

#if WITH_EDITOR
void ASpoutSenderActor::CheckForErrors()
{
	Super::CheckForErrors();
	if (SendFrom == ESpoutSendTextureFrom::TextureRenderTarget2D && !TextureRenderTarget)
	{
		FMessageLog("MapCheck").Error()
			->AddToken(FUObjectToken::Create(this))
			->AddToken(FTextToken::Create(LOCTEXT("MissingTarget", "Send From is Texture Render Target 2D but no Texture Render Target is set: this Spout Sender will not send.")));
	}
	if (SpoutName.IsNone())
	{
		FMessageLog("MapCheck").Error()
			->AddToken(FUObjectToken::Create(this))
			->AddToken(FTextToken::Create(LOCTEXT("EmptyName", "Spout Name is empty: this Spout Sender will not send.")));
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ASpoutSenderActor> It(World); It; ++It)
		{
			if (*It != this && It->SpoutName == SpoutName && GetName() < It->GetName())
			{
				FMessageLog("MapCheck").Warning()
					->AddToken(FUObjectToken::Create(this))
					->AddToken(FTextToken::Create(FText::Format(LOCTEXT("DuplicateName", "Another Spout Sender uses the name '{0}'; only the first to start will send."), FText::FromName(SpoutName))));
			}
		}
	}
}

void ASpoutSenderActor::MakeNameUniqueInEditor(bool bNewlyPlaced)
{
	UWorld* World = GetWorld();
	if (!World || World->IsGameWorld()) return;
	// The project's default name (Project Settings > Plugins > Spout) is the base of every name made here.
	const FString Base = USpoutSettings::Get().GetDefaultSenderName();
	if (bNewlyPlaced && SpoutName == GetDefault<ASpoutSenderActor>()->SpoutName)
	{
		SpoutName = FName(*Base);
	}
	TSet<FName> Used;
	for (TActorIterator<ASpoutSenderActor> It(World); It; ++It)
	{
		if (*It != this) Used.Add(It->SpoutName);
	}
	if (!SpoutName.IsNone() && !Used.Contains(SpoutName)) return;
	FName Candidate(*Base);
	for (int32 Index = 2; Used.Contains(Candidate); ++Index)
	{
		Candidate = FName(*FString::Printf(TEXT("%s %d"), *Base, Index));
	}
	SpoutName = Candidate;
}

void ASpoutSenderActor::PostActorCreated()
{
	Super::PostActorCreated();
	MakeNameUniqueInEditor(true);
}

void ASpoutSenderActor::PostEditImport()
{
	Super::PostEditImport();
	MakeNameUniqueInEditor(false);
}

void ASpoutSenderActor::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);
	if (!bDuplicateForPIE) MakeNameUniqueInEditor(false);
}
#endif

#undef LOCTEXT_NAMESPACE
