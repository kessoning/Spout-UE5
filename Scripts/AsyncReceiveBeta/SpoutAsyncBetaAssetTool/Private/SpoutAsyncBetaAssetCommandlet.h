// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5
#pragma once

#include "Commandlets/Commandlet.h"
#include "SpoutAsyncBetaAssetCommandlet.generated.h"

/**
 * Edits and inspects the shipped receiver example for Async Receive (Beta) through Unreal's own Blueprint editing
 * API: no asset byte is written by anything but Unreal's package saver. See Scripts/AsyncReceiveBeta/README.md.
 *
 *   UnrealEditor-Cmd <host>.uproject -run=SpoutAsyncBetaAsset -Mode=Inspect   -Out=<report.json> [-Map=/Game/...]
 *   UnrealEditor-Cmd <host>.uproject -run=SpoutAsyncBetaAsset -Mode=Migrate   -Out=<report.json>
 *   UnrealEditor-Cmd <host>.uproject -run=SpoutAsyncBetaAsset -Mode=MakeLevel -Out=<report.json> -Map=/Game/AsyncBeta/<level> -Label=<actor label> [-AsyncOn]
 *
 * Inspect   reports the example's variables, the generated property's editor flags and metadata, the class default,
 *           how the receive node's Async Receive (Beta) pin is connected, the compile result and which engine saved
 *           the package; with -Map, the saved example actors of that level and their values.
 * Migrate   adds the instance-editable variable Async Receive (Beta) (default false, Spout|Experimental, the Beta
 *           tooltip) and connects it to the Async Receive (Beta) input of every Spout Receiver With Material node in
 *           the example; compiles; saves with the running engine. Refuses when the variable already exists.
 * MakeLevel saves a level with one example actor (the example always receives the name "Spout"): with -AsyncOn its
 *           Async Receive (Beta) is set on the placed instance, without it the instance keeps the class default (false).
 *           For the saved-value, reopen and packaged checks. A validation level of the host, never shipped.
 * Exit code 0 when everything held, 1 otherwise; the report says what.
 */
UCLASS()
class USpoutAsyncBetaAssetCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	USpoutAsyncBetaAssetCommandlet();
	virtual int32 Main(const FString& Params) override;
};
