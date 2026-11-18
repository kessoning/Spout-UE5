// Copyright (c) 2023 RockmanZuyi
// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

using System;
using System.IO;
using System.Linq;
using System.Text;
using UnrealBuildTool;

public class SpoutPlugin : ModuleRules
{
    private string ModulePath {
        // Plugin root directory (two levels up from ModuleDirectory).
        get { return Path.GetFullPath(Path.Combine(ModuleDirectory, "../../")); }
    }

    private string ThirdPartyPath {
        // Third-party dependencies bundled with the plugin, inside Source/ (Fab technical requirement 4.3.7.3.d).
        get { return Path.GetFullPath(Path.Combine(ModulePath, "Source", "ThirdParty/")); }
    }

    /**
     * The digest of every plugin input (sources, SDK, content, configuration, shaders, resources and the descriptor),
     * in a fixed order, so the test host can tell which files a binary was built from. Each file is registered with
     * UBT, so a cached makefile cannot keep an older digest after an edit. A file that cannot be read gives
     * "unidentified" rather than a failed build.
     */
    private string ComputeSourceIdentity()
    {
        try
        {
            var identityFiles = new[] { "Source", "Content", "Config", "Shaders", "Resources" }
                .SelectMany(dir => Directory.Exists(Path.Combine(ModulePath, dir))
                    ? Directory.GetFiles(Path.Combine(ModulePath, dir), "*", SearchOption.AllDirectories) : new string[0])
                .Concat(new[] { Path.Combine(ModulePath, "SpoutPlugin.uplugin") })
                .OrderBy(file => Path.GetRelativePath(ModulePath, file).Replace('\\', '/'), StringComparer.Ordinal).ToArray();
            // UE 5.3 leaves Cryptography.Primitives out of the rules references, so SHA256.HashData is resolved at run time.
            var hashMethod = System.Type.GetType("System.Security.Cryptography.SHA256, System.Security.Cryptography.Algorithms", true)
                .GetMethod("HashData", new[] { typeof(byte[]) });
            Func<byte[], string> hash = data => BitConverter.ToString((byte[])hashMethod.Invoke(null, new object[] { data }))
                .Replace("-", "").ToLowerInvariant();
            var canonical = new StringBuilder();
            foreach (var file in identityFiles)
            {
                ExternalDependencies.Add(file);
                canonical.Append(Path.GetRelativePath(ModulePath, file).Replace('\\', '/')).Append('\t')
                    .Append(hash(File.ReadAllBytes(file))).Append('\n');
            }
            return hash(Encoding.UTF8.GetBytes(canonical.ToString()));
        }
        catch (Exception)
        {
            return "unidentified";
        }
    }

    public SpoutPlugin(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        // The plugin's own test builds only. With the marker file SpoutTests.enable in the plugin's root folder,
        // editor and development builds compile the automation tests, their hooks and the development console
        // commands, and every binary records the digest of the plugin files it was built from, which the test host
        // compares with the files it installed. Without the marker, which is how the plugin ships, none of this is
        // compiled and the build reads only the files it compiles.
        string testsMarker = Path.Combine(ModulePath, "SpoutTests.enable");
        bool bTests = File.Exists(testsMarker);
        if (bTests)
        {
            ExternalDependencies.Add(testsMarker);
        }
        PrivateDefinitions.Add("SPOUT_WITH_TESTS=" + (bTests ? "WITH_DEV_AUTOMATION_TESTS" : "0"));
        PrivateDefinitions.Add("SPOUT_BUILD_SOURCE_ID=\"" + (bTests ? ComputeSourceIdentity() : "unidentified") + "\"");

        // Instrumented builds only. The marker file sits in the plugin's root folder, outside every
        // directory hashed above and outside the declared release inputs, so the source identity of
        // an instrumented build equals that of the release build made from the same commit, and no
        // release can carry the instrumentation. Without the marker SPOUT_WITH_INSTRUMENT is 0 and
        // SpoutInstrument.cpp and every SPOUT_INSTRUMENT(...) statement compile to nothing.
        //
        // The marker is registered with UBT only while it exists: a registered file that is missing
        // invalidates the makefile on every build. Removing the marker is therefore always noticed;
        // adding it to a tree that was already built may not be, so make instrumented builds in a
        // fresh tree and check for the instrumentation's console variable afterwards.
        string instrumentMarker = Path.Combine(ModulePath, "SpoutInstrument.enable");
        bool bInstrument = File.Exists(instrumentMarker);
        if (bInstrument)
        {
            ExternalDependencies.Add(instrumentMarker);
        }
        PrivateDefinitions.Add("SPOUT_WITH_INSTRUMENT=" + (bInstrument ? "1" : "0"));

        // D3D diagnostic builds only, by the same rule as the instrumentation marker above (a marker file in the plugin's
        // root, outside every hashed directory). Without it SPOUT_WITH_D3D_DIAG is 0 and the diagnostic code in
        // SpoutDebugCapture.cpp and every SPOUT_D3D_DIAG_TRANSITION(...) statement compile to nothing.
        string d3dDiagMarker = Path.Combine(ModulePath, "SpoutD3DDiag.enable");
        bool bD3DDiag = File.Exists(d3dDiagMarker);
        if (bD3DDiag)
        {
            ExternalDependencies.Add(d3dDiagMarker);
        }
        PrivateDefinitions.Add("SPOUT_WITH_D3D_DIAG=" + (bD3DDiag ? "1" : "0"));

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
            // The Project Settings page (USpoutSettings, a UDeveloperSettings).
            "DeveloperSettings",
            "Engine",
            "RHI",
            "RenderCore"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            // "Projects" provides IPluginManager, used for startup Spout.dll diagnostics.
            // "SpoutPluginShaders" decodes 10-bit frames received with Color Space = sRGB (SpoutColorDecode.h).
            "ApplicationCore", "Slate", "SlateCore", "Projects", "SpoutPluginShaders"
        });
        if (Target.bBuildEditor && bTests)
        {
            // The editor tests compile and execute the public Blueprint nodes.
            PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "BlueprintGraph", "KismetCompiler" });
        }

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

            // Stage Spout.dll beside the binary this module links into via Unreal's
            // RuntimeDependencies system instead of manually copying it. $(BinaryOutputDir)
            // resolves to the plugin's own Binaries/Win64 in editor/modular project builds,
            // and to the project's Binaries/Win64 (next to the game executable) in packaged
            // monolithic games. Raw RunUAT BuildPlugin output runs no target staging and so
            // does not carry the DLL; build.ps1 stages it into release zips explicitly. The
            // startup loader (LoadSpoutRuntime) checks all of these locations.
            RuntimeDependencies.Add("$(BinaryOutputDir)/Spout.dll", SpoutDllPath);
        }
    }
}
