/**
 * Implements texture utility helpers for transient texture lifetime and render target
 * creation used by Blueprint-facing Spout APIs.
 */
#include "SpoutTextureUtils.h"

#include "TextureResource.h"
#include "RenderResource.h"

namespace SpoutTextureUtils
{
	void DestroyTexture(UTexture2D*& Texture)
	{
		if (!Texture)
		{
			return;
		}

		// Transient textures are rooted to avoid GC while in use by the plugin.
		if (Texture->IsRooted())
		{
			Texture->RemoveFromRoot();
		}

		// Ensure the render resource is released before GC.
		// FTextureResource derives from FRenderResource, so no cast is required.
		if (FTextureResource* Resource = Texture->GetResource())
		{
			BeginReleaseResource(Resource);
		}

		Texture->MarkAsGarbage();
		Texture = nullptr;
	}

	bool EnsureTransientTexture(UTexture2D*& Texture, int32 Width, int32 Height, EPixelFormat Format, bool bSRGB)
	{
		// Identity is (size, pixel format, sRGB). A format- or sRGB-only change at the same
		// resolution must still recreate the texture, otherwise a stale-format destination is
		// reused and the per-frame CopyResource/RHIUpdateTexture2D copies into the wrong layout.
		if (Texture
			&& Texture->GetSizeX() == Width
			&& Texture->GetSizeY() == Height
			&& Texture->GetPixelFormat() == Format
			&& (Texture->SRGB != 0) == bSRGB)
		{
			return false;
		}

		DestroyTexture(Texture);

		// Transient textures are created on the game thread and updated on the render thread.
		Texture = UTexture2D::CreateTransient(Width, Height, Format);
		if (Texture)
		{
			// Set the sRGB flag explicitly before UpdateResource so the SRV is built with the
			// matching (sRGB vs linear) view. CreateTransient leaves UTexture's default (SRGB=true),
			// which is correct for 8-bit color but wrong to rely on for linear/data formats.
			Texture->SRGB = bSRGB ? 1 : 0;
			Texture->AddToRoot();
			Texture->UpdateResource();
		}

		return true;
	}

	UTextureRenderTarget2D* CreateRenderTarget2D(int32 Width, int32 Height, EPixelFormat Format, bool bForceLinearGamma)
	{
		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>();
		// Do NOT AddToRoot here: the Blueprint caller receives and stores the return value, whose
		// reference keeps it alive (mirrors UKismetRenderingLibrary::CreateRenderTarget2D). Rooting
		// it would make every created render target a permanent GC root that is never reclaimed.
		// Create a render target for GPU-side writes before sharing via Spout.
		RenderTarget->InitCustomFormat(Width, Height, Format, bForceLinearGamma);
		RenderTarget->AddressX = TextureAddress::TA_Wrap;
		RenderTarget->AddressY = TextureAddress::TA_Wrap;
		RenderTarget->UpdateResource();
		return RenderTarget;
	}
}
