// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5
#pragma once

#include "Commandlets/Commandlet.h"
#include "SpoutExampleAssetCommandlet.generated.h"

/**
 * -run=SpoutExampleAsset -Mode=<Inspect|Update> -Out=<report.json>
 *
 * Inspect reports both example Blueprints in full (variables and their class defaults, every graph node with its pins,
 * pin values and links, the components and what they change from their class), the two shipped materials (domain,
 * shading, parameters and their defaults) and the two example render targets. Update changes the receiver example
 * (see Scripts/ExampleContent/README.md), compiles it, checks the result and saves it with the running engine.
 */
UCLASS()
class USpoutExampleAssetCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	USpoutExampleAssetCommandlet();
	virtual int32 Main(const FString& Params) override;
};
