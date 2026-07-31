/**
 * Process introspection used to prove - not assume - that the packaged host loaded the plugin and
 * the intended Spout.dll.
 *
 * The distinction this file exists for: a file sitting at the expected path proves staging, not
 * loading. Windows is asked which Spout.dll is actually mapped into this process and what path it
 * was resolved from, so a host that quietly bound to a different copy on PATH is a failure rather
 * than a pass.
 */
#pragma once

#include "CoreMinimal.h"

namespace SpoutHostTest
{
	struct FRuntimeDllInfo
	{
		/** True when a module named Spout.dll is mapped into this process right now. */
		bool bLoaded = false;

		/** Absolute path the loader resolved it from. Empty when not loaded. */
		FString ResolvedPath;

		/** Size on disk of ResolvedPath, or -1 when it could not be read. */
		int64 SizeBytes = -1;

		/** Populated when the OS query itself failed, as opposed to the DLL simply not being loaded. */
		FString Error;
	};

	namespace Diagnostics
	{
		/** Whether the SpoutPlugin module is loaded, via the module manager. */
		bool IsPluginModuleLoaded();

		/**
		 * Asks the Windows loader for the mapped Spout.dll and where it came from.
		 *
		 * Deliberately NOT paired with FSpoutModule::IsSpoutDllLoaded(): that accessor is inline over
		 * a private static the plugin does not export, so calling it from here would require widening
		 * the plugin's exported surface purely for a test. The behavioural proof is better anyway -
		 * GetMaxSenders() returns 0 unless Spout genuinely answers, so the smoke sequence's API step
		 * establishes that the plugin can USE the DLL, not merely that it holds a non-null handle.
		 */
		FRuntimeDllInfo QueryRuntimeDll();

		/** Absolute path of the running executable. */
		FString GetExecutablePath();

		/** Absolute plugin base directory as IPluginManager reports it, or empty when unknown. */
		FString GetPluginBaseDir();

		/** "Development", "Shipping", "DebugGame", "Test" - the configuration this host was built as. */
		FString GetBuildConfigurationName();

		/** Engine version as "5.8.0". */
		FString GetEngineVersionString();
	}
}
