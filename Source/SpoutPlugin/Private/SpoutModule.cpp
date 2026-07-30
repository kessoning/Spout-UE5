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

#if WITH_EDITOR
#include "Engine/World.h"
#endif

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
		TArray<FString> CandidatePaths;

		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpoutPlugin"));
		if (Plugin.IsValid())
		{
			const FString BaseDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
			// Staged copy beside the plugin's own module binary (editor/modular builds,
			// BuildPlugin output, precompiled release zips).
			CandidatePaths.Add(FPaths::Combine(BaseDir, TEXT("Binaries"), TEXT("Win64"), TEXT("Spout.dll")));
			// Source tree under ThirdParty (source/editor builds before first stage).
			CandidatePaths.Add(FPaths::Combine(BaseDir, TEXT("ThirdParty"), TEXT("Spout"), TEXT("lib"), TEXT("amd64"), TEXT("Spout.dll")));
		}
		else
		{
			UE_LOG(LogSpoutPlugin, Warning,
				TEXT("Could not locate the SpoutPlugin descriptor via IPluginManager; falling back to project/executable search paths for Spout.dll."));
		}

		// Packaged (monolithic) games: the plugin module is linked into the game executable,
		// so the RuntimeDependencies target $(BinaryOutputDir) resolves to the PROJECT's
		// Binaries/Win64 next to the executable — the plugin's own Binaries folder is not
		// staged at all in that layout.
		CandidatePaths.Add(FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), TEXT("Binaries"), TEXT("Win64"), TEXT("Spout.dll"))));
		// Executable directory, for staged layouts where the running binary does not live
		// under ProjectDir (e.g. content-only projects packaged against a shared game exe).
		CandidatePaths.Add(FPaths::Combine(FString(FPlatformProcess::BaseDir()), TEXT("Spout.dll")));

		IFileManager& FileManager = IFileManager::Get();

		FString DllToLoad;
		for (const FString& Candidate : CandidatePaths)
		{
			if (FileManager.FileExists(*Candidate))
			{
				DllToLoad = Candidate;
				break;
			}
		}

		if (DllToLoad.IsEmpty())
		{
			// Last resort: let the OS resolve the bare name through the standard search
			// order (which includes the executable directory and PATH). Covers layouts
			// none of the explicit candidates anticipate.
			if (void* SearchOrderHandle = FPlatformProcess::GetDllHandle(TEXT("Spout.dll")))
			{
				UE_LOG(LogSpoutPlugin, Log, TEXT("Loaded Spout.dll via the OS search order (no explicit candidate path existed)."));
				return SearchOrderHandle;
			}

			UE_LOG(LogSpoutPlugin, Error,
				TEXT("Spout.dll not found. Spout is disabled. Paths checked: %s. ")
				TEXT("Precompiled releases must ship Spout.dll in the plugin's Binaries/Win64; packaged games stage it next to the game executable automatically."),
				*FString::Join(CandidatePaths, TEXT(", ")));
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

#if WITH_EDITOR
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(this, &FSpoutModule::OnWorldCleanup);
#endif
}

#if WITH_EDITOR
void FSpoutModule::OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	// Only PIE. A packaged game's world is EWorldType::Game and tears down on every level
	// transition — closing streams there would kill senders that are meant to survive a
	// travel. Editor worlds never carry Spout streams.
	if (!World || World->WorldType != EWorldType::PIE)
	{
		return;
	}

	// Release the streams but keep the interop device: rebuilding it on every PIE run would
	// cost far more than it saves.
	USpoutBPFunctionLibrary::CloseAllStreams();
}
#endif

void FSpoutModule::ShutdownModule()
{
#if WITH_EDITOR
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
	WorldCleanupHandle.Reset();
#endif

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
