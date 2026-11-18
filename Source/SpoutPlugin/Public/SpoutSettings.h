// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * The plugin's page in Project Settings (Plugins > Spout), saved in the project's DefaultEngine.ini.
 *
 * The access wait and the limits are console variables (r.Spout.*). The page sets them at project-setting priority,
 * so a value given in a [ConsoleVariables] ini section, on the command line or in the console still takes precedence.
 */
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SpoutSettings.generated.h"

/** When Spout streams may run. */
UENUM(BlueprintType)
enum class ESpoutStartupPolicy : uint8
{
	/** Streams run while the game plays: Play In Editor, standalone or packaged. */
	PlayOnly UMETA(DisplayName = "Play only"),
	/** Also while the level is edited, for Spout Sender and Spout Receiver actors set to send or receive in the editor. */
	EditorAndPlay UMETA(DisplayName = "Editor and Play")
};

UCLASS(config = Engine, defaultconfig, meta = (DisplayName = "Spout"))
class SPOUTPLUGIN_API USpoutSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	USpoutSettings();

	/**
	 * When Spout streams may run. Play only: while the game plays. Editor and Play: Spout Sender and Spout Receiver
	 * actors set to send or receive in the editor also run while the level is edited.
	 */
	UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Startup")
	ESpoutStartupPolicy StartupPolicy = ESpoutStartupPolicy::PlayOnly;

	/** The name a Spout Sender actor gets when it is placed in the editor. A number is added when the name is taken. */
	UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Senders")
	FString DefaultSenderName;

	/**
	 * Longest wait, in milliseconds, for a shared texture's cross-process access lock while another application holds
	 * it. 0 never waits: a busy texture skips that transfer. Console variable r.Spout.AccessWaitMs.
	 */
	UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Access", meta = (DisplayName = "Access Wait (ms)", ClampMin = "0", ClampMax = "10", UIMin = "0", UIMax = "10"))
	int32 AccessWaitMs;

	/** Sender and receiver roles open at once, combined. Console variable r.Spout.MaxActiveStreams. */
	UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Limits", meta = (ClampMin = "1", ClampMax = "1024"))
	int32 MaxActiveStreams;

	/** Queued Spout commands, all streams together. Console variable r.Spout.MaxPendingCommands. */
	UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Limits", meta = (ClampMin = "1", ClampMax = "4096"))
	int32 MaxPendingCommands;

	/**
	 * Queued commands per stream. Each send or receive call queues one, so this is also how many receive calls one
	 * stream accepts within a frame. Console variable r.Spout.MaxPendingPerStream.
	 */
	UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Limits", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxPendingPerStream;

	/** Material bindings, and output render targets, per receiver. Console variable r.Spout.MaxConsumersPerReceiver. */
	UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Limits", meta = (ClampMin = "1", ClampMax = "1024"))
	int32 MaxConsumersPerReceiver;

	/** Largest width or height of a stream, in pixels. Console variable r.Spout.MaxTextureDimension. */
	UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Limits", meta = (ClampMin = "1", ClampMax = "16384"))
	int32 MaxTextureDimension;

	/** Largest frame, uncompressed, in MiB. Console variable r.Spout.MaxFrameMiB. */
	UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Limits", meta = (DisplayName = "Max Frame (MiB)", ClampMin = "1", ClampMax = "2048"))
	int32 MaxFrameMiB;

	/**
	 * Memory the plugin may keep for stream images, all streams together, in MiB. An estimate kept on the safe side;
	 * the driver's own allocations come on top. Console variable r.Spout.MaxRetainedMiB.
	 */
	UPROPERTY(config, EditAnywhere, BlueprintReadOnly, Category = "Limits", meta = (DisplayName = "Max Retained (MiB)", ClampMin = "1", ClampMax = "65536"))
	int32 MaxRetainedMiB;

	static const USpoutSettings& Get() { return *GetDefault<USpoutSettings>(); }

	/** The base of a new Spout Sender actor's name: Default Sender Name, or "UE Spout Sender" when that is empty. */
	FString GetDefaultSenderName() const;

	/**
	 * Sets the console variables from this page at project-setting priority. A variable that already has a value of
	 * higher priority (an ini file's [ConsoleVariables], the command line, the console) keeps it.
	 * bOnlyChangedValues: leave the variables whose value on this page is the default.
	 */
	void ApplyToConsoleVariables(bool bOnlyChangedValues) const;

	virtual FName GetCategoryName() const override;
	virtual void PostInitProperties() override;
#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
