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
#if SPOUT_WITH_TESTS
	/** One-shot failure at the real CPU readback boundary, for retry regression tests. */
	void FailNextCpuReadbackForTests();
#endif
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

/** Why a receive was or was not accepted. Internal; used by the placeable receiver actor for its status. */
enum class ESpoutReceiveResult : uint8
{
	Accepted,
	InvalidName,
	Unavailable,        // interop context unavailable or session stopping
	NoSender,           // no sender of that name is advertised
	BadMetadata,        // the sender's advertised size, handle or format failed validation
	FrameTooLarge,      // over r.Spout.MaxFrameMiB / r.Spout.MaxTextureDimension
	ColorSpaceUnsupported, // the requested Color Space cannot be applied to the sender's format
	Busy,               // stream or queue admission limits (r.Spout.MaxActiveStreams, MaxPendingCommands, ...)
	ModeConflict,       // the open session uses the other Async Receive setting
	ColorConflict,      // the open session reads this format with another Color Space
	ConsumerLimit,      // r.Spout.MaxConsumersPerReceiver or the output reservation budget
	TextureFailed       // the destination texture could not be created
};

/** The session-wide settings of an open receiver, for a consumer that wants to join it without being refused. */
struct FSpoutReceiverSessionInfo
{
	bool bExists = false;
	bool bReceiveModeEstablished = false;
	bool bAsyncReceive = true;
	bool bColorSpaceEstablished = false;
	ESpoutColorSpace ColorSpace = ESpoutColorSpace::Auto;
	int32 ManagedConsumers = 0;
	bool bUnmanagedConsumer = false;
	/** Whether the consumer passed to GetSessionInfo is itself a registered (accepted) managed consumer of this session. */
	bool bIsManagedConsumer = false;
	/** Identity of this session; changes when the name's session is closed and reopened. 0 when none exists. */
	uint64 SessionId = 0;
};

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
	 *
	 * bAsyncReceive is the Async Receive request (true by default; false asks for the synchronous receive). The
	 * receive mode belongs to the SESSION, like the
	 * texture: the first accepted call on an entry fixes it for the entry's life (until Close or global cleanup),
	 * and a later call asking for the other mode is refused before anything is reserved or queued, with a
	 * throttled warning, leaving the session and its other consumers as they were. Nothing else selects the
	 * asynchronous path: the retired r.Spout.AsyncReceive variable is ignored.
	 *
	 * ManagedConsumer: null for the Blueprint/C++ entry points, which mark the session as used by an unmanaged caller
	 * once a receive is ACCEPTED (a refused call marks nothing). A placeable receiver actor passes itself; it is then
	 * recorded as a lease, released with ReleaseManagedConsumer. OutResult, when given, says why a call was refused.
	 */
	static bool Receive(const FName SpoutName, UMaterialInterface* InputMaterial, FName TextureParameterName, UMaterialInstanceDynamic*& OutMat, UTexture2D*& OutTexture, UTextureRenderTarget2D* OptionalOutputRenderTarget, ESpoutColorSpace ColorSpace = ESpoutColorSpace::Auto, bool bAsyncReceive = true,
		const UObject* ManagedConsumer = nullptr, ESpoutReceiveResult* OutResult = nullptr);
	// Removes the receiver's registry entry, releasing its opened shared texture and cached wraps.
	static void Close(const FName SpoutName);

	/**
	 * Settings, consumers and identity of the open receiver session for a name (bExists false when none). With a
	 * Consumer, bIsManagedConsumer says whether that consumer holds a lease on THIS session. Game thread.
	 */
	static FSpoutReceiverSessionInfo GetSessionInfo(const FName SpoutName, const UObject* Consumer = nullptr);

	/**
	 * Releases one managed consumer (a placeable receiver actor). Idempotent; safe when the session was already closed
	 * or the consumer never had an accepted receive. Releases only what is this consumer's: its material binding
	 * (restored to the value it had before binding, as ReleaseReceiverMaterialBinding does) and its use of
	 * OutputTarget (the reservation stays while another consumer uses the same target). Then, if no managed consumer
	 * remains and no Blueprint/C++ caller had a receive accepted on the session, the session is closed exactly as
	 * Close Receiver closes it. Returns whether the session was closed.
	 */
	static bool ReleaseManagedConsumer(const FName SpoutName, const UObject* Consumer, UMaterialInstanceDynamic* Material, FName TextureParameterName, UTextureRenderTarget2D* OutputTarget);

	/** Releases only this managed consumer's use of one output target (for example when it is replaced). Idempotent. */
	static void ReleaseManagedOutputTarget(const FName SpoutName, const UObject* Consumer, UTextureRenderTarget2D* OutputTarget);
};
