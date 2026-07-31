// Build rules for the Spout release-validation host module.
//
// SpoutPlugin is a *public* dependency of nothing here; it is taken privately because the host
// only calls into it, and taking it at all is what proves the packaged host actually links and
// loads the plugin module rather than merely shipping its files.

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
		}
	}
}
