/**
 * Declares centralized validation for public Blueprint inputs and for metadata that arrives from
 * external Spout senders through shared memory.
 *
 * Everything here is a pure function over plain values: no engine state, no D3D, no Spout. That
 * keeps the rules self-contained and lets every caller validate *before* allocating a resource or
 * queueing render-thread work, so bad input is refused at the boundary rather than part-way
 * through a resource it already created.
 */
#pragma once

#include "CoreMinimal.h"
#include "PixelFormat.h"

namespace SpoutValidation
{
	/**
	 * Maximum UTF-8 byte length of a Spout sender name, excluding the terminator.
	 *
	 * Spout stores names in fixed SpoutMaxSenderNameLen (256) byte fields in shared memory, so the
	 * encoded form plus its null terminator must fit in 256 bytes. This is a *byte* budget, not a
	 * character count: a name of 200 accented or CJK characters is well under any TCHAR limit and
	 * still overflows the field once encoded. SpoutValidation.cpp static_asserts this against the
	 * Spout header so the two cannot drift.
	 */
	static constexpr int32 MaxSenderNameUtf8Bytes = 255;

	/**
	 * Maximum width or height accepted for any texture this plugin creates or opens.
	 * 16384 is the maximum 2D texture dimension at D3D feature level 11_0/11_1, which is the floor
	 * this plugin targets. Anything larger cannot be created by the device regardless of intent.
	 */
	static constexpr int32 MaxTextureDimension = 16384;

	/**
	 * Maximum total pixel count, evaluated in 64-bit.
	 * Equal to MaxTextureDimension squared. Checking the product separately in int64 is what
	 * catches a pair of dimensions that each pass individually but whose product would wrap when
	 * multiplied in 32-bit, and it bounds the allocation implied by an extreme aspect ratio.
	 */
	static constexpr int64 MaxTexturePixels = static_cast<int64>(MaxTextureDimension) * static_cast<int64>(MaxTextureDimension);

	/** Inclusive range accepted by SetMaxSenders. Spout's own default is 10. */
	static constexpr int32 MinMaxSenders = 1;
	static constexpr int32 MaxMaxSenders = 1024;

	/** Outcome of a validation check. Ok is the only success value. */
	enum class EResult : uint8
	{
		Ok,
		/** Name was NAME_None, empty, or whitespace-only. */
		EmptyName,
		/** Name's UTF-8 encoding exceeds MaxSenderNameUtf8Bytes. */
		NameTooLong,
		/** A dimension was zero or negative. */
		InvalidDimensions,
		/** A dimension exceeded MaxTextureDimension, or the pixel count exceeded MaxTexturePixels. */
		DimensionsTooLarge,
		/** Format is not in the supported-format policy. */
		UnsupportedFormat,
		/** Shared handle was null or otherwise unusable. */
		InvalidHandle,
		/** Sample count was not 1; shared textures are always single-sampled. */
		InvalidSampleCount,
		/** A numeric input fell outside its documented accepted range. */
		OutOfRange
	};

	/** Human-readable reason, used in diagnostics. */
	const TCHAR* ToString(EResult Result);

	/**
	 * Validates a sender or receiver name and, on success, produces its null-terminated UTF-8
	 * encoding.
	 *
	 * OutUtf8Name is filled with the encoded bytes *including* a trailing zero, so callers can hand
	 * GetData() straight to the Spout SDK without a separate termination step. It is emptied on
	 * failure, so a rejected name can never leave a partially-encoded buffer behind.
	 */
	EResult ValidateName(FName Name, TArray<ANSICHAR>* OutUtf8Name = nullptr);

	/**
	 * Validates a texture extent. Takes int64 so that both signed Blueprint inputs and unsigned
	 * external metadata convert losslessly before the range check: a negative int32 stays negative
	 * and is rejected, and a 4-billion uint32 stays large and is rejected, neither one wrapping
	 * into a plausible-looking value first.
	 */
	EResult ValidateDimensions(int64 Width, int64 Height);

	/** Validates a shared texture handle received from another process. */
	EResult ValidateSharedHandle(const void* SharedHandle);

	/** Validates a sample count. Only single-sampled surfaces can back a Spout share. */
	EResult ValidateSampleCount(int32 NumSamples);

	/** Validates the argument to SetMaxSenders against [MinMaxSenders, MaxMaxSenders]. */
	EResult ValidateMaxSenders(int32 RequestedMax);

	/** Validates a UE pixel format against the supported-format policy. */
	EResult ValidatePixelFormat(EPixelFormat PixelFormat);

	/**
	 * Validates everything an external sender advertises, in the order that fails cheapest first.
	 * Called before any resource is created or opened for that sender.
	 */
	EResult ValidateExternalSenderMetadata(int64 Width, int64 Height, const void* SharedHandle, uint32 DxgiFormat);
}
