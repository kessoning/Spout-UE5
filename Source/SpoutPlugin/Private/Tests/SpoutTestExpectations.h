// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * The one place an automation test declares the D3D12 debug layer's wrap message (id 916). Every test that expects
 * it calls this rather than declaring it itself.
 *
 * In a process latched to the shared-slot path (r.Spout.SharedSlotInterop 1, the default) nothing of Unreal's is
 * wrapped for D3D11, and this does nothing: id 916 is neither required nor excused there, and if it appears, Unreal's
 * error line fails the test.
 *
 * In a process latched to the legacy wrap path with -d3ddebug (the positive-control process), it REQUIRES the message:
 * Unreal's AddExpectedError with Occurrences 0 means "one or more times, or the test fails" (Misc/AutomationTest.h,
 * UE 5.3 and 5.8). A test that calls this must therefore wrap at least once in that process, so a positive-control
 * run must select only tests that do, by name; a test that may return before wrapping
 * (P0.NativeDescriptorMismatchAndMipRejection) must be left out of it.
 */
#pragma once

#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "SpoutD3DContext.h"
#include "SpoutSharedSlot.h"

namespace SpoutTestExpectations
{
	/** The text Unreal logs for the D3D12 debug layer's message id 916. */
	inline const TCHAR* WrapReflectionText()
	{
		return TEXT("ID3D12CompatibilityDevice::ReflectSharedProperties: Resource provided was not shared by D3D11, or with a D3D11 desc.");
	}

	/**
	 * Game thread. In a process latched to the wrap path with the debug layer on: requires id 916 at least once in this
	 * test. Elsewhere: nothing.
	 */
	inline void ExpectLegacyWrapDiagnostic(FAutomationTestBase& Test)
	{
		// The latch happens at the first initialization; make sure it has happened before it is asked.
		FSpoutD3DContext::Get().InitializeIfNeeded();
		if (SpoutInterop::GetMode() == SpoutInterop::EMode::Wrap && FParse::Param(FCommandLine::Get(), TEXT("d3ddebug")))
		{
			// Occurrences 0: one or more times, or the test fails.
			Test.AddExpectedError(WrapReflectionText(), EAutomationExpectedErrorFlags::Contains, 0);
		}
	}
}
#endif
