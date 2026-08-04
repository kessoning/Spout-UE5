// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Module for the Spout release-validation host.
 *
 * Startup arms whichever harness the command line asked for; shutdown is where the final verdict is
 * written. Putting the verdict in shutdown rather than at the end of the test sequence is
 * deliberate: SPOUT_SMOKE_PASS then cannot exist unless the process also survived teardown with the
 * plugin still initialised, which is the failure mode hardest to catch any other way.
 */
#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSpoutHostTest, Log, All);

class FSpoutHostTestModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
