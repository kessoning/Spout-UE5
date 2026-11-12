// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Placeable Spout Receiver: drag it into a level from the Place Actors panel (Spout category) and, while the game is
 * playing, it shows a Spout stream on its built-in unlit screen, with no Blueprint. It uses the same receive path as
 * the Spout Receiver With Material node, with one stored material instance.
 */
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SpoutBPFunctionLibrary.h"
#include "SpoutReceiverActor.generated.h"

class UStaticMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture2D;
class UTextureRenderTarget2D;
class UArrowComponent;

/** What the screen shows after a connected sender disappears. */
UENUM(BlueprintType)
enum class ESpoutLostSenderDisplay : uint8
{
	HoldLastFrame UMETA(DisplayName = "Hold Last Frame", ToolTip = "Keep showing the last received frame until the sender returns."),
	ShowNoSignal UMETA(DisplayName = "Show No-Signal Color", ToolTip = "Show the No-Signal color until the sender returns.")
};

/** The receiver actor's state, as shown in Status. */
UENUM(BlueprintType)
enum class ESpoutReceiverActorState : uint8
{
	Disabled,
	/** No sender to connect to yet (none chosen, or the named sender is not running). Quietly retried. */
	Waiting,
	/** The receive request was accepted, but no frame has arrived yet. */
	RequestAccepted UMETA(DisplayName = "Request Accepted"),
	/** Frames are arriving. */
	Receiving,
	/** The sender is advertised but the receive is refused; Status says why and what to change. */
	Refused
};

UCLASS(ClassGroup = Spout, meta = (DisplayName = "Spout Receiver"),
	HideCategories = (Physics, Input, HLOD, LOD, Cooking, Replication, Networking, Actor))
class SPOUTPLUGIN_API ASpoutReceiverActor : public AActor
{
	GENERATED_BODY()

public:
	ASpoutReceiverActor();

	/** Receive while the game is playing. Turning it off releases this actor's use of the stream at once. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spout", meta = (DisplayName = "Enabled"))
	bool bEnabled = true;

	/**
	 * The Spout sender to show. Leave empty to connect automatically to a sender from another application (Spout's
	 * active sender, otherwise the first one listed); senders of this game are not chosen automatically. Once
	 * connected, the actor stays with that sender, and waits for it if it stops, instead of switching to another.
	 * Type a name to receive a specific sender, including one sent by this game (for example a render-target sender).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spout", meta = (DisplayName = "Sender Name"))
	FName SenderName;

#if WITH_EDITORONLY_DATA
	/** Pick one of the Spout senders running on this machine right now; the choice is copied into Sender Name. */
	UPROPERTY(EditInstanceOnly, Transient, Category = "Spout", meta = (DisplayName = "Choose Running Sender", GetOptions = "GetRunningSenderOptions"))
	FString ChooseRunningSender;
#endif

	/**
	 * How the received colors are read. Auto: 8-bit streams are sRGB, 10-bit and float streams linear (right for most
	 * applications and render targets). For a stream from an Unreal viewport (10-bit display values), choose sRGB.
	 * All consumers of one sender share one setting: if another consumer opened it first with a different setting,
	 * that one stays in effect and Status says so.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spout|Color", meta = (DisplayName = "Color Space"))
	ESpoutColorSpace ColorSpace = ESpoutColorSpace::Auto;

	/**
	 * Async Receive (default on): a worker thread takes the sender's texture. Off: the synchronous receive. Shared by
	 * every consumer of one sender, like Color Space.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category = "Spout|Color", meta = (DisplayName = "Async Receive"))
	bool bAsyncReceive = true;

	/**
	 * Optional: also copy each frame into this render target. Its format must match the stream (RGBA8 for 8-bit, RGB10A2
	 * for 10-bit with Auto, RGBA16f for 10-bit with sRGB and for float streams) and Auto Generate Mips must be off; a
	 * mismatched target is not used and Status names the format it needs. It is resized to the stream.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spout|Output", meta = (DisplayName = "Output Render Target"))
	TObjectPtr<UTextureRenderTarget2D> OutputRenderTarget = nullptr;

	/** Show the built-in screen. Off: receive only (for the Output Render Target or the Received Texture). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spout|Display", meta = (DisplayName = "Show Display"))
	bool bShowDisplay = true;

	/** Height of the screen in centimeters. The width follows the stream's aspect ratio. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spout|Display", meta = (DisplayName = "Display Height", ClampMin = "1", UIMin = "10", UIMax = "2000", Units = "cm"))
	float DisplayHeight = 180.f;

	/** Size the screen to the stream's aspect ratio (16:9 until the first frame). Off: a square screen you can scale. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spout|Display", meta = (DisplayName = "Keep Aspect Ratio"))
	bool bKeepAspectRatio = true;

	/** What the screen shows after the sender stops. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spout|Display", meta = (DisplayName = "When Sender Is Lost"))
	ESpoutLostSenderDisplay WhenSenderLost = ESpoutLostSenderDisplay::HoldLastFrame;

	/** Multiplies the screen's brightness. The screen is an unlit surface in the level, so the level's exposure and tone mapping still apply. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spout|Display", meta = (DisplayName = "Brightness", ClampMin = "0", UIMax = "4"))
	float Brightness = 1.f;

	/**
	 * The screen's material. The default is the plugin's unlit display material. A custom material needs a texture
	 * parameter named as below (and may use the HasSignal, NoSignalColor and Brightness parameters).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category = "Spout|Display", meta = (DisplayName = "Display Material"))
	TObjectPtr<UMaterialInterface> DisplayMaterial = nullptr;

	/** The texture parameter of the Display Material that receives the stream. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category = "Spout|Display", meta = (DisplayName = "Texture Parameter Name"))
	FName TextureParameterName = TEXT("SpoutTexture");

	/** State (Play only): Waiting, Request Accepted, Receiving or Refused. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "Spout|Status")
	ESpoutReceiverActorState State = ESpoutReceiverActorState::Disabled;

	/** What the receiver is doing, and what to change when something is wrong (Play only). */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "Spout|Status")
	FString Status;

	/** The sender this actor is connected to (or waiting for). */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "Spout|Status")
	FName ConnectedSenderName;

	/** The received stream's size in pixels (0 before the first frame). */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "Spout|Status")
	FIntPoint ReceivedSize = FIntPoint::ZeroValue;

	/** The settings in effect for this stream, which can differ from the ones above when the stream is shared. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "Spout|Status")
	FString AppliedSettings;

	/** The received texture (owned by the plugin; it changes when the stream's size or format changes). */
	UFUNCTION(BlueprintPure, Category = "Spout")
	UTexture2D* GetReceivedTexture() const { return ReceivedTexture.Get(); }

	/** The screen's material instance, which this actor creates once. */
	UFUNCTION(BlueprintPure, Category = "Spout")
	UMaterialInstanceDynamic* GetDisplayMaterialInstance() const { return DisplayMID; }

	UFUNCTION(BlueprintCallable, Category = "Spout")
	void SetEnabled(bool bNewEnabled);

	UFUNCTION(BlueprintCallable, Category = "Spout")
	void SetSenderName(FName NewSenderName);

	UFUNCTION(BlueprintCallable, Category = "Spout")
	void SetOutputRenderTarget(UTextureRenderTarget2D* NewOutputRenderTarget);

#if WITH_EDITOR
	UFUNCTION()
	static TArray<FString> GetRunningSenderOptions();
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

private:
	void UpdateReceiving();
	/** Releases this actor's use of the connected stream (idempotent). */
	void Release(bool bKeepConnectedName);
	void EnsureMaterialInstance();
	void ApplyDisplay();
	void SetSignal(bool bHasSignal);
	void SetState(ESpoutReceiverActorState NewState, const FString& NewStatus, bool bActionable = false);

	UPROPERTY(VisibleAnywhere, Category = "Spout|Display")
	TObjectPtr<UStaticMeshComponent> Display;

	UPROPERTY()
	TObjectPtr<USceneComponent> SceneRoot;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UArrowComponent> Arrow;
#endif

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DisplayMID;

	TWeakObjectPtr<UTexture2D> ReceivedTexture;

	// Applied state.
	bool bConnected = false;           // this actor has (or may have) a lease on ConnectedSenderName
	bool bAutoChosen = false;          // ConnectedSenderName came from the empty-name rule
	FName RequestedSenderName;         // SenderName when ConnectedSenderName was resolved
	ESpoutColorSpace AppliedColorSpace = ESpoutColorSpace::Auto;
	bool bAppliedAsync = true;
	TWeakObjectPtr<UTextureRenderTarget2D> AppliedOutputTarget;
	TWeakObjectPtr<UMaterialInterface> MIDSource;
	FName MIDParameter;
	int64 LastFrames = 0;               // frame count of the session identified by AppliedSessionId
	uint64 AppliedSessionId = 0;        // identity of the session this actor last received from (0: none)
	bool bSessionHasFrame = false;      // that session has delivered a frame since this actor joined it
	double LastFrameTime = 0.0;
	double NextDiscoveryTime = 0.0;
	double NextRefusedRetryTime = 0.0;
	bool bEverReceived = false;
	bool bSignal = false;
	FString LastActionable;
	FString LastOutputWarning;
};
