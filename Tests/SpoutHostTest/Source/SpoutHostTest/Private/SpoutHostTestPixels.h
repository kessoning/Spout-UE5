// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#pragma once
#include "CoreMinimal.h"
class FTextureResource;
class UWorld;
namespace SpoutHostTest
{
	TArray<FColor> ReadPixels(FTextureResource* Resource, int32 Width, int32 Height);
	bool QualifyExternalPixels(FString& Error);
	bool QualifyShippedExamples(UWorld* World, FString& Error);
}
