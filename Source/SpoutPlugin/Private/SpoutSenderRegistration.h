// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Declares the explicit sender-registration state model.
 *
 * Whether this plugin owns a Spout sender name is not a boolean. Tracking it as one -- consulted
 * only to choose between CreateSender and UpdateSender -- loses the distinctions that matter:
 *
 *   - a CreateSender failure after the texture was created is never retried;
 *   - a resize replaces the texture and handle but leaves the advertised registration untouched;
 *   - a discarded UpdateSender return value keeps advertising a handle already released;
 *   - destruction releases the sender name even when the name was never created.
 *
 * FSpoutSenderRegistration is a state machine that owns those decisions instead. It deliberately
 * knows nothing about D3D, Spout, or Unreal render threads: the SDK calls arrive through
 * ISpoutSenderNameBackend, so the transitions depend only on that interface and can be driven by
 * any implementation of it.
 */
#pragma once

#include "CoreMinimal.h"

/** Lifecycle of this plugin's registration of one sender name in Spout's process-global list. */
enum class ESpoutRegistrationState : uint8
{
	/** No registration has been attempted, or a previous one was released. */
	Unregistered,
	/** An attempt is in progress. Only observable from inside a Publish() call. */
	Registering,
	/** The name is registered and the advertised geometry matches the live shared texture. */
	Active,
	/**
	 * The last attempt failed. Retryable: the next Publish() tries again from scratch.
	 * The invariant is that Failed always implies the name is *not* owned, so a failed sender never
	 * leaves a dangling handle advertised to other processes.
	 */
	Failed,
	/** Resources were torn down and any owned name was released. A later Publish() may restart. */
	Released
};

const TCHAR* LexToString(ESpoutRegistrationState State);

/**
 * The Spout sender-name operations this state machine needs.
 *
 * Names arrive as null-terminated UTF-8 because that is what the SDK consumes and what
 * SpoutValidation produces; the state machine never re-encodes them.
 */
class ISpoutSenderNameBackend
{
public:
	virtual ~ISpoutSenderNameBackend() = default;

	/** Registers a new name. Returns false if Spout refused it (list full, name taken, no SDK). */
	virtual bool CreateSender(const ANSICHAR* Utf8Name, uint32 Width, uint32 Height, void* SharedHandle, uint32 Format) = 0;

	/** Republishes new geometry or a new handle for a name this plugin already owns. */
	virtual bool UpdateSender(const ANSICHAR* Utf8Name, uint32 Width, uint32 Height, void* SharedHandle, uint32 Format) = 0;

	/** Removes a name this plugin owns from Spout's list. */
	virtual void ReleaseSenderName(const ANSICHAR* Utf8Name) = 0;
};

/**
 * Deterministic registration state for one sender name.
 *
 * Not internally synchronized. Every instance lives inside an FSpoutSharedSender and is only
 * touched under that entry's ResourceMutex, matching the existing lock discipline (entry resource
 * state first, then the process-local Spout mutex taken inside the backend).
 */
class FSpoutSenderRegistration
{
public:
	ESpoutRegistrationState GetState() const { return State; }

	/** True only when Spout advertises this name with the geometry currently backed by resources. */
	bool IsActive() const { return State == ESpoutRegistrationState::Active; }

	/** True when a CreateSender call succeeded and the name has not been released since. */
	bool OwnsName() const { return bNameOwned; }

	/** Consecutive failed attempts; reset on success. Drives rate-limited diagnostics. */
	int32 GetConsecutiveFailureCount() const { return ConsecutiveFailures; }

	/**
	 * Publishes Width/Height/SharedHandle/Format for this name, creating the registration or
	 * updating it as required, and returns true when the registration is Active afterwards.
	 *
	 * Behaviour by entry state:
	 *   - not owned (Unregistered, Failed, Released): CreateSender. Success gives Active; failure
	 *     gives Failed and stays retryable on the next call.
	 *   - owned and Active with matching advertised values: no SDK call, returns true.
	 *   - owned otherwise (resize, new handle, format change): UpdateSender. Success re-advertises
	 *     and gives Active.
	 *   - owned and UpdateSender fails: the name is released before returning. Continuing to
	 *     advertise geometry or a handle that no longer exists is worse than briefly disappearing
	 *     from the sender list, and releasing here is what makes the retry deterministic: the next
	 *     call takes the CreateSender path from a clean slate.
	 *   - owned but called with a DIFFERENT name: the owned registration is released first, then the
	 *     new name is created. An entry's name is fixed in practice, so this is a defensive branch,
	 *     but it keeps the owned name and the advertised name from ever disagreeing.
	 *
	 * A rejected argument set (null/empty name, null handle, zero extent) also releases any owned
	 * registration before returning, because the alternative is a Failed state that still owns a
	 * name -- which would break the invariant the whole class is built on.
	 */
	bool Publish(ISpoutSenderNameBackend& Backend, const ANSICHAR* Utf8Name, uint32 Width, uint32 Height, void* SharedHandle, uint32 Format);

	/**
	 * Marks the registration failed because the resources behind it could not be produced (for
	 * example shared-texture creation failed). Releases the name if it was owned, so no stale
	 * handle stays advertised, and leaves the entry retryable.
	 */
	void Invalidate(ISpoutSenderNameBackend& Backend);

	/**
	 * Final teardown. Releases the name only when this plugin actually owns the registration, so
	 * a name that never registered is never released.
	 */
	void Release(ISpoutSenderNameBackend& Backend);

	/** The name this registration currently owns, or an empty string when it owns none. */
	const ANSICHAR* GetOwnedName() const { return bNameOwned && OwnedUtf8Name.Num() > 1 ? OwnedUtf8Name.GetData() : ""; }

private:
	/**
	 * Drops ownership, clearing the advertised values.
	 *
	 * Releases through OwnedUtf8Name -- the name CreateSender actually succeeded with -- never
	 * through a caller-supplied argument. A release is an act on state this object owns, so it must
	 * not be able to fail (or, worse, remove a different process's sender) because a caller passed a
	 * null, empty or mismatched name.
	 */
	void ReleaseOwnedName(ISpoutSenderNameBackend& Backend);
	void ClearAdvertised();
	void RecordAdvertised(uint32 Width, uint32 Height, void* SharedHandle, uint32 Format);
	bool MatchesAdvertised(uint32 Width, uint32 Height, void* SharedHandle, uint32 Format) const;

	ESpoutRegistrationState State = ESpoutRegistrationState::Unregistered;

	/** True between a successful CreateSender and the matching ReleaseSenderName. */
	bool bNameOwned = false;

	/**
	 * Null-terminated copy of the name CreateSender succeeded with. This is the registration's own
	 * identity: it is the only thing ReleaseSenderName is ever called with, so ownership can always
	 * be given back regardless of what a later caller passes in. Empty when nothing is owned.
	 */
	TArray<ANSICHAR> OwnedUtf8Name;

	/** What Spout was last told. Compared against the request to decide whether an update is due. */
	uint32 AdvertisedWidth = 0;
	uint32 AdvertisedHeight = 0;
	uint32 AdvertisedFormat = 0;
	void* AdvertisedHandle = nullptr;

	int32 ConsecutiveFailures = 0;
};
