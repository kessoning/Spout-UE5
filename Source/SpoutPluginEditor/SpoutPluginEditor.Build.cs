// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

using UnrealBuildTool;

/**
 * Editor-only integration: the "Spout" category in the Place Actors panel with the Spout Sender and Spout Receiver
 * actors, and their class icons. Everything that runs in a game lives in the SpoutPlugin runtime module.
 */
public class SpoutPluginEditor : ModuleRules
{
    public SpoutPluginEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "UnrealEd",
            "PlacementMode",
            "Slate",
            "SlateCore",
            "Projects",
            "SpoutPlugin"
        });
    }
}
