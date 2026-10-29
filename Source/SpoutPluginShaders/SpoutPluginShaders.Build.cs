// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

using UnrealBuildTool;

/**
 * The plugin's global shaders. A module that defines global shaders and maps a shader directory must be loaded at
 * PostConfigInit, before Unreal compiles or loads its global shader map; the runtime module (SpoutPlugin) loads later
 * and only calls the functions declared in Public/SpoutColorDecode.h.
 */
public class SpoutPluginShaders : ModuleRules
{
    public SpoutPluginShaders(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "RHI",
            "RenderCore"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            // IPluginManager: the plugin's base directory, for the shader directory mapping.
            "Projects"
        });
    }
}
