/**
 * Wires the module lifecycle to Spout resource startup/shutdown, delegating teardown to
 * the Blueprint function library's global shutdown path.
 */
#include "SpoutModule.h"
#include "SpoutBPFunctionLibrary.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

DEFINE_LOG_CATEGORY(LogSpoutPlugin);

#define LOCTEXT_NAMESPACE "FSpoutModule"

void* FSpoutModule::SpoutDllHandle = nullptr;

#if PLATFORM_WINDOWS
namespace
{
	// One-shot startup load: explicitly bring Spout.dll into the process by full path.
	// Spout symbols are delay-loaded (PublicDelayLoadDLLs), so nothing resolves the DLL
	// until the first Spout call on tick — and the OS delay-load resolver uses the host
	// exe's search order, which does NOT include the plugin's own Binaries/Win64. Loading
	// the DLL here by full path puts it in the process so the later delay-load thunk binds
	// by name instead of raising 0xC06D007E (ERROR_MOD_NOT_FOUND). Returns the handle, or
	// nullptr if the DLL is missing/failed to load (Spout then stays disabled, no crash).
	// Runs only at module startup — never per frame.
	void* LoadSpoutRuntime()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpoutPlugin"));
		if (!Plugin.IsValid())
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("Could not locate the SpoutPlugin descriptor via IPluginManager; Spout.dll will not be loaded and Spout will be disabled."));
			return nullptr;
		}

		const FString BaseDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
		const FString BinariesDll = FPaths::Combine(BaseDir, TEXT("Binaries"), TEXT("Win64"), TEXT("Spout.dll"));
		const FString ThirdPartyDll = FPaths::Combine(BaseDir, TEXT("ThirdParty"), TEXT("Spout"), TEXT("lib"), TEXT("amd64"), TEXT("Spout.dll"));

		IFileManager& FileManager = IFileManager::Get();

		// Prefer the staged copy beside the module binary (packaged/precompiled releases);
		// fall back to the source tree under ThirdParty (source/editor builds).
		FString DllToLoad;
		if (FileManager.FileExists(*BinariesDll))
		{
			DllToLoad = BinariesDll;
		}
		else if (FileManager.FileExists(*ThirdPartyDll))
		{
			DllToLoad = ThirdPartyDll;
		}

		if (DllToLoad.IsEmpty())
		{
			UE_LOG(LogSpoutPlugin, Error,
				TEXT("Spout.dll not found in either '%s' or '%s'. Spout is disabled. ")
				TEXT("Precompiled releases must ship Spout.dll in Binaries/Win64; source builds need it under ThirdParty/Spout/lib/amd64."),
				*BinariesDll, *ThirdPartyDll);
			return nullptr;
		}

		// Push the containing directory onto the loader search path while loading so any
		// co-located dependency of Spout.dll also resolves, then restore it immediately.
		const FString DllDir = FPaths::GetPath(DllToLoad);
		FPlatformProcess::PushDllDirectory(*DllDir);
		void* Handle = FPlatformProcess::GetDllHandle(*DllToLoad);
		FPlatformProcess::PopDllDirectory(*DllDir);

		if (Handle == nullptr)
		{
			UE_LOG(LogSpoutPlugin, Error,
				TEXT("Failed to load Spout.dll from '%s'. Spout is disabled."), *DllToLoad);
		}
		else
		{
			UE_LOG(LogSpoutPlugin, Log, TEXT("Loaded Spout.dll from '%s'."), *DllToLoad);
		}

		return Handle;
	}
}
#endif // PLATFORM_WINDOWS

void FSpoutModule::StartupModule()
{
	// Nothing to initialize eagerly; D3D/Spout resources are created on first use.
	UE_LOG(LogSpoutPlugin, Log, TEXT("Spout Plugin Loaded"));

#if PLATFORM_WINDOWS
	// Explicitly load Spout.dll so its delay-loaded imports bind on first Spout call
	// instead of crashing with 0xC06D007E.
	SpoutDllHandle = LoadSpoutRuntime();
#endif
}

void FSpoutModule::ShutdownModule()
{
	// Ensure all DX/Spout resources are properly released before module teardown.
	USpoutBPFunctionLibrary::GlobalShutdown();

#if PLATFORM_WINDOWS
	if (SpoutDllHandle != nullptr)
	{
		FPlatformProcess::FreeDllHandle(SpoutDllHandle);
		SpoutDllHandle = nullptr;
	}
#endif

	UE_LOG(LogSpoutPlugin, Log, TEXT("Spout Plugin Unloaded"));
}

#undef LOCTEXT_NAMESPACE
IMPLEMENT_MODULE(FSpoutModule, SpoutPlugin)
