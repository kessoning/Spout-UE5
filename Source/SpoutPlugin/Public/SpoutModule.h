/**
 * Declares the Spout plugin module interface and log category used by the runtime implementation.
 */
#pragma once

#include "Modules/ModuleManager.h"

// Global log category for the plugin to keep Spout/D3D diagnostics consistent.
DECLARE_LOG_CATEGORY_EXTERN(LogSpoutPlugin, Log, All);

class FSpoutModule : public IModuleInterface
{
public:
	// Startup/shutdown are called on the game thread by the module manager.
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// True once Spout.dll has been explicitly loaded into the process at startup.
	// Spout symbols are delay-loaded, so callers must confirm the DLL is present
	// before touching any spoutSenderNames/spoutDirectX object — otherwise the
	// delay-load helper raises 0xC06D007E (ERROR_MOD_NOT_FOUND).
	static bool IsSpoutDllLoaded() { return SpoutDllHandle != nullptr; }

private:
	// Handle to the explicitly loaded Spout.dll. Loading it by full path at startup
	// puts the module in the process so its delay-loaded imports resolve by name.
	// Static because the module is a process-wide singleton and FSpoutD3DContext
	// queries it without a module reference.
	static void* SpoutDllHandle;

#if WITH_EDITOR
	// Closes streams when a PIE world tears down, so senders started in PIE stop publishing
	// when the user hits Stop instead of lingering until the editor exits.
	void OnWorldCleanup(class UWorld* World, bool bSessionEnded, bool bCleanupResources);

	FDelegateHandle WorldCleanupHandle;
#endif
};
