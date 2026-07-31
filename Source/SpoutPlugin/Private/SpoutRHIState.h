/**
 * Declares the RHI resource-state contract for every Spout copy.
 *
 * A Spout copy has to put a UE-tracked resource into a copy state, hand it to the D3D11-on-12
 * device, and put it back. Three separate declarations have to agree for that to be correct:
 *
 *   1. the access this plugin claims the resource was in before the copy;
 *   2. the access the copy itself needs;
 *   3. the D3D12 state declared to ID3D11On12Device::CreateWrappedResource, because
 *      AcquireWrappedResources requires the resource to ALREADY be in that state, and
 *      ReleaseWrappedResources puts it back into it.
 *
 * All three live in one pure function rather than as literals at each call site. Spread across
 * call sites they drift, and the failure is quiet: declaring the viewport source as starting in
 * Present is wrong on every engine in the supported matrix, and restoring it to Present
 * invalidates the engine's own follow-up transition. As one function the contract is a single
 * thing that can be stated and checked.
 *
 * The rule behind the table:
 *
 *   - The access a resource was in BEFORE this plugin touched it is declared Unknown whenever this
 *     plugin does not own it. That is every source and every destination here. Unknown is the
 *     RHI's documented answer for exactly this case -- it resolves the barrier from the RHI's own
 *     whole-resource tracking rather than from a guess -- and it is used only where the guess would
 *     otherwise be wrong, never as a blanket.
 *   - The access AFTER the bracket is always declared from the copy state, because this plugin is
 *     what put the resource there and nothing else has touched it in between. No exit transition
 *     ever declares Unknown.
 *   - The wrap state always matches the copy access. That pairing is what makes Acquire legal.
 */
#pragma once

#include "CoreMinimal.h"
// RHIAccess.h, not RHIDefinitions.h: ERHIAccess is declared there on every engine from 5.3 to 5.8,
// and including it directly keeps this header free of the rest of the RHI's definitions.
#include "RHIAccess.h"

namespace SpoutRHIState
{
	/** Which resource, in which direction, is being bracketed for a Spout copy. */
	enum class EResourceRole : uint8
	{
		/**
		 * The Slate viewport back buffer, read by the viewport sender.
		 *
		 * Its access at OnBackBufferReadyToPresent time is not contractual across the supported
		 * engines: UE 5.3 and 5.4 transition it to SRVGraphics immediately before the broadcast,
		 * while 5.5 onwards moved the broadcast into PresentWindow_RenderThread with no transition
		 * attached, leaving whatever RDG produced. Other delegate subscribers run in the same
		 * broadcast and may change it again.
		 */
		ViewportBackBuffer,

		/** A caller-supplied UTextureRenderTarget2D read by the sender. */
		SenderRenderTarget,

		/** The plugin-owned UTexture2D written by the receiver. */
		ReceiverTexture,

		/** A caller-supplied UTextureRenderTarget2D written by the receiver. */
		ReceiverRenderTarget
	};

	/**
	 * The D3D12 resource state declared when wrapping a resource for the D3D11-on-12 device.
	 *
	 * Kept as an enum rather than a D3D12_RESOURCE_STATES so this contract can be stated and checked
	 * without <d3d12.h>. SpoutD3DUtils translates it at the one point it is needed.
	 */
	enum class EWrapState : uint8
	{
		CopySource,
		CopyDest
	};

	/** The complete state bracket for one role. */
	struct FCopyBracket
	{
		/** Declared previous access when entering the copy state. Unknown for every role here. */
		ERHIAccess EntryBefore = ERHIAccess::Unknown;

		/** The access the copy needs, and the state the resource is in while wrapped. */
		ERHIAccess CopyAccess = ERHIAccess::Unknown;

		/** The access the resource is left in. Never Unknown. */
		ERHIAccess ExitAfter = ERHIAccess::Unknown;

		/** The D3D12 state declared to CreateWrappedResource. Always paired with CopyAccess. */
		EWrapState WrapState = EWrapState::CopySource;

		/** True when this role is read from (a sender source) rather than written to. */
		bool bIsCopySource = false;
	};

	/** The one definition of the bracket for each role. */
	FCopyBracket GetCopyBracket(EResourceRole Role);

	/** Role name, for diagnostic messages. */
	const TCHAR* ToString(EResourceRole Role);
}
