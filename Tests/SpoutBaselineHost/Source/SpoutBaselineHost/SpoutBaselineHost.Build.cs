// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
using System;
using System.IO;
using System.Linq;
using System.Text;
using UnrealBuildTool;
public class SpoutBaselineHost : ModuleRules
{
    public SpoutBaselineHost(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {"Core", "CoreUObject", "Engine", "RHI", "RenderCore", "Json"});
        // Deliberately no Spout module, SDK headers, import library or runtime DLL dependency.
        var root = Path.GetFullPath(Path.Combine(ModuleDirectory, "../.."));
        var files = new[] {"Source", "Config"}.SelectMany(dir => Directory.GetFiles(Path.Combine(root, dir), "*", SearchOption.AllDirectories))
            .Concat(new[] {Path.Combine(root, "SpoutHostTest.uproject")})
            .OrderBy(file => Path.GetRelativePath(root, file).Replace('\\','/'), StringComparer.Ordinal);
        var method = System.Type.GetType("System.Security.Cryptography.SHA256, System.Security.Cryptography.Algorithms", true)
            .GetMethod("HashData", new[] {typeof(byte[])});
        Func<byte[],byte[]> hash = data => (byte[])method.Invoke(null, new object[] {data});
        var canonical = new StringBuilder();
        foreach (var file in files)
        {
            ExternalDependencies.Add(file);
            canonical.Append(Path.GetRelativePath(root,file).Replace('\\','/')).Append('\t')
                .Append(BitConverter.ToString(hash(File.ReadAllBytes(file))).Replace("-", "").ToLowerInvariant()).Append('\n');
        }
        PrivateDefinitions.Add("SPOUT_CONTROL_SOURCE_ID=\""+BitConverter.ToString(hash(Encoding.UTF8.GetBytes(canonical.ToString()))).Replace("-", "").ToLowerInvariant()+"\"");
    }
}
