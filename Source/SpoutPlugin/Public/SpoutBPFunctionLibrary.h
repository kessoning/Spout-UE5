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
 * ecosystem publishes plain 8-bit _UNORM for ordinary gamma-encoded display output, so the format
 * alone is not always enough. This enum is the override for the case where it is not.
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

UCLASS(ClassGroup = Spout, Blueprintable)
class SPOUTPLUGIN_API USpoutBPFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
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
	 * This does NOT stall the game thread, but does perform a GPU copy on the render thread.
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
	 *
	 * OutMat and OutTexture are outputs, not ownership. The received texture belongs to the plugin,
	 * which keeps it alive for as long as the receiver is open and releases it on Close Receiver or
	 * on session shutdown; Out Texture is the plugin's texture, handed to you to use. Keep the
	 * values this node returns and pass them back on subsequent calls and to Close Receiver. They
	 * are only ever assigned on success and are deliberately left untouched on failure, so a
	 * temporary sender dropout does not orphan anything.
	 *
	 * There is one receiver session per sender name. Every graph receiving the same name shares that
	 * one received texture — but each gets its OWN material instance, created from its OWN Input
	 * Material. A graph is never handed another graph's material because the sender names matched.
	 * Close Receiver on that name closes the shared session for every graph using it, and detaches
	 * the Spout texture parameter from every material the plugin bound it into.
	 *
	 * OptionalOutputRenderTarget must match the incoming frame's pixel format and be single-sampled.
	 * A mismatched target is left completely untouched and the mismatch is logged; the call still
	 * succeeds and OutTexture still updates. Size mismatches are resolved by resizing the target.
	 *
	 * The frame is only copied when the sender has actually published a new one, so a sender running
	 * slower than this graph's tick costs nothing on the frames in between.
	 *
	 * ColorSpace decides how the received bytes are interpreted:
	 *   - Auto (default): 8-bit frames decode as sRGB, 10-bit and 16-bit float frames stay linear.
	 *     This matches what other Spout applications publish.
	 *   - Linear: no decoding. Use when the sender publishes linear or data values in an 8-bit
	 *     format — a render target created with Force Linear Gamma, for example.
	 *   - sRGB: force decoding. Only valid for 8-bit frames; asking for it on a 10-bit or float
	 *     frame fails the call and logs, rather than being silently ignored.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spout", meta = (AdvancedDisplay = "5",
		ToolTip = "Queues a Spout receive on the render thread. Returns true when the sender was found and the copy was queued - not that the copy completed. Only copies when the sender published a new frame. Keep Out Texture / Out Mat and pass them to Close Receiver."))
	static bool SpoutReceiver(const FName SpoutName,
							  UMaterialInterface* InputMaterial,
							  FName TextureParameterName,
							  UMaterialInstanceDynamic*& OutMat,
							  UTexture2D*& OutTexture,
							  UTextureRenderTarget2D* OptionalOutputRenderTarget = nullptr,
							  ESpoutColorSpace ColorSpace = ESpoutColorSpace::Auto);

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
	 * bForceLinearGamma decides what colour space the sender advertises for 8-bit formats, and it
	 * is the one place a gamma shift can be introduced:
	 *   - true (default): the render target is linear and the sender publishes a plain _UNORM
	 *     format. A receiver on Auto assumes 8-bit _UNORM is sRGB, so set that receiver's Color
	 *     Space to Linear to see the values you sent.
	 *   - false: the render target is sRGB and the sender publishes _UNORM_SRGB, which states the
	 *     colour space in the format itself, so a receiver on Auto is already correct.
	 * 10-bit and float formats are linear in both directions and are unaffected.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spout",
		meta = (ToolTip = "Creates a Spout-compatible render target. Returns null for invalid sizes or unsupported pixel formats. Keep a reference: the result is not rooted. Force Linear Gamma publishes a linear _UNORM format - pair it with Color Space = Linear on the receiver."))
	static UTextureRenderTarget2D* CreateTextureRenderTarget2D(int32 Width = 1024, int32 Height = 768, EPixelFormat Format = EPixelFormat::PF_B8G8R8A8, bool bForceLinearGamma = true);

	/** Reads the current Spout sender limit. Max is set to 0 when it cannot be read. */
	UFUNCTION(BlueprintCallable, Category = "Spout",
		meta = (ToolTip = "Reads the current Spout sender limit. Max is set to 0 when Spout is unavailable."))
	static void GetMaxSenders(int32& Max);

private:
	// Creates the D3D11-on-D3D12 context and Spout helpers on first use.
	static void EnsureSpoutInitialized();

};
