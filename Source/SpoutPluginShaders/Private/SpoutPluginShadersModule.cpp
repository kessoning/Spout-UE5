// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "ShaderCompilerCore.h"
#include "HAL/PlatformProperties.h"

/**
 * Maps the plugin's Shaders folder to /Plugin/SpoutPlugin. Loaded at PostConfigInit (SpoutPlugin.uplugin), before
 * Unreal compiles or loads its global shader map, as a module with global shaders must be. Cooked builds load the
 * compiled shaders and need no mapping.
 */
class FSpoutPluginShadersModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Cooked builds load compiled shaders and stage no shader sources.
		if (FPlatformProperties::RequiresCookedData() || !AllowShaderCompiling())
		{
			return;
		}
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpoutPlugin"));
		if (!Plugin.IsValid())
		{
			return;
		}
		// The engine asserts on a missing folder and on a second mapping of the same name; an incomplete installation is
		// reported instead. The shader then fails to compile, which the editor reports with the file it looked for.
		const FString ShaderDirectory = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		if (!FPaths::DirectoryExists(ShaderDirectory))
		{
			UE_LOG(LogInit, Error, TEXT("Spout plugin: the plugin's Shaders folder is missing (%s); reinstall the plugin."), *ShaderDirectory);
			return;
		}
		if (!AllShaderSourceDirectoryMappings().Contains(TEXT("/Plugin/SpoutPlugin")))
		{
			AddShaderSourceDirectoryMapping(TEXT("/Plugin/SpoutPlugin"), ShaderDirectory);
		}
	}
};

IMPLEMENT_MODULE(FSpoutPluginShadersModule, SpoutPluginShaders)
