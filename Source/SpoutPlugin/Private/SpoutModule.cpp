/**
 * Wires the module lifecycle to Spout resource startup/shutdown, delegating teardown to
 * the Blueprint function library's global shutdown path.
 */
#include "SpoutModule.h"
#include "SpoutBPFunctionLibrary.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

DEFINE_LOG_CATEGORY(LogSpoutPlugin);

#define LOCTEXT_NAMESPACE "FSpoutModule"

#if PLATFORM_WINDOWS
namespace
{
	// One-shot startup check: report whether Spout.dll is reachable so that
	// "module could not be loaded" failures are diagnosable from the log.
	// Runs only at module startup — never per frame.
	void VerifySpoutRuntime()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpoutPlugin"));
		if (!Plugin.IsValid())
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("Could not locate the SpoutPlugin descriptor via IPluginManager; skipping Spout.dll diagnostics."));
			return;
		}

		const FString BaseDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
		const FString BinariesDll = FPaths::Combine(BaseDir, TEXT("Binaries"), TEXT("Win64"), TEXT("Spout.dll"));
		const FString ThirdPartyDll = FPaths::Combine(BaseDir, TEXT("ThirdParty"), TEXT("Spout"), TEXT("lib"), TEXT("amd64"), TEXT("Spout.dll"));

		IFileManager& FileManager = IFileManager::Get();
		const bool bBinariesExists = FileManager.FileExists(*BinariesDll);
		const bool bThirdPartyExists = FileManager.FileExists(*ThirdPartyDll);

		if (!bBinariesExists && !bThirdPartyExists)
		{
			UE_LOG(LogSpoutPlugin, Error,
				TEXT("Spout.dll not found in either '%s' or '%s'. The module will fail to load. ")
				TEXT("Precompiled releases must ship Spout.dll in Binaries/Win64; source builds need it under ThirdParty/Spout/lib/amd64."),
				*BinariesDll, *ThirdPartyDll);
		}
		else if (!bBinariesExists && bThirdPartyExists)
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("Spout.dll is present under ThirdParty ('%s') but missing beside the module binary ('%s'). ")
				TEXT("Precompiled/binary release users may fail to load the module — rebuild from source or copy Spout.dll into Binaries/Win64."),
				*ThirdPartyDll, *BinariesDll);
		}
		else
		{
			UE_LOG(LogSpoutPlugin, Verbose, TEXT("Spout.dll located at '%s'."), *BinariesDll);
		}
	}
}
#endif // PLATFORM_WINDOWS

void FSpoutModule::StartupModule()
{
	// Nothing to initialize eagerly; D3D/Spout resources are created on first use.
	UE_LOG(LogSpoutPlugin, Log, TEXT("Spout Plugin Loaded"));

#if PLATFORM_WINDOWS
	VerifySpoutRuntime();
#endif
}

void FSpoutModule::ShutdownModule()
{
	// Ensure all DX/Spout resources are properly released before module teardown.
	USpoutBPFunctionLibrary::GlobalShutdown();
	UE_LOG(LogSpoutPlugin, Log, TEXT("Spout Plugin Unloaded"));
}

#undef LOCTEXT_NAMESPACE
IMPLEMENT_MODULE(FSpoutModule, SpoutPlugin)
