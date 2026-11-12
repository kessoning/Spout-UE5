// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Small game-thread helpers shared by the placeable Spout Sender and Spout Receiver actors. They reuse the plugin's
 * existing context, registry and enumeration; nothing here sends or receives.
 */
#pragma once

#include "CoreMinimal.h"

class AActor;

namespace SpoutActorSupport
{
	/** True when this process publishes a sender of that name (an open sender stream or registry entry). */
	bool IsPublishedByThisProcess(FName Name);

	/** True when any sender of that name is advertised on this machine (any process, this one included). */
	bool IsAdvertised(FName Name);

	/**
	 * Chooses a sender published by ANOTHER process: Spout's active sender if it is external, otherwise the first
	 * external sender in the list. False when there is none (or Spout is unavailable).
	 */
	bool ChooseExternalSender(FName& OutName);

	/** Sender names currently advertised on this machine, for the editor's Details dropdown. */
	TArray<FString> ListAdvertisedSenderNames();

	/**
	 * Process-wide claims of sender names by sender actors (game thread). Claim fails when another live actor holds
	 * the name; claiming a name the actor already holds succeeds. Release is idempotent and only drops the actor's own
	 * claim.
	 */
	bool ClaimSenderName(FName Name, const AActor* Actor, const AActor*& OutHolder);
	void ReleaseSenderName(FName Name, const AActor* Actor);
}
