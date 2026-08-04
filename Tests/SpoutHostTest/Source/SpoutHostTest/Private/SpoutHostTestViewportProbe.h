// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * External-facing observation of one Spout sender, used by the GameViewport smoke scenario.
 *
 * The point of this class is INDEPENDENCE from the code under test. It never touches the plugin:
 * it instantiates its own copies of the bundled SDK classes (spoutSenderNames, spoutDirectX,
 * spoutFrameCount) and its own plain D3D11 device, and observes the sender through the same
 * cross-process artifacts an external receiver application would use:
 *
 *   - advertisement and geometry come from Spout's shared-memory sender registry;
 *   - the shared texture is opened from its share handle on the probe's own device, which is the
 *     actual interop boundary a peer crosses (the same OpenSharedResource call, whether the peer
 *     is another device in this process or another process entirely);
 *   - frame advancement comes from the sender name's frame-count semaphore, the receiver side of
 *     the protocol the plugin's publish path participates in.
 *
 * None of that is a counter the plugin sets for itself; a sender that never really publishes
 * cannot move any of it.
 */
#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

namespace SpoutHostTest
{
	class FViewportSenderProbe
	{
	public:
		FViewportSenderProbe();
		~FViewportSenderProbe();

		/**
		 * Creates the probe's own D3D11 device and the SDK observer objects.
		 * False (with OutError set) when the device cannot be created or the SDK is unavailable
		 * in this build; the caller must fail its scenario rather than continue without evidence.
		 */
		bool Initialize(FString& OutError);

		/** Closes every observer resource. Safe to call more than once. */
		void Close();

		/**
		 * Whether the machine-wide Spout frame-count feature is enabled. When it is disabled in
		 * SpoutSettings there is no external frame semaphore to observe, so the scenario cannot
		 * produce frame-advancement evidence and must fail rather than pretend.
		 */
		bool IsFrameCountingAvailable() const;

		/** True when the name is present in Spout's shared sender registry. */
		bool IsSenderAdvertised(const FString& SenderName) const;

		/** Reads one sender's registry entry. False when it is not advertised. */
		bool GetSenderDescription(const FString& SenderName,
			uint32& OutWidth, uint32& OutHeight, uint32& OutDxgiFormat) const;

		/**
		 * Opens the sender's shared texture on the probe's own device and returns the texture's
		 * REAL descriptor (from the opened resource, not from the registry). Any previously opened
		 * texture is released first, so this can be called again after a resize.
		 */
		bool OpenSharedTexture(const FString& SenderName,
			uint32& OutWidth, uint32& OutHeight, uint32& OutDxgiFormat, FString& OutError);

		/** Releases the opened shared texture, if any. */
		void ReleaseSharedTexture();

		/** Opens the receiver side of the name's frame-count semaphore. */
		void BeginFrameObservation(const FString& SenderName);

		/** True when the sender has published at least one new frame since the last poll. */
		bool PollNewFrame();

		/** The sender's frame count as last read by PollNewFrame. */
		long GetSenderFrameCount() const;

		/** Closes the frame-count observation. */
		void EndFrameObservation();

	private:
		// All D3D11 and SDK types stay out of this header, mirroring the plugin's own hygiene.
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};
}
