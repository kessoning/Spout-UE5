// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Which graphics card a sender's shared texture is on. Spout shares textures through handles that open only on the
 * GPU that created them, so a sender on another GPU (a laptop's integrated GPU, a second card) can never be received
 * by Unreal and would otherwise only show black.
 *
 * The texture is opened on Unreal's own Direct3D 11 device; when that fails, on a device made for each other hardware
 * adapter, the way the Spout SDK's own GetSenderAdapter finds a sender's adapter. Results are kept per sender name and
 * texture handle, so a receive call costs one map lookup; the devices for other adapters are made once, and only when
 * a texture does not open on Unreal's GPU. Game thread.
 */
#pragma once

#include "CoreMinimal.h"
#include "SpoutBPFunctionLibrary.h"

namespace SpoutGpu
{
	struct FSenderGpu
	{
		ESpoutSenderGpu Where = ESpoutSenderGpu::Unknown;
		/** The adapter's name: the other GPU's when Where is Other, Unreal's when SameAsUnreal. */
		FString AdapterName;
	};

	/** Where the texture behind SharedHandle (a Spout sender's handle) is. Cached per name and handle. */
	FSenderGpu Locate(FName SenderName, void* SharedHandle);

	/** The name of the graphics card Unreal renders on, as Windows reports it; empty before Spout starts. */
	FString UnrealAdapterName();

	/** Drops the cache and the devices made for other adapters (shutdown). */
	void Reset();
}
