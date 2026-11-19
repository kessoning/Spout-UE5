// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#include "SpoutReceiverActor.h"

#include "Components/ArrowComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "SpoutActorSupport.h"
#include "SpoutModule.h"
#include "SpoutReceiver.h"
#include "SpoutSettings.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// The engine's 100 x 100 cm Plane (/Engine/BasicShapes/Plane) lies in its local XY plane facing +Z, with U along local
	// +X and V along local +Y (read from its mesh description: (50,50) has UV (1,1), (-50,-50) has UV (0,0)). Seen from
	// the actor's front (a viewer at +X looking back along -X, whose right is -Y), the image's right (+U) must run along
	// world -Y and its downward direction (+V) along world -Z; the face then points along +X.
	FRotator MakeDisplayRotation()
	{
		return FRotationMatrix::MakeFromXY(FVector(0.0, -1.0, 0.0), FVector(0.0, 0.0, -1.0)).Rotator();
	}
	constexpr float PlaneSize = 100.f;
	// Scale components for (width, height): local X carries U, so it is the image's width.
	constexpr bool bWidthIsLocalX = true;

	constexpr double DiscoveryIntervalSeconds = 0.5;   // empty-name discovery while nothing is connected
	constexpr double RefusedRetrySeconds = 2.0;        // a refused configuration is retried this often, not per frame
	constexpr double StalledNoticeSeconds = 1.0;       // no new frame for this long while accepted -> say so

	const FName HasSignalParameter(TEXT("HasSignal"));
	const FName BrightnessParameter(TEXT("Brightness"));

	const TCHAR* ColorSpaceName(ESpoutColorSpace Value)
	{
		switch (Value)
		{
		case ESpoutColorSpace::SRGB: return TEXT("sRGB");
		case ESpoutColorSpace::Linear: return TEXT("Linear");
		default: return TEXT("Auto");
		}
	}

	/** The receive path this stream actually uses, which differs from the request when Async Receive cannot take the sender. */
	FString DescribeReceiveMode(bool bAsyncRequested, const FSpoutStreamStatus& StreamStatus)
	{
		if (!bAsyncRequested)
		{
			return TEXT("Async Receive off");
		}
		if (StreamStatus.EffectiveReceiveMode == ESpoutReceiveMode::AsyncBeta)
		{
			return TEXT("Async Receive on");
		}
		if (!StreamStatus.ReceiveModeNote.IsEmpty())
		{
			return FString::Printf(TEXT("synchronous receive (%s)"), *StreamStatus.ReceiveModeNote);
		}
		return TEXT("Async Receive starting");
	}

	FString RequiredTargetFormat(EPixelFormat Format)
	{
		switch (Format)
		{
		case PF_B8G8R8A8: return TEXT("RTF RGBA8 (8-bit, BGRA order)");
		case PF_R8G8B8A8: return TEXT("RTF RGBA8 (8-bit, RGBA order)");
		case PF_A2B10G10R10: return TEXT("RTF RGB10A2");
		case PF_FloatRGBA: return TEXT("RTF RGBA16f");
		default: return GetPixelFormatString(Format);
		}
	}
}

ASpoutReceiverActor::ASpoutReceiverActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	Display = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Display"));
	Display->SetupAttachment(SceneRoot);
	Display->SetRelativeRotation(MakeDisplayRotation());
	Display->CastShadow = false;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneMesh(TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (PlaneMesh.Succeeded()) Display->SetStaticMesh(PlaneMesh.Object);
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> DisplayMaterialAsset(TEXT("/SpoutPlugin/Content/M_SpoutDisplayUnlit.M_SpoutDisplayUnlit"));
	if (DisplayMaterialAsset.Succeeded()) DisplayMaterial = DisplayMaterialAsset.Object;

#if WITH_EDITORONLY_DATA
	Arrow = CreateEditorOnlyDefaultSubobject<UArrowComponent>(TEXT("Arrow"));
	if (Arrow)
	{
		Arrow->SetupAttachment(SceneRoot);
		Arrow->ArrowColor = FColor(0, 112, 224);
		Arrow->ArrowSize = 1.5f;
	}
#endif
}

void ASpoutReceiverActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	// Editor and spawn: the screen shows the material's No-Signal color until Play delivers frames.
	if (Display && !DisplayMID)
	{
		Display->SetMaterial(0, DisplayMaterial);
	}
	ApplyDisplay();
}

void ASpoutReceiverActor::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() == NM_DedicatedServer)
	{
		State = ESpoutReceiverActorState::Disabled;
		Status = TEXT("Not receiving: dedicated server");
		SetActorTickEnabled(false);
		return;
	}
	EnsureMaterialInstance();
	SetSignal(false);
	ApplyDisplay();
}

void ASpoutReceiverActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Release(false);
	Super::EndPlay(EndPlayReason);
}

void ASpoutReceiverActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateReceiving();
}

bool ASpoutReceiverActor::ShouldTickIfViewportsOnly() const
{
	// In the level editor: while Receive In Editor is on, and until a stream it still uses is released.
	return bReceiveInEditor || bConnected;
}

void ASpoutReceiverActor::Destroyed()
{
	// The level editor's actors get no EndPlay: deleting one must release its stream too.
	Release(false);
	Super::Destroyed();
}

bool ASpoutReceiverActor::MayReceiveInEditor(FString& Reason) const
{
	if (!bReceiveInEditor)
	{
		Reason = TEXT("Not receiving in the editor: Receive In Editor is off");
		return false;
	}
	if (USpoutSettings::Get().StartupPolicy != ESpoutStartupPolicy::EditorAndPlay)
	{
		Reason = TEXT("Not receiving in the editor: Project Settings > Plugins > Spout > Startup Policy is Play only");
		return false;
	}
	return true;
}

void ASpoutReceiverActor::SetEnabled(bool bNewEnabled)
{
	bEnabled = bNewEnabled;
	if (HasActorBegunPlay()) UpdateReceiving();
}

void ASpoutReceiverActor::SetSenderName(FName NewSenderName)
{
	SenderName = NewSenderName;
	if (HasActorBegunPlay()) UpdateReceiving();
}

void ASpoutReceiverActor::SetOutputRenderTarget(UTextureRenderTarget2D* NewOutputRenderTarget)
{
	OutputRenderTarget = NewOutputRenderTarget;
	if (HasActorBegunPlay()) UpdateReceiving();
}

void ASpoutReceiverActor::SetState(ESpoutReceiverActorState NewState, const FString& NewStatus, bool bActionable)
{
	State = NewState;
	Status = NewStatus;
	if (bActionable)
	{
		// Configuration problems are logged once when they appear; ordinary waiting is never logged.
		if (LastActionable != NewStatus)
		{
			LastActionable = NewStatus;
			UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout Receiver actor '%s': %s"), *GetActorNameOrLabel(), *NewStatus);
		}
	}
	else if (NewState == ESpoutReceiverActorState::Receiving)
	{
		LastActionable.Reset();
	}
}

void ASpoutReceiverActor::SetSignal(bool bHasSignal)
{
	bSignal = bHasSignal;
	if (DisplayMID) DisplayMID->SetScalarParameterValue(HasSignalParameter, bHasSignal ? 1.f : 0.f);
}

void ASpoutReceiverActor::EnsureMaterialInstance()
{
	const FName Parameter = TextureParameterName.IsNone() ? FName(TEXT("SpoutTexture")) : TextureParameterName;
	if (DisplayMID && MIDSource.Get() == DisplayMaterial && MIDParameter == Parameter) return;
	if (!DisplayMID && !DisplayMaterial) return;
	// The material or its parameter changed: release only this actor's binding of the old instance (the session and
	// any other consumer stay), then create one new instance. Never per frame.
	if (DisplayMID && bConnected && !ConnectedSenderName.IsNone())
	{
		USpoutBPFunctionLibrary::ReleaseReceiverMaterialBinding(ConnectedSenderName, DisplayMID, MIDParameter);
	}
	DisplayMID = DisplayMaterial ? UMaterialInstanceDynamic::Create(DisplayMaterial, this) : nullptr;
	if (DisplayMID)
	{
		// Made while receiving, also in the level editor, and never saved with the level.
		DisplayMID->SetFlags(RF_Transient);
	}
	MIDSource = DisplayMaterial;
	MIDParameter = Parameter;
	if (Display) Display->SetMaterial(0, DisplayMID ? static_cast<UMaterialInterface*>(DisplayMID) : DisplayMaterial.Get());
	if (DisplayMID)
	{
		DisplayMID->SetScalarParameterValue(BrightnessParameter, Brightness);
		DisplayMID->SetScalarParameterValue(HasSignalParameter, bSignal ? 1.f : 0.f);
	}
}

void ASpoutReceiverActor::ApplyDisplay()
{
	if (!Display) return;
	Display->SetVisibility(bShowDisplay);
	float Aspect = 16.f / 9.f;
	if (!bKeepAspectRatio) Aspect = 1.f;
	else if (ReceivedSize.X > 0 && ReceivedSize.Y > 0) Aspect = static_cast<float>(ReceivedSize.X) / static_cast<float>(ReceivedSize.Y);
	const float Height = FMath::Max(DisplayHeight, 1.f) / PlaneSize;
	const float Width = Height * Aspect;
	const FVector Scale = bWidthIsLocalX ? FVector(Width, Height, 1.f) : FVector(Height, Width, 1.f);
	if (!Display->GetRelativeScale3D().Equals(Scale, 1e-4f)) Display->SetRelativeScale3D(Scale);
	if (DisplayMID)
	{
		float Current = 0.f;
		if (!DisplayMID->GetScalarParameterValue(BrightnessParameter, Current) || Current != Brightness)
		{
			DisplayMID->SetScalarParameterValue(BrightnessParameter, Brightness);
		}
	}
}

void ASpoutReceiverActor::Release(bool bKeepConnectedName)
{
	if (bConnected && !ConnectedSenderName.IsNone())
	{
		// Releases only this actor's binding and output-target use; the session closes only if nobody else uses it.
		FSpoutReceiver::ReleaseManagedConsumer(ConnectedSenderName, this, DisplayMID, MIDParameter, AppliedOutputTarget.Get());
	}
	bConnected = false;
	AppliedOutputTarget.Reset();
	ReceivedTexture.Reset();
	LastFrames = 0;
	AppliedSessionId = 0;
	bSessionHasFrame = false;
	if (!bKeepConnectedName)
	{
		ConnectedSenderName = NAME_None;
		RequestedSenderName = NAME_None;
		bAutoChosen = false;
		bEverReceived = false;
		ReceivedSize = FIntPoint::ZeroValue;
	}
	SetSignal(false);
}

void ASpoutReceiverActor::UpdateReceiving()
{
#if WITH_EDITOR
	if (bShowingEditorPause)
	{
		bShowingEditorPause = false;
		Status = StatusBeforeEditorPause;
	}
#endif
	UWorld* World = GetWorld();
	const bool bInEditor = SpoutActorSupport::IsInLevelEditorWorld(this);
	if (!World || (!World->IsGameWorld() && !bInEditor) || GetNetMode() == NM_DedicatedServer) return;
	const double Now = FPlatformTime::Seconds();
	if (bInEditor)
	{
		FString Reason;
		if (!MayReceiveInEditor(Reason))
		{
			if (bConnected || State != ESpoutReceiverActorState::Disabled || Status != Reason)
			{
				Release(false);
				SetState(ESpoutReceiverActorState::Disabled, Reason);
			}
			return;
		}
	}

	if (!bEnabled)
	{
		if (bConnected || State != ESpoutReceiverActorState::Disabled) Release(false);
		SetState(ESpoutReceiverActorState::Disabled, TEXT("Disabled"));
		return;
	}
	EnsureMaterialInstance();
	ApplyDisplay();

	// 1. Which sender. An explicit name is used as typed (this game's own senders included). An empty name picks a
	//    sender of another application once; that resolved name is then kept, through disconnections, until Sender
	//    Name changes or Play ends.
	if (!ConnectedSenderName.IsNone() && SenderName != RequestedSenderName)
	{
		Release(false);
	}
	if (ConnectedSenderName.IsNone())
	{
		if (!SenderName.IsNone())
		{
			ConnectedSenderName = SenderName;
			RequestedSenderName = SenderName;
			bAutoChosen = false;
		}
		else
		{
			if (Now < NextDiscoveryTime) return;
			NextDiscoveryTime = Now + DiscoveryIntervalSeconds;
			FName Chosen;
			if (!SpoutActorSupport::ChooseExternalSender(Chosen))
			{
				SetState(ESpoutReceiverActorState::Waiting, TEXT("Waiting: Sender Name is empty and no sender from another application is running."));
				return;
			}
			ConnectedSenderName = Chosen;
			RequestedSenderName = NAME_None;
			bAutoChosen = true;
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout Receiver actor '%s': Sender Name is empty; connecting to '%s'."), *GetActorNameOrLabel(), *Chosen.ToString());
		}
	}
	const FName Name = ConnectedSenderName;

	// 2. Output target replacement: release only this actor's use of the old target.
	UTextureRenderTarget2D* DesiredTarget = OutputRenderTarget;
	if (bConnected && AppliedOutputTarget.Get() != DesiredTarget && AppliedOutputTarget.IsValid())
	{
		FSpoutReceiver::ReleaseManagedOutputTarget(Name, this, AppliedOutputTarget.Get());
		AppliedOutputTarget.Reset();
	}

	// 3. Session-wide settings. Shared with other consumers: join with the settings in effect and say so. Used by this
	//    actor alone: a changed setting reopens the session (a frame or two of No-Signal).
	ESpoutColorSpace UseColorSpace = ColorSpace;
	bool bUseAsync = bAsyncReceive;
	FString SettingsNote;
	{
		// Membership of THIS session, not a count: an actor whose attempts were refused holds no lease, and the
		// consumers counted may all be other actors.
		const FSpoutReceiverSessionState Info = FSpoutReceiver::GetSessionState(Name, this);
		const bool bHeldByMe = Info.bIsManagedConsumer;
		const int32 Others = Info.ManagedConsumers - (bHeldByMe ? 1 : 0);
		const bool bShared = Info.bExists && (Others > 0 || Info.bUnmanagedConsumer);
		const bool bModeDiffers = Info.bReceiveModeEstablished && Info.bAsyncReceive != bAsyncReceive;
		const bool bColorDiffers = Info.bColorSpaceEstablished && Info.ColorSpace != ColorSpace;
		if (bShared)
		{
			if (bModeDiffers)
			{
				bUseAsync = Info.bAsyncReceive;
				SettingsNote += FString::Printf(TEXT(" Async Receive %s not applied: the stream is shared and uses %s."), bAsyncReceive ? TEXT("on") : TEXT("off"), Info.bAsyncReceive ? TEXT("on") : TEXT("off"));
			}
			if (bColorDiffers)
			{
				UseColorSpace = Info.ColorSpace;
				SettingsNote += FString::Printf(TEXT(" Color Space %s not applied: the stream is shared and uses %s."), ColorSpaceName(ColorSpace), ColorSpaceName(Info.ColorSpace));
			}
		}
		else if (bHeldByMe && (bModeDiffers || bColorDiffers))
		{
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout Receiver actor '%s': reopening '%s' to apply Color Space %s, Async Receive %s."),
				*GetActorNameOrLabel(), *Name.ToString(), ColorSpaceName(ColorSpace), bAsyncReceive ? TEXT("on") : TEXT("off"));
			Release(true);
		}
	}

	// 4. Output target: pass it only when it fits the stream, so a mismatch is reported once here instead of warned
	//    about on every receive.
	UTextureRenderTarget2D* PassTarget = nullptr;
	FString OutputNote;
	if (DesiredTarget)
	{
		const UTexture2D* Texture = ReceivedTexture.Get();
		if (Texture)
		{
			const EPixelFormat Needed = Texture->GetPixelFormat();
			if (DesiredTarget->GetFormat() == Needed && !DesiredTarget->bAutoGenerateMips)
			{
				PassTarget = DesiredTarget;
				LastOutputWarning.Reset();
			}
			else
			{
				OutputNote = FString::Printf(TEXT(" Output Render Target not used: it needs format %s%s."), *RequiredTargetFormat(Needed),
					DesiredTarget->bAutoGenerateMips ? TEXT(" and Auto Generate Mips off") : TEXT(""));
				if (LastOutputWarning != OutputNote)
				{
					LastOutputWarning = OutputNote;
					UE_LOG(LogSpoutPlugin, Warning, TEXT("Spout Receiver actor '%s':%s"), *GetActorNameOrLabel(), *OutputNote);
				}
			}
		}
	}

	// 5. Receive. A refused configuration is retried every couple of seconds rather than every frame.
	if (State == ESpoutReceiverActorState::Refused && Now < NextRefusedRetryTime) return;
	bConnected = true; // from the first attempt on, Release() runs for this name (idempotent, also after a refusal)
	UMaterialInstanceDynamic* Material = DisplayMID;
	UTexture2D* OutTexture = nullptr;
	ESpoutReceiveResult Result = ESpoutReceiveResult::Accepted;
	const bool bAccepted = FSpoutReceiver::Receive(Name, nullptr, MIDParameter, Material, OutTexture, PassTarget, UseColorSpace, bUseAsync, this, &Result);
	AppliedSettings = FString::Printf(TEXT("Color Space %s, Async Receive %s"), ColorSpaceName(UseColorSpace), bUseAsync ? TEXT("on") : TEXT("off"));

	if (bAccepted)
	{
		AppliedColorSpace = UseColorSpace;
		bAppliedAsync = bUseAsync;
		if (PassTarget) AppliedOutputTarget = PassTarget;
		ReceivedTexture = OutTexture;
		if (OutTexture)
		{
			const FIntPoint Size(OutTexture->GetSizeX(), OutTexture->GetSizeY());
			if (Size != ReceivedSize)
			{
				ReceivedSize = Size;
				ApplyDisplay();
			}
		}
		// The frame count is per session. A session closed elsewhere (a Blueprint Close Receiver) and reopened by this
		// receive starts counting again, and its texture starts empty: take a new baseline and show No-Signal until the
		// reopened session's first frame.
		const uint64 SessionId = FSpoutReceiver::GetSessionState(Name, this).SessionId;
		if (SessionId != AppliedSessionId)
		{
			if (AppliedSessionId != 0) SetSignal(false);
			AppliedSessionId = SessionId;
			LastFrames = 0;
			bSessionHasFrame = false;
		}
		const FSpoutStreamStatus StreamStatus = USpoutBPFunctionLibrary::GetSpoutStreamStatus(Name, ESpoutType::Receiver);
		AppliedSettings = FString::Printf(TEXT("Color Space %s, %s"), ColorSpaceName(UseColorSpace), *DescribeReceiveMode(bUseAsync, StreamStatus));
		const int64 Frames = StreamStatus.FramesSubmitted;
		if (Frames > LastFrames)
		{
			// A frame really arrived (the session's received-frame count advanced), not merely a request accepted.
			LastFrames = Frames;
			LastFrameTime = Now;
			bEverReceived = true;
			bSessionHasFrame = true;
			SetSignal(true);
			FString Text = FString::Printf(TEXT("Receiving '%s' (%dx%d)."), *Name.ToString(), ReceivedSize.X, ReceivedSize.Y);
			if (UseColorSpace == ESpoutColorSpace::Auto && OutTexture && OutTexture->GetPixelFormat() == PF_A2B10G10R10)
			{
				Text += TEXT(" 10-bit stream read as linear (Auto); if it comes from an Unreal viewport, choose Color Space sRGB.");
			}
			bool bRealtime = true;
			if (bInEditor && FSpoutModule::GetLevelViewport(bRealtime).IsValid() && !bRealtime)
			{
				Text += TEXT(" The level viewport is not Realtime: the screen changes only when the viewport redraws.");
			}
			SetState(ESpoutReceiverActorState::Receiving, Text + SettingsNote + OutputNote);
		}
		else if (!bSessionHasFrame)
		{
			const FString Waiting = bEverReceived
				? FString::Printf(TEXT("Reconnected to '%s' (the stream was reopened); waiting for its first frame."), *Name.ToString())
				: FString::Printf(TEXT("Request accepted for '%s'; waiting for the first frame."), *Name.ToString());
			SetState(ESpoutReceiverActorState::RequestAccepted, Waiting + SettingsNote + OutputNote);
		}
		else if (Now - LastFrameTime > StalledNoticeSeconds)
		{
			SetState(ESpoutReceiverActorState::RequestAccepted, FString::Printf(TEXT("Connected to '%s', but no new frame for %.0f s (the sender may be paused)."), *Name.ToString(), Now - LastFrameTime) + SettingsNote + OutputNote);
		}
		return;
	}

	switch (Result)
	{
	case ESpoutReceiveResult::NoSender:
		if (bAutoChosen && !bEverReceived)
		{
			// The automatically chosen sender went away before anything arrived: choose again.
			Release(false);
			SetState(ESpoutReceiverActorState::Waiting, TEXT("Waiting: the sender chosen automatically is no longer running."));
			return;
		}
		if (bEverReceived)
		{
			if (WhenSenderLost == ESpoutLostSenderDisplay::ShowNoSignal) SetSignal(false);
			SetState(ESpoutReceiverActorState::Waiting, FString::Printf(TEXT("Sender '%s' lost; waiting for it to return."), *Name.ToString()));
		}
		else
		{
			SetState(ESpoutReceiverActorState::Waiting, FString::Printf(TEXT("Waiting for sender '%s'."), *Name.ToString()));
		}
		return;
	case ESpoutReceiveResult::Busy:
		return; // transient admission limit; quietly retried
	case ESpoutReceiveResult::StreamQueueFull:
		SetState(ESpoutReceiverActorState::Refused, TEXT("Refused: this sender already has as many consumers this frame as r.Spout.MaxPendingPerStream allows (4 by default); raise it in Project Settings > Plugins > Spout."), true);
		break;
	case ESpoutReceiveResult::OtherGpu:
		// Not logged here: the receive itself logs one warning naming both graphics cards.
		SetState(ESpoutReceiverActorState::Refused, TEXT("Refused: the sender's texture is on another graphics card than Unreal's, and Spout shares textures on one GPU only. Run the sending application on the same GPU (see the Output Log)."));
		break;
	case ESpoutReceiveResult::Unavailable:
		SetState(ESpoutReceiverActorState::Waiting, TEXT("Spout is unavailable: the plugin needs Direct3D 12 on Windows (see the Output Log)."), true);
		return;
	case ESpoutReceiveResult::ColorSpaceUnsupported:
		SetState(ESpoutReceiverActorState::Refused, FString::Printf(TEXT("Refused: Color Space %s cannot be used with this sender's format (16-bit float streams are linear). Choose Auto or Linear."), ColorSpaceName(UseColorSpace)), true);
		break;
	case ESpoutReceiveResult::ModeConflict:
	case ESpoutReceiveResult::ColorConflict:
		SetState(ESpoutReceiverActorState::Refused, TEXT("Refused: another consumer of this sender uses different Color Space or Async Receive settings; retrying with the shared settings."));
		NextRefusedRetryTime = Now; // the next tick reads the session's settings and joins with them
		return;
	case ESpoutReceiveResult::ConsumerLimit:
		SetState(ESpoutReceiverActorState::Refused, TEXT("Refused: too many consumers of this sender or no memory budget left (r.Spout.MaxConsumersPerReceiver, r.Spout.MaxRetainedMiB)."), true);
		break;
	case ESpoutReceiveResult::FrameTooLarge:
		SetState(ESpoutReceiverActorState::Refused, TEXT("Refused: the sender's frame is larger than the plugin's limits (r.Spout.MaxFrameMiB, r.Spout.MaxTextureDimension)."), true);
		break;
	case ESpoutReceiveResult::BadMetadata:
		SetState(ESpoutReceiverActorState::Refused, TEXT("Refused: the sender's size, handle or pixel format is not supported (RGBA8, BGRA8, RGB10A2, RGBA16F)."), true);
		break;
	case ESpoutReceiveResult::InvalidName:
		SetState(ESpoutReceiverActorState::Refused, TEXT("Refused: the Sender Name is not a valid Spout name (too long?)."), true);
		break;
	default:
		SetState(ESpoutReceiverActorState::Refused, TEXT("Refused: the received texture could not be created (see the Output Log)."), true);
		break;
	}
	NextRefusedRetryTime = Now + RefusedRetrySeconds;
}

#if WITH_EDITOR
namespace
{
	// "Name  (1920x1080 BGRA8)": the dropdown shows each running sender's size and format next to its name.
	FString SenderOption(const FSpoutSenderInfo& Sender)
	{
		return FString::Printf(TEXT("%s  (%dx%d %s%s)"), *Sender.Name.ToString(), Sender.Width, Sender.Height, *Sender.Format,
			Sender.Gpu == ESpoutSenderGpu::Other ? TEXT(", on another GPU") : TEXT(""));
	}

	// The sender name of a dropdown entry: matched against the senders running now, else the text before the details.
	FName SenderNameFromOption(const FString& Option)
	{
		TArray<FSpoutSenderInfo> Senders;
		if (USpoutBPFunctionLibrary::ListSpoutSenders(Senders))
		{
			for (const FSpoutSenderInfo& Sender : Senders)
			{
				if (SenderOption(Sender) == Option)
				{
					return Sender.Name;
				}
			}
		}
		const int32 Details = Option.Find(TEXT("  ("), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		return FName(*(Details == INDEX_NONE ? Option : Option.Left(Details)));
	}
}

TArray<FString> ASpoutReceiverActor::GetRunningSenderOptions()
{
	// Read each time the dropdown opens, so the list is always current.
	TArray<FString> Options;
	TArray<FSpoutSenderInfo> Senders;
	if (USpoutBPFunctionLibrary::ListSpoutSenders(Senders))
	{
		for (const FSpoutSenderInfo& Sender : Senders)
		{
			Options.Add(SenderOption(Sender));
		}
	}
	return Options;
}

void ASpoutReceiverActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(ASpoutReceiverActor, ChooseRunningSender) && !ChooseRunningSender.IsEmpty())
	{
		SenderName = SenderNameFromOption(ChooseRunningSender);
	}
	ApplyDisplay();
}

bool ASpoutReceiverActor::ShowPausedInEditor()
{
	FString Reason;
	if (!SpoutActorSupport::IsInLevelEditorWorld(this) || (!bConnected && !(bEnabled && MayReceiveInEditor(Reason))))
	{
		return false;
	}
	if (!bShowingEditorPause)
	{
		bShowingEditorPause = true;
		StatusBeforeEditorPause = Status;
	}
	Status = SpoutActorSupport::PausedInEditorStatus();
	return true;
}
#endif
