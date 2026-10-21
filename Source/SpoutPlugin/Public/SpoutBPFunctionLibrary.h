// Copyright (c) 2023 RockmanZuyi
// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Blueprint-facing API and data types for Spout send/receive, implemented in SpoutBPFunctionLibrary.cpp
 * and backed by the private sender/receiver and D3D interop systems.
 */
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "SpoutBPFunctionLibrary.generated.h"

UENUM(BlueprintType)
enum class ESpoutType : uint8
{
	// Publishes a shared DX texture to other Spout clients.
	Sender,
	// Consumes a shared DX texture from another Spout client.
	Receiver
};

UENUM(BlueprintType)
enum class ESpoutSendTextureFrom : uint8
{
	// Uses the active game viewport's backbuffer texture (render thread only).
	GameViewport,
	// Uses a user-provided UTextureRenderTarget2D.
	TextureRenderTarget2D
};

/**
 * How a received frame's colour data should be interpreted.
 *
 * Spout carries a DXGI format and nothing else, so the format field is the only colour-space
 * channel the protocol has. DXGI distinguishes _UNORM_SRGB from _UNORM, but the wider Spout
 * ecosystem, this plugin's sender included, publishes plain 8-bit _UNORM for gamma-encoded
 * display output, so an 8-bit _UNORM format does not mean the bytes are linear. The format alone
 * is not always enough; this enum is the override for the case where it is not.
 */
UENUM(BlueprintType)
enum class ESpoutColorSpace : uint8
{
	/**
	 * Use the documented convention for the advertised format: 8-bit formats decode as sRGB,
	 * 10-bit and 16-bit float formats are treated as linear. Matches what other Spout applications
	 * expect and is the right answer for almost every sender.
	 */
	Auto        UMETA(DisplayName = "Auto (format convention)"),

	/** Force sRGB decoding. Only valid for 8-bit formats; any other format is rejected. */
	SRGB        UMETA(DisplayName = "sRGB"),

	/**
	 * Force linear sampling with no decode. Use when the sender publishes linear or data values in
	 * an 8-bit format, such as a render target created with Force Linear Gamma.
	 */
	Linear      UMETA(DisplayName = "Linear")
};

/**
 * How a receiver session moves frames. One session exists per sender name, so the mode belongs to the
 * session, not to one caller: the first accepted receive for a name decides it, and it stays fixed until
 * that receiver is closed. Synchronous is the default for every caller and every existing asset.
 */
UENUM(BlueprintType)
enum class ESpoutReceiveMode : uint8
{
	/** The render thread takes the sender's texture and waits for the copy. The default. */
	Synchronous UMETA(DisplayName = "Synchronous"),

	/**
	 * Async Receive (Beta): a worker thread takes the sender's texture and waits for the copy, and the render
	 * thread only queues a copy of the newest completed image. Opt-in per receiver.
	 */
	AsyncBeta   UMETA(DisplayName = "Async Receive (Beta)")
};

/**
 * Lightweight struct for Blueprint interaction.
 * Heavy D3D resources are managed internally by the CPP file.
 */
USTRUCT(BlueprintType)
struct FSenderStruct
{
	GENERATED_BODY()

	// Spout sender name used for registry lookup and external discovery.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spout Struct")
	FName Name;

	// Best-effort status flag; true means the sender was found/created at query time.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spout Struct")
	bool bIsAlive = false;

	// Whether this struct represents a Sender or Receiver flow.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spout Struct")
	ESpoutType SpoutType = ESpoutType::Sender;

	// Cached texture dimensions reported by Spout.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spout Struct")
	int32 Width = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spout Struct")
	int32 Height = 0;

	// Transient texture owned by this plugin; updated by receiver on the render thread.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spout Struct")
	UTexture2D* TextureColor = nullptr;

	// Optional dynamic material instance that references TextureColor.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spout Struct")
	UMaterialInstanceDynamic* MaterialInstanceColor = nullptr;

	// Optional base material for auto-creating MaterialInstanceColor.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spout")
	UMaterialInterface* UserBaseMaterial = nullptr;

	// Parameter name used to bind TextureColor in the material.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spout")
	FName UserTextureParameter = "SpoutTexture";

	// Convenience slot for a caller-held output render target. The plugin never reads this field;
	// the receiver's own OptionalOutputRenderTarget parameter is what drives the copy.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spout")
	UTextureRenderTarget2D* OutputRenderTarget = nullptr;

	FSenderStruct() {}
};

/** Snapshot counters never turn an accepted request into a synchronous pixel-delivery guarantee. */
USTRUCT(BlueprintType)
struct FSpoutStreamStatus
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Spout") bool bActive = false;
	UPROPERTY(BlueprintReadOnly, Category="Spout") bool bHasFrame = false;
	UPROPERTY(BlueprintReadOnly, Category="Spout") int32 PendingCommands = 0;
	/** Enqueued non-viewport commands; excludes requests refused after queue reservation. */
	UPROPERTY(BlueprintReadOnly, Category="Spout") int64 AcceptedRequests = 0;
	UPROPERTY(BlueprintReadOnly, Category="Spout") int64 QueueRejections = 0;
	/** Published sender frames, or receiver copies whose CPU upload may still be queued. */
	UPROPERTY(BlueprintReadOnly, Category="Spout") int64 FramesSubmitted = 0;
	UPROPERTY(BlueprintReadOnly, Category="Spout") int64 AccessMisses = 0;
	UPROPERTY(BlueprintReadOnly, Category="Spout") int64 NativeFailures = 0;
	/** Zero after recovery; signed Windows HRESULT on a native failure. */
	UPROPERTY(BlueprintReadOnly, Category="Spout") int64 LastNativeError = 0;
	/**
	 * Receiver role: the mode this name's open session was opened with (Async Receive (Beta) or synchronous).
	 * Fixed until Close Receiver; a receive asking for the other mode is refused meanwhile.
	 */
	UPROPERTY(BlueprintReadOnly, Category="Spout") ESpoutReceiveMode RequestedReceiveMode = ESpoutReceiveMode::Synchronous;
	/**
	 * Receiver role: the path the most recent receive on the render thread actually used. Synchronous until the
	 * first one, and whenever Async Receive (Beta) was requested but refused or stopped (see ReceiveModeNote).
	 */
	UPROPERTY(BlueprintReadOnly, Category="Spout") ESpoutReceiveMode EffectiveReceiveMode = ESpoutReceiveMode::Synchronous;
	/** Receiver role: why a session opened with Async Receive (Beta) is running synchronously. Empty otherwise. */
	UPROPERTY(BlueprintReadOnly, Category="Spout") FString ReceiveModeNote;
};

USTRUCT(BlueprintType)
struct FSpoutRuntimeStatus
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Spout") bool bInteropAvailable = false;
	UPROPERTY(BlueprintReadOnly, Category="Spout") int32 ActiveStreams = 0;
	UPROPERTY(BlueprintReadOnly, Category="Spout") int32 PendingCommands = 0;
	UPROPERTY(BlueprintReadOnly, Category="Spout") int32 PeakPendingCommands = 0;
	UPROPERTY(BlueprintReadOnly, Category="Spout") int64 RejectedRequests = 0;
	UPROPERTY(BlueprintReadOnly, Category="Spout") int64 EstimatedRetainedBytes = 0;
	UPROPERTY(BlueprintReadOnly, Category="Spout") int64 PeakEstimatedRetainedBytes = 0;
	UPROPERTY(BlueprintReadOnly, Category="Spout") int64 RejectedAllocations = 0;
};

/** All public entry points require the game thread, including C++ callers. */
UCLASS(ClassGroup = Spout, Blueprintable)
class SPOUTPLUGIN_API USpoutBPFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Snapshot of one active name/role; closed or undiscovered names return bActive=false. */
	UFUNCTION(BlueprintPure, Category="Spout")
	static FSpoutStreamStatus GetSpoutStreamStatus(FName SpoutName, ESpoutType Type);
	/** Process-wide queue/resource admission counters, without initializing the graphics bridge. */
	UFUNCTION(BlueprintPure, Category="Spout")
	static FSpoutRuntimeStatus GetSpoutRuntimeStatus();
	// --- Lifecycle ---
	
	// Called by module shutdown; drains render commands and releases D3D/Spout resources.
	static void GlobalShutdown();

	/**
	 * Closes every active sender and receiver, leaving the D3D/Spout context alive.
	 * Used to tear down streams that belong to a session (PIE) without paying to rebuild the
	 * process-wide interop device. GlobalShutdown() calls this before releasing the context.
	 */
	static void CloseAllStreams();

	// --- Senders ---

	/**
	 * Sends a texture to Spout asynchronously via the render thread.
	 * Call on the game thread. GPU copies, completion waits and the bounded wait for a peer that
	 * holds the shared texture (r.Spout.AccessWaitMs per attempt) run on the render thread;
	 * queueing or closing a stream can wait for an in-progress native installation.
	 * Sources must have one mip, one array slice and one sample; disable Auto Generate Mips.
	 *
	 * Returns whether the request was ACCEPTED, not whether it succeeded. The copy, the creation of
	 * the shared texture, and the registration of the name with Spout all happen afterwards on the
	 * render thread; failures there are reported in the log (LogSpoutPlugin), because there is no
	 * longer a Blueprint call to return them to.
	 *
	 * True therefore means: the name is valid, the interop context is available, the source exists,
	 * and the work was queued. False means the request was rejected outright and nothing was queued
	 * — an empty or overlong name, an unavailable context, a missing game viewport, a render
	 * target with invalid dimensions or an unsupported pixel format, or a session shutdown already
	 * in progress.
	 *
	 * Calling Close Sender with the same name cancels queued work that has not run yet, so a send
	 * and a close in the same frame cannot publish the sender afterwards.
	 */
	UFUNCTION(BlueprintCallable, Category="Spout",
		meta = (ToolTip = "Queues a Spout send on the render thread. Returns true when the request was accepted and queued - not that a frame was published. Watch the log for registration or format failures."))
	static bool SpoutSender(FName SpoutName,
							ESpoutSendTextureFrom SendTextureFrom,
							UTextureRenderTarget2D* TextureRenderTarget2D);

	UFUNCTION(BlueprintCallable, Category = "Spout")
	static void CloseSender(FName SpoutName);

	// --- Receivers ---

	/**
	 * Receives a named Spout sender into a plugin-created texture, optionally binding it to a
	 * dynamic material instance and copying into a caller-supplied render target.
	 *
	 * Returns whether the request was ACCEPTED, not whether the copy completed. True means the
	 * sender was found, its advertised size/format/handle passed validation, the destination texture
	 * exists, and the copy was queued for the render thread. False means the request was rejected on
	 * the game thread and nothing was queued.
	 * Call on the game thread. Opening the native handle and validating its actual
	 * descriptor happen later on the render thread; an incompatible or vanished
	 * source is skipped and logged even when this call returned true. The copy waits at most
	 * r.Spout.AccessWaitMs per attempt for a sender that is still writing, also on the render thread.
	 *
	 * OutMat and OutTexture are outputs, not ownership. The received texture belongs to the plugin,
	 * which keeps it alive for as long as the receiver is open and releases it on Close Receiver or
	 * on session shutdown; Out Texture is the plugin's texture, handed to you to use. Keep the
	 * values this node returns and pass them to Close Receiver. C++ callers can reuse OutMat;
	 * Blueprint callers should use Spout Receiver With Material with a stored MID input. Outputs
	 * are only ever assigned on success and are deliberately left untouched on failure, so a
	 * temporary sender dropout does not orphan anything.
	 *
	 * There is one receiver session per sender name. Every graph receiving the same name shares that
	 * one received texture — but each gets its OWN material instance, created from its OWN Input
	 * Material. A graph is never handed another graph's material because the sender names matched.
	 * Close Receiver on that name closes the shared session for every graph using it, and restores
	 * material parameters that still reference this session's texture. Later user edits are preserved.
	 *
	 * OptionalOutputRenderTarget must match the incoming pixel format, be single-sampled,
	 * and have Auto Generate Mips disabled.
	 * A mismatched target is left completely untouched and the mismatch is logged; the call still
	 * succeeds and OutTexture still updates. Size mismatches are resolved by resizing the target.
	 *
	 * Upstream frames are consumed once. Each optional target catches up independently, including
	 * late attachment and resource recreation while the sender is paused. Unchanged targets skip copies.
	 * Consumers sharing a name must agree on resolved color space; conflicting calls fail until close
	 * or a source pixel-format change creates a new interpretation.
	 *
	 * ColorSpace decides how the received bytes are interpreted:
	 *   - Auto (default): 8-bit frames decode as sRGB, 10-bit and 16-bit float frames stay linear.
	 *     This matches what other Spout applications publish.
	 *   - Linear: no decoding. Use when the sender publishes linear or data values in an 8-bit
	 *     format — a render target created with Force Linear Gamma, for example.
	 *   - sRGB: force decoding. Only valid for 8-bit frames; asking for it on a 10-bit or float
	 *     frame fails the call and logs, rather than being silently ignored.
	 *
	 * bAsyncReceive (Async Receive (Beta), default false) asks for the asynchronous receive: a worker thread takes
	 * the sender's texture and waits for the copy, and the render thread only queues a copy of the newest completed
	 * image. It may improve delivery under load, but can increase frame-time spikes or image latency. It is per
	 * receiver session, that is per sender name: the first accepted receive for a name decides the mode, every later
	 * call for that name must ask for the same mode or is refused (false, outputs untouched, a throttled warning),
	 * and the mode stays until Close Receiver. Spout Receiver and Spout Receiver With Material share this rule. A
	 * session that asked for it and could not use it (an unsupported format, an unavailable device, a worker
	 * failure) runs synchronously; GetSpoutStreamStatus reports the requested and the effective mode and why.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spout", meta = (AdvancedDisplay = "5",
		ToolTip = "Queues a receive; true means accepted, not completed. For repeated Blueprint material use, prefer Spout Receiver With Material and a stored MID. Keep the outputs for Close Receiver. Async Receive (Beta) is per sender name, fixed when the receiver opens; a call asking for the other mode is refused until Close Receiver."))
	static bool SpoutReceiver(const FName SpoutName,
							  UMaterialInterface* InputMaterial,
							  FName TextureParameterName,
							  UMaterialInstanceDynamic*& OutMat,
							  UTexture2D*& OutTexture,
							  UTextureRenderTarget2D* OptionalOutputRenderTarget = nullptr,
							  ESpoutColorSpace ColorSpace = ESpoutColorSpace::Auto,
							  UPARAM(DisplayName = "Async Receive (Beta)") bool bAsyncReceive = false);

	/**
	 * Receives into an explicitly reusable MID. Create the MID once with Unreal's Create Dynamic
	 * Material Instance node, keep it in a variable and supply it here each tick. Null is allowed
	 * for texture-only use. This node allocates no material. The texture is shared per sender name;
	 * the first accepted color interpretation remains in effect until close or a source format change.
	 * Optional targets catch up from the completed image even when the sender is paused.
	 * bAsyncReceive (Async Receive (Beta), default false) follows the same per-name rule as Spout Receiver: the
	 * first accepted receive for a name decides the mode until Close Receiver, and a call asking for the other
	 * mode is refused meanwhile.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spout", meta = (DisplayName = "Spout Receiver With Material", AdvancedDisplay = "bAsyncReceive"))
	static bool SpoutReceiverWithMaterial(FName SpoutName, UMaterialInstanceDynamic* ConsumerMaterial,
		FName TextureParameterName, UTexture2D*& OutTexture, UTextureRenderTarget2D* OptionalOutputRenderTarget = nullptr,
		ESpoutColorSpace ColorSpace = ESpoutColorSpace::Auto,
		UPARAM(DisplayName = "Async Receive (Beta)") bool bAsyncReceive = false);

	/** Releases only this material parameter binding. Other consumers and the shared session remain
	 * open. Restores the saved texture (or engine default) only if the parameter still points to
	 * this binding's image; subsequent user or other-session edits are preserved. */
	UFUNCTION(BlueprintCallable, Category = "Spout")
	static bool ReleaseReceiverMaterialBinding(FName SpoutName, UMaterialInstanceDynamic* ConsumerMaterial, FName TextureParameterName);

	/**
	 * Tears down a receiver started with SpoutReceiver.
	 *
	 * Cancels any receive still queued for that name, drops the registry entry (releasing the opened
	 * shared texture and cached wraps), clears the Spout texture parameter from EVERY material this
	 * plugin bound it into — not only the most recent one — and releases the plugin's ownership of
	 * the received texture so the engine collects it normally.
	 *
	 * There is one receiver session per sender name, so this closes it for every graph receiving
	 * that name. Those graphs get a fresh texture on their next Spout Receiver call.
	 *
	 * The name decides what is released, not the pointers. Pass the same Out Texture / Out Mat you
	 * got back from SpoutReceiver and both are cleared. Pass something else and it is left exactly as
	 * it was, with a warning: this node will not destroy a texture it did not create, so a wrong
	 * pointer cannot take out another receiver's texture.
	 *
	 * Call when a receiver is no longer needed to avoid per-name resource growth.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spout",
		meta = (ToolTip = "Closes the shared Spout receiver session for that name, for every graph using it, and detaches the Spout texture from every material the plugin bound it into. A texture or material this plugin did not create is never destroyed and is left untouched."))
	static void CloseReceiver(FName SpoutName, UPARAM(ref) UTexture2D*& ReceivedTexture, UPARAM(ref) UMaterialInstanceDynamic*& ReceivedMaterial);

	// --- Info / Utils ---

	/**
	 * Lists every Spout sender currently advertised on this machine.
	 *
	 * Senders is emptied first on every path, so a failed enumeration returns an empty array rather
	 * than the previous result. Entries that vanish mid-enumeration, or whose advertised metadata
	 * fails validation, are skipped rather than reported with placeholder values. Returns false only
	 * when the sender list itself could not be read.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spout",
		meta = (ToolTip = "Lists the Spout senders on this machine. The output array is always cleared first, so it is empty on failure."))
	static bool SpoutInfo(TArray<FSenderStruct>& Senders);

	/**
	 * Looks up one sender by name.
	 *
	 * OutSenderStruct is reset to its defaults before anything can fail, so a missing sender yields
	 * an empty struct rather than the previous query's values.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spout",
		meta = (ToolTip = "Looks up a single Spout sender. The output struct is reset before the query, so it holds no stale data on failure."))
	static bool GetSenderInfo(FName SpoutName, FSenderStruct& OutSenderStruct);

	/**
	 * Sets the maximum number of Spout senders and returns the limit that is actually in effect.
	 *
	 * Returns 0 when nothing was changed: either the requested value was outside the accepted range
	 * (1-1024) or Spout is unavailable. It does not echo the requested value back.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spout",
		meta = (ToolTip = "Sets the Spout sender limit and returns the limit actually in effect. Returns 0 if the value was rejected or Spout is unavailable."))
	static int32 SetMaxSenders(int32 Max);

	/**
	 * Creates a render target suitable for sending over Spout.
	 *
	 * Returns null for invalid dimensions or for a pixel format this plugin cannot share
	 * (supported: B8G8R8A8, R8G8B8A8, A2B10G10R10, FloatRGBA). The returned object is NOT rooted;
	 * keep a reference to it, exactly as with Create Render Target 2D.
	 *
	 * bForceLinearGamma decides what the render target's 8-bit bytes contain, and it is the one
	 * place a gamma shift can be introduced. The sender copies the bytes unchanged and publishes
	 * the plain _UNORM format either way, so receivers cannot tell the two apart from the format:
	 *   - true (default): the render target stores linear values. A receiver on Auto assumes
	 *     8-bit _UNORM is sRGB-encoded, so set that receiver's Color Space to Linear to see the
	 *     values you sent.
	 *   - false: the render target stores sRGB-encoded bytes, the Spout convention for 8-bit
	 *     video, so a receiver on Auto (or any application following the convention) is correct.
	 * 10-bit and float formats are linear in both directions and are unaffected.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spout",
		meta = (ToolTip = "Creates a Spout-compatible render target. Returns null for invalid sizes or unsupported pixel formats. Keep a reference: the result is not rooted. 8-bit targets are published as _UNORM either way. With Force Linear Gamma on, the bytes are linear: set Color Space = Linear on the receiver. With it off, the bytes are sRGB-encoded, which receivers on Auto expect."))
	static UTextureRenderTarget2D* CreateTextureRenderTarget2D(int32 Width = 1024, int32 Height = 768, EPixelFormat Format = EPixelFormat::PF_B8G8R8A8, bool bForceLinearGamma = true);

	/** Reads the current Spout sender limit. Max is set to 0 when it cannot be read. */
	UFUNCTION(BlueprintCallable, Category = "Spout",
		meta = (ToolTip = "Reads the current Spout sender limit. Max is set to 0 when Spout is unavailable."))
	static void GetMaxSenders(int32& Max);

private:
	// Creates the D3D11-on-D3D12 context and Spout helpers on first use.
	static void EnsureSpoutInitialized();

};
