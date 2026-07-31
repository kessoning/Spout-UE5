/**
 * Declares small texture utility helpers for creating and managing transient textures
 * and render targets used by the Spout receiver and sender flows.
 */
#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"

namespace SpoutTextureUtils
{
	/**
	 * Ensures a transient texture exists with the requested size/format/color space, recreating it
	 * when any of size, pixel format or sRGB differ. Returns whether a new texture was created, so
	 * the caller knows when to take ownership of the replacement.
	 *
	 * Creates only. It does not root the texture and it does not destroy the one it replaces:
	 * lifetime belongs to FSpoutReceiverOwnership, and release is dropping that reference and
	 * letting normal UObject destruction and GC run. Rooting every texture created here and
	 * hand-releasing the old one through BeginReleaseResource plus MarkAsGarbage is what would
	 * make a lost reference a permanent leak, and would let a mismatched close destroy somebody
	 * else's texture.
	 */
	bool EnsureTransientTexture(UTexture2D*& Texture, int32 Width, int32 Height, EPixelFormat Format, bool bSRGB);
	// Creates a render target suitable for sending via Spout.
	// Returns nullptr, with a logged reason, for invalid dimensions or a pixel format the Spout send
	// path cannot publish. Validation happens here so it cannot be bypassed by an internal caller.
	UTextureRenderTarget2D* CreateRenderTarget2D(int32 Width, int32 Height, EPixelFormat Format, bool bForceLinearGamma);
}
