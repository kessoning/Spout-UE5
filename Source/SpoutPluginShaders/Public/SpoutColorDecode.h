// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#pragma once

#include "CoreMinimal.h"
#include "RHI.h"

class FRHICommandList;

/**
 * Colour decoding of received frames, for formats whose values a sampler cannot decode (no sRGB view exists).
 *
 * The receiver uses it when a 10-bit frame is received with Color Space = sRGB: the frame's sRGB-encoded values are
 * written, as linear values, into a 16-bit float texture of the same size. See Shaders/Private/SpoutColorDecode.usf.
 */
namespace SpoutColorDecode
{
	/** The format a decoded frame is written in: 16-bit float, linear. */
	constexpr EPixelFormat DecodedFormat = PF_FloatRGBA;

	/**
	 * Records, on RHICmdList, one pass that writes every texel of Source into Destination with the exact sRGB-to-linear
	 * function applied to RGB and alpha unchanged. Source must be shader-readable, Destination render-targetable, both
	 * the same size. The caller states each resource's access before the pass; after it Source is in SRVMask and
	 * Destination in DestinationAfter. Render thread only. False, with nothing recorded, when the shader or a resource
	 * is unavailable or the sizes differ.
	 */
	SPOUTPLUGINSHADERS_API bool DecodeSrgbToLinear(FRHICommandList& RHICmdList,
		FRHITexture* Source, ERHIAccess SourceBefore,
		FRHITexture* Destination, ERHIAccess DestinationBefore, ERHIAccess DestinationAfter);
}
