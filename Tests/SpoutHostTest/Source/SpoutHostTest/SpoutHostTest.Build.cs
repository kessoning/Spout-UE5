// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// Build rules for the Spout release-validation host module.
//
// SpoutPlugin is a *public* dependency of nothing here; it is taken privately because the host
// only calls into it, and taking it at all is what proves the packaged host actually links and
// loads the plugin module rather than merely shipping its files.

using System.IO;
using UnrealBuildTool;

public class SpoutHostTest : ModuleRules
{
	public SpoutHostTest(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RHI",
			"RenderCore",

			// The subject under test. A link-time dependency, so a packaged host that starts at all
			// has necessarily loaded the plugin module.
			"SpoutPlugin"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",
			"ApplicationCore"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// GetModuleHandle/GetModuleFileName, used to ask the OS which Spout.dll is actually
			// mapped into this process and where it came from. File presence is not proof of load.
			PublicSystemLibraries.Add("Kernel32.lib");

			// The GameViewport scenario proves publication through the real
			// cross-process Spout protocol rather than through anything the plugin says about
			// itself: FViewportSenderProbe reads the shared sender registry, opens the shared
			// texture on its own plain D3D11 device and watches the frame-count semaphore, exactly
			// as an external receiver application would. That needs the bundled SDK headers (kept
			// private by the plugin on purpose) and D3D11CreateDevice. Spout.lib and the Spout.dll
			// delay-load already propagate from the SpoutPlugin dependency; only the headers and
			// d3d11.lib are added here. This project always builds from a staged layout with the
			// plugin at Plugins/SpoutPlugin; if the headers are ever absent the probe compiles to
			// a stub that fails the scenario loudly instead of skipping it.
			string StagedSpoutSdkInclude = Path.GetFullPath(Path.Combine(
				ModuleDirectory, "..", "..", "Plugins", "SpoutPlugin", "ThirdParty", "Spout", "include"));
			if (Directory.Exists(StagedSpoutSdkInclude))
			{
				PrivateIncludePaths.Add(StagedSpoutSdkInclude);
				PrivateDefinitions.Add("SPOUT_HOSTTEST_HAS_SDK=1");
			}
			else
			{
				PrivateDefinitions.Add("SPOUT_HOSTTEST_HAS_SDK=0");
			}
			PublicSystemLibraries.Add("d3d11.lib");
		}
	}
}
