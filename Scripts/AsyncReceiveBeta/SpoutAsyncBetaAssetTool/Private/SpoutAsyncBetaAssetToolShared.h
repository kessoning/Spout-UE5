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
}
