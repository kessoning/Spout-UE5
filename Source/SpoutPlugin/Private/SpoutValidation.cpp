// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements centralized input and external-metadata validation.
 */
#include "SpoutValidation.h"
#include "SpoutFormatPolicy.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include "SpoutDX/SpoutSenderNames.h"
#include "Windows/HideWindowsPlatformTypes.h"
#ifdef GetNextSibling
#undef GetNextSibling
#endif

// Keep the documented byte budget tied to the SDK's own field size. If a future Spout release
// changes SpoutMaxSenderNameLen this fails to compile instead of silently truncating names.
static_assert(
	SpoutValidation::MaxSenderNameUtf8Bytes == SpoutMaxSenderNameLen - 1,
	"MaxSenderNameUtf8Bytes must be SpoutMaxSenderNameLen minus one byte for the null terminator.");
#endif

namespace SpoutValidation
{
	const TCHAR* ToString(EResult Result)
	{
		switch (Result)
		{
		case EResult::Ok:                  return TEXT("Ok");
		case EResult::EmptyName:           return TEXT("EmptyName");
		case EResult::NameTooLong:         return TEXT("NameTooLong");
		case EResult::InvalidDimensions:   return TEXT("InvalidDimensions");
		case EResult::DimensionsTooLarge:  return TEXT("DimensionsTooLarge");
		case EResult::UnsupportedFormat:   return TEXT("UnsupportedFormat");
		case EResult::InvalidHandle:       return TEXT("InvalidHandle");
		case EResult::InvalidSampleCount:  return TEXT("InvalidSampleCount");
		case EResult::OutOfRange:          return TEXT("OutOfRange");
		default:                           return TEXT("Invalid");
		}
	}

	EResult ValidateName(FName Name, TArray<ANSICHAR>* OutUtf8Name)
	{
		if (OutUtf8Name)
		{
			// Reset up front so every failure path leaves an empty buffer rather than stale bytes.
			OutUtf8Name->Reset();
		}

		if (Name.IsNone())
		{
			return EResult::EmptyName;
		}

		const FString NameString = Name.ToString();
		if (NameString.IsEmpty() || NameString.TrimStartAndEnd().IsEmpty())
		{
			return EResult::EmptyName;
		}

		// Measure the UTF-8 encoding, not the TCHAR count: Spout's name field is a fixed byte
		// array, so a short string of multi-byte characters can still overflow it.
		const FTCHARToUTF8 Converter(*NameString);
		const int32 Utf8Length = Converter.Length();
		if (Utf8Length <= 0)
		{
			return EResult::EmptyName;
		}

		if (Utf8Length > MaxSenderNameUtf8Bytes)
		{
			return EResult::NameTooLong;
		}

		if (OutUtf8Name)
		{
			// Size for the encoded bytes plus a terminator, zero-filled, so GetData() is always a
			// valid C string even if the copy below were ever short.
			OutUtf8Name->SetNumZeroed(Utf8Length + 1);
			FMemory::Memcpy(OutUtf8Name->GetData(), Converter.Get(), Utf8Length);
			(*OutUtf8Name)[Utf8Length] = '\0';
		}

		return EResult::Ok;
	}

	EResult ValidateDimensions(int64 Width, int64 Height)
	{
		if (Width <= 0 || Height <= 0)
		{
			return EResult::InvalidDimensions;
		}

		if (Width > MaxTextureDimension || Height > MaxTextureDimension)
		{
			return EResult::DimensionsTooLarge;
		}

		// Evaluated in int64: both operands are already bounded above, so this cannot itself
		// overflow, and it rejects extreme aspect ratios whose product is still unreasonable.
		if (Width * Height > MaxTexturePixels)
		{
			return EResult::DimensionsTooLarge;
		}

		return EResult::Ok;
	}

	EResult ValidateSharedHandle(const void* SharedHandle)
	{
		// Spout publishes the handle as a uint32 through shared memory, so a dead or never-written
		// sender slot surfaces as null. INVALID_HANDLE_VALUE is also rejected: OpenSharedResource
		// would fail on it, and treating it as valid would mean opening resources per frame for a
		// sender that cannot be read.
		if (SharedHandle == nullptr)
		{
			return EResult::InvalidHandle;
		}

#if PLATFORM_WINDOWS
		if (SharedHandle == INVALID_HANDLE_VALUE)
		{
			return EResult::InvalidHandle;
		}
#endif

		return EResult::Ok;
	}

	EResult ValidateSampleCount(int32 NumSamples)
	{
		// Shared textures are single-sampled; CopyResource requires the sample counts to match.
		return NumSamples == 1 ? EResult::Ok : EResult::InvalidSampleCount;
	}

	EResult ValidateMaxSenders(int32 RequestedMax)
	{
		if (RequestedMax < MinMaxSenders || RequestedMax > MaxMaxSenders)
		{
			return EResult::OutOfRange;
		}
		return EResult::Ok;
	}

	EResult ValidatePixelFormat(EPixelFormat PixelFormat)
	{
		if (PixelFormat == PF_Unknown)
		{
			return EResult::UnsupportedFormat;
		}

		return SpoutFormatPolicy::IsSupportedSendPixelFormat(PixelFormat)
			? EResult::Ok
			: EResult::UnsupportedFormat;
	}

	EResult ValidateExternalSenderMetadata(int64 Width, int64 Height, const void* SharedHandle, uint32 DxgiFormat)
	{
		const EResult DimensionResult = ValidateDimensions(Width, Height);
		if (DimensionResult != EResult::Ok)
		{
			return DimensionResult;
		}

		const EResult HandleResult = ValidateSharedHandle(SharedHandle);
		if (HandleResult != EResult::Ok)
		{
			return HandleResult;
		}

		if (!SpoutFormatPolicy::ResolveReceiverFormat(DxgiFormat).IsUsable())
		{
			return EResult::UnsupportedFormat;
		}

		return EResult::Ok;
	}
}
