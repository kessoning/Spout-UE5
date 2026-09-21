// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#pragma once

#include "Engine/Texture2D.h"
#include "SpoutLimits.h"
#include "Engine/AssetUserData.h"
#include "SpoutReceivedTexture.generated.h"

// The texture references this reservation through its reflected asset-user-data array.
// It follows GC, including outputs retained by callers after CloseReceiver.
UCLASS(Transient)
class USpoutTextureReservation final : public UAssetUserData
{
	GENERATED_BODY()
public:
	TSharedPtr<FSpoutResourceReservation, ESPMode::ThreadSafe> Reservation;
};

/** Install a writable resource on a plugin-owned transient texture, including after UpdateResource. */
void EnsureSpoutReceivedTextureResource(UTexture2D& Texture);
