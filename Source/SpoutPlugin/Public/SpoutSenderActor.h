// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Placeable Spout Sender: drag it into a level from the Place Actors panel (Spout category) and it sends while the game
 * is playing, with no Blueprint. It calls the same Spout Sender / Close Sender functions a Blueprint would.
 */
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SpoutBPFunctionLibrary.h"
#include "SpoutSenderActor.generated.h"

class UTextureRenderTarget2D;
class UBillboardComponent;

UCLASS(ClassGroup = Spout, meta = (DisplayName = "Spout Sender"),
	HideCategories = (Rendering, Physics, Collision, Input, HLOD, LOD, Cooking, Replication, Networking, Actor))
class SPOUTPLUGIN_API ASpoutSenderActor : public AActor
{
	GENERATED_BODY()

public:
	ASpoutSenderActor();

	/** Send while the game is playing. Turning it off stops this sender and removes its stream at once. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spout", meta = (DisplayName = "Enabled"))
	bool bEnabled = true;

	/**
	 * The name other Spout applications see. Each new Spout Sender gets a unique name when placed, duplicated or
	 * pasted in the editor. If another sender (another actor, a Blueprint, or another application) already publishes
	 * this name, this actor does not send and says so in Status: one name, one source.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spout", meta = (DisplayName = "Spout Name"))
	FName SpoutName = TEXT("UE Spout Sender");

	/**
	 * What to send. Game Viewport Only (the default) sends the rendered game without the editor's toolbars and panels.
	 * Game Viewport (Whole Window) sends the whole window. Texture Render Target 2D sends the render target set below;
	 * without one, nothing is sent (there is no fallback to another source).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spout", meta = (DisplayName = "Send From"))
	ESpoutSendTextureFrom SendFrom = ESpoutSendTextureFrom::GameViewportOnly;

	/**
	 * Required when Send From is Texture Render Target 2D: the render target to send, for example the Texture Target of
	 * a Scene Capture 2D. One mip, Auto Generate Mips off; formats RGBA8/BGRA8, RGB10A2 or RGBA16f. Without a render
	 * target this sender does not send.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spout", meta = (DisplayName = "Texture Render Target",
		EditCondition = "SendFrom == ESpoutSendTextureFrom::TextureRenderTarget2D", EditConditionHides))
	TObjectPtr<UTextureRenderTarget2D> TextureRenderTarget = nullptr;

	/**
	 * Also send while the level is edited, not only while the game plays. Needs Project Settings > Plugins > Spout >
	 * Startup Policy set to Editor and Play. In the editor, Game Viewport Only sends the level editor's active viewport
	 * and Game Viewport (Whole Window) the level editor's window; a Texture Render Target is sent as during Play. While
	 * Play runs, the Play copy of this actor takes the name over, and this one resumes when Play ends.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spout", meta = (DisplayName = "Send In Editor"))
	bool bSendInEditor = false;

	/** What this sender is doing: during Play, and in the editor when Send In Editor is on. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "Spout|Status")
	FString Status;

	/** True while frames are actually being published (the frame count advances), not merely requested. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "Spout|Status")
	bool bPublishing = false;

	/** Turns sending on or off immediately (prefer this to toggling the actor's tick). */
	UFUNCTION(BlueprintCallable, Category = "Spout")
	void SetEnabled(bool bNewEnabled);

	/** Changes the name; the old stream is closed first. */
	UFUNCTION(BlueprintCallable, Category = "Spout")
	void SetSpoutName(FName NewName);

	/** Changes the source; the stream is restarted from the new source under the same name. */
	UFUNCTION(BlueprintCallable, Category = "Spout")
	void SetSource(ESpoutSendTextureFrom NewSendFrom, UTextureRenderTarget2D* NewTextureRenderTarget);

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override;
	virtual void Destroyed() override;

#if WITH_EDITOR
	virtual void CheckForErrors() override;
	virtual void PostActorCreated() override;
	virtual void PostEditImport() override;
	virtual void PostDuplicate(bool bDuplicateForPIE) override;
#endif

private:
	void UpdateSending();
	/** Whether this actor may send in the level editor now; otherwise Reason says why not. */
	bool MaySendInEditor(FString& Reason) const;
	/** The level editor's sender gives its name to Play's (see Send In Editor). */
	void YieldToPlay();
	void StopSending(const TCHAR* Reason);
	void SetProblem(const FString& NewStatus);
#if WITH_EDITOR
	void MakeNameUniqueInEditor(bool bNewlyPlaced);
#endif

	UPROPERTY()
	TObjectPtr<USceneComponent> SceneRoot;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> Sprite;
#endif

	// Applied state (what is being sent now).
	bool bClaimed = false;
	FName AppliedName;
	ESpoutSendTextureFrom AppliedSendFrom = ESpoutSendTextureFrom::GameViewportOnly;
	TWeakObjectPtr<UTextureRenderTarget2D> AppliedTarget;
	int64 LastFrames = 0;
	double LastFrameAdvanceTime = 0.0;
	double NextOwnershipCheckTime = 0.0;
	FName RecentlyClosedName;
	double RecentlyClosedTime = -1000.0;
	FString LastProblem;
	// The level editor's sender gave its name to Play's and waits for Play to end.
	bool bYieldedToPlay = false;
};
