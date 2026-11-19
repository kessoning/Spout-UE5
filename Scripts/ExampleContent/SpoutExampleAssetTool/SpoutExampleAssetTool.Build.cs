// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// Editor-only tool module for the example content (Scripts/ExampleContent/README.md). It is added to a throwaway host
// project, never to the plugin, and it is not shipped.

using UnrealBuildTool;

public class SpoutExampleAssetTool : ModuleRules
{
	public SpoutExampleAssetTool(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "UnrealEd", "BlueprintGraph", "Kismet", "KismetCompiler", "Json", "SpoutPlugin"
		});
	}
}
