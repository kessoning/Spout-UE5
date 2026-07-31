// Game target for the Spout release-validation host.
//
// Kept deliberately plain so it builds unchanged on UE 5.3 through UE 5.8: no target-specific
// feature toggles, no version-gated properties, nothing that would make one engine in the matrix
// behave differently from another.

using UnrealBuildTool;
using System.Collections.Generic;

public class SpoutHostTestTarget : TargetRules
{
	public SpoutHostTestTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;

		// Latest, not a pinned version. An installed engine builds its own UnrealGame/UnrealEditor
		// with the latest defaults and a SHARED build environment, and UBT refuses any project
		// target that changes a shared property - so pinning V2 here fails outright on UE 5.8 with
		// "has build products in common with UnrealEditor". Latest resolves to whatever each engine
		// in the matrix already uses, which is exactly what shared-environment builds require.
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.Add("SpoutHostTest");
	}
}
