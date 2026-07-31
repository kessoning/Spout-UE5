/**
 * Implements the shared supported-format policy. One table drives both the sender and the
 * receiver so the two directions cannot drift apart.
 */
#include "SpoutFormatPolicy.h"

namespace SpoutFormatPolicy
{
	namespace
	{
		/**
		 * The complete set of shareable formats.
		 *
		 * NativeFormat  - what the protocol or the source resource reports.
		 * SharedFormat  - what the sender creates its shared texture with. Differs from
		 *                 NativeFormat only for typeless entries, which carry no interpretation
		 *                 of their own and are given the one typed view Spout peers expect.
		 * PixelFormat   - what the receiver samples it as.
		 * bPrefersSRGB  - what ESpoutColorSpace::Auto resolves to for this row.
		 * bDeclaresSRGB - whether the DXGI format itself states sRGB (the _UNORM_SRGB variants),
		 *                 as opposed to being assumed sRGB by ecosystem convention.
		 * bSRGBCapable  - whether an sRGB destination view exists for PixelFormat at all, which
		 *                 is what makes an explicit sRGB override representable or not.
		 *
		 * Adding a row here is the only way to widen support, and it widens both directions at
		 * once. The set is deliberately narrow: a format is listed only where both directions are
		 * known to round-trip it correctly, so widening it is an explicit decision rather than a
		 * side effect of some other change.
		 *
		 * The 8-bit rows carry the whole colour-space question. DXGI *can* state sRGB, and this
		 * plugin's sender publishes _UNORM_SRGB when the UE source is an sRGB render target, so
		 * intent survives round-tripping through this plugin. It does not survive every peer:
		 * most Spout applications publish plain _UNORM for gamma-encoded display output, so Auto
		 * treats 8-bit _UNORM as sRGB to match them, and a sender that really does publish linear
		 * 8-bit data is handled by the receiver's explicit Linear override rather than by
		 * guessing differently.
		 */
		struct FFormatEntry
		{
			DXGI_FORMAT NativeFormat;
			DXGI_FORMAT SharedFormat;
			EPixelFormat PixelFormat;
			bool bPrefersSRGB;
			bool bDeclaresSRGB;
			bool bSRGBCapable;
			bool bTypeless;
		};

		const FFormatEntry FormatTable[] =
		{
			// 8-bit color. Gamma-encoded bytes by convention, so Auto decodes as sRGB.
			{ DXGI_FORMAT_B8G8R8A8_UNORM,       DXGI_FORMAT_B8G8R8A8_UNORM,       PF_B8G8R8A8,     true,  false, true,  false },
			{ DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,  DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,  PF_B8G8R8A8,     true,  true,  true,  false },
			{ DXGI_FORMAT_R8G8B8A8_UNORM,       DXGI_FORMAT_R8G8B8A8_UNORM,       PF_R8G8B8A8,     true,  false, true,  false },
			{ DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,  PF_R8G8B8A8,     true,  true,  true,  false },

			// 10-bit and float carry linear/HDR data and must be sampled linearly. UE has no sRGB
			// view for either, which is why an explicit sRGB request against them is rejected.
			{ DXGI_FORMAT_R10G10B10A2_UNORM,    DXGI_FORMAT_R10G10B10A2_UNORM,    PF_A2B10G10R10,  false, false, false, false },
			{ DXGI_FORMAT_R16G16B16A16_FLOAT,   DXGI_FORMAT_R16G16B16A16_FLOAT,   PF_FloatRGBA,    false, false, false, false },

			// Typeless sources: mapped to the typed view of the same layout. This is the
			// intentional reinterpretation ESupport::MappedTyped describes. A typeless resource
			// states no colour space, so the 8-bit ones map to the _UNORM variant -- the same
			// convention, and the same Linear override, as a peer that publishes _UNORM directly.
			{ DXGI_FORMAT_B8G8R8A8_TYPELESS,       DXGI_FORMAT_B8G8R8A8_UNORM,     PF_B8G8R8A8,     true,  false, true,  true },
			{ DXGI_FORMAT_R8G8B8A8_TYPELESS,       DXGI_FORMAT_R8G8B8A8_UNORM,     PF_R8G8B8A8,     true,  false, true,  true },
			{ DXGI_FORMAT_R10G10B10A2_TYPELESS,    DXGI_FORMAT_R10G10B10A2_UNORM,  PF_A2B10G10R10,  false, false, false, true },
			{ DXGI_FORMAT_R16G16B16A16_TYPELESS,   DXGI_FORMAT_R16G16B16A16_FLOAT, PF_FloatRGBA,    false, false, false, true },
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
			// What the shared texture announces, not what the bytes conventionally mean. Only the
			// _UNORM_SRGB rows carry sRGB in the format, and a typeless source maps to a _UNORM
			// variant, so it announces Linear even where the receiver's Auto convention will
			// decode it as sRGB. Reported here so the send path can say which it published.
			Result.DeclaredColorSpace = Entry->bDeclaresSRGB ? ESpoutColorSpace::SRGB : ESpoutColorSpace::Linear;
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

	EColorSpaceResult ResolveReceiverColorSpace(
		const FReceiverFormat& Format,
		ESpoutColorSpace Requested,
		bool& bOutSRGB)
	{
		// Assigned on every path before returning, so a caller that ignores the result still gets a
		// defined value rather than whatever it passed in.
		bOutSRGB = false;

		if (!Format.IsUsable())
		{
			return EColorSpaceResult::UnusableFormat;
		}

		switch (Requested)
		{
		case ESpoutColorSpace::Auto:
			bOutSRGB = Format.bPrefersSRGB;
			return EColorSpaceResult::Ok;

		case ESpoutColorSpace::Linear:
			// Always representable: leaving UTexture::SRGB clear is what every pixel format does by
			// default, so a linear request is honoured for the 8-bit formats and is already the Auto
			// answer for the 10-bit and float ones.
			bOutSRGB = false;
			return EColorSpaceResult::Ok;

		case ESpoutColorSpace::SRGB:
			if (!Format.bSupportsSRGBDecode)
			{
				return EColorSpaceResult::UnsupportedOverride;
			}
			bOutSRGB = true;
			return EColorSpaceResult::Ok;

		default:
			return EColorSpaceResult::UnsupportedOverride;
		}
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
