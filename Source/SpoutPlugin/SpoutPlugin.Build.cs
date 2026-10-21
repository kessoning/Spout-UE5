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
        // Third-party dependencies bundled with the plugin.
        get { return Path.GetFullPath(Path.Combine(ModulePath, "ThirdParty/")); }
    }

    public SpoutPlugin(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        // Every compiled binary carries the digest of its actual staged inputs. Register each
        // file with UBT so a cached rules/makefile cannot retain an older digest after an edit.
        var identityFiles = new[] { "Source", "ThirdParty", "Content", "Config", "Shaders", "Resources" }
            .SelectMany(dir => Directory.Exists(Path.Combine(ModulePath, dir))
                ? Directory.GetFiles(Path.Combine(ModulePath, dir), "*", SearchOption.AllDirectories) : new string[0])
            .Concat(new[] { Path.Combine(ModulePath, "SpoutPlugin.uplugin") })
            .OrderBy(file => Path.GetRelativePath(ModulePath, file).Replace('\\', '/'), StringComparer.Ordinal).ToArray();
        // UE 5.3 omits Cryptography.Primitives from rules references. Resolve the platform SHA256
        // static method at runtime, without changing engine assemblies or implementing cryptography.

        {
            var hashMethod = System.Type.GetType("System.Security.Cryptography.SHA256, System.Security.Cryptography.Algorithms", true)
                .GetMethod("HashData", new[] { typeof(byte[]) });
            Func<byte[], byte[]> hash = data => (byte[])hashMethod.Invoke(null, new object[] { data });
            var canonical = new StringBuilder();
            foreach (var file in identityFiles)
            {
                ExternalDependencies.Add(file);
                canonical.Append(Path.GetRelativePath(ModulePath, file).Replace('\\', '/')).Append('\t')
                    .Append(BitConverter.ToString(hash(File.ReadAllBytes(file))).Replace("-", "").ToLowerInvariant()).Append('\n');
            }
            string digest = BitConverter.ToString(hash(Encoding.UTF8.GetBytes(canonical.ToString()))).Replace("-", "").ToLowerInvariant();
            PrivateDefinitions.Add("SPOUT_BUILD_SOURCE_ID=\"" + digest + "\"");
        }

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
            "Engine",
            "RHI",
            "RenderCore"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            // "Projects" provides IPluginManager, used for startup Spout.dll diagnostics.
            "ApplicationCore", "Slate", "SlateCore", "Projects"
        });
        if (Target.bBuildEditor)
        {
            // Compile and execute the public Blueprint node in editor automation.
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
