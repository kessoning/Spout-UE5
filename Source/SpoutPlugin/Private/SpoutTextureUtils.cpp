// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements texture utility helpers for transient texture lifetime and render target
 * creation used by Blueprint-facing Spout APIs.
 */
#include "SpoutTextureUtils.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include "SpoutModule.h"
#include "SpoutValidation.h"
#include "SpoutReceivedTexture.h"

#include "TextureResource.h"
#include "RenderResource.h"

namespace SpoutTextureUtils
{
	bool EnsureTransientTexture(UTexture2D*& Texture, int32 Width, int32 Height, EPixelFormat Format, bool bSRGB)
	{
		int64 FrameBytes = 0;
		if (!SpoutLimits::FrameBytes(Width, Height, Format, FrameBytes)) { Texture = nullptr; return true; }
		// Identity is (size, pixel format, sRGB). A format- or sRGB-only change at the same
		// resolution must still recreate the texture, otherwise a stale-format destination is
		// reused and the per-frame CopyResource/RHIUpdateTexture2D copies into the wrong layout.
		if (Texture
			&& Texture->GetSizeX() == Width
			&& Texture->GetSizeY() == Height
			&& Texture->GetPixelFormat() == Format
			&& (Texture->SRGB != 0) == bSRGB)
		{
			EnsureSpoutReceivedTextureResource(*Texture);
			return false;
		}

		// The outgoing texture is simply dropped from this variable. Its owner releases the last
		// managed reference, and UTexture's own destruction path releases the render resource on the
		// render thread at the right time. A hand-rolled release cannot do that safely: it could run
		// while render commands are still holding the resource.
		// Keep the CPU mip for UTexture2D::UpdateResource compatibility on installed
		// engines. Reserve it plus GPU resource recreation overlap before allocating.
		const auto Reservation = FSpoutResourceReservation::TryReserve(FrameBytes * 3);
		if (!Reservation) { Texture = nullptr; return true; }
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Spout_CreateReceivedTexture);
			Texture = UTexture2D::CreateTransient(Width, Height, Format);
		}
		if (Texture)
		{
			USpoutTextureReservation* Data = NewObject<USpoutTextureReservation>(Texture);
			Data->Reservation = Reservation;
			Texture->AddAssetUserData(Data);
		}

		if (Texture)
		{
			// Set the sRGB flag explicitly before creating the resource so the SRV uses the
			// matching (sRGB vs linear) view. CreateTransient leaves UTexture's default (SRGB=true),
			// which is correct for 8-bit color but wrong to rely on for linear/data formats.
			//
			// Deliberately NOT rooted. The texture is kept alive by FSpoutReceiverOwnership, which
			// can enumerate and release it; a root cannot be enumerated, so it outlives every code
			// path meant to remove it.
			Texture->SRGB = bSRGB ? 1 : 0;
			EnsureSpoutReceivedTextureResource(*Texture);
		}

		return true;
	}

	UTextureRenderTarget2D* CreateRenderTarget2D(int32 Width, int32 Height, EPixelFormat Format, bool bForceLinearGamma)
	{
		// Validated here, at the allocation site, so no caller can reach InitCustomFormat with an
		// invalid extent. InitCustomFormat takes uint32 dimensions, so an unchecked negative int32
		// argument would become an enormous unsigned extent inside the engine rather than an error.
		const SpoutValidation::EResult ExtentResult = SpoutValidation::ValidateDimensions(Width, Height);
		if (ExtentResult != SpoutValidation::EResult::Ok)
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("CreateTextureRenderTarget2D: rejected %dx%d (%s). Dimensions must be 1..%d and the total pixel count at most %lld."),
				Width,
				Height,
				SpoutValidation::ToString(ExtentResult),
				SpoutValidation::MaxTextureDimension,
				SpoutValidation::MaxTexturePixels);
			return nullptr;
		}

		// Only formats the Spout send path can actually publish. Creating a render target this
		// plugin cannot share just defers the failure to the first send.
		const SpoutValidation::EResult FormatResult = SpoutValidation::ValidatePixelFormat(Format);
		if (FormatResult != SpoutValidation::EResult::Ok)
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("CreateTextureRenderTarget2D: pixel format %s cannot be shared over Spout (%s). ")
				TEXT("Supported formats are B8G8R8A8, R8G8B8A8, A2B10G10R10 and FloatRGBA."),
				GetPixelFormatString(Format),
				SpoutValidation::ToString(FormatResult));
			return nullptr;
		}

		int64 FrameBytes = 0;
		if (!SpoutLimits::FrameBytes(Width, Height, Format, FrameBytes)) return nullptr;
		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>();
		// Do NOT AddToRoot here: the Blueprint caller receives and stores the return value, whose
		// reference keeps it alive (mirrors UKismetRenderingLibrary::CreateRenderTarget2D). Rooting
		// it would make every created render target a permanent GC root that is never reclaimed.
		// Create a render target for GPU-side writes before sharing via Spout. Addressing is set before
		// the resource exists, and creation ends with an immediate update, as the engine's own
		// UKismetRenderingLibrary::CreateRenderTarget2D does. A trailing deferred UpdateResource()
		// would re-create the resource with a pending clear that runs at the next scene render, silently
		// wiping anything the caller drew into the target in the frame it was created.
		RenderTarget->AddressX = TextureAddress::TA_Wrap;
		RenderTarget->AddressY = TextureAddress::TA_Wrap;
		RenderTarget->InitCustomFormat(Width, Height, Format, bForceLinearGamma);
		RenderTarget->UpdateResourceImmediate(true);
		return RenderTarget;
	}
}
