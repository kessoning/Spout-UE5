/**
 * Declares the single supported-format policy shared by the Spout sender and receiver paths.
 *
 * Both directions resolve formats here so they cannot disagree. The risk this guards against is
 * silent reinterpretation: mapping an unhandled DXGI format onto PF_B8G8R8A8, or substituting
 * BGRA8 when a native format resolves to DXGI_FORMAT_UNKNOWN, hands the caller an unrelated
 * memory layout and shows up as dropped, stale or corrupted-looking output rather than as an
 * error anyone can act on.
 *
 * The tables below are the only place a format is declared shareable. Anything absent is
 * rejected deterministically by both paths, so a format either round-trips correctly or is
 * refused with a diagnostic naming it.
 */
#pragma once

#include "CoreMinimal.h"
#include "PixelFormat.h"
#include "SpoutBPFunctionLibrary.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <dxgiformat.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace SpoutFormatPolicy
{
	/** How a native format relates to the set this plugin can share. */
	enum class ESupport : uint8
	{
		/** Explicitly shareable; used exactly as the protocol reports it. */
		Supported,
		/**
		 * Shareable through a deliberate typeless-to-typed mapping. A typeless resource carries no
		 * interpretation of its own, so the plugin picks the one typed view Spout peers expect.
		 * This is a documented reinterpretation, not a fallback.
		 */
		MappedTyped,
		/** Recognised, but this plugin cannot share it (no matching layout on the other side). */
		Unsupported,
		/** DXGI_FORMAT_UNKNOWN, or a format the policy does not recognise at all. */
		Unknown
	};

	/** Human-readable support classification, used in diagnostics. */
	const TCHAR* ToString(ESupport Support);

	/** Sender-side resolution: native source format to the DXGI format of the shared texture. */
	struct FSenderFormat
	{
		ESupport Support = ESupport::Unknown;
		/** DXGI format to create the shared texture with. Only meaningful when IsUsable(). */
		DXGI_FORMAT SharedFormat = DXGI_FORMAT_UNKNOWN;
		/**
		 * The colour space the shared format announces to peers.
		 *
		 * SRGB only for the _UNORM_SRGB variants, which say so in the format itself. Everything
		 * else is Linear as far as the *protocol* is concerned; whether an 8-bit _UNORM feed
		 * actually carries gamma-encoded bytes is a convention, not something the format states,
		 * and that is exactly why the receiver has an override.
		 */
		ESpoutColorSpace DeclaredColorSpace = ESpoutColorSpace::Linear;

		bool IsUsable() const
		{
			return Support == ESupport::Supported || Support == ESupport::MappedTyped;
		}
	};

	/** Receiver-side resolution: advertised DXGI format to the UE pixel format to sample it as. */
	struct FReceiverFormat
	{
		ESupport Support = ESupport::Unknown;
		/** UE pixel format for the destination texture. Only meaningful when IsUsable(). */
		EPixelFormat PixelFormat = PF_Unknown;
		/**
		 * What ESpoutColorSpace::Auto resolves to. 8-bit color formats carry gamma-encoded bytes
		 * (which is also what this plugin's sender publishes for UE sRGB render targets); 10-bit
		 * and float formats carry linear/HDR data.
		 */
		bool bPrefersSRGB = false;
		/**
		 * Whether the format itself declares sRGB, rather than being assumed sRGB by convention.
		 * True only for DXGI's _UNORM_SRGB variants. Diagnostics use this to tell a sender that
		 * stated its intent from one that did not.
		 */
		bool bFormatDeclaresSRGB = false;
		/**
		 * Whether an sRGB destination view exists for this pixel format at all.
		 *
		 * True for the 8-bit formats and false for A2B10G10R10 and FloatRGBA: UE has no sRGB
		 * variant of those, so setting UTexture::SRGB on them is silently ignored. An explicit
		 * sRGB request against such a format is therefore rejected rather than quietly dropped.
		 */
		bool bSupportsSRGBDecode = false;

		bool IsUsable() const
		{
			return Support == ESupport::Supported || Support == ESupport::MappedTyped;
		}
	};

	/**
	 * Classifies a sender source's native DXGI format.
	 * Never substitutes a different layout: an unknown or unsupported source is reported as such
	 * so the caller can refuse to create a shared texture that misrepresents it.
	 */
	FSenderFormat ResolveSenderFormat(DXGI_FORMAT NativeFormat);

	/**
	 * Classifies the DXGI format an external sender advertises.
	 * Takes uint32 because Spout reports the format as a DWORD through shared memory, and the
	 * value therefore arrives unvalidated from another process.
	 */
	FReceiverFormat ResolveReceiverFormat(uint32 DxgiFormat);

	/**
	 * Whether a UE pixel format is one this plugin can publish. Used to reject unsupported
	 * formats in the public render-target factory before any resource is allocated.
	 * Kept symmetric with the receiver table: these are exactly the pixel formats
	 * ResolveReceiverFormat can produce.
	 */
	bool IsSupportedSendPixelFormat(EPixelFormat PixelFormat);

	/** Diagnostic name for a DXGI format value, including the numeric value for unknown ones. */
	FString DescribeDxgiFormat(uint32 DxgiFormat);

	/** Outcome of resolving a receiver's requested colour space against an advertised format. */
	enum class EColorSpaceResult : uint8
	{
		/** The request is representable; bOutSRGB carries the destination's decode setting. */
		Ok,
		/** The format is not one this plugin shares, so there is no colour space to resolve. */
		UnusableFormat,
		/**
		 * The requested colour space cannot be represented for this format -- currently only
		 * ESpoutColorSpace::SRGB against a 10-bit or float format. Rejected rather than
		 * approximated: setting an sRGB flag UE ignores would change nothing about the output
		 * while reporting success, so the caller who asked for it would never find out.
		 */
		UnsupportedOverride
	};

	const TCHAR* ToString(EColorSpaceResult Result);

	/** Human-readable colour space, used in diagnostics and documentation. */
	const TCHAR* ToString(ESpoutColorSpace ColorSpace);

	/**
	 * The one colour-space contract, applied to a resolved receiver format.
	 *
	 * Auto follows the format's documented convention; Linear is always representable; SRGB is
	 * representable only where an sRGB destination view exists. bOutSRGB is the value written to
	 * UTexture::SRGB, which SpoutTextureUtils::EnsureTransientTexture treats as part of the
	 * destination texture's identity -- so changing the colour space at runtime recreates the
	 * destination rather than reinterpreting the existing one.
	 *
	 * Pure comparison with no engine state.
	 */
	EColorSpaceResult ResolveReceiverColorSpace(
		const FReceiverFormat& Format,
		ESpoutColorSpace Requested,
		bool& bOutSRGB);

	/** Result of comparing an optional output render target against the received frame. */
	enum class EOutputTargetStatus : uint8
	{
		/** Every attribute matches; the target can be copied into. */
		Compatible,
		/** Extent differs, so a full-surface CopyResource is invalid. */
		SizeMismatch,
		/** Pixel format differs; D3D drops the copy and the CPU path reinterprets bytes. */
		FormatMismatch,
		/** Destination is multisampled; CopyResource requires matching sample counts. */
		SampleCountMismatch
	};

	const TCHAR* ToString(EOutputTargetStatus Status);

	/**
	 * Decides whether an optional caller-supplied render target may take a copy of the received
	 * frame. A mismatched target must be excluded from the copy entirely and left untouched:
	 * the GPU path's CopyResource between differing formats or extents is dropped by D3D, and the
	 * CPU path writes bytes the destination reinterprets. Either way the caller sees a black or
	 * stale target, so the mismatch is reported instead of attempted.
	 *
	 * Pure comparison with no engine state.
	 */
	EOutputTargetStatus CheckOutputTarget(
		int32 SourceWidth,
		int32 SourceHeight,
		EPixelFormat SourceFormat,
		int32 DestWidth,
		int32 DestHeight,
		EPixelFormat DestFormat,
		int32 DestNumSamples);
}
