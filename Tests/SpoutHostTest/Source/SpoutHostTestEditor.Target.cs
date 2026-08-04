// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// Editor target for the Spout release-validation host.
//
// This is the target the automation suite runs under: UnrealEditor-Cmd loads this project and
// executes "Automation RunTests SpoutPlugin". The packaged-host matrix uses SpoutHostTest.Target.cs
// instead.

using UnrealBuildTool;
using System.Collections.Generic;

public class SpoutHostTestEditorTarget : TargetRules
{
	public SpoutHostTestEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;

		// See SpoutHostTest.Target.cs: an installed engine's shared build environment forbids a
		// project target from pinning older build settings.
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.Add("SpoutHostTest");
	}
}
