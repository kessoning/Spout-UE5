// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5
#pragma once

#include "CoreMinimal.h"

/** The example and the setting the tool adds to it: one definition for the commandlet and the screenshot command. */
namespace SpoutAsyncBetaAsset
{
	inline const TCHAR* const ExampleAssetPath = TEXT("/SpoutPlugin/Content/BP_SpoutReceiverExample.BP_SpoutReceiverExample");
	inline const TCHAR* const ExampleClassPath = TEXT("/SpoutPlugin/Content/BP_SpoutReceiverExample.BP_SpoutReceiverExample_C");
	inline const FName VarName(TEXT("AsyncReceiveBeta"));
	inline const FName PinName(TEXT("bAsyncReceive"));
	inline const TCHAR* const FriendlyName = TEXT("Async Receive (Beta)");
	inline const TCHAR* const Category = TEXT("Spout|Experimental");
	/** The Beta tooltip, then what applying it means. */
	inline const TCHAR* const Tooltip = TEXT("Moves receive work off the render thread. May improve delivery under load, but can increase frame-time spikes or image latency. ")
		TEXT("Applies when this actor's receiver opens: every actor receiving the same Spout Name must use the same setting (a conflicting one is refused and logged), ")
		TEXT("and a change takes effect after Close Receiver, for example on the next Play.");

	/**
	 * Promotion (-Mode=Promote): Async Receive leaves Beta and is the default. The variable keeps its name, so that
	 * values saved on placed examples stay attached to it; its label, category, tooltip and class default change.
	 */
	inline const TCHAR* const PromotedFriendlyName = TEXT("Async Receive");
	inline const TCHAR* const PromotedCategory = TEXT("Spout");
	inline const TCHAR* const PromotedTooltip = TEXT("On by default: a worker thread takes the sender's texture and waits for each copy, so the render thread does not. ")
		TEXT("Turn it off for the synchronous receive, where the render thread waits for each copy. ")
		TEXT("Applies when this actor's receiver opens: every actor receiving the same Spout Name must use the same setting (a conflicting one is refused and logged), ")
		TEXT("and a change takes effect after Close Receiver, for example on the next Play.");

	/** The migration fixture (-Mode=MakeFixtures): receive nodes saved by an earlier plugin, in each state of the input. */
	inline const TCHAR* const FixtureAssetPath = TEXT("/Game/Migration/BP_MigrationReceive");
	inline const FName FixtureVarName(TEXT("ChosenReceiveMode"));
}
