#include "SpoutHostTestDiagnostics.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <libloaderapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace SpoutHostTest
{
	namespace Diagnostics
	{
		bool IsPluginModuleLoaded()
		{
			return FModuleManager::Get().IsModuleLoaded(TEXT("SpoutPlugin"));
		}

		FRuntimeDllInfo QueryRuntimeDll()
		{
			FRuntimeDllInfo Info;

#if PLATFORM_WINDOWS
			// GetModuleHandle does NOT load; it only reports an already-mapped module. That is the
			// point: this must observe the loader's state, never influence it.
			HMODULE Handle = ::GetModuleHandleW(TEXT("Spout.dll"));
			if (Handle == nullptr)
			{
				Info.bLoaded = false;
				return Info;
			}

			Info.bLoaded = true;

			TCHAR PathBuffer[2048];
			const DWORD Written = ::GetModuleFileNameW(Handle, PathBuffer, UE_ARRAY_COUNT(PathBuffer));
			if (Written == 0 || Written >= UE_ARRAY_COUNT(PathBuffer))
			{
				Info.Error = FString::Printf(
					TEXT("GetModuleFileNameW failed or truncated (written=%u, lastError=%u)"),
					static_cast<uint32>(Written), static_cast<uint32>(::GetLastError()));
				return Info;
			}

			PathBuffer[Written] = TEXT('\0');
			Info.ResolvedPath = FPaths::ConvertRelativePathToFull(FString(PathBuffer));
			FPaths::NormalizeFilename(Info.ResolvedPath);

			Info.SizeBytes = IFileManager::Get().FileSize(*Info.ResolvedPath);
#else
			Info.Error = TEXT("Spout is Windows-only; this host is not built for Windows.");
#endif

			return Info;
		}

		FString GetExecutablePath()
		{
			FString Path = FPaths::ConvertRelativePathToFull(
				FPaths::Combine(FString(FPlatformProcess::BaseDir()), FPlatformProcess::ExecutableName(false)));
			if (!Path.EndsWith(TEXT(".exe")))
			{
				Path += TEXT(".exe");
			}
			FPaths::NormalizeFilename(Path);
			return Path;
		}

		FString GetPluginBaseDir()
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpoutPlugin"));
			if (!Plugin.IsValid())
			{
				return FString();
			}
			FString Dir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
			FPaths::NormalizeDirectoryName(Dir);
			return Dir;
		}

		FString GetBuildConfigurationName()
		{
			return LexToString(FApp::GetBuildConfiguration());
		}

		FString GetEngineVersionString()
		{
			const FEngineVersion& Version = FEngineVersion::Current();
			return FString::Printf(TEXT("%u.%u.%u"),
				Version.GetMajor(), Version.GetMinor(), Version.GetPatch());
		}
	}
}
