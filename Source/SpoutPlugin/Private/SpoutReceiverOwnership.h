// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Declares plugin-side ownership of the UObjects a receiver creates.
 *
 * Received textures are tracked here rather than pushed into the GC root set at creation and
 * taken back out by whichever CloseReceiver call happens to be handed a pointer to one. Rooting
 * fails in two ways:
 *
 *   - Nothing can enumerate the textures, so a lost Blueprint reference, an actor destruction,
 *     a PIE stop or a forgotten close leaves a permanent GC root behind.
 *   - The close path treats the caller's pointer as its authority to destroy, so a mismatched
 *     name/pointer pair releases and garbage-marks a texture belonging to somebody else.
 *
 * Ownership is instead keyed by receiver name, which is the plugin's session identity at every
 * layer: the native registry is keyed by (Name, Type) and so is cancellation.
 *
 * The contract this implements, deliberately:
 *
 *   - One receiver session per sender name. Every consumer of a name shares one received texture.
 *   - Each consumer may have its OWN material instance. A session therefore holds a COLLECTION of
 *     material bindings, not one. A single slot would detach only whichever consumer ticked last,
 *     leaving every other consumer's material still referencing -- and, since a material parameter
 *     is a strong reference, still keeping alive -- the texture the session had released.
 *   - CloseReceiver(Name) closes that shared session for every consumer of it.
 *   - No reference counting and no public consumer handles.
 *
 * Releasing an entry does not destroy anything: it removes the last plugin-side reference and lets
 * ordinary UObject lifetime and GC take it from there, rather than forcing the issue with
 * BeginReleaseResource plus MarkAsGarbage.
 *
 * Game thread only, apart from AddReferencedObjects, which the collector calls. The mutex is there
 * for that one crossing.
 */
#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectPtr.h"
// Required explicitly: Core's WeakObjectPtrTemplates.h declares TWeakObjectPtr but FWeakObjectPtr
// itself lives in CoreUObject. UE 5.3 and 5.8 happen to pull it in transitively; UE 5.4 does not, and
// fails with "'FWeakObjectPtr': base class undefined".
#include "UObject/WeakObjectPtr.h"

class UMaterialInstanceDynamic;
class UTexture2D;

class FSpoutReceiverOwnership : public FGCObject
{
public:
	/** Creates the tracker on first use. Call on the game thread. */
	static FSpoutReceiverOwnership& Get();

	/** The tracker if it exists, otherwise null. Never creates it. */
	static FSpoutReceiverOwnership* Peek();

	/**
	 * Destroys the tracker.
	 *
	 * Called during module shutdown, while the UObject system is still up. An FGCObject must not
	 * outlive it, which is why this is explicit rather than a static destructor.
	 */
	static void TearDown();

	/**
	 * Takes ownership of the texture created for this receiver session, replacing any previous one.
	 *
	 * The replaced texture is not destroyed. It loses this reference and is collected normally once
	 * every consumer's reference and any in-flight render command have gone.
	 */
	void AdoptTexture(FName ReceiverName, UTexture2D* Texture);

	/** The single texture this session shares between every consumer of the name, or null. */
	UTexture2D* FindTexture(FName ReceiverName) const;

	/** True only when Texture is the exact texture tracked for that session. Null is never owned. */
	bool OwnsTexture(FName ReceiverName, const UTexture2D* Texture) const;

	/**
	 * Records that a material has this session's received texture bound to one of its parameters.
	 *
	 * Called once per consumer per tick. Bindings are de-duplicated on (material, parameter), so a
	 * per-frame Blueprint call cannot grow the collection, while the same material bound through two
	 * different parameter names is legitimately two bindings.
	 *
	 * bPluginCreated distinguishes a material this plugin created -- kept alive here for as long as
	 * the session lives -- from one a consumer supplied, which is tracked weakly so that close can
	 * still detach it but is never kept alive by having once been bound. Registering a
	 * consumer-supplied material never displaces a plugin-created one: they are separate bindings.
	 */
	void RegisterMaterialBinding(FName ReceiverName, UMaterialInstanceDynamic* Material, FName ParameterName, bool bPluginCreated);

	/** True only when Material is a material this plugin created for that session. */
	bool OwnsMaterial(FName ReceiverName, const UMaterialInstanceDynamic* Material) const;


	/** Live binding count for the session, dead weak references excluded. */
	int32 NumMaterialBindings(FName ReceiverName) const;

	/** Whether that exact (material, parameter) pair is registered for the session. */
	bool HasMaterialBinding(FName ReceiverName, const UMaterialInstanceDynamic* Material, FName ParameterName) const;

	/**
	 * Closes one receiver session: detaches EVERY registered material binding, clears the binding
	 * records, releases plugin ownership of the materials it created, and then releases the received
	 * texture. Returns whether anything was tracked. Destroys nothing, and is idempotent.
	 */
	bool Release(FName ReceiverName);

	/** Release() for every tracked session. The managed-ownership step of a full teardown. */
	void ReleaseAll();

	int32 Num() const;
	bool Contains(FName ReceiverName) const;

	//~ FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;
	//~ End FGCObject

private:
	/** One material with this session's texture bound to one of its parameters. */
	struct FMaterialBinding
	{
		/**
		 * Weak for EVERY binding, including plugin-created ones. Liveness is expressed only by
		 * OwnedMaterial, so a consumer-supplied material is never kept alive merely by having been
		 * bound, and a binding whose material has been collected is simply skipped.
		 */
		TWeakObjectPtr<UMaterialInstanceDynamic> Material;

		/**
		 * The same object as Material, held strongly and reported to the collector, but only when the
		 * plugin created it. Null for a consumer-supplied material.
		 */
		TObjectPtr<UMaterialInstanceDynamic> OwnedMaterial = nullptr;

		FName TextureParameter;
	};

	struct FEntry
	{
		/** The one texture shared by every consumer of this name. Always plugin-created. */
		TObjectPtr<UTexture2D> Texture = nullptr;

		/** Every material any consumer has had this session's texture bound into. */
		TArray<FMaterialBinding> MaterialBindings;
	};

	/**
	 * Rebinds Binding's parameter away from the received texture.
	 *
	 * Rebinds to a real texture rather than to null: UMaterialInstance's texture setter assigns only
	 * inside `if (Value && ensureMsgf(...))`, so a null value is silently ignored and would leave the
	 * received texture bound and referenced.
	 */
	static void DetachBinding(const FMaterialBinding& Binding, UTexture2D* FallbackTexture);

	/** The texture parameters are rebound to on detach, or null if none can be resolved. */
	static UTexture2D* ResolveDetachFallbackTexture();

	/** Drops bindings whose material has been collected. Keeps the collection from growing forever. */
	static void PruneDeadBindings(FEntry& Entry);

	/** Detach-all, clear, release-owned. Shared by Release and ReleaseAll; caller holds the lock. */
	static void CloseEntry(FEntry& Entry, UTexture2D* FallbackTexture, int32& OutDetachedCount);

	mutable FCriticalSection Mutex;
	TMap<FName, FEntry> Entries;

	static FSpoutReceiverOwnership* Instance;
	static FCriticalSection InstanceMutex;
};
