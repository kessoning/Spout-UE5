// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the sender-registration state machine.
 */
#include "SpoutSenderRegistration.h"

const TCHAR* LexToString(ESpoutRegistrationState State)
{
	switch (State)
	{
	case ESpoutRegistrationState::Unregistered: return TEXT("Unregistered");
	case ESpoutRegistrationState::Registering:  return TEXT("Registering");
	case ESpoutRegistrationState::Active:       return TEXT("Active");
	case ESpoutRegistrationState::Failed:       return TEXT("Failed");
	case ESpoutRegistrationState::Released:     return TEXT("Released");
	default:                                    return TEXT("Invalid");
	}
}

bool FSpoutSenderRegistration::Publish(
	ISpoutSenderNameBackend& Backend,
	const ANSICHAR* Utf8Name,
	uint32 Width,
	uint32 Height,
	void* SharedHandle,
	uint32 Format)
{
	if (Utf8Name == nullptr || Utf8Name[0] == '\0' || SharedHandle == nullptr || Width == 0 || Height == 0)
	{
		// Callers validate before reaching here; this is the last line of defence against
		// advertising a name or handle that cannot work.
		//
		// Releasing first is not optional. Entering Failed while still owning a name would leave
		// Spout advertising a registration this object no longer considers live, and would break
		// the invariant every other branch relies on (Failed => !OwnsName()). The release goes
		// through the stored OwnedUtf8Name, so it works even though the name argument passed to
		// this call is the unusable one.
		ReleaseOwnedName(Backend);
		State = ESpoutRegistrationState::Failed;
		++ConsecutiveFailures;
		return false;
	}

	// Defensive: an entry's name is fixed in practice (the registry is keyed by it), but if a
	// caller ever publishes a different name onto an owned registration, give the old name back
	// before taking the new one rather than silently advertising two identities.
	if (bNameOwned && FCStringAnsi::Strcmp(GetOwnedName(), Utf8Name) != 0)
	{
		ReleaseOwnedName(Backend);
	}

	// Nothing to do: already advertising exactly this. Checked before entering Registering so a
	// steady-state frame costs one comparison and no SDK call.
	if (State == ESpoutRegistrationState::Active
		&& bNameOwned
		&& MatchesAdvertised(Width, Height, SharedHandle, Format))
	{
		return true;
	}

	State = ESpoutRegistrationState::Registering;

	if (!bNameOwned)
	{
		// Fresh registration. Reached on the first frame, after a previous failure, and after a
		// release, which is what makes a failed initial registration retryable.
		if (Backend.CreateSender(Utf8Name, Width, Height, SharedHandle, Format))
		{
			// Take a copy of the name the SDK accepted. From here until the matching release, this
			// copy -- not any caller argument -- is what identifies the registration.
			const int32 NameLength = FCStringAnsi::Strlen(Utf8Name);
			OwnedUtf8Name.SetNumUninitialized(NameLength + 1);
			FMemory::Memcpy(OwnedUtf8Name.GetData(), Utf8Name, NameLength);
			OwnedUtf8Name[NameLength] = '\0';

			bNameOwned = true;
			State = ESpoutRegistrationState::Active;
			RecordAdvertised(Width, Height, SharedHandle, Format);
			ConsecutiveFailures = 0;
			return true;
		}

		State = ESpoutRegistrationState::Failed;
		++ConsecutiveFailures;
		ClearAdvertised();
		return false;
	}

	// Owned but the advertised values are stale: a resize, a replaced shared handle, or a format
	// change. Republish against the owned name and check the result.
	if (Backend.UpdateSender(GetOwnedName(), Width, Height, SharedHandle, Format))
	{
		State = ESpoutRegistrationState::Active;
		RecordAdvertised(Width, Height, SharedHandle, Format);
		ConsecutiveFailures = 0;
		return true;
	}

	// The update failed, so Spout still advertises the previous geometry and handle. That handle
	// belongs to a texture this sender has already dropped, and any receiver opening it gets a dead
	// resource. Release the name and fall back to a clean CreateSender on the next attempt.
	ReleaseOwnedName(Backend);
	State = ESpoutRegistrationState::Failed;
	++ConsecutiveFailures;
	return false;
}

void FSpoutSenderRegistration::Invalidate(ISpoutSenderNameBackend& Backend)
{
	// The resources behind the registration are gone, so the advertisement must go too. This keeps
	// the invariant that Failed implies !OwnsName().
	ReleaseOwnedName(Backend);
	State = ESpoutRegistrationState::Failed;
	++ConsecutiveFailures;
}

void FSpoutSenderRegistration::Release(ISpoutSenderNameBackend& Backend)
{
	ReleaseOwnedName(Backend);
	State = ESpoutRegistrationState::Released;
	ConsecutiveFailures = 0;
}

void FSpoutSenderRegistration::ReleaseOwnedName(ISpoutSenderNameBackend& Backend)
{
	// Only ever release a name this plugin successfully created, and only ever the exact bytes
	// CreateSender accepted. Releasing an unowned or caller-supplied name would remove another
	// process's sender from Spout's global list.
	if (bNameOwned && OwnedUtf8Name.Num() > 1)
	{
		Backend.ReleaseSenderName(OwnedUtf8Name.GetData());
	}

	bNameOwned = false;
	OwnedUtf8Name.Reset();
	ClearAdvertised();
}

void FSpoutSenderRegistration::ClearAdvertised()
{
	AdvertisedWidth = 0;
	AdvertisedHeight = 0;
	AdvertisedFormat = 0;
	AdvertisedHandle = nullptr;
}

void FSpoutSenderRegistration::RecordAdvertised(uint32 Width, uint32 Height, void* SharedHandle, uint32 Format)
{
	AdvertisedWidth = Width;
	AdvertisedHeight = Height;
	AdvertisedFormat = Format;
	AdvertisedHandle = SharedHandle;
}

bool FSpoutSenderRegistration::MatchesAdvertised(uint32 Width, uint32 Height, void* SharedHandle, uint32 Format) const
{
	return AdvertisedWidth == Width
		&& AdvertisedHeight == Height
		&& AdvertisedFormat == Format
		&& AdvertisedHandle == SharedHandle;
}
