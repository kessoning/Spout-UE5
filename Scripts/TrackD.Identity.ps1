# Copyright (c) 2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
# Dot-sourced by TrackD.psm1. Build.cs uses the same ordinal path/hash encoding.
function Get-TrackDSourceIdentity {
    param([Parameter(Mandatory)][string]$Root, [ValidateSet('Plugin','Host')][string]$Kind = 'Plugin')
    $rootPath = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\','/')
    $dirs = if ($Kind -eq 'Plugin') { @('Source','ThirdParty','Content','Config','Shaders','Resources') } else { @('Source','Config') }
    $descriptor = if ($Kind -eq 'Plugin') { 'SpoutPlugin.uplugin' } else { 'SpoutHostTest.uproject' }
    $paths = @($descriptor)
    foreach ($dir in $dirs) {
        $absolute = Join-Path $rootPath $dir
        if (Test-Path -LiteralPath $absolute) {
            $paths += @(Get-ChildItem -LiteralPath $absolute -Recurse -File | ForEach-Object { $_.FullName.Substring($rootPath.Length + 1).Replace('\','/') })
        }
    }
    [string[]]$ordered = $paths
    [Array]::Sort($ordered, [StringComparer]::Ordinal)
    $canonical = New-Object Text.StringBuilder
    $files = @()
    foreach ($relative in $ordered) {
        $hash = Get-TrackDSha256 (Join-Path $rootPath $relative)
        if (-not $hash) { throw "Cannot identify missing build input: $relative" }
        $hash = $hash.ToLowerInvariant()
        [void]$canonical.Append($relative).Append("`t").Append($hash).Append("`n")
        $files += [ordered]@{ Path=$relative; SHA256=$hash }
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $digest = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($canonical.ToString()))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
    return [ordered]@{ Schema='spout-build-inputs/1'; Kind=$Kind; Digest=$digest; Files=$files }
}

function Expand-TrackDArchiveCandidate {
    param([Parameter(Mandatory)][string]$ArchivePath,[Parameter(Mandatory)][string]$DestinationParent,[Parameter(Mandatory)][string]$EvidenceRoot)
    $identity = Read-TrackDArtifactIdentity -Artifact $ArchivePath
    $root = Join-Path ([IO.Path]::GetFullPath($DestinationParent)) ('zip-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
    New-Item -ItemType Directory -Path $root | Out-Null
    $zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $ArchivePath).Path)
    $files = @()
    try {
        foreach ($entry in $zip.Entries) {
            $relative = $entry.FullName.Replace('\','/')
            if (-not $relative.StartsWith('SpoutPlugin/') -or $relative.Split('/') -contains '..' -or $relative -match ':') { throw "Unsafe or unexpected archive entry: $relative" }
            if (-not $entry.Name) { continue }
            $target = [IO.Path]::GetFullPath((Join-Path $root $relative))
            if (-not $target.StartsWith($root + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Archive entry escapes its destination' }
            New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$target,$false)
            $files += @{Path=$relative;SHA256=(Get-TrackDSha256 $target);Bytes=$entry.Length}
        }
    } finally { $zip.Dispose() }
    $plugin = Join-Path $root 'SpoutPlugin'
    $installedInputs = Get-TrackDSourceIdentity $plugin
    # BuildPlugin rewrites .uplugin metadata (Installed/EngineVersion/default fields) and
    # consumes FilterPlugin.ini instead of shipping it. Keep producer and installed hashes
    # separate. Every other source/content/dependency byte must match the producer record;
    # the immutable archive hash also covers both packaging transformations and binaries.
    $packagingInputs = @('SpoutPlugin.uplugin','Config/FilterPlugin.ini')
    $expected = @{}
    foreach ($file in $identity.SourceIdentity.Files) { if ($file.Path -notin $packagingInputs) { $expected[$file.Path] = $file.SHA256 } }
    foreach ($file in $installedInputs.Files) {
        if ($file.Path -in $packagingInputs) { continue }
        if (-not $expected.ContainsKey($file.Path) -or $expected[$file.Path] -ne $file.SHA256) { throw "Extracted archive source differs from its build identity: $($file.Path)" }
        $expected.Remove($file.Path)
    }
    if ($expected.Count) { throw "Archive is missing build inputs: $($expected.Keys -join ', ')" }
    $result = [ordered]@{ArchivePath=(Resolve-Path -LiteralPath $ArchivePath).Path;SHA256=(Get-TrackDSha256 $ArchivePath);PluginRoot=$plugin;BuildIdentity=$identity;InstalledSourceIdentity=$installedInputs;Files=$files}
    Write-TrackDJson $result (Join-Path $EvidenceRoot 'archive-candidate.json')
    return $result
}

function Build-TrackDNativeTools {
    param([Parameter(Mandatory)][string]$RepoRoot, [Parameter(Mandatory)][string]$WorkRoot, [Parameter(Mandatory)][string]$EvidenceRoot)
    $buildDir = Join-Path $WorkRoot ('native-' + [Guid]::NewGuid().ToString('N'))
    $nativeSources = @(Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'Tests/Native') -Recurse -File | Sort-Object FullName | ForEach-Object { [ordered]@{Path=$_.FullName; SHA256=(Get-TrackDSha256 $_.FullName)} })
    $pluginInputs = Get-TrackDSourceIdentity -Root $RepoRoot
    $configure = Invoke-TrackDProcess -FilePath (Get-Command cmake -ErrorAction Stop).Source -ArgumentList @('-S',(Join-Path $RepoRoot 'Tests/Native'),'-B',$buildDir,'-A','x64') -StdOutPath (Join-Path $EvidenceRoot 'native-configure.log') -TimeoutSeconds 120 -WorkingDirectory $RepoRoot
    if ($configure.ExitCode -ne 0 -or $configure.TimedOut) { throw 'Native regression configuration failed' }
    $build = Invoke-TrackDProcess -FilePath (Get-Command cmake).Source -ArgumentList @('--build',$buildDir,'--config','Release') -StdOutPath (Join-Path $EvidenceRoot 'native-build.log') -TimeoutSeconds 300 -WorkingDirectory $RepoRoot
    if ($build.ExitCode -ne 0 -or $build.TimedOut) { throw 'Native regression build failed' }
    $test = Invoke-TrackDProcess -FilePath (Get-Command ctest -ErrorAction Stop).Source -ArgumentList @('--test-dir',$buildDir,'-C','Release','--output-on-failure','--no-tests=error') -StdOutPath (Join-Path $EvidenceRoot 'native-tests.log') -TimeoutSeconds 120 -WorkingDirectory $RepoRoot
    if ($test.ExitCode -ne 0 -or $test.TimedOut) { throw 'Native SDK recovery/debug probe tests failed' }
    $exe = Join-Path $buildDir 'Release/SpoutPixelPeer.exe'
    foreach ($file in $nativeSources) { if ((Get-TrackDSha256 $file.Path) -ne $file.SHA256) { throw 'Native test sources changed during the build' } }
    if ((Get-TrackDSourceIdentity -Root $RepoRoot).Digest -ne $pluginInputs.Digest) { throw 'Plugin inputs changed during native oracle build' }
    $identity = [ordered]@{ Executable=$exe; SHA256=(Get-TrackDSha256 $exe); SourceFiles=$nativeSources; PluginInputs=$pluginInputs; Build=$build; Tests=$test }
    Write-TrackDJson $identity (Join-Path $EvidenceRoot 'native-tools.json')
    return $identity
}

function Build-TrackDControlHost {
    param([string]$RepoRoot, [string]$WorkRoot, [string]$EvidenceRoot, $Engine,
        [string]$Configuration='Development', [int]$TimeoutSeconds=5400)
    $root = Join-Path $WorkRoot ('b'+$Engine.Version.Replace('.','')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
    $project = Join-Path $root 'P'
    $archive = Join-Path $root 'A'
    $null = Copy-TrackDTree -Source (Join-Path $RepoRoot 'Tests/SpoutBaselineHost') -Destination $project
    $null = Copy-TrackDTree -Source (Join-Path $RepoRoot 'Tests/SpoutHostTest/Config') -Destination (Join-Path $project 'Config')
    $hostInputs = Get-TrackDSourceIdentity -Root $project -Kind Host
    $git = Get-TrackDGitInfo $RepoRoot
    $log = Join-Path $EvidenceRoot 'package-control.log'
    $arguments = @('BuildCookRun', "-project=$project/SpoutHostTest.uproject", '-noP4',
        '-platform=Win64', '-targetplatform=Win64', "-clientconfig=$Configuration",
        '-map=/Engine/Maps/Entry+/Engine/Maps/Templates/Template_Default',
        '-build','-cook','-stage','-pak','-archive', "-archivedirectory=$archive",
        '-nodebuginfo','-utf8output','-unattended','-nocompileuat','-ubtargs=-MaxParallelActions=4')
    for ($attempt=1; $attempt -le 4; $attempt++) {
        $null = Wait-TrackDBuildToolIdle
        $result = Invoke-TrackDProcess -FilePath $Engine.RunUAT -ArgumentList $arguments -StdOutPath $log -TimeoutSeconds $TimeoutSeconds -WorkingDirectory $WorkRoot
        if ($result.ExitCode -eq 0 -or $result.TimedOut -or -not (Test-TrackDLogForBuildMutex $log)) { break }
    }
    if ($result.ExitCode -ne 0 -or $result.TimedOut) { throw "Control host packaging failed: see $log" }
    if ((Get-TrackDSourceIdentity -Root $project -Kind Host).Digest -ne $hostInputs.Digest) { throw 'Control host inputs changed during packaging' }
    if (@(Get-ChildItem -LiteralPath $archive -Recurse -File | Where-Object { $_.Name -in @('Spout.dll','SpoutPlugin.uplugin') }).Count -ne 0) { throw 'Control package contains Spout' }
    $binaryDir = Join-Path $archive 'Windows/SpoutHostTest/Binaries/Win64'
    $exe = @(Get-ChildItem -LiteralPath $binaryDir -Filter 'SpoutHostTest*.exe' -File)
    if ($exe.Count -ne 1) { throw 'Expected one control executable' }
    $absent = [ordered]@{ Schema='spout-absent-plugin/1'; Kind='Control'; Digest='no-plugin'; Files=@() }
    $identity = New-TrackDArtifactIdentity -Artifact $exe[0].FullName -SourceIdentity $absent -HostSourceIdentity $hostInputs -Engine $Engine -Configuration $Configuration -Git $git -CompanionRoot $archive
    return [ordered]@{ Executable=$exe[0].FullName; BuildIdentity=$identity; BuildProcess=$result }
}

function New-TrackDEvidenceRun {
    param([Parameter(Mandatory)][string]$Parent, [string]$Label='run')
    if ($Label -notmatch '^[a-zA-Z0-9_.-]+$') { throw 'Invalid evidence label' }
    $id = [Guid]::NewGuid().ToString('N')
    $path = [IO.Path]::GetFullPath((Join-Path $Parent "$Label-$id"))
    if (Test-Path -LiteralPath $path) { throw 'Evidence path already exists' }
    New-Item -ItemType Directory -Path $path | Out-Null
    return [ordered]@{ RunId=$id; Directory=$path }
}

function New-TrackDLaunchIdentity {
    param([Parameter(Mandatory)][string]$Directory, [string]$PluginRoot,
        [string]$HostRoot, [Parameter(Mandatory)][string]$Executable, $ArtifactIdentity)
    $run = New-TrackDEvidenceRun -Parent $Directory -Label 'identity'
    $plugin = if ($ArtifactIdentity) { $ArtifactIdentity.SourceIdentity } else { Get-TrackDSourceIdentity -Root $PluginRoot }
    $hostInputs = if ($ArtifactIdentity) { $ArtifactIdentity.HostSourceIdentity } else { Get-TrackDSourceIdentity -Root $HostRoot -Kind Host }
    if (-not $hostInputs -or -not $hostInputs.Digest) { throw 'Missing host build identity' }
    $identity = [ordered]@{ RunId=$run.RunId; SourceId=$plugin.Digest; HostSourceId=$hostInputs.Digest;
        Executable=(Resolve-Path -LiteralPath $Executable).Path; ExecutableSHA256=(Get-TrackDSha256 $Executable);
        AttestationPath=(Join-Path $run.Directory 'observed.json'); SourceInputs=$plugin; HostInputs=$hostInputs;
        ArtifactIdentity=$ArtifactIdentity; ResolvedCompanionRoot=(Resolve-TrackDCompanionRoot $Executable $ArtifactIdentity) }
    Write-TrackDJson $identity (Join-Path $run.Directory 'expected.json')
    return $identity
}

function Get-TrackDIdentityArguments {
    param([Parameter(Mandatory)]$Identity)
    return @("-SpoutRunId=$($Identity.RunId)","-SpoutSourceId=$($Identity.SourceId)","-SpoutHostSourceId=$($Identity.HostSourceId)","-SpoutAttestationPath=$($Identity.AttestationPath)")
}

function Test-TrackDLaunchIdentity {
    param([Parameter(Mandatory)]$Identity, [Parameter(Mandatory)]$ProcessResult)
    $problems = @()
    if (-not (Test-Path -LiteralPath $Identity.AttestationPath)) { $problems += 'No current-process identity attestation was produced' }
    else {
        try {
            $observed = Get-Content -LiteralPath $Identity.AttestationPath -Raw | ConvertFrom-Json
            if (-not $observed.matched -or $observed.runId -ne $Identity.RunId -or $observed.sourceId -ne $Identity.SourceId -or $observed.hostSourceId -ne $Identity.HostSourceId) { $problems += 'Run/source identity mismatch' }
            if ($observed.processId -ne $ProcessResult.ProcessId) { $problems += 'Attestation belongs to a different process' }
            if ([IO.Path]::GetFullPath($observed.executable).Replace('/','\') -ine $Identity.Executable.Replace('/','\')) { $problems += 'Attestation belongs to a different executable' }
        } catch { $problems += "Invalid identity attestation: $($_.Exception.Message)" }
    }
    if ((Get-TrackDSha256 $Identity.Executable) -ne $Identity.ExecutableSHA256) { $problems += 'Executed binary changed during the run' }
    if ($Identity.ArtifactIdentity -and $Identity.ArtifactIdentity.CompanionRoot) {
        foreach ($file in $Identity.ArtifactIdentity.CompanionFiles) {
            if ((Get-TrackDSha256 (Join-Path $Identity.ResolvedCompanionRoot $file.Path)) -ne $file.SHA256) { $problems += "Packaged input changed: $($file.Path)" }
        }
    }
    return [ordered]@{ Passed=($problems.Count -eq 0); Problems=$problems; Expected=$Identity }
}

function New-TrackDArtifactIdentity {
    param([Parameter(Mandatory)][string]$Artifact, [Parameter(Mandatory)]$SourceIdentity,
        [Parameter(Mandatory)]$Engine, [string]$Configuration, $Git, $HostSourceIdentity, [string]$CompanionRoot)
    $companions = @()
    $artifactRelativePath = $null
    if ($CompanionRoot) {
        $CompanionRoot = (Resolve-Path -LiteralPath $CompanionRoot).Path
        $artifactPath = (Resolve-Path -LiteralPath $Artifact).Path
        if (-not $artifactPath.StartsWith($CompanionRoot.TrimEnd('\')+'\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Artifact is outside its package root' }
        $artifactRelativePath = $artifactPath.Substring($CompanionRoot.Length+1).Replace('\','/')
        $companions = @(Get-ChildItem -LiteralPath $CompanionRoot -Recurse -File | Where-Object { $_.Name -notlike '*.identity.json' } | Sort-Object FullName | ForEach-Object {
            [ordered]@{ Path=$_.FullName.Substring($CompanionRoot.Length+1).Replace('\','/'); SHA256=(Get-TrackDSha256 $_.FullName) }
        })
    }
    $record = [ordered]@{ Schema='spout-artifact/1'; BuildId=[Guid]::NewGuid().ToString('N'); CreatedUtc=[DateTime]::UtcNow.ToString('o');
        SHA256=(Get-TrackDSha256 $Artifact); SourceIdentity=$SourceIdentity; HostSourceIdentity=$HostSourceIdentity; Engine=$Engine; Configuration=$Configuration; BuildGit=$Git;
        CompanionRoot=$CompanionRoot; ArtifactRelativePath=$artifactRelativePath; CompanionFiles=$companions }
    $sidecar = "$Artifact.identity.json"
    if (Test-Path -LiteralPath $sidecar) { throw "Artifact identity already exists: $sidecar" }
    Write-TrackDJson $record $sidecar
    return $record
}

function Resolve-TrackDCompanionRoot {
    param([string]$Artifact, $Identity)
    if (-not $Identity -or -not $Identity.CompanionRoot) { return $null }
    $root = (Resolve-Path -LiteralPath $Artifact).Path
    foreach ($segment in $Identity.ArtifactRelativePath.Split('/')) {
        if ($segment -in @('..','.','')) { throw 'Invalid package-relative artifact path' }
        $root = Split-Path -Parent $root
    }
    return $root
}

function Read-TrackDArtifactIdentity {
    param([Parameter(Mandatory)][string]$Artifact)
    $sidecar = "$Artifact.identity.json"
    if (-not (Test-Path -LiteralPath $sidecar)) { throw "No build provenance for $Artifact. Rebuild the artifact; current checkout metadata cannot identify an old binary." }
    $identity = Get-Content -LiteralPath $sidecar -Raw | ConvertFrom-Json
    if ($identity.Schema -ne 'spout-artifact/1' -or -not $identity.BuildId -or -not $identity.SourceIdentity.Digest -or $identity.SHA256 -ne (Get-TrackDSha256 $Artifact)) { throw "Artifact/provenance mismatch: $Artifact" }
    if ($identity.CompanionRoot) {
        $resolvedRoot = Resolve-TrackDCompanionRoot $Artifact $identity
        foreach ($file in $identity.CompanionFiles) {
            if ((Get-TrackDSha256 (Join-Path $resolvedRoot $file.Path)) -ne $file.SHA256) { throw "Packaged companion/provenance mismatch: $($file.Path)" }
        }
    }
    return $identity
}
