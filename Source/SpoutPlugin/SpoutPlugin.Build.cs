using System;
using System.IO;
using UnrealBuildTool;

public class SpoutPlugin : ModuleRules
{
    private string ModulePath {
        // Plugin root directory (two levels up from ModuleDirectory).
        get { return Path.GetFullPath(Path.Combine(ModuleDirectory, "../../")); }
    }

    private string ThirdPartyPath {
        // Third-party dependencies bundled with the plugin.
        get { return Path.GetFullPath(Path.Combine(ModulePath, "ThirdParty/")); }
    }

    public SpoutPlugin(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // Public include paths are consumed by downstream modules; keep Spout headers
        // private so they do not pull <d3d11.h>/<Windows.h> into every consumer.
        PublicIncludePaths.AddRange(new string[] {
            Path.Combine(ModuleDirectory, "Public")
        });

        PrivateIncludePaths.AddRange(new string[] {
            Path.Combine(ThirdPartyPath, "Spout", "include")
        });

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "RHI",
            "RenderCore"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            // "Projects" provides IPluginManager, used for startup Spout.dll diagnostics.
            "ApplicationCore", "Slate", "SlateCore", "Projects"
        });

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            // D3D RHI modules are only available on Windows; keep them private since
            // no public header exposes D3D types.
            PrivateDependencyModuleNames.AddRange(new string[]
            {
                "D3D12RHI",
                "D3D11RHI"
            });

            // Pull in engine DX11/DX12 libs for D3D11-on-12 interop.
            AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");
            AddEngineThirdPartyPrivateStaticDependencies(Target, "DX11");

            string PlatformString = "amd64";
            string SpoutLibPath = Path.Combine(ThirdPartyPath, "Spout", "lib", PlatformString, "Spout.lib");
            string SpoutDllPath = Path.Combine(ThirdPartyPath, "Spout", "lib", PlatformString, "Spout.dll");

            // Link against the Spout import library.
            PublicAdditionalLibraries.Add(SpoutLibPath);

            // Delay-load Spout.dll so the module itself loads even when the DLL is
            // resolved late, and so the loader picks it up from the staged binary dir.
            PublicDelayLoadDLLs.Add("Spout.dll");

            // Stage Spout.dll next to the plugin's own module binary via Unreal's
            // RuntimeDependencies system instead of manually copying it into the project
            // folder. $(BinaryOutputDir) (the plugin's Binaries/Win64) is used rather than
            // $(TargetOutputDir) so the DLL lands in the same place for normal project
            // builds, RunUAT BuildPlugin packaging, and precompiled release zips — and so
            // it matches the location the startup diagnostics check.
            RuntimeDependencies.Add("$(BinaryOutputDir)/Spout.dll", SpoutDllPath);
        }
    }
}
