// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// Editor-only tool module for the Async Receive (Beta) example asset (Scripts/AsyncReceiveBeta/README.md).
// It is added to a throwaway validation host project, never to the plugin, and it is not shipped.

using UnrealBuildTool;

public class SpoutAsyncBetaAssetTool : ModuleRules
{
	public SpoutAsyncBetaAssetTool(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "RenderCore", "UnrealEd", "BlueprintGraph", "Kismet", "KismetCompiler",
			"PropertyEditor", "Slate", "SlateCore", "UMG", "ImageCore", "Json", "SpoutPlugin"
		});
	}
}
