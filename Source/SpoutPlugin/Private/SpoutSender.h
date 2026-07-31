/**
 * Declares the sender API that bridges Blueprint requests to the render-thread Spout
 * publishing path implemented in SpoutSender.cpp.
 */
#pragma once

#include "CoreMinimal.h"
#include "SpoutBPFunctionLibrary.h"

class FSpoutSender
{
public:
	/**
	 * Kicks a render-thread copy from an RHI texture to a shared Spout DX11 texture.
	 *
	 * Return value contract: true means the request passed immediate validation and was accepted
	 * for the render thread. It does NOT mean the sender is registered with Spout, that a shared
	 * texture exists, or that a frame was published — all three happen later, on the render thread,
	 * and are reported through the log rather than through this return value.
	 *
	 * false means the request was rejected outright on the game thread: an invalid name, an
	 * unavailable interop context, a missing viewport, or a render target with invalid dimensions
	 * or an unsupported pixel format. Nothing was queued.
	 */
	static bool Send(FName SpoutName, ESpoutSendTextureFrom SendTextureFrom, UTextureRenderTarget2D* TextureRenderTarget2D);

	/**
	 * Closes one sender.
	 *
	 * Cancels the stream first, so a send queued earlier in the same frame is a no-op by the time
	 * the render thread reaches it, then drops the name from the viewport's pending set and removes
	 * its registry entry. All three steps matter: removing only an already-existing registry entry
	 * would let a Send/Close pair in one frame still create and publish the sender afterwards, and
	 * would leave a closed viewport name queued for the next present.
	 */
	static void Close(FName SpoutName);

	/**
	 * Step 2 of the ordered teardown: unsubscribes the viewport back-buffer delegate and waits for
	 * any in-flight invocation of it to return, so no further render commands can be produced.
	 */
	static void DetachProducers();

	/** Step 3 of the ordered teardown: drops viewport names that were queued but never presented. */
	static void ClearPendingWork();

	/**
	 * Step 5 of the ordered teardown: releases the cached D3D11-on-12 back-buffer wraps.
	 *
	 * Separate from DetachProducers because it must happen after the render-command flush, not
	 * before it: the wraps are used by commands that may still be queued at detach time.
	 */
	static void ReleaseNativeResources();
};
