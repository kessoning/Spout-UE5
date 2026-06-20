/**
 * Declares the receiver API that connects Blueprint calls to the Spout receive path
 * implemented in SpoutReceiver.cpp.
 */
#pragma once

#include "CoreMinimal.h"
#include "SpoutBPFunctionLibrary.h"

class FSpoutReceiver
{
public:
	// Receives a shared Spout texture and updates a transient UTexture2D on the render thread.
	static bool Receive(const FName SpoutName, UMaterialInterface* InputMaterial, FName TextureParameterName, UMaterialInstanceDynamic*& OutMat, UTexture2D*& OutTexture, UTextureRenderTarget2D* OptionalOutputRenderTarget);
	// Removes the receiver's registry entry, releasing its opened shared texture and cached wraps.
	static void Close(const FName SpoutName);
};
