// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the one RHI resource-state contract for Spout copies.
 *
 * Pure data: no RHI calls, no device, no engine state. That is deliberate. Transition
 * correctness has to be checkable, and a table can be checked in isolation, while four
 * literals spread through two render-thread functions cannot.
 */
#include "SpoutRHIState.h"

namespace SpoutRHIState
{
	FCopyBracket GetCopyBracket(EResourceRole Role)
	{
		FCopyBracket Bracket;

		// Every role's entry state is Unknown, and every one of them earns it: none of these
		// resources has its previous access owned by this plugin. See the header for why the
		// viewport back buffer in particular cannot be declared.
		Bracket.EntryBefore = ERHIAccess::Unknown;

		switch (Role)
		{
		case EResourceRole::ViewportBackBuffer:
			Bracket.CopyAccess = ERHIAccess::CopySrc;
			// SRVGraphics, not Present and not SRVMask. UE 5.3 and 5.4 put the back buffer in
			// SRVGraphics immediately before broadcasting, and transition it back out of
			// SRVGraphics afterwards for stereo; leaving it in exactly that state makes this
			// plugin's bracket invisible to the engine and keeps the engine's own follow-up
			// transition's declared previous state true.
			Bracket.ExitAfter = ERHIAccess::SRVGraphics;
			Bracket.WrapState = EWrapState::CopySource;
			Bracket.bIsCopySource = true;
			break;

		case EResourceRole::SenderRenderTarget:
			Bracket.CopyAccess = ERHIAccess::CopySrc;
			// SRVMask rather than SRVGraphics: a render target is normally sampled next, and it may
			// be sampled from a compute shader, which SRVGraphics alone would not cover.
			Bracket.ExitAfter = ERHIAccess::SRVMask;
			Bracket.WrapState = EWrapState::CopySource;
			Bracket.bIsCopySource = true;
			break;

		case EResourceRole::ReceiverTexture:
		case EResourceRole::ReceiverRenderTarget:
			Bracket.CopyAccess = ERHIAccess::CopyDest;
			Bracket.ExitAfter = ERHIAccess::SRVMask;
			Bracket.WrapState = EWrapState::CopyDest;
			Bracket.bIsCopySource = false;
			break;

		default:
			break;
		}

		return Bracket;
	}

	const TCHAR* ToString(EResourceRole Role)
	{
		switch (Role)
		{
		case EResourceRole::ViewportBackBuffer:   return TEXT("ViewportBackBuffer");
		case EResourceRole::SenderRenderTarget:   return TEXT("SenderRenderTarget");
		case EResourceRole::ReceiverTexture:      return TEXT("ReceiverTexture");
		case EResourceRole::ReceiverRenderTarget: return TEXT("ReceiverRenderTarget");
		default:                                  return TEXT("Invalid");
		}
	}
}
