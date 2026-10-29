// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the shared supported-format policy. One table drives both the sender and the
 * receiver so the two directions cannot drift apart.
 */
#include "SpoutFormatPolicy.h"
#include "SpoutColorDecode.h"

namespace SpoutFormatPolicy
{
	namespace
	{
		/**
		 * The complete set of shareable formats.
		 *
		 * NativeFormat  - what the protocol or the source resource reports.
		 * SharedFormat  - what the sender creates its shared texture with. Differs from
		 *                 NativeFormat for typeless entries, which carry no interpretation of
		 *                 their own and are given the one typed view Spout peers expect, and for
		 *                 the 8-bit _UNORM_SRGB entries, which are shared as _UNORM (see below).
		 * PixelFormat   - what the receiver samples it as.
		 * bPrefersSRGB  - what ESpoutColorSpace::Auto resolves to for this row.
		 * bDeclaresSRGB - whether the DXGI format itself states sRGB (the _UNORM_SRGB variants),
		 *                 as opposed to being assumed sRGB by ecosystem convention.
		 * bSRGBCapable  - whether an sRGB destination view exists for PixelFormat at all, which
		 *                 is what lets the sampler honour an explicit sRGB request.
		 * bShaderSRGB   - whether an explicit sRGB request is honoured instead by a shader decode
		 *                 into linear 16-bit float (SpoutColorDecode.h): the 10-bit rows. Their
		 *                 values may be display-encoded (an Unreal viewport stream) or linear (a
		 *                 10-bit render target); the format cannot say which, so Auto stays linear.
		 *
		 * Adding a row here is the only way to widen support, and it widens both directions at
		 * once. The set is deliberately narrow: a format is listed only where both directions are
		 * known to round-trip it correctly, so widening it is an explicit decision rather than a
		 * side effect of some other change.
		 *
		 * The 8-bit rows carry the whole colour-space question. DXGI *can* state sRGB, but the
		 * sender never publishes _UNORM_SRGB: SpoutGL receivers (Spout 2.007's OpenGL/DX interop
		 * path, used by the Spout demo applications) check the shared texture's real format
		 * against a list that has no B8G8R8A8_UNORM_SRGB and refuse the sender. Every 8-bit
		 * source is therefore shared as _UNORM with its bytes copied unchanged, which is also what
		 * most Spout applications publish for gamma-encoded display output. The price is that the
		 * format does not state the source's colour intent: Auto treats 8-bit _UNORM as sRGB to
		 * match the ecosystem convention, and a sender that really does publish linear 8-bit data
		 * is handled by the receiver's explicit Linear override rather than by guessing
		 * differently. _UNORM_SRGB stays accepted on receive, from peers that do publish it.
		 */
		struct FFormatEntry
		{
			DXGI_FORMAT NativeFormat;
			DXGI_FORMAT SharedFormat;
			EPixelFormat PixelFormat;
			bool bPrefersSRGB;
			bool bDeclaresSRGB;
			bool bSRGBCapable;
			bool bShaderSRGB;
			bool bTypeless;
		};

		const FFormatEntry FormatTable[] =
		{
			// 8-bit color. Gamma-encoded bytes by convention, so Auto decodes as sRGB.
			{ DXGI_FORMAT_B8G8R8A8_UNORM,       DXGI_FORMAT_B8G8R8A8_UNORM,       PF_B8G8R8A8,     true,  false, true,  false, false },
			{ DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,  DXGI_FORMAT_B8G8R8A8_UNORM,       PF_B8G8R8A8,     true,  true,  true,  false, false },
			{ DXGI_FORMAT_R8G8B8A8_UNORM,       DXGI_FORMAT_R8G8B8A8_UNORM,       PF_R8G8B8A8,     true,  false, true,  false, false },
			{ DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,  DXGI_FORMAT_R8G8B8A8_UNORM,       PF_R8G8B8A8,     true,  true,  true,  false, false },

			// 10-bit and float are sampled linearly by Auto. UE has no sRGB view for either: an explicit sRGB request is
			// decoded by a shader for 10-bit (which may carry display values, as a viewport stream does) and refused for float.
			{ DXGI_FORMAT_R10G10B10A2_UNORM,    DXGI_FORMAT_R10G10B10A2_UNORM,    PF_A2B10G10R10,  false, false, false, true,  false },
			{ DXGI_FORMAT_R16G16B16A16_FLOAT,   DXGI_FORMAT_R16G16B16A16_FLOAT,   PF_FloatRGBA,    false, false, false, false, false },

			// Typeless sources: mapped to the typed view of the same layout. This is the
			// intentional reinterpretation ESupport::MappedTyped describes. A typeless resource
			// states no colour space, so the 8-bit ones map to the _UNORM variant -- the same
			// convention, and the same Linear override, as a peer that publishes _UNORM directly.
			{ DXGI_FORMAT_B8G8R8A8_TYPELESS,       DXGI_FORMAT_B8G8R8A8_UNORM,     PF_B8G8R8A8,     true,  false, true,  false, true },
			{ DXGI_FORMAT_R8G8B8A8_TYPELESS,       DXGI_FORMAT_R8G8B8A8_UNORM,     PF_R8G8B8A8,     true,  false, true,  false, true },
			{ DXGI_FORMAT_R10G10B10A2_TYPELESS,    DXGI_FORMAT_R10G10B10A2_UNORM,  PF_A2B10G10R10,  false, false, false, true,  true },
			{ DXGI_FORMAT_R16G16B16A16_TYPELESS,   DXGI_FORMAT_R16G16B16A16_FLOAT, PF_FloatRGBA,    false, false, false, false, true },
		};

		const FFormatEntry* FindEntry(DXGI_FORMAT NativeFormat)
		{
			for (const FFormatEntry& Entry : FormatTable)
			{
				if (Entry.NativeFormat == NativeFormat)
				{
					return &Entry;
				}
			}
			return nullptr;
		}
	}

	const TCHAR* ToString(ESupport Support)
	{
		switch (Support)
		{
		case ESupport::Supported:   return TEXT("Supported");
		case ESupport::MappedTyped: return TEXT("SupportedViaTypedMapping");
		case ESupport::Unsupported: return TEXT("Unsupported");
		case ESupport::Unknown:     return TEXT("Unknown");
		default:                    return TEXT("Invalid");
		}
	}

	const TCHAR* ToString(EOutputTargetStatus Status)
	{
		switch (Status)
		{
		case EOutputTargetStatus::Compatible:            return TEXT("Compatible");
		case EOutputTargetStatus::SizeMismatch:          return TEXT("SizeMismatch");
		case EOutputTargetStatus::FormatMismatch:        return TEXT("FormatMismatch");
		case EOutputTargetStatus::SampleCountMismatch:   return TEXT("SampleCountMismatch");
		default:                                         return TEXT("Invalid");
		}
	}

	FSenderFormat ResolveSenderFormat(DXGI_FORMAT NativeFormat, bool bSourceSRGB)
	{
		// The table already shares every 8-bit source as _UNORM. The render target's sRGB flag
		// only records what its bytes are, for diagnostics; it never changes the shared format
		// (see FSenderFormat::bSourceSRGB for why the tag is not published).
		FSenderFormat Result = ResolveSenderFormat(NativeFormat);
		const bool bEightBit = Result.SharedFormat == DXGI_FORMAT_B8G8R8A8_UNORM || Result.SharedFormat == DXGI_FORMAT_R8G8B8A8_UNORM;
		Result.bSourceSRGB = Result.bSourceSRGB || (bEightBit && bSourceSRGB);
		return Result;
	}

	FSenderFormat ResolveSenderFormat(DXGI_FORMAT NativeFormat)
	{
		FSenderFormat Result;

		if (NativeFormat == DXGI_FORMAT_UNKNOWN)
		{
			// A genuinely unknown source layout. Creating a BGRA8 shared texture here would
			// advertise a layout the source does not have, so peers would misread every frame.
			Result.Support = ESupport::Unknown;
			return Result;
		}

		if (const FFormatEntry* Entry = FindEntry(NativeFormat))
		{
			Result.Support = Entry->bTypeless ? ESupport::MappedTyped : ESupport::Supported;
			Result.SharedFormat = Entry->SharedFormat;
			// What the source format itself states about its bytes. Only the _UNORM_SRGB rows say
			// sRGB; a typeless or plain _UNORM source states nothing. Either way the share is
			// _UNORM, so this is not published.
			Result.bSourceSRGB = Entry->bDeclaresSRGB;
			return Result;
		}

		// Recognised by DXGI but absent from the table: compressed, planar, depth, single- and
		// two-channel, 32-bit float, and everything else this plugin does not share.
		Result.Support = ESupport::Unsupported;
		return Result;
	}

	FReceiverFormat ResolveReceiverFormat(uint32 DxgiFormat)
	{
		FReceiverFormat Result;

		const DXGI_FORMAT NativeFormat = static_cast<DXGI_FORMAT>(DxgiFormat);
		if (NativeFormat == DXGI_FORMAT_UNKNOWN)
		{
			Result.Support = ESupport::Unknown;
			return Result;
		}

		if (const FFormatEntry* Entry = FindEntry(NativeFormat))
		{
			Result.Support = Entry->bTypeless ? ESupport::MappedTyped : ESupport::Supported;
			Result.PixelFormat = Entry->PixelFormat;
			Result.bPrefersSRGB = Entry->bPrefersSRGB;
			Result.bFormatDeclaresSRGB = Entry->bDeclaresSRGB;
			Result.bSupportsSRGBDecode = Entry->bSRGBCapable;
			Result.bSupportsShaderSRGBDecode = Entry->bShaderSRGB;
			return Result;
		}

		Result.Support = ESupport::Unsupported;
		return Result;
	}

	const TCHAR* ToString(EColorSpaceResult Result)
	{
		switch (Result)
		{
		case EColorSpaceResult::Ok:                  return TEXT("Ok");
		case EColorSpaceResult::UnusableFormat:      return TEXT("UnusableFormat");
		case EColorSpaceResult::UnsupportedOverride: return TEXT("UnsupportedOverride");
		default:                                     return TEXT("Invalid");
		}
	}

	const TCHAR* ToString(ESpoutColorSpace ColorSpace)
	{
		switch (ColorSpace)
		{
		case ESpoutColorSpace::Auto:   return TEXT("Auto");
		case ESpoutColorSpace::SRGB:   return TEXT("sRGB");
		case ESpoutColorSpace::Linear: return TEXT("Linear");
		default:                       return TEXT("Invalid");
		}
	}

	EColorSpaceResult ResolveReceiverInterpretation(
		const FReceiverFormat& Format,
		ESpoutColorSpace Requested,
		FReceiveInterpretation& OutInterpretation)
	{
		// Assigned on every path before returning, so a caller that ignores the result still gets a
		// defined value rather than whatever it passed in.
		OutInterpretation = FReceiveInterpretation();

		if (!Format.IsUsable())
		{
			return EColorSpaceResult::UnusableFormat;
		}

		switch (Requested)
		{
		case ESpoutColorSpace::Auto:
			OutInterpretation.TexturePixelFormat = Format.PixelFormat;
			OutInterpretation.bSRGB = Format.bPrefersSRGB;
			return EColorSpaceResult::Ok;

		case ESpoutColorSpace::Linear:
			// Always representable: leaving UTexture::SRGB clear is what every pixel format does by
			// default, so a linear request is honoured for the 8-bit formats and is already the Auto
			// answer for the 10-bit and float ones.
			OutInterpretation.TexturePixelFormat = Format.PixelFormat;
			return EColorSpaceResult::Ok;

		case ESpoutColorSpace::SRGB:
			if (Format.bSupportsSRGBDecode)
			{
				OutInterpretation.TexturePixelFormat = Format.PixelFormat;
				OutInterpretation.bSRGB = true;
				return EColorSpaceResult::Ok;
			}
			if (Format.bSupportsShaderSRGBDecode)
			{
				// No sRGB view exists, so the sampler cannot decode it: the frame is decoded once per received frame
				// into a linear 16-bit float texture, which keeps the 10-bit code's precision.
				OutInterpretation.TexturePixelFormat = SpoutColorDecode::DecodedFormat;
				OutInterpretation.Decode = EReceiveDecode::SrgbToLinear;
				return EColorSpaceResult::Ok;
			}
			return EColorSpaceResult::UnsupportedOverride;

		default:
			return EColorSpaceResult::UnsupportedOverride;
		}
	}

	EColorSpaceResult ResolveReceiverColorSpace(
		const FReceiverFormat& Format,
		ESpoutColorSpace Requested,
		bool& bOutSRGB)
	{
		FReceiveInterpretation Interpretation;
		const EColorSpaceResult Result = ResolveReceiverInterpretation(Format, Requested, Interpretation);
		bOutSRGB = Interpretation.bSRGB;
		return Result;
	}

	bool IsSupportedSendPixelFormat(EPixelFormat PixelFormat)
	{
		for (const FFormatEntry& Entry : FormatTable)
		{
			if (Entry.PixelFormat == PixelFormat)
			{
				return true;
			}
		}
		return false;
	}

	FString DescribeDxgiFormat(uint32 DxgiFormat)
	{
		const TCHAR* Name = nullptr;
		switch (static_cast<DXGI_FORMAT>(DxgiFormat))
		{
		case DXGI_FORMAT_UNKNOWN:                Name = TEXT("DXGI_FORMAT_UNKNOWN"); break;
		case DXGI_FORMAT_B8G8R8A8_UNORM:         Name = TEXT("DXGI_FORMAT_B8G8R8A8_UNORM"); break;
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:    Name = TEXT("DXGI_FORMAT_B8G8R8A8_UNORM_SRGB"); break;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:      Name = TEXT("DXGI_FORMAT_B8G8R8A8_TYPELESS"); break;
		case DXGI_FORMAT_R8G8B8A8_UNORM:         Name = TEXT("DXGI_FORMAT_R8G8B8A8_UNORM"); break;
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:    Name = TEXT("DXGI_FORMAT_R8G8B8A8_UNORM_SRGB"); break;
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:      Name = TEXT("DXGI_FORMAT_R8G8B8A8_TYPELESS"); break;
		case DXGI_FORMAT_R10G10B10A2_UNORM:      Name = TEXT("DXGI_FORMAT_R10G10B10A2_UNORM"); break;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:   Name = TEXT("DXGI_FORMAT_R10G10B10A2_TYPELESS"); break;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:     Name = TEXT("DXGI_FORMAT_R16G16B16A16_FLOAT"); break;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:  Name = TEXT("DXGI_FORMAT_R16G16B16A16_TYPELESS"); break;
		case DXGI_FORMAT_R16G16B16A16_UNORM:     Name = TEXT("DXGI_FORMAT_R16G16B16A16_UNORM"); break;
		case DXGI_FORMAT_R32G32B32A32_FLOAT:     Name = TEXT("DXGI_FORMAT_R32G32B32A32_FLOAT"); break;
		case DXGI_FORMAT_R11G11B10_FLOAT:        Name = TEXT("DXGI_FORMAT_R11G11B10_FLOAT"); break;
		case DXGI_FORMAT_BC1_UNORM:              Name = TEXT("DXGI_FORMAT_BC1_UNORM"); break;
		case DXGI_FORMAT_BC3_UNORM:              Name = TEXT("DXGI_FORMAT_BC3_UNORM"); break;
		case DXGI_FORMAT_BC7_UNORM:              Name = TEXT("DXGI_FORMAT_BC7_UNORM"); break;
		case DXGI_FORMAT_NV12:                   Name = TEXT("DXGI_FORMAT_NV12"); break;
		case DXGI_FORMAT_P010:                   Name = TEXT("DXGI_FORMAT_P010"); break;
		case DXGI_FORMAT_D32_FLOAT:              Name = TEXT("DXGI_FORMAT_D32_FLOAT"); break;
		default:                                 break;
		}

		if (Name)
		{
			return FString::Printf(TEXT("%s (%u)"), Name, DxgiFormat);
		}
		return FString::Printf(TEXT("DXGI format %u"), DxgiFormat);
	}

	EOutputTargetStatus CheckOutputTarget(
		int32 SourceWidth,
		int32 SourceHeight,
		EPixelFormat SourceFormat,
		int32 DestWidth,
		int32 DestHeight,
		EPixelFormat DestFormat,
		int32 DestNumSamples)
	{
		if (DestWidth != SourceWidth || DestHeight != SourceHeight)
		{
			return EOutputTargetStatus::SizeMismatch;
		}

		if (DestFormat != SourceFormat)
		{
			return EOutputTargetStatus::FormatMismatch;
		}

		// CopyResource requires identical sample counts, and the shared texture is always
		// single-sampled.
		if (DestNumSamples != 1)
		{
			return EOutputTargetStatus::SampleCountMismatch;
		}

		return EOutputTargetStatus::Compatible;
	}
}
