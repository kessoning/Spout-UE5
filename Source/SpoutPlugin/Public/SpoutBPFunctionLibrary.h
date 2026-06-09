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

	// Optional output target for receiver copy (currently unused by the plugin).
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

	// --- Senders ---

	/**
	 * Sends a texture to Spout asynchronously via the render thread.
	 * This does NOT stall the game thread, but does perform a GPU copy on the render thread.
	 */
	UFUNCTION(BlueprintCallable, Category="Spout")
	static bool SpoutSender(FName SpoutName,
							ESpoutSendTextureFrom SendTextureFrom,
							UTextureRenderTarget2D* TextureRenderTarget2D);

	UFUNCTION(BlueprintCallable, Category = "Spout")
	static void CloseSender(FName SpoutName);

	// --- Receivers ---

	UFUNCTION(BlueprintCallable, Category = "Spout", meta = (AdvancedDisplay = "5"))
	static bool SpoutReceiver(const FName SpoutName,
							  UMaterialInterface* InputMaterial,
							  FName TextureParameterName,
							  UMaterialInstanceDynamic*& OutMat,
							  UTexture2D*& OutTexture,
							  UTextureRenderTarget2D* OptionalOutputRenderTarget = nullptr);

	/**
	 * Tears down a receiver started with SpoutReceiver: drops its registry entry (releasing the
	 * opened shared texture and cached wraps), destroys the transient ReceivedTexture, and clears the
	 * ReceivedMaterial reference so it no longer samples the destroyed texture. Pass the same
	 * OutTexture / OutMat you got back from SpoutReceiver. Call when a receiver is no longer needed to
	 * avoid per-name resource growth.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spout")
	static void CloseReceiver(FName SpoutName, UPARAM(ref) UTexture2D*& ReceivedTexture, UPARAM(ref) UMaterialInstanceDynamic*& ReceivedMaterial);

	// --- Info / Utils ---

	UFUNCTION(BlueprintCallable, Category = "Spout")
	static bool SpoutInfo(TArray<FSenderStruct>& Senders);

	UFUNCTION(BlueprintCallable, Category = "Spout")
	static bool GetSenderInfo(FName SpoutName, FSenderStruct& OutSenderStruct);

	UFUNCTION(BlueprintCallable, Category = "Spout")
	static int32 SetMaxSenders(int32 Max);

	UFUNCTION(BlueprintCallable, Category = "Spout")
	static UTextureRenderTarget2D* CreateTextureRenderTarget2D(int32 Width = 1024, int32 Height = 768, EPixelFormat Format = EPixelFormat::PF_B8G8R8A8, bool bForceLinearGamma = true);

	UFUNCTION(BlueprintCallable, Category = "Spout")
	static void GetMaxSenders(int32& Max);

private:
	// Creates the D3D11-on-D3D12 context and Spout helpers on first use.
	static void EnsureSpoutInitialized();

};
