// Copyright (c) 2023 RockmanZuyi
// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Declares the Spout plugin module interface and log category used by the runtime implementation.
 */
#pragma once

#include "Modules/ModuleManager.h"

// Global log category for the plugin to keep Spout/D3D diagnostics consistent (the editor module logs to it too).
SPOUTPLUGIN_API DECLARE_LOG_CATEGORY_EXTERN(LogSpoutPlugin, Log, All);

class FSpoutModule : public IModuleInterface
{
public:
	/** Digest of the actual plugin inputs compiled into this module. */
	static SPOUTPLUGIN_API FString GetBuildSourceIdentity();
	// Startup/shutdown are called on the game thread by the module manager.
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// True once Spout.dll has been explicitly loaded into the process at startup.
	// Spout symbols are delay-loaded, so callers must confirm the DLL is present
	// before touching any spoutSenderNames/spoutDirectX object — otherwise the
	// delay-load helper raises 0xC06D007E (ERROR_MOD_NOT_FOUND).
	static bool IsSpoutDllLoaded() { return SpoutDllHandle != nullptr; }

	/**
	 * Set by the plugin's editor module: returns the level editor's active viewport widget, which a Spout Sender actor
	 * sends while it sends in the editor, and whether that viewport redraws continuously (Realtime). Null outside the
	 * editor or when there is no level viewport.
	 */
	static SPOUTPLUGIN_API void SetLevelViewportProvider(TFunction<TSharedPtr<class SWidget>(bool& bOutRealtime)> Provider);
	static TSharedPtr<class SWidget> GetLevelViewport(bool& bOutRealtime);

private:
	// Handle to the explicitly loaded Spout.dll. Loading it by full path at startup
	// puts the module in the process so its delay-loaded imports resolve by name.
	// Static because the module is a process-wide singleton and FSpoutD3DContext
	// queries it without a module reference.
	static void* SpoutDllHandle;
};
