// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Declares the receiver API that connects Blueprint calls to the Spout receive path
 * implemented in SpoutReceiver.cpp.
 */
#pragma once

#include "CoreMinimal.h"
#include "SpoutBPFunctionLibrary.h"

class FSpoutReceiverOwnership;
class UMaterialInstanceDynamic;
class UMaterialInterface;

namespace SpoutReceiverInternal
{
	/**
	 * Chooses the material instance one consumer uses this frame.
	 *
	 * Exposed rather than kept file-local. The rule is per CONSUMER, unlike the received texture,
	 * which is per session:
	 *
	 *   - a caller that already holds a material keeps using it;
	 *   - a caller with none gets a NEW instance created from its OWN InputMaterial;
	 *   - a material tracked for the receiver NAME is never handed to a caller that did not already
	 *     have it.
	 *
	 * That last rule keeps a second consumer of one name from inheriting the first consumer's
	 * material instance and having its own InputMaterial silently ignored, with the winner decided
	 * by Blueprint tick order.
	 *
	 * bOutPluginCreated reports whether the returned material is one this plugin owns for that
	 * session, which is what the caller passes to FSpoutReceiverOwnership::RegisterMaterialBinding.
	 */
	UMaterialInstanceDynamic* ResolveConsumerMaterial(
		FSpoutReceiverOwnership& Ownership,
		FName ReceiverName,
		UMaterialInstanceDynamic* CallerMaterial,
		UMaterialInterface* InputMaterial,
		bool& bOutPluginCreated);
}

class FSpoutReceiver
{
public:
	/**
	 * Receives a shared Spout texture and updates a transient UTexture2D on the render thread.
	 *
	 * Return value contract: true means the named sender was found, its advertised metadata passed
	 * validation, the destination texture exists, and the copy was accepted for the render thread.
	 * It does NOT mean the copy has completed — that happens later, on the render thread.
	 *
	 * false means the request was rejected on the game thread: an invalid name, an unavailable
	 * interop context, no such sender, or sender metadata that failed validation (invalid extent,
	 * null handle, or a format outside the supported-format policy). Nothing was queued and the
	 * OutMat/OutTexture parameters are left exactly as the caller passed them in.
	 *
	 * OutMat and OutTexture are outputs. The received texture is owned by FSpoutReceiverOwnership,
	 * keyed by sender name, and the caller's variable is a handle to it rather than the thing that
	 * keeps it alive. They are only assigned on success and are deliberately not cleared on failure,
	 * so a temporary dropout does not disturb a caller that is still using the texture.
	 *
	 * The texture is per SESSION, shared by every consumer of the name; the material is per CONSUMER.
	 * See SpoutReceiverInternal::ResolveConsumerMaterial.
	 *
	 * An incompatible OptionalOutputRenderTarget does not fail the call. It is excluded from the
	 * copy and left untouched, the mismatch is logged, and the plugin-owned received texture still
	 * updates.
	 *
	 * ColorSpace decides how the received bytes are interpreted. Auto applies the
	 * documented per-format convention. An explicit value that the advertised format cannot
	 * represent -- sRGB against a 10-bit or float frame -- fails the call with a diagnostic rather
	 * than being quietly ignored.
	 */
	static bool Receive(const FName SpoutName, UMaterialInterface* InputMaterial, FName TextureParameterName, UMaterialInstanceDynamic*& OutMat, UTexture2D*& OutTexture, UTextureRenderTarget2D* OptionalOutputRenderTarget, ESpoutColorSpace ColorSpace = ESpoutColorSpace::Auto);
	// Removes the receiver's registry entry, releasing its opened shared texture and cached wraps.
	static void Close(const FName SpoutName);
};
