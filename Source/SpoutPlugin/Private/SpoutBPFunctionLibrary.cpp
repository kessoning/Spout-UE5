// Copyright (c) 2023 RockmanZuyi
// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the Blueprint API for Spout by routing game-thread calls into the D3D/Spout
 * subsystems and enqueuing render-thread work where required.
 */
#include "SpoutBPFunctionLibrary.h"
#include "SpoutModule.h"

#include "SpoutD3DContext.h"
#include "SpoutFormatPolicy.h"
#include "SpoutLogThrottle.h"
#include "SpoutReceiver.h"
#include "SpoutReceiverOwnership.h"
#include "SpoutSender.h"
#include "SpoutSenderRegistry.h"
#include "SpoutSession.h"
#include "SpoutTextureUtils.h"
#include "SpoutValidation.h"
#include "SpoutLimits.h"

#include "Materials/MaterialInstanceDynamic.h"
#include "RHI.h"
#include "RenderingThread.h"

namespace
{
	/**
	 * The production wiring of the ordered teardown.
	 *
	 * Each method is one line of real work; the value is that the ORDER lives in
	 * SpoutLifecycle::RunOrderedShutdown, in one place. Teardown correctness here is purely a
	 * matter of sequence, and a sequence expressed only as the order of statements inside a
	 * function is one that can silently regress.
	 */
	class FSpoutShutdownSteps final : public ISpoutLifecycleSteps
	{
	public:
		explicit FSpoutShutdownSteps(bool bInRestart = true) : bRestart(bInRestart) {}
		bool bRestart;
		virtual void EnterStoppingState() override
		{
			FSpoutSession::Get().BeginStop();
		}

		virtual void DetachProducers() override
		{
			FSpoutSender::DetachProducers();
		}

		virtual void ClearPendingWork() override
		{
			FSpoutSender::ClearPendingWork();
		}

		virtual void FlushRenderingWork() override
		{
			FlushRenderingCommands();
		}

		virtual void ReleaseNativeResources() override
		{
			FSpoutSender::ReleaseNativeResources();
			FSpoutSenderRegistry::Get().Clear();
		}

		virtual void ReleaseManagedObjects() override
		{
			// Only if the tracker was ever created. Shutdown must not be the thing that constructs
			// an FGCObject, least of all during module teardown.
			if (FSpoutReceiverOwnership* Ownership = FSpoutReceiverOwnership::Peek())
			{
				Ownership->ReleaseAll();
			}
		}

		virtual void LeaveStoppingState() override
		{
			if (bRestart) FSpoutSession::Get().EndStop();
		}
	};
}

void USpoutBPFunctionLibrary::EnsureSpoutInitialized()
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return;
	// Public entry points and UObject work are game-thread only.
	if (!FSpoutSession::Get().IsRunning()) return;
	FSpoutD3DContext::Get().InitializeIfNeeded();
	// The interop path is latched at the first initialization; a later setting change is reported, not applied.
	SpoutInterop::NoteSettingChange();
	// Retired shared-slot objects whose GPU work has finished are released here as well as on the render thread.
	FSpoutGpuRetirement::Get().Poll();
}

void USpoutBPFunctionLibrary::CloseAllStreams()
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return;
	// Order matters: flushing rendering commands FIRST and removing the back-buffer delegate
	// afterwards lets the render thread pass the flush marker and invoke the still-subscribed
	// delegate, which enqueues more module-owned work behind the very flush the teardown was
	// relying on. The order is fixed and enforced in one place.
	FSpoutShutdownSteps Steps;
	SpoutLifecycle::RunOrderedShutdown(Steps);
}

void USpoutBPFunctionLibrary::GlobalShutdown()
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return;
	FSpoutShutdownSteps Steps(false);
	SpoutLifecycle::RunOrderedShutdown(Steps);
	FSpoutD3DContext::Get().Shutdown();

	// After the context is gone nothing else will use the tracker, and the UObject system is still
	// up at this point, which is the only safe time to destroy an FGCObject.
	FSpoutReceiverOwnership::TearDown();
}

int32 USpoutBPFunctionLibrary::SetMaxSenders(int32 Max)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return 0;
	// Returns the EFFECTIVE limit, not the requested one. Echoing the argument back unconditionally
	// would leave a caller unable to tell a successful change from a rejected value, or from Spout
	// being entirely unavailable.
	const SpoutValidation::EResult RangeResult = SpoutValidation::ValidateMaxSenders(Max);
	if (RangeResult != SpoutValidation::EResult::Ok)
	{
		UE_LOG(LogSpoutPlugin, Warning,
			TEXT("SetMaxSenders: %d is outside the accepted range [%d, %d] (%s); no change was made."),
			Max,
			SpoutValidation::MinMaxSenders,
			SpoutValidation::MaxMaxSenders,
			SpoutValidation::ToString(RangeResult));
		return 0;
	}

	EnsureSpoutInitialized();
	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	spoutSenderNames* SenderNames = Context.GetSenderNames();
	if (!SenderNames)
	{
		UE_LOG(LogSpoutPlugin, Warning,
			TEXT("SetMaxSenders: Spout is unavailable, so the limit was not changed. Requested %d."),
			Max);
		return 0;
	}

	// Spout sender registry is not thread-safe; guard with the shared mutex.
	// SetMaxSenders itself returns void, so read the value back to report what actually took effect.
	FScopeLock SpoutLock(&Context.GetSpoutMutex());
	SenderNames->SetMaxSenders(Max);
	const int32 EffectiveMax = SenderNames->GetMaxSenders();

	if (EffectiveMax != Max)
	{
		UE_LOG(LogSpoutPlugin, Warning,
			TEXT("SetMaxSenders: requested %d but the effective limit is %d."),
			Max,
			EffectiveMax);
	}

	return EffectiveMax;
}

UTextureRenderTarget2D* USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(int32 Width, int32 Height, EPixelFormat Format, bool bForceLinearGamma)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return nullptr;
	// Dimension and format validation lives in CreateRenderTarget2D, at the allocation site, so it
	// cannot be bypassed by an internal caller. Returns null on rejection.
	return SpoutTextureUtils::CreateRenderTarget2D(Width, Height, Format, bForceLinearGamma);
}

bool USpoutBPFunctionLibrary::SpoutSender(FName SpoutName, ESpoutSendTextureFrom SendTextureFrom, UTextureRenderTarget2D* TextureRenderTarget2D)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return false;
	return FSpoutSender::Send(SpoutName, SendTextureFrom, TextureRenderTarget2D);
}

void USpoutBPFunctionLibrary::CloseSender(FName SpoutName)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return;
	FSpoutSender::Close(SpoutName);
}

bool USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(FName SpoutName, UMaterialInstanceDynamic* ConsumerMaterial,
	FName TextureParameterName, UTexture2D*& OutTexture, UTextureRenderTarget2D* OptionalOutputRenderTarget, ESpoutColorSpace ColorSpace)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return false;
	check(IsInGameThread());
	return FSpoutReceiver::Receive(SpoutName, nullptr, TextureParameterName, ConsumerMaterial, OutTexture, OptionalOutputRenderTarget, ColorSpace);
}

bool USpoutBPFunctionLibrary::ReleaseReceiverMaterialBinding(FName SpoutName, UMaterialInstanceDynamic* ConsumerMaterial, FName TextureParameterName)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return false;
	check(IsInGameThread());
	FSpoutReceiverOwnership* Ownership = FSpoutReceiverOwnership::Peek();
	return Ownership && Ownership->ReleaseMaterialBinding(SpoutName, ConsumerMaterial,
		TextureParameterName.IsNone() ? FName(TEXT("SpoutTexture")) : TextureParameterName);
}

void USpoutBPFunctionLibrary::CloseReceiver(FName SpoutName, UTexture2D*& ReceivedTexture, UMaterialInstanceDynamic*& ReceivedMaterial)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return;
	// The plugin's own record for SpoutName decides what may be released, and nothing is destroyed
	// by hand at all.
	//
	// Trusting the caller's pointer instead -- unrooting, BeginReleaseResource and MarkAsGarbage on
	// whatever texture it was handed, with no check that the texture has anything to do with
	// SpoutName -- lets a mismatched name/pointer pair destroy an unrelated texture and leaves every
	// other reference to it pointing at an object marked for destruction.

	// 1. Cancel and drop the native side. This also stops any queued receive for this name.
	FSpoutReceiver::Close(SpoutName);

	FSpoutReceiverOwnership& Ownership = FSpoutReceiverOwnership::Get();

	// 2. Decide, before releasing, whether the caller handed over objects this plugin actually owns.
	//    Checked first because Release forgets the record.
	const bool bTextureIsOurs = Ownership.OwnsTexture(SpoutName, ReceivedTexture);
	const bool bMaterialIsOurs = Ownership.OwnsMaterial(SpoutName, ReceivedMaterial);
	const bool bHadRecord = Ownership.Contains(SpoutName);

	// 3. Release plugin ownership. This detaches the texture parameter from the material this
	//    plugin bound -- which is the part that actually stops other holders of that material from
	//    sampling the texture, something clearing the caller's local variable cannot do -- and drops
	//    the managed references so ordinary GC can reclaim the objects.
	Ownership.Release(SpoutName);

	// 4. Clear the caller's variables only where they name something this plugin owned. A pointer to
	//    somebody else's texture is left exactly as it was: clearing it could drop that object's
	//    last reference and destroy it, the same harm as an unchecked MarkAsGarbage, just by a
	//    slower route.
	if (ReceivedTexture && !bTextureIsOurs)
	{
		UE_LOG(LogSpoutPlugin, Warning,
			TEXT("CloseReceiver('%s'): the supplied texture '%s' is not the texture this plugin created for that receiver. ")
			TEXT("It has been left untouched. The receiver was closed; pass the Out Texture you got back from Spout Receiver."),
			*SpoutName.ToString(),
			*ReceivedTexture->GetName());
	}
	else
	{
		ReceivedTexture = nullptr;
	}

	if (ReceivedMaterial && !bMaterialIsOurs)
	{
		// A caller-created material instance is a normal, supported case: the texture parameter has
		// been cleared, but the object stays theirs and their variable is left alone.
		UE_LOG(LogSpoutPlugin, Verbose,
			TEXT("CloseReceiver('%s'): the supplied material instance was not created by this plugin. ")
			TEXT("Its Spout texture parameter was cleared and the reference was left untouched."),
			*SpoutName.ToString());
	}
	else
	{
		ReceivedMaterial = nullptr;
	}

	if (!bHadRecord)
	{
		UE_LOG(LogSpoutPlugin, Verbose,
			TEXT("CloseReceiver('%s'): no receiver was open under that name."),
			*SpoutName.ToString());
	}
}

bool USpoutBPFunctionLibrary::SpoutReceiver(
	const FName SpoutName,
	UMaterialInterface* InputMaterial,
	FName TextureParameterName,
	UMaterialInstanceDynamic*& OutMat,
	UTexture2D*& OutTexture,
	UTextureRenderTarget2D* OptionalOutputRenderTarget,
	ESpoutColorSpace ColorSpace
)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return false;
	// OptionalOutputRenderTarget is copied into by both the GPU and CPU receive paths when it is
	// compatible with the incoming frame, and excluded and left untouched when it is not.
	return FSpoutReceiver::Receive(SpoutName, InputMaterial, TextureParameterName, OutMat, OutTexture, OptionalOutputRenderTarget, ColorSpace);
}

bool USpoutBPFunctionLibrary::SpoutInfo(TArray<FSenderStruct>& Senders)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return false;
	// Reset first, on every path. A failed enumeration must never leave the caller holding the
	// previous frame's list.
	Senders.Reset();
	EnsureSpoutInitialized();

	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	spoutSenderNames* SenderNames = Context.GetSenderNames();
	if (!SenderNames)
	{
		return false;
	}

	// Access to the global Spout sender list is serialized via SpoutMutex. That mutex is
	// process-local, so the list can still be mutated by another process between the count query
	// and any indexed lookup — every lookup below is therefore treated as able to fail.
	FScopeLock SpoutLock(&Context.GetSpoutMutex());

	const int32 ReportedCount = SenderNames->GetSenderCount();
	if (ReportedCount < 0)
	{
		// The list could not be read at all. Output stays empty.
		return false;
	}

	// The count is read out of shared memory that another process owns, so it is not trusted as an
	// allocation size. Clamp to the largest sender list this plugin will accept before reserving or
	// iterating: a corrupt or hostile count must not turn into a huge reservation.
	const int32 Count = FMath::Min(ReportedCount, SpoutValidation::MaxMaxSenders);
	if (Count < ReportedCount)
	{
		UE_LOG(LogSpoutPlugin, Warning,
			TEXT("SpoutInfo: Spout reported %d senders, which exceeds the supported maximum of %d. Only the first %d are enumerated."),
			ReportedCount,
			SpoutValidation::MaxMaxSenders,
			Count);
	}

	Senders.Reserve(Count);

	int32 SkippedEntries = 0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		// Zero-initialized, and GetSenderNameInfo's return value is checked below. Leaving the
		// buffer uninitialized and ignoring that return value would pass raw stack contents to
		// GetSenderInfo and FName, so a failed lookup would read unterminated stack memory.
		ANSICHAR NameBuf[SpoutMaxSenderNameLen] = {};
		unsigned int IndexedWidth = 0;
		unsigned int IndexedHeight = 0;
		HANDLE IndexedHandle = nullptr;

		if (!SenderNames->GetSenderNameInfo(Index, NameBuf, UE_ARRAY_COUNT(NameBuf), IndexedWidth, IndexedHeight, IndexedHandle))
		{
			// The entry disappeared, or the index is no longer valid because the list shrank.
			++SkippedEntries;
			continue;
		}

		// Guarantee termination regardless of what the SDK wrote, then require a non-empty name.
		NameBuf[UE_ARRAY_COUNT(NameBuf) - 1] = '\0';
		const int32 NameLength = FCStringAnsi::Strlen(NameBuf);
		if (NameLength <= 0 || NameLength > SpoutValidation::MaxSenderNameUtf8Bytes)
		{
			++SkippedEntries;
			continue;
		}

		// Re-query by name for the authoritative metadata. This is a second lookup against a list
		// that may have changed since the indexed call, so its result is checked too.
		unsigned int Width = 0;
		unsigned int Height = 0;
		HANDLE Handle = nullptr;
		unsigned long Format = 0;
		if (!SenderNames->GetSenderInfo(NameBuf, Width, Height, Handle, Format))
		{
			++SkippedEntries;
			continue;
		}

		// Do not expose metadata that cannot describe a real shared texture.
		if (SpoutValidation::ValidateExternalSenderMetadata(
				static_cast<int64>(Width),
				static_cast<int64>(Height),
				Handle,
				static_cast<uint32>(Format)) != SpoutValidation::EResult::Ok)
		{
			++SkippedEntries;
			continue;
		}

		// Only now is it safe to build an FName. Decoded as UTF-8, matching how names are encoded
		// on the way out.
		FSenderStruct SenderInfo;
		SenderInfo.Name = FName(UTF8_TO_TCHAR(NameBuf));
		SenderInfo.Width = static_cast<int32>(Width);
		SenderInfo.Height = static_cast<int32>(Height);
		SenderInfo.bIsAlive = true;
		Senders.Add(SenderInfo);
	}

	if (SkippedEntries > 0)
	{
		// Rate-limited: a sender appearing or disappearing mid-enumeration is normal, and this can
		// be called every frame.
		static FSpoutLogThrottle SkipLogThrottle(10.0);
		if (SkipLogThrottle.ShouldLog())
		{
			UE_LOG(LogSpoutPlugin, Verbose,
				TEXT("SpoutInfo: skipped %d of %d sender list entries that vanished or failed validation during enumeration."),
				SkippedEntries,
				Count);
		}
	}

	return true;
}

bool USpoutBPFunctionLibrary::GetSenderInfo(FName SpoutName, FSenderStruct& OutSenderStruct)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return false;
	// Reset before anything can fail. Otherwise a failed lookup leaves the caller's struct holding
	// whatever it contained before, which reads as a live sender.
	OutSenderStruct = FSenderStruct();

	TArray<ANSICHAR> Utf8Name;
	if (SpoutValidation::ValidateName(SpoutName, &Utf8Name) != SpoutValidation::EResult::Ok)
	{
		return false;
	}

	EnsureSpoutInitialized();

	unsigned int Width = 0;
	unsigned int Height = 0;
	HANDLE Handle = nullptr;
	unsigned long Format = 0;

	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	{
		FScopeLock SpoutLock(&Context.GetSpoutMutex());
		spoutSenderNames* SenderNames = Context.GetSenderNames();
		if (!SenderNames)
		{
			return false;
		}

		// Spout returns a shared handle + format for the sender if it exists.
		if (!SenderNames->GetSenderInfo(Utf8Name.GetData(), Width, Height, Handle, Format))
		{
			return false;
		}
	}

	if (SpoutValidation::ValidateExternalSenderMetadata(
			static_cast<int64>(Width),
			static_cast<int64>(Height),
			Handle,
			static_cast<uint32>(Format)) != SpoutValidation::EResult::Ok)
	{
		// Found, but the metadata cannot describe a usable frame. Report failure with the output
		// still at its defaults rather than advertising a sender that cannot be received.
		return false;
	}

	OutSenderStruct.Name = SpoutName;
	OutSenderStruct.Width = static_cast<int32>(Width);
	OutSenderStruct.Height = static_cast<int32>(Height);
	OutSenderStruct.bIsAlive = true;
	return true;
}

void USpoutBPFunctionLibrary::GetMaxSenders(int32& Max)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return;
	// Initialize before anything can fail: leaving the caller's previous value in place would make
	// a failed query indistinguishable from a successful one. Zero is not a valid limit, so it
	// unambiguously means "could not be read".
	Max = 0;

	EnsureSpoutInitialized();
	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	if (spoutSenderNames* SenderNames = Context.GetSenderNames())
	{
		// Spout registry uses global state; protect access.
		FScopeLock SpoutLock(&Context.GetSpoutMutex());
		Max = SenderNames->GetMaxSenders();
	}
}

FSpoutStreamStatus USpoutBPFunctionLibrary::GetSpoutStreamStatus(FName SpoutName, ESpoutType Type)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return {};
	FSpoutStreamStatus Result = FSpoutSession::Get().GetStreamStatus(SpoutName, Type);
	if (const auto Entry = FSpoutSenderRegistry::Get().Find(SpoutName, Type))
	{
		Entry->FrameSync.GetStatus(Result.FramesSubmitted, Result.AccessMisses);
		Result.bHasFrame = Result.FramesSubmitted > 0;
		Result.NativeFailures = Entry->NativeFailures.load();
		Result.LastNativeError = Entry->LastNativeError.load();
	}
	return Result;
}

FSpoutRuntimeStatus USpoutBPFunctionLibrary::GetSpoutRuntimeStatus()
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Spout API requires the game thread."))) return {};
	FSpoutRuntimeStatus Result = FSpoutSession::Get().GetRuntimeStatus();
	Result.bInteropAvailable = FSpoutD3DContext::Get().IsSpoutAvailable();
	Result.EstimatedRetainedBytes = SpoutLimits::RetainedBytes();
	Result.PeakEstimatedRetainedBytes = SpoutLimits::PeakRetainedBytes();
	Result.RejectedAllocations = SpoutLimits::RejectedAllocations();
	return Result;
}
