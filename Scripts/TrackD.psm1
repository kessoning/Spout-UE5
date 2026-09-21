#requires -Version 7.2
# Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
# Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

<#
.SYNOPSIS
    Shared helpers for the release validation pipeline.

.DESCRIPTION
    Everything the validation scripts have in common: engine resolution, evidence-tree layout, hashing,
    process launching with a real timeout, host/GPU identification, staging-copy construction and
    JSON emission.

    Two rules this module exists to enforce:

      1. Nothing reports success it did not observe. Helpers return structured results with an
         explicit status; callers decide, and every script funnels its exit code through
         Get-TrackDExitCode so a skipped or missing row can never be printed as a pass.

      2. Nothing depends on an optional PowerShell module. Get-FileHash and Compress-Archive live in
         modules that a mismatched PSModulePath can make unresolvable - which is exactly what happens
         when pwsh is launched from a batch file that inherited Windows PowerShell's environment.
         SHA-256 and zip reading are done through .NET directly for the same reason build.ps1 does.

    PowerShell 7 is the target. Nothing here uses PS7-only syntax, so Windows PowerShell 5.1 also
    works, matching build.ps1's existing policy.
#>

Set-StrictMode -Version Latest

# The engine matrix the plugin declares support for. Kept identical to build.ps1's
# $DeclaredEngineVersions; Validate-Documentation.ps1 asserts they have not drifted apart.
$script:DeclaredEngineVersions = @('5.3', '5.4', '5.5', '5.6', '5.7', '5.8')

$script:PluginName = 'SpoutPlugin'
$script:HostProjectName = 'SpoutHostTest'

function Get-TrackDDeclaredEngineVersions
{
    return $script:DeclaredEngineVersions
}

function Get-TrackDPluginName      { return $script:PluginName }
function Get-TrackDHostProjectName { return $script:HostProjectName }

function Expand-TrackDList
{
    <#
    .SYNOPSIS
        Flattens a string array, splitting any element that is itself a comma-separated list.

    .DESCRIPTION
        "pwsh -File script.ps1 -EngineVersions 5.3,5.8" does not always bind 5.3,5.8 as two array
        elements: launched through cmd, a batch wrapper or a non-PowerShell shell it arrives as the
        single string "5.3,5.8", and the script then looks for an engine called "5.3,5.8". Splitting
        here makes both forms mean the same thing regardless of how the script was invoked.
    #>
    param([string[]] $Values)

    $expanded = New-Object System.Collections.ArrayList
    foreach ($value in $Values)
    {
        if ([string]::IsNullOrWhiteSpace($value)) { continue }
        foreach ($part in ($value -split ','))
        {
            $trimmed = $part.Trim()
            if ($trimmed) { [void] $expanded.Add($trimmed) }
        }
    }

    # Comma operator, not a bare return: PowerShell unrolls a returned array, and an EMPTY one
    # unrolls to nothing at all, so the caller gets $null. Under Set-StrictMode that turns an
    # ordinary "$list.Count -eq 0" into a runtime error instead of a false test.
    return ,([string[]] $expanded.ToArray())
}

# ---------------------------------------------------------------------------------------------
# Console output
# ---------------------------------------------------------------------------------------------

function Write-TrackDSection
{
    param([Parameter(Mandatory)][string] $Text)
    Write-Host ''
    Write-Host ('=' * 78) -ForegroundColor Cyan
    Write-Host $Text -ForegroundColor Cyan
    Write-Host ('=' * 78) -ForegroundColor Cyan
}

function Write-TrackDStep { param([string] $Text) Write-Host "  $Text" }
function Write-TrackDGood { param([string] $Text) Write-Host "  [ok]   $Text" -ForegroundColor Green }
function Write-TrackDBad  { param([string] $Text) Write-Host "  [FAIL] $Text" -ForegroundColor Red }
function Write-TrackDWarn { param([string] $Text) Write-Host "  [warn] $Text" -ForegroundColor Yellow }

# ---------------------------------------------------------------------------------------------
# Hashing and file identity
# ---------------------------------------------------------------------------------------------

function Get-TrackDSha256
{
    <#
    .SYNOPSIS
        SHA-256 of a file as lowercase hex, or $null when the file does not exist.
    #>
    param([Parameter(Mandatory)][string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf))
    {
        return $null
    }

    $stream = $null
    $sha = $null
    try
    {
        $stream = [System.IO.File]::OpenRead($Path)
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $bytes = $sha.ComputeHash($stream)
        return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
    }
    finally
    {
        if ($sha) { $sha.Dispose() }
        if ($stream) { $stream.Dispose() }
    }
}

function Get-TrackDFileFacts
{
    <#
    .SYNOPSIS
        Path, existence, size and SHA-256 for one file, in the shape the manifests expect.
    #>
    param([Parameter(Mandatory)][string] $Path)

    $exists = Test-Path -LiteralPath $Path -PathType Leaf
    return [ordered] @{
        Path      = $Path
        Exists    = $exists
        SizeBytes = if ($exists) { (Get-Item -LiteralPath $Path).Length } else { $null }
        Sha256    = if ($exists) { Get-TrackDSha256 $Path } else { $null }
    }
}

# ---------------------------------------------------------------------------------------------
# Repository facts
# ---------------------------------------------------------------------------------------------

function Get-TrackDPluginVersion
{
    <#
    .SYNOPSIS
        VersionName from SpoutPlugin.uplugin. The release version is never hard-coded anywhere.
    #>
    param([Parameter(Mandatory)][string] $RepoRoot)

    $descriptorPath = Join-Path $RepoRoot 'SpoutPlugin.uplugin'
    if (-not (Test-Path -LiteralPath $descriptorPath -PathType Leaf))
    {
        throw "Plugin descriptor not found: $descriptorPath"
    }

    $descriptor = Get-Content -LiteralPath $descriptorPath -Raw | ConvertFrom-Json
    if ([string]::IsNullOrWhiteSpace($descriptor.VersionName))
    {
        throw "Plugin descriptor has no VersionName: $descriptorPath"
    }
    return [string] $descriptor.VersionName
}

function Get-TrackDGitInfo
{
    <#
    .SYNOPSIS
        Commit, branch and dirty state, using read-only git only.

    .DESCRIPTION
        Every command here is read-only by construction. Nothing in the validation pipeline stages,
        commits, checks out or cleans; the clean-source copy is built by an explicit file copy from a
        declared manifest precisely so no destructive git command is ever needed.
    #>
    param([Parameter(Mandatory)][string] $RepoRoot)

    $info = [ordered] @{
        Commit           = $null
        Branch           = $null
        IsDirty          = $null
        TrackedFileCount = $null
    }

    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) { return $info }

    $commit = & git -C $RepoRoot rev-parse HEAD 2>$null
    if ($LASTEXITCODE -eq 0) { $info.Commit = ($commit | Out-String).Trim() }

    $branch = & git -C $RepoRoot rev-parse --abbrev-ref HEAD 2>$null
    if ($LASTEXITCODE -eq 0) { $info.Branch = ($branch | Out-String).Trim() }

    $status = & git -C $RepoRoot status --porcelain 2>$null
    if ($LASTEXITCODE -eq 0)
    {
        $info.IsDirty = -not [string]::IsNullOrWhiteSpace(($status | Out-String).Trim())
    }

    $tracked = & git -C $RepoRoot ls-files 2>$null
    if ($LASTEXITCODE -eq 0)
    {
        $info.TrackedFileCount = @($tracked | Where-Object { $_ }).Count
    }

    return $info
}

function Get-TrackDTrackedFiles
{
    <#
    .SYNOPSIS
        Repository-relative paths of every file git currently tracks, forward-slashed.
    #>
    param([Parameter(Mandatory)][string] $RepoRoot)

    # Comma operator throughout, for the same reason as Expand-TrackDList: an empty result must
    # reach the caller as an empty array, not as $null.
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) { return ,([string[]] @()) }

    $tracked = & git -C $RepoRoot ls-files 2>$null
    if ($LASTEXITCODE -ne 0) { return ,([string[]] @()) }

    return ,([string[]] @($tracked | Where-Object { $_ } | ForEach-Object { $_.Replace('\', '/') }))
}

function Get-TrackDHostSummary
{
    <#
    .SYNOPSIS
        Machine identity for the manifest: OS build, CPU, RAM, GPU and driver.

    .DESCRIPTION
        Best effort throughout. A machine that will not answer a CIM query still produces a usable
        manifest with nulls rather than aborting a release.
    #>

    $summary = [ordered] @{
        MachineName        = $env:COMPUTERNAME
        OperatingSystem    = $null
        OsVersion          = $null
        ProcessorName      = $null
        LogicalProcessors  = [System.Environment]::ProcessorCount
        TotalPhysicalBytes = $null
        Gpus               = @()
        PowerShellVersion  = $PSVersionTable.PSVersion.ToString()
    }

    try
    {
        $os = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction Stop
        $summary.OperatingSystem = $os.Caption
        $summary.OsVersion = $os.Version
        $summary.TotalPhysicalBytes = [int64] $os.TotalVisibleMemorySize * 1024
    }
    catch { }

    try
    {
        $cpu = Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop | Select-Object -First 1
        $summary.ProcessorName = $cpu.Name
    }
    catch { }

    try
    {
        $gpus = New-Object System.Collections.ArrayList
        foreach ($gpu in (Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop))
        {
            [void] $gpus.Add([ordered] @{
                Name          = $gpu.Name
                DriverVersion = $gpu.DriverVersion
                DriverDate    = if ($gpu.DriverDate) { $gpu.DriverDate.ToString('o') } else { $null }
                AdapterRamBytes = $gpu.AdapterRAM
            })
        }
        $summary.Gpus = $gpus.ToArray()
    }
    catch { }

    return $summary
}

# ---------------------------------------------------------------------------------------------
# Engines
# ---------------------------------------------------------------------------------------------

function Resolve-TrackDEnginePath
{
    <#
    .SYNOPSIS
        Absolute path of one engine install, from an explicit map or the <EngineRoot>\UE_<version>
        convention.
    #>
    param(
        [Parameter(Mandatory)][string] $Version,
        [string] $EngineRoot = 'D:\UE',
        [hashtable] $ExplicitMap = @{}
    )

    if ($ExplicitMap.ContainsKey($Version))
    {
        return [string] $ExplicitMap[$Version]
    }
    return (Join-Path $EngineRoot "UE_$Version")
}

function Get-TrackDEngineFacts
{
    <#
    .SYNOPSIS
        Everything the manifest records about one engine, plus whether it is usable at all.
    #>
    param(
        [Parameter(Mandatory)][string] $Version,
        [Parameter(Mandatory)][string] $EnginePath
    )

    $runUat = Join-Path $EnginePath 'Engine\Build\BatchFiles\RunUAT.bat'
    $buildBat = Join-Path $EnginePath 'Engine\Build\BatchFiles\Build.bat'
    $editorCmd = Join-Path $EnginePath 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'

    $facts = [ordered] @{
        Version       = $Version
        EnginePath    = $EnginePath
        RunUAT        = $runUat
        BuildBat      = $buildBat
        EditorCmd     = $editorCmd
        Found         = (Test-Path -LiteralPath $runUat -PathType Leaf)
        EditorPresent = (Test-Path -LiteralPath $editorCmd -PathType Leaf)
        BuildId       = $null
        BuildIdSource = $null
        BuildVersion  = $null
        BuildVersionData = $null
        BuildVersionSHA256 = $null
        EditorModulesSHA256 = $null
        MetadataWarnings = @()
    }

    $buildVersionPath = Join-Path $EnginePath 'Engine\Build\Build.version'
    if (Test-Path -LiteralPath $buildVersionPath -PathType Leaf)
    {
        try
        {
            $buildVersionBytes = [IO.File]::ReadAllBytes($buildVersionPath)
            $facts.BuildVersionSHA256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($buildVersionBytes)).ToLowerInvariant()
            $buildVersion = [Text.Encoding]::UTF8.GetString($buildVersionBytes).TrimStart([char]0xfeff) | ConvertFrom-Json
            $facts.BuildVersionData = $buildVersion
            $facts.BuildVersion = "$($buildVersion.MajorVersion).$($buildVersion.MinorVersion).$($buildVersion.PatchVersion)"
            if ($buildVersion.PSObject.Properties.Name -contains 'BuildId')
            {
                $facts.BuildId = [string] $buildVersion.BuildId
                $facts.BuildIdSource = 'Engine/Build/Build.version'
            }
        }
        catch { $facts.MetadataWarnings += "Cannot read Build.version metadata: $($_.Exception.Message)" }
    }

    # Installed engines keep the editor module compatibility ID in UnrealEditor.modules,
    # not Build.version. Preserve the full version/changelist separately from this ID.
    $modulesPath = Join-Path $EnginePath 'Engine/Binaries/Win64/UnrealEditor.modules'
    if (Test-Path -LiteralPath $modulesPath -PathType Leaf)
    {
        try
        {
            $modulesBytes = [IO.File]::ReadAllBytes($modulesPath)
            $facts.EditorModulesSHA256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($modulesBytes)).ToLowerInvariant()
            $modules = [Text.Encoding]::UTF8.GetString($modulesBytes).TrimStart([char]0xfeff) | ConvertFrom-Json
            if ($modules.PSObject.Properties.Name -contains 'BuildId' -and -not [string]::IsNullOrWhiteSpace([string]$modules.BuildId))
            {
                $facts.BuildId = [string]$modules.BuildId
                $facts.BuildIdSource = 'Engine/Binaries/Win64/UnrealEditor.modules'
            }
        }
        catch { $facts.MetadataWarnings += "Cannot read UnrealEditor.modules metadata: $($_.Exception.Message)" }
    }

    if ($facts.BuildVersion -and "$($facts.BuildVersionData.MajorVersion).$($facts.BuildVersionData.MinorVersion)" -ne $Version)
    {
        throw "Engine version mismatch: requested UE $Version, but '$EnginePath' reports UE $($facts.BuildVersion). Correct the engine path override."
    }
    return $facts
}

function Test-TrackDUnrealBuildToolBusy
{
    <#
    .SYNOPSIS
        True when another UnrealBuildTool / AutomationTool is running on this machine.

    .DESCRIPTION
        The UBT global mutex is machine-wide, so an unrelated build elsewhere aborts this one with a
        "conflicting instance" error. The pipeline retries those, but only after confirming the conflict
        is genuinely gone - it never kills a process it did not start. The retry is recorded in the
        manifest as an environmental event, kept separate from plugin failures.

        The process-NAME check alone is not enough: UE 5.x runs both UBT and AutomationTool inside
        `dotnet.exe`, so there is no process called UnrealBuildTool to find. The command line is what
        identifies them.
    #>

    $names = @('UnrealBuildTool', 'AutomationTool', 'UnrealHeaderTool')
    foreach ($name in $names)
    {
        $running = Get-Process -Name $name -ErrorAction SilentlyContinue
        if ($running) { return $true }
    }

    $hosted = @(Get-CimInstance Win32_Process -Filter "Name='dotnet.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -match 'UnrealBuildTool\.dll|AutomationTool\.dll' })
    if ($hosted.Count -gt 0) { return $true }

    return $false
}

function Get-TrackDBuildToolStatePaths
{
    <#
    .SYNOPSIS
        The machine-global files UnrealBuildTool rewrites on startup, shared by every engine.

    .DESCRIPTION
        Neither of these is per engine or per project, which is the entire problem. Two engines
        built back to back contend on the same two things:

          %LOCALAPPDATA%\UnrealBuildTool\Log.txt
              Rotated to Log-backup.txt before UBT reads a single argument.

          %LOCALAPPDATA%\UnrealEngine\Intermediate\Build\BuildRules\*.dll
              The compiled plugin/marketplace rules assembly. Each engine version compiles a
              DIFFERENT assembly to this ONE path, so every engine rebuilds it, and a previous
              UBT that still has it loaded holds a lock on it.
    #>

    $paths = New-Object System.Collections.ArrayList

    $localAppData = $env:LOCALAPPDATA
    if ([string]::IsNullOrWhiteSpace($localAppData)) { return ,([string[]] @()) }

    [void] $paths.Add((Join-Path $localAppData 'UnrealBuildTool\Log.txt'))

    $rulesDir = Join-Path $localAppData 'UnrealEngine\Intermediate\Build\BuildRules'
    if (Test-Path -LiteralPath $rulesDir -PathType Container)
    {
        foreach ($dll in (Get-ChildItem -LiteralPath $rulesDir -File -Filter '*.dll' -ErrorAction SilentlyContinue))
        {
            [void] $paths.Add($dll.FullName)
        }
    }

    return ,([string[]] $paths.ToArray())
}

function Test-TrackDBuildToolStateBusy
{
    <#
    .SYNOPSIS
        True while another process still holds UnrealBuildTool's machine-global state open.

    .DESCRIPTION
        Probes the contended files themselves for an exclusive handle rather than guessing at
        process names, because the holder is not always a process you would think to look for: a
        departing Unreal editor holds the shared log for a few seconds after its tests report
        complete, and a departing UBT holds the rules assembly it loaded.

        That is how a matrix can fail several engines within seconds and then recover on its own,
        with each failure an unhandled IOException that says nothing about mutexes at all.

        Read-only: each handle is opened and immediately closed. A path that does not exist is not
        contention - on a machine that has never run UBT there is nothing to rotate.
    #>
    param([string[]] $Path)

    if (-not $PSBoundParameters.ContainsKey('Path')) { $Path = Get-TrackDBuildToolStatePaths }

    foreach ($candidate in $Path)
    {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }

        $stream = $null
        try
        {
            $stream = [System.IO.File]::Open($candidate, [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        }
        catch
        {
            return $true
        }
        finally
        {
            if ($stream) { $stream.Dispose() }
        }
    }

    return $false
}

function Wait-TrackDBuildToolIdle
{
    <#
    .SYNOPSIS
        Waits until no other build tool is running and the shared UBT log is free. Returns the
        seconds waited.

    .DESCRIPTION
        Called BEFORE a build rather than only after one has already failed, because the contention
        it waits out is normal and transient - the previous engine's editor exiting - and a build
        that never starts into a conflict needs no retry, no log to classify and no manifest entry
        arguing that a failure was environmental.

        It waits; it never kills. If the timeout expires the build is attempted anyway, so a
        genuinely stuck machine still produces a real failure with a real log instead of hanging.
    #>
    param(
        [int] $TimeoutSeconds = 300,
        [int] $PollSeconds = 5
    )

    $waited = 0
    while ($waited -lt $TimeoutSeconds)
    {
        if (-not (Test-TrackDUnrealBuildToolBusy) -and -not (Test-TrackDBuildToolStateBusy)) { break }
        Start-Sleep -Seconds $PollSeconds
        $waited += $PollSeconds
    }
    return $waited
}

function Test-TrackDLogForBuildMutex
{
    <#
    .SYNOPSIS
        True when a captured log shows environmental build-tool contention rather than a real
        build failure.

    .DESCRIPTION
        Two signatures, because UBT reports the same underlying conflict two entirely different ways.

        Without -waitmutex it says so plainly: "A conflicting instance of UnrealBuildTool is already
        running". With -waitmutex it never gets that far - it dies first on the shared log rotation
        described in Test-TrackDBuildToolLogBusy, as an unhandled IOException with no mention of a
        mutex anywhere in the text. Matching only the first phrasing would classify every build that
        passes -waitmutex, which is every build in the automation stage, as a genuine failure and
        never retry it.
    #>
    param([string] $LogPath)

    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) { return $false }
    $text = Get-Content -LiteralPath $LogPath -Raw -ErrorAction SilentlyContinue
    if (-not $text) { return $false }

    if ($text -match 'conflicting instance' -or
        $text -match 'Another instance of UnrealBuildTool is already running' -or
        $text -match 'Timed out waiting for another instance')
    {
        return $true
    }

    # The two "shared state was locked" forms. Narrow deliberately: an IOException anywhere else in
    # a build is a real failure, so the exception must appear together with the specific frame or
    # path that identifies the contended machine-global file.
    #
    #   Log.BackupLogFile / Log.AddFileWriter   %LOCALAPPDATA%\UnrealBuildTool\Log.txt
    #   BuildRules\...Rules.dll                 the shared compiled rules assembly, which each
    #                                           engine version rebuilds to the same one path
    #
    # 'IOException' unqualified, not 'System.IO.IOException': engine versions disagree about which
    # they print. UE 5.4 writes the full type name, UE 5.8 writes the short one, and requiring the
    # long form would silently miss UE 5.8. Narrowness comes from the shared-state frame below, not
    # from how verbosely the exception was named.
    if ($text -notmatch 'IOException') { return $false }

    return ($text -match 'Log\.BackupLogFile' -or
            $text -match 'Log\.AddFileWriter' -or
            $text -match 'Intermediate[\\/]Build[\\/]BuildRules' -or
            $text -match 'DynamicCompilation\.CompileAssembly')
}

function Test-TrackDLogForResourceExhaustion
{
    <#
    .SYNOPSIS
        True when a build failed because the MACHINE ran out of resources, not because the code is
        wrong.

    .DESCRIPTION
        MSVC compiling an Unreal PCH is memory-hungry, and UBT runs one process per physical core.
        On a busy machine that combination produces:

            c1xx: error C3859: Failed to create virtual memory for PCH
            c1xx: fatal error C1076: compiler limit: internal heap limit reached

        Those say nothing about the plugin - the same source compiles cleanly on the same engine
        when memory is available. They are retried for the same reason the UnrealBuildTool mutex is,
        and counted separately in the manifest so an environmental retry can never be mistaken for
        a flaky plugin.

        Deliberately narrow: only the specific out-of-resources diagnostics match. A genuine
        compiler error still fails the row on the first attempt.
    #>
    param([string] $LogPath)

    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) { return $false }
    $text = Get-Content -LiteralPath $LogPath -Raw -ErrorAction SilentlyContinue
    if (-not $text) { return $false }

    return ($text -match 'C3859' -or
            $text -match 'C1076' -or
            $text -match 'Failed to create virtual memory for PCH' -or
            $text -match 'compiler limit: internal heap limit reached' -or
            $text -match 'C1060' -or
            $text -match 'out of heap space')
}

function Test-TrackDLogForZenStoreFailure
{
    <#
    .SYNOPSIS
        True when a build failed because the Zen Store service was unreachable.

    .DESCRIPTION
        From UE 5.5 onward the cooker stores its output in Zen, a machine-global background service
        that the engine starts on demand and shuts down when idle. It is shared by every project and
        every engine on the box, exactly like the UnrealBuildTool state, and when it is restarting or
        has been stopped by something else UnrealPak cannot reach it:

            Zen project 'X' get FAILED: Failed: Could not connect to server (7)
            Fatal error: [File:...\ZenStoreHttpClient.cpp]

        Curl error 7 is connection refused - nothing was listening. That says nothing about the
        plugin: a row that hits it has already compiled and cooked successfully.

        Retried and counted as its own class rather than folded into the others, because a reader
        distinguishing a contended machine from a loaded one from a restarting service needs the
        three kept apart. Deliberately narrow: the Zen diagnostic AND its source file must both be
        present, so an ordinary pak or cook failure still fails the row on its first attempt.
    #>
    param([string] $LogPath)

    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) { return $false }
    $text = Get-Content -LiteralPath $LogPath -Raw -ErrorAction SilentlyContinue
    if (-not $text) { return $false }

    return (($text -match 'Zen project .* get FAILED' -or $text -match 'Could not connect to server \(7\)') -and
            ($text -match 'ZenStoreHttpClient' -or $text -match 'Zen project'))
}

# ---------------------------------------------------------------------------------------------
# Evidence tree
# ---------------------------------------------------------------------------------------------

function New-TrackDRunContext
{
    <#
    .SYNOPSIS
        Creates the evidence directory tree for one validation run and returns its layout.

    .DESCRIPTION
        Everything a run produces lands under Artifacts/TrackD/<runId>/, outside anything the plugin
        release ZIP is built from. The release archive validator asserts none of it leaked in.
    #>
    param(
        [Parameter(Mandatory)][string] $RepoRoot,
        [string] $RunId,
        [string] $ArtifactsRoot
    )

    if ([string]::IsNullOrWhiteSpace($RunId))
    {
        $RunId = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N')
    }
    if ([string]::IsNullOrWhiteSpace($ArtifactsRoot))
    {
        $ArtifactsRoot = Join-Path $RepoRoot 'Artifacts\TrackD'
    }

    $runRoot = Join-Path $ArtifactsRoot $RunId
    if (Test-Path -LiteralPath $runRoot) { throw "Run directory already exists: $runRoot. Use a new RunId to prevent stale stage evidence." }

    $context = [ordered] @{
        RepoRoot          = $RepoRoot
        RunId             = $RunId
        RunRoot           = $runRoot
        Manifests         = Join-Path $runRoot 'manifests'
        BuildLogs         = Join-Path $runRoot 'build-logs'
        Automation        = Join-Path $runRoot 'automation'
        PackagedHosts     = Join-Path $runRoot 'packaged-hosts'
        SmokeTests        = Join-Path $runRoot 'smoke-tests'
        Lifecycle         = Join-Path $runRoot 'lifecycle'
        Performance       = Join-Path $runRoot 'performance'
        Insights          = Join-Path $runRoot 'insights'
        Hashes            = Join-Path $runRoot 'hashes'
        ArchiveValidation = Join-Path $runRoot 'archive-validation'
        Documentation     = Join-Path $runRoot 'documentation'
        CleanSource       = Join-Path $runRoot 'clean-source'
    }

    foreach ($key in @('RunRoot', 'Manifests', 'BuildLogs', 'Automation', 'PackagedHosts',
                       'SmokeTests', 'Lifecycle', 'Performance', 'Insights', 'Hashes',
                       'ArchiveValidation', 'Documentation', 'CleanSource'))
    {
        $path = $context[$key]
        if (-not (Test-Path -LiteralPath $path))
        {
            New-Item -ItemType Directory -Path $path -Force | Out-Null
        }
    }

    return $context
}

function Write-TrackDJson
{
    <#
    .SYNOPSIS
        Writes an object as UTF-8 JSON, creating the parent directory if needed.
    #>
    param(
        [Parameter(Mandatory)] $InputObject,
        [Parameter(Mandatory)][string] $Path,
        [int] $Depth = 24
    )

    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path -LiteralPath $parent))
    {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }

    $InputObject | ConvertTo-Json -Depth $Depth | Set-Content -LiteralPath $Path -Encoding UTF8
}

# ---------------------------------------------------------------------------------------------
# Process execution
# ---------------------------------------------------------------------------------------------

function Invoke-TrackDProcess
{
    <#
    .SYNOPSIS
        Runs a process with a hard timeout, capturing stdout and stderr to files.

    .DESCRIPTION
        Returns a result object with ExitCode, TimedOut, DurationSeconds and the captured log paths.
        On timeout the whole process tree is killed - Unreal spawns children (shader compile workers,
        the crash reporter) and killing only the parent leaves them holding the log file open.

        Never throws on a non-zero exit code. Deciding what a given exit code means belongs to the
        caller, not here.
    #>
    param(
        [Parameter(Mandatory)][string] $FilePath,
        [string[]] $ArgumentList = @(),
        [Parameter(Mandatory)][string] $StdOutPath,
        [string] $StdErrPath,
        [ValidateRange(1,86400)][int] $TimeoutSeconds = 900,
        [string] $WorkingDirectory,
        [ValidateRange(0,60)][int] $TerminationTimeoutSeconds = 15,
        [ValidateRange(0,60)][int] $OutputDrainTimeoutSeconds = 5,
        # Test seam for a refused termination. Production uses tree kill with a parent fallback.
        [scriptblock] $TerminateProcess
    )

    if ([string]::IsNullOrWhiteSpace($StdErrPath))
    {
        $StdErrPath = [System.IO.Path]::ChangeExtension($StdOutPath, '.stderr.log')
    }

    foreach ($path in @($StdOutPath, $StdErrPath))
    {
        $parent = Split-Path -Parent $path
        if ($parent -and -not (Test-Path -LiteralPath $parent))
        {
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }
    }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    foreach ($argument in $ArgumentList)
    {
        [void] $psi.ArgumentList.Add($argument)
    }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    if ($WorkingDirectory) { $psi.WorkingDirectory = $WorkingDirectory }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $psi

    if (-not ('TrackDProcessOutput' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'TrackD.ProcessOutput.cs') }
    $cancellation = [Threading.CancellationTokenSource]::new()
    $terminationErrors = @()
    $outputDrainTimedOut = $false
    $cleanupFailed = $false
    $processId = $null
    $exitCode = $null

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false

    try
    {
        [void] $process.Start()
        $processId = $process.Id
        $outputTask = [TrackDProcessOutput]::Drain($process.StandardOutput, [IO.Path]::GetFullPath($StdOutPath), $cancellation.Token)
        $errorTask = [TrackDProcessOutput]::Drain($process.StandardError, [IO.Path]::GetFullPath($StdErrPath), $cancellation.Token)

        if (-not $process.WaitForExit($TimeoutSeconds * 1000))
        {
            $timedOut = $true
            try
            {
                # Entire tree: Unreal's children keep running (and keep the log open) otherwise.
                if ($TerminateProcess) { & $TerminateProcess $process } else { $process.Kill($true) }
            }
            catch
            {
                $terminationErrors += $_.Exception.Message
                if (-not $TerminateProcess) {
                    try { $process.Kill() } catch { $terminationErrors += $_.Exception.Message }
                }
            }
            # Give the kill a moment to land so the captured output is complete.
            $cleanupFailed = -not $process.WaitForExit($TerminationTimeoutSeconds * 1000)
        }

        # An orphan can keep a pipe open even after its parent exits. Drain under its own
        # deadline; neither process termination nor redirected output may wait forever.
        $drain = [Threading.Tasks.Task]::WhenAll([Threading.Tasks.Task[]]@($outputTask,$errorTask))
        try { $outputDrainTimedOut = -not $drain.Wait($OutputDrainTimeoutSeconds * 1000) }
        catch { $cleanupFailed = $true; $terminationErrors += "Output capture: $($_.Exception.Message)" }
        if ($outputDrainTimedOut) { $cleanupFailed = $true; $cancellation.Cancel() }
        if ($process.HasExited -and -not $cleanupFailed) { $exitCode = $process.ExitCode }
    }
    finally
    {
        $stopwatch.Stop()
        $cancellation.Cancel()
        $process.Dispose()
        $cancellation.Dispose()
    }

    return [ordered] @{
        FilePath        = $FilePath
        Arguments       = $ArgumentList
        CommandLine     = ('"{0}" {1}' -f $FilePath, ($ArgumentList -join ' '))
        ExitCode        = $exitCode
        ProcessId       = $processId
        TimedOut        = $timedOut
        CleanupFailed   = $cleanupFailed
        OutputDrainTimedOut = $outputDrainTimedOut
        TerminationErrors = $terminationErrors
        DurationSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 2)
        StdOutPath      = $StdOutPath
        StdErrPath      = $StdErrPath
    }
}

# ---------------------------------------------------------------------------------------------
# Staging copies
# ---------------------------------------------------------------------------------------------

function Copy-TrackDTree
{
    <#
    .SYNOPSIS
        Recursively copies a directory, skipping build output and other non-source directories.

    .DESCRIPTION
        Used to build isolated host-project staging copies and the plugin copies inside them. The
        exclusions are what stop a previous engine's Intermediate from being compiled by the next
        one - a real failure mode: UHT writes engine-version-specific generated code there, and a
        staging copy that preserves it will make a 5.8 run compile 5.3's generated code.
    #>
    param(
        [Parameter(Mandatory)][string] $Source,
        [Parameter(Mandatory)][string] $Destination,
        [string[]] $ExcludeDirectories = @('Binaries', 'Intermediate', 'Saved', 'DerivedDataCache', '.git', '.vs', 'Build'),
        [string[]] $ExcludeFilePatterns = @()
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Container))
    {
        throw "Copy-TrackDTree source does not exist: $Source"
    }

    if (-not (Test-Path -LiteralPath $Destination))
    {
        New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    }

    $sourceFull = (Resolve-Path -LiteralPath $Source).Path.TrimEnd('\')
    $copied = 0

    foreach ($item in (Get-ChildItem -LiteralPath $Source -Recurse -Force -File))
    {
        $relative = $item.FullName.Substring($sourceFull.Length).TrimStart('\')
        $segments = $relative -split '\\'

        $skip = $false
        foreach ($segment in $segments[0..([Math]::Max(0, $segments.Count - 2))])
        {
            if ($ExcludeDirectories -contains $segment) { $skip = $true; break }
        }
        if ($skip) { continue }

        foreach ($pattern in $ExcludeFilePatterns)
        {
            if ($item.Name -like $pattern) { $skip = $true; break }
        }
        if ($skip) { continue }

        $targetPath = Join-Path $Destination $relative
        $targetDir = Split-Path -Parent $targetPath
        if (-not (Test-Path -LiteralPath $targetDir))
        {
            New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
        }
        Copy-Item -LiteralPath $item.FullName -Destination $targetPath -Force
        ++$copied
    }

    return $copied
}

function New-TrackDHostStaging
{
    <#
    .SYNOPSIS
        Builds an isolated copy of the host test project with the plugin installed inside it.

    .DESCRIPTION
        The plugin is installed at <staging>/Plugins/SpoutPlugin. A project-installed plugin always
        recompiles from source at package time (UBT treats the project rules assembly as writable),
        so this exercises the same compile path a consuming project hits - which is the layout the
        packaged-host matrix is meant to validate.

        -PluginSourceRoot lets the caller install a BuildPlugin output instead of the repository
        tree, so both plugin layouts can be staged from the same function.

        Short staging paths matter: Windows' 260-character action-path limit is reached quickly once
        UBT appends Intermediate/Build/Win64/x64/<Target>/<Config>/... , and UBT fails outright
        rather than truncating.
    #>
    param(
        [Parameter(Mandatory)][string] $RepoRoot,
        [Parameter(Mandatory)][string] $StagingProjectDir,
        [string] $PluginSourceRoot,
		[switch] $PreservePluginBuildOutput,
        [switch] $Force
    )

    if ([string]::IsNullOrWhiteSpace($PluginSourceRoot))
    {
        $PluginSourceRoot = $RepoRoot
    }

    if ($Force -and (Test-Path -LiteralPath $StagingProjectDir))
    {
        Remove-Item -LiteralPath $StagingProjectDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $StagingProjectDir -Force | Out-Null

    $hostSource = Join-Path $RepoRoot ('Tests\{0}' -f $script:HostProjectName)
    if (-not (Test-Path -LiteralPath $hostSource -PathType Container))
    {
        throw "Host test project not found at $hostSource"
    }

    [void] (Copy-TrackDTree -Source $hostSource -Destination $StagingProjectDir)

    $pluginDest = Join-Path $StagingProjectDir ('Plugins\{0}' -f $script:PluginName)
    New-Item -ItemType Directory -Path $pluginDest -Force | Out-Null
	if ($PreservePluginBuildOutput) {
		[void](Copy-TrackDTree -Source $PluginSourceRoot -Destination $pluginDest -ExcludeDirectories @())
		foreach ($file in Get-ChildItem -LiteralPath $PluginSourceRoot -Recurse -File) {
			$relative = [IO.Path]::GetRelativePath($PluginSourceRoot,$file.FullName)
			if ((Get-TrackDSha256 $file.FullName) -ne (Get-TrackDSha256 (Join-Path $pluginDest $relative))) { throw "Archive installation differs: $relative" }
		}
		return [ordered]@{ProjectDir=$StagingProjectDir;ProjectFile=(Join-Path $StagingProjectDir "$($script:HostProjectName).uproject");PluginDir=$pluginDest;PluginSourceRoot=$PluginSourceRoot;MissingItems=@();ExactArchiveInstallation=$true}
	}

    # Exactly what a consuming project needs, and nothing else. Notably absent: Tests/, Scripts/,
    # Docs/ and Artifacts/ - internal material that must never travel with the plugin.
    $pluginItems = @(
        'SpoutPlugin.uplugin',
        'Source',
        'ThirdParty',
        'Config',
        'Resources',
        'Content',
        'LICENSE',
        'THIRD_PARTY_NOTICES.md',
        'README.md'
    )

    $missing = New-Object System.Collections.ArrayList
    foreach ($item in $pluginItems)
    {
        $sourcePath = Join-Path $PluginSourceRoot $item
        if (-not (Test-Path -LiteralPath $sourcePath))
        {
            [void] $missing.Add($item)
            continue
        }

        if (Test-Path -LiteralPath $sourcePath -PathType Container)
        {
            [void] (Copy-TrackDTree -Source $sourcePath -Destination (Join-Path $pluginDest $item))
        }
        else
        {
            Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $pluginDest $item) -Force
        }
    }

    return [ordered] @{
        ProjectDir       = $StagingProjectDir
        ProjectFile      = Join-Path $StagingProjectDir ('{0}.uproject' -f $script:HostProjectName)
        PluginDir        = $pluginDest
        PluginSourceRoot = $PluginSourceRoot
        MissingItems     = $missing.ToArray()
    }
}

# ---------------------------------------------------------------------------------------------
# Smoke-test evidence
# ---------------------------------------------------------------------------------------------

function Read-TrackDSmokeResult
{
    <#
    .SYNOPSIS
        Reads the host's marker log and result.json and returns a single verdict.

    .DESCRIPTION
        A pass requires ALL of:
          - result.json exists, parses and says passed;
          - markers.log contains every required marker;
          - no SPOUT_SMOKE_FAIL marker;
          - the process exited 0 and did not time out.

        Any one of those missing is a failure. Process launch alone is never a pass, and a marker
        file left over from a previous row cannot produce one either: the host truncates it at
        startup, so an absent process leaves an absent or empty file rather than a stale pass.
    #>
    param(
        [Parameter(Mandatory)][string] $ResultDir,
        [Parameter(Mandatory)][string[]] $RequiredMarkers,
        $ProcessResult
    )

    $markerPath = Join-Path $ResultDir 'markers.log'
    $resultPath = Join-Path $ResultDir 'result.json'

    $verdict = [ordered] @{
        ResultDir        = $ResultDir
        MarkerFile       = $markerPath
        ResultFile       = $resultPath
        MarkerFileExists = (Test-Path -LiteralPath $markerPath -PathType Leaf)
        ResultFileExists = (Test-Path -LiteralPath $resultPath -PathType Leaf)
        ObservedMarkers  = @()
        MissingMarkers   = @()
        FailureMarkers   = @()
        HostReportedPass = $false
        HostFailureReason = $null
        HostFields       = $null
        ProcessExitCode  = $null
        ProcessTimedOut  = $null
        Passed           = $false
        Problems         = @()
    }

    $problems = New-Object System.Collections.ArrayList

    if ($ProcessResult)
    {
        $verdict.ProcessExitCode = $ProcessResult.ExitCode
        $verdict.ProcessTimedOut = $ProcessResult.TimedOut
        if ($ProcessResult.TimedOut)
        {
            [void] $problems.Add('the packaged host did not exit within its timeout and was terminated')
        }
        if ($null -eq $ProcessResult.ExitCode -or $ProcessResult.ExitCode -ne 0)
        {
            [void] $problems.Add("the packaged host exited with code $($ProcessResult.ExitCode)")
        }
    }

    if ($verdict.MarkerFileExists)
    {
        $lines = @(Get-Content -LiteralPath $markerPath -ErrorAction SilentlyContinue)
        $observed = New-Object System.Collections.ArrayList
        foreach ($line in $lines)
        {
            # "[timestamp] MARKER" - keep everything after the closing bracket.
            $marker = $line
            $bracket = $line.IndexOf('] ')
            if ($bracket -ge 0) { $marker = $line.Substring($bracket + 2) }
            $marker = $marker.Trim()
            if ($marker) { [void] $observed.Add($marker) }
        }
        $verdict.ObservedMarkers = $observed.ToArray()

        $verdict.FailureMarkers = @($verdict.ObservedMarkers | Where-Object { $_ -like 'SPOUT_SMOKE_FAIL:*' -or $_ -like 'SPOUT_PERF_FAIL:*' })
        foreach ($failure in $verdict.FailureMarkers)
        {
            [void] $problems.Add("host reported $failure")
        }

        $missing = New-Object System.Collections.ArrayList
        foreach ($required in $RequiredMarkers)
        {
            $found = $false
            foreach ($marker in $verdict.ObservedMarkers)
            {
                if ($marker -eq $required -or $marker.StartsWith("$required`:")) { $found = $true; break }
            }
            if (-not $found)
            {
                [void] $missing.Add($required)
                [void] $problems.Add("required marker '$required' was never emitted")
            }
        }
        $verdict.MissingMarkers = $missing.ToArray()
    }
    else
    {
        [void] $problems.Add("no marker file was produced at $markerPath")
        $verdict.MissingMarkers = $RequiredMarkers
    }

    if ($verdict.ResultFileExists)
    {
        try
        {
            $json = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
            $verdict.HostReportedPass = [bool] $json.passed
            $verdict.HostFailureReason = $json.failureReason
            $verdict.HostFields = $json.fields
            if (-not $verdict.HostReportedPass)
            {
                [void] $problems.Add("host result.json reports a failure: $($json.failureReason)")
            }
        }
        catch
        {
            [void] $problems.Add("host result.json could not be parsed: $($_.Exception.Message)")
        }
    }
    else
    {
        [void] $problems.Add("no result.json was produced at $resultPath")
    }

    $verdict.Problems = $problems.ToArray()
    $verdict.Passed = ($problems.Count -eq 0)
    return $verdict
}

function Test-TrackDViewportEvidence
{
    <#
    .SYNOPSIS
        Validates the GameViewportSenderSmoke evidence carried in one launch's result.json fields.

    .DESCRIPTION
        The marker contract proves the scenario RAN; this proves what it OBSERVED. Every required
        field must be present with a sane value - a missing field is a failure, never a partial
        green, because an old host binary that predates the scenario would otherwise pass on
        markers alone.

        The expected access path is derived from the engine version: UE 5.8 changed the
        back-buffer delegate's signature to ISlateViewportProvider, and the plugin's two compile
        branches are mutually exclusive, so 5.8+ evidence must say SlateViewportProvider and older
        engines must say LegacyBackBuffer. Frames advancing under that path name is what proves
        the named branch executed.
    #>
    param(
        [Parameter(Mandatory)][string] $EngineVersion,
        $HostFields
    )

    $problems = New-Object System.Collections.ArrayList
    $evidence = [ordered] @{}

    $get = {
        param([string] $Name)
        if ($HostFields -and ($HostFields.PSObject.Properties.Name -contains $Name))
        {
            return [string] $HostFields.$Name
        }
        return $null
    }

    $getInt = {
        param([string] $Name)
        $raw = & $get $Name
        $value = 0
        if ($null -ne $raw -and [int]::TryParse($raw, [ref] $value)) { return $value }
        return $null
    }

    if ($null -eq $HostFields)
    {
        [void] $problems.Add('viewport evidence: the launch produced no result.json fields at all')
    }

    # --- Scenario identity and environment -----------------------------------------------------
    $scenario = & $get 'viewportScenario'
    $evidence.Scenario = $scenario
    if ($scenario -ne 'GameViewportSenderSmoke')
    {
        [void] $problems.Add("viewport evidence: field 'viewportScenario' is '$scenario', expected 'GameViewportSenderSmoke'")
    }

    $senderName = & $get 'viewportSenderName'
    $evidence.SenderName = $senderName
    if ([string]::IsNullOrWhiteSpace($senderName))
    {
        [void] $problems.Add("viewport evidence: field 'viewportSenderName' is missing or empty")
    }

    $rhi = & $get 'viewportRhi'
    $evidence.Rhi = $rhi
    if ($rhi -notlike '*D3D12*')
    {
        [void] $problems.Add("viewport evidence: RHI is '$rhi', expected D3D12")
    }

    $expectedPath = if ([version] $EngineVersion -ge [version] '5.8') { 'SlateViewportProvider' } else { 'LegacyBackBuffer' }
    $path = & $get 'viewportAccessPath'
    $evidence.AccessPath = $path
    $evidence.ExpectedAccessPath = $expectedPath
    if ($path -ne $expectedPath)
    {
        [void] $problems.Add("viewport evidence: access path is '$path' but UE $EngineVersion must use '$expectedPath'")
    }

    # UE 5.3 declares the back-buffer delegate non-thread-safe and must mutate its
    # invocation list on the rendering thread; 5.4+ use the thread-safe declaration and keep the
    # game-thread path. A row whose evidence reports the wrong branch means the version guard did
    # not select what this engine requires.
    $expectedDelegateThread = if ([version] $EngineVersion -ge [version] '5.4') { 'GameThread' } else { 'RenderThread' }
    $delegateThread = & $get 'viewportDelegateMutationThread'
    $evidence.DelegateMutationThread = $delegateThread
    $evidence.ExpectedDelegateMutationThread = $expectedDelegateThread
    if ($delegateThread -ne $expectedDelegateThread)
    {
        [void] $problems.Add("viewport evidence: delegate mutation thread is '$delegateThread' but UE $EngineVersion must use '$expectedDelegateThread'")
    }

    $restartCycles = & $getInt 'viewportRestartCyclesCompleted'
    $evidence.RestartCyclesCompleted = $restartCycles
    $evidence.RestartWorstCycleSeconds = & $get 'viewportRestartWorstCycleSeconds'
    if ($null -eq $restartCycles -or $restartCycles -lt 3)
    {
        [void] $problems.Add("viewport evidence: '$restartCycles' attach/detach restart cycle(s) completed; at least 3 are required")
    }

    foreach ($required in @(
        @{ Field = 'viewportGameViewportFound'; Expected = 'true';  What = 'a game viewport was found' },
        @{ Field = 'viewportGameWindowFound';   Expected = 'true';  What = 'a game window was found' },
        @{ Field = 'viewportFrameCountEnabled'; Expected = 'true';  What = 'Spout frame counting is enabled' },
        @{ Field = 'viewportSenderAdvertised';  Expected = 'true';  What = 'the sender was advertised in the shared registry' },
        @{ Field = 'viewportSharedTextureOpened'; Expected = 'true'; What = 'the shared texture opened on the independent device' },
        @{ Field = 'viewportResizeAttempted';   Expected = 'true';  What = 'the controlled resize was attempted' },
        @{ Field = 'viewportCloseRequested';    Expected = 'true';  What = 'the sender close was requested' },
        @{ Field = 'viewportAdvertisedAfterClose'; Expected = 'false'; What = 'the sender disappeared after close' }))
    {
        $value = & $get $required.Field
        $evidence[$required.Field] = $value
        if ($value -ne $required.Expected)
        {
            [void] $problems.Add("viewport evidence: field '$($required.Field)' is '$value', expected '$($required.Expected)' ($($required.What))")
        }
    }

    # --- Geometry -------------------------------------------------------------------------------
    $width = & $getInt 'viewportSenderWidth'
    $height = & $getInt 'viewportSenderHeight'
    $evidence.SenderWidth = $width
    $evidence.SenderHeight = $height
    if ($null -eq $width -or $width -le 0 -or $null -eq $height -or $height -le 0)
    {
        [void] $problems.Add("viewport evidence: sender dimensions are '$width'x'$height'; both must be positive")
    }

    $texWidth = & $getInt 'viewportSharedTextureWidth'
    $texHeight = & $getInt 'viewportSharedTextureHeight'
    $evidence.SharedTextureWidth = $texWidth
    $evidence.SharedTextureHeight = $texHeight
    if ($null -eq $texWidth -or $texWidth -le 0 -or $null -eq $texHeight -or $texHeight -le 0)
    {
        [void] $problems.Add("viewport evidence: opened shared-texture dimensions are '$texWidth'x'$texHeight'; both must be positive")
    }
    elseif (($null -ne $width) -and (($texWidth -ne $width) -or ($texHeight -ne $height)))
    {
        [void] $problems.Add("viewport evidence: the opened texture ($texWidth x $texHeight) disagrees with the registry ($width x $height)")
    }

    $evidence.SenderDxgiFormat = & $get 'viewportSenderDxgiFormat'
    $evidence.SharedTextureDxgiFormat = & $get 'viewportSharedTextureDxgiFormat'

    # --- Frame advancement ----------------------------------------------------------------------
    $advances = & $getInt 'viewportObservedFrameAdvances'
    $delta = & $getInt 'viewportFrameAdvanceDelta'
    $callbacks = & $getInt 'viewportCallbackObservations'
    $evidence.ObservedFrameAdvances = $advances
    $evidence.FrameAdvanceDelta = $delta
    $evidence.CallbackObservations = $callbacks
    $evidence.InitialSenderFrame = & $get 'viewportInitialSenderFrame'
    $evidence.FinalSenderFrame = & $get 'viewportFinalSenderFrame'
    $evidence.FirstAdvanceSeconds = & $get 'viewportFirstAdvanceSeconds'
    $evidence.LastAdvanceSeconds = & $get 'viewportLastAdvanceSeconds'

    if ($null -eq $advances -or $advances -lt 3)
    {
        [void] $problems.Add("viewport evidence: only '$advances' frame-advance observation(s); at least 3 are required")
    }
    if ($null -eq $delta -or $delta -lt 2)
    {
        [void] $problems.Add("viewport evidence: the sender frame count moved by '$delta'; at least 2 is required")
    }
    if ($null -eq $callbacks -or $callbacks -lt 3)
    {
        [void] $problems.Add("viewport evidence: only '$callbacks' externally observed callback execution(s); at least 3 are required")
    }

    # --- Resize (two full cycles plus a post-close resize) --------------------------------------
    $resizeW = & $getInt 'viewportResizeObservedWidth'
    $resizeH = & $getInt 'viewportResizeObservedHeight'
    $resizeAdvances = & $getInt 'viewportResizeObservedFrameAdvances'
    $evidence.ResizeObservedWidth = $resizeW
    $evidence.ResizeObservedHeight = $resizeH
    $evidence.ResizeObservedFrameAdvances = $resizeAdvances
    if ($null -eq $resizeW -or $resizeW -le 0 -or $null -eq $resizeH -or $resizeH -le 0)
    {
        [void] $problems.Add("viewport evidence: post-resize dimensions are '$resizeW'x'$resizeH'; both must be positive")
    }
    if ($null -eq $resizeAdvances -or $resizeAdvances -lt 1)
    {
        [void] $problems.Add("viewport evidence: '$resizeAdvances' frame advance(s) observed after the resize; at least 1 is required")
    }

    $resize2W = & $getInt 'viewportResize2ObservedWidth'
    $resize2H = & $getInt 'viewportResize2ObservedHeight'
    $resize2Advances = & $getInt 'viewportResize2ObservedFrameAdvances'
    $evidence.Resize2ObservedWidth = $resize2W
    $evidence.Resize2ObservedHeight = $resize2H
    $evidence.Resize2ObservedFrameAdvances = $resize2Advances
    if ($null -eq $resize2W -or $resize2W -le 0 -or $null -eq $resize2H -or $resize2H -le 0)
    {
        [void] $problems.Add("viewport evidence: second-resize dimensions are '$resize2W'x'$resize2H'; both must be positive")
    }
    if (($null -ne $resize2W) -and ($null -ne $resizeW) -and ($resize2W -eq $resizeW) -and ($resize2H -eq $resizeH))
    {
        [void] $problems.Add("viewport evidence: the second resize reports the same size as the first ($resize2W x $resize2H); it must be a distinct third size")
    }
    if ($null -eq $resize2Advances -or $resize2Advances -lt 1)
    {
        [void] $problems.Add("viewport evidence: '$resize2Advances' frame advance(s) observed after the second resize; at least 1 is required")
    }

    $postClose = & $get 'viewportPostCloseResizeSurvived'
    $evidence.PostCloseResizeSurvived = $postClose
    if ($postClose -ne 'true')
    {
        [void] $problems.Add("viewport evidence: field 'viewportPostCloseResizeSurvived' is '$postClose', expected 'true' (a resize after the sender closed must not crash the host)")
    }

    return [ordered] @{
        Problems = $problems.ToArray()
        Evidence = $evidence
        Passed   = ($problems.Count -eq 0)
    }
}

# ---------------------------------------------------------------------------------------------
# Release-source enumeration
#
# The clean-source check does three things with the declared release inputs: it counts them, it
# compares them against `git ls-files`, and it copies them. All three must agree on what counts,
# which is why the list below is produced once, filtered once, and shared by every stage.
#
# Running them as three passes does not work. If the manifest's Excluded list is consulted only
# AFTER the copy is made, as a leak check, it cannot remove anything from the count or from the
# git comparison, and a filter built from directory NAMES alone cannot express "generated output"
# or "machine-local file". A broad 'Docs/...' declaration then sweeps up generated and ignored
# files, declares them as required tracked inputs, and fails fresh-clone reproducibility for files
# git is configured never to track.
# ---------------------------------------------------------------------------------------------

$script:ReleaseExcludedSegments = @('Binaries', 'Intermediate', 'Saved', 'DerivedDataCache', '.git', '.vs', 'Artifacts')

$script:PathMatcherCache = @{}

function Get-TrackDExcludedPathSegments
{
    <#
    .SYNOPSIS
        Directory names that are never a release input, at any depth.

    .DESCRIPTION
        Separate from the manifest's Excluded patterns, which are anchored at the repository root.
        'Binaries/...' says nothing about Source/ThirdParty/Binaries; this list does, so a
        'Source/...' declaration cannot sweep up a build tree that happens to be nested inside it.
    #>
    return ,([string[]] $script:ReleaseExcludedSegments)
}

function ConvertTo-TrackDPathRegex
{
    <#
    .SYNOPSIS
        Compiles one release-source pattern into an anchored regex.

    .DESCRIPTION
        The pattern vocabulary, all repository-relative and forward-slashed:

            Dir/...   the directory and everything below it     Artifacts/...
            **        zero or more leading directories          **/.claude/settings.local.json
            *         any run of characters within one segment  Docs/*.pdf
            ?         one character within one segment

        '*' deliberately does not cross a '/', so 'Docs/*.pdf' means exactly what the identical
        line in .gitignore means and nothing wider: a PDF sitting directly in Docs is generated
        output, while a hypothetical Docs/sub/diagram.pdf is not covered by either rule and would
        still have to be tracked like any other input.
    #>
    param([Parameter(Mandatory)][string] $Pattern)

    $normalised = $Pattern.Replace('\', '/').Trim().TrimEnd('/')
    if ($normalised.EndsWith('/...')) { $normalised = $normalised.Substring(0, $normalised.Length - 4) + '/**' }
    elseif ($normalised -eq '...')    { $normalised = '**' }

    $tokens = $normalised -split '/'
    $builder = New-Object System.Text.StringBuilder
    [void] $builder.Append('^')

    for ($i = 0; $i -lt $tokens.Count; ++$i)
    {
        $token = $tokens[$i]
        $isLast = ($i -eq ($tokens.Count - 1))

        if ($token -eq '**')
        {
            if ($isLast)
            {
                # 'Docs/**' is every file BELOW Docs, so at least one more segment must follow.
                [void] $builder.Append('.+')
            }
            else
            {
                # '**/x' matches x at the repository root as well as at any depth. The group carries
                # its own trailing separator, so no separator is appended for this token.
                [void] $builder.Append('(?:[^/]+/)*')
                continue
            }
        }
        else
        {
            # Regex.Escape turns * into \* and ? into \?; put the glob meaning back afterwards.
            $escaped = [System.Text.RegularExpressions.Regex]::Escape($token)
            $escaped = $escaped.Replace('\*', '[^/]*').Replace('\?', '[^/]')
            [void] $builder.Append($escaped)
        }

        if (-not $isLast) { [void] $builder.Append('/') }
    }

    [void] $builder.Append('$')
    return $builder.ToString()
}

function Test-TrackDPathMatchesPattern
{
    <#
    .SYNOPSIS
        Does one repository-relative path match one release-source pattern?

    .DESCRIPTION
        Case-insensitive, because Windows paths are, and because .gitignore is being mirrored on a
        case-insensitive filesystem. Compiled matchers are cached: the enumeration tests every file
        against every exclusion, and recompiling the same dozen patterns a few thousand times is
        pure waste.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string] $Path,
        [Parameter(Mandatory)][string] $Pattern
    )

    if (-not $script:PathMatcherCache.ContainsKey($Pattern))
    {
        $options = [System.Text.RegularExpressions.RegexOptions]::IgnoreCase -bor
                   [System.Text.RegularExpressions.RegexOptions]::CultureInvariant
        $script:PathMatcherCache[$Pattern] =
            [System.Text.RegularExpressions.Regex]::new((ConvertTo-TrackDPathRegex $Pattern), $options)
    }

    return $script:PathMatcherCache[$Pattern].IsMatch($Path.Replace('\', '/'))
}

function Get-TrackDPathExclusion
{
    <#
    .SYNOPSIS
        The exclusion that removes a path from the release inputs, or $null when none does.

    .DESCRIPTION
        Returns the rule that matched rather than a bare boolean, so a filtered file can be reported
        with the reason it was filtered instead of silently disappearing from the manifest. A file
        that vanishes without explanation is the same class of problem as one that is wrongly
        required: neither can be checked by a reader.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string] $Path,
        [AllowEmptyCollection()][object[]] $Exclusions = @(),
        [string[]] $Segments
    )

    if (-not $PSBoundParameters.ContainsKey('Segments')) { $Segments = Get-TrackDExcludedPathSegments }

    $normalised = $Path.Replace('\', '/')

    foreach ($segment in ($normalised -split '/'))
    {
        if ($Segments -contains $segment)
        {
            return [ordered] @{
                Pattern = "<segment>/$segment"
                Reason  = "'$segment' is build output, editor scratch or version-control metadata, at any depth."
            }
        }
    }

    foreach ($exclusion in $Exclusions)
    {
        if (-not $exclusion) { continue }
        $pattern = [string] $exclusion.Path
        if ([string]::IsNullOrWhiteSpace($pattern)) { continue }

        if (Test-TrackDPathMatchesPattern -Path $normalised -Pattern $pattern)
        {
            $reason = if ($exclusion.Contains('Reason')) { [string] $exclusion.Reason } else { 'declared as an excluded input' }
            return [ordered] @{ Pattern = $pattern; Reason = $reason }
        }
    }

    return $null
}

function Expand-TrackDDeclaredPattern
{
    <#
    .SYNOPSIS
        Repository-relative paths matching one declared release-source pattern.

    .DESCRIPTION
        'Dir/...' means the directory and everything below it. Anything else is handed to the
        filesystem as a literal path or a wildcard. -Force throughout, because a hidden file that
        the release needs is still a release input and one that it must not ship is still something
        the exclusion rules have to be given the chance to reject.
    #>
    param(
        [Parameter(Mandatory)][string] $RepoRoot,
        [Parameter(Mandatory)][string] $Pattern
    )

    $prefix = $RepoRoot.TrimEnd('\')
    $results = New-Object System.Collections.ArrayList

    if ($Pattern.EndsWith('/...'))
    {
        $relativeDir = $Pattern.Substring(0, $Pattern.Length - 4)
        $absoluteDir = Join-Path $prefix ($relativeDir -replace '/', '\')
        if (Test-Path -LiteralPath $absoluteDir -PathType Container)
        {
            foreach ($file in (Get-ChildItem -LiteralPath $absoluteDir -Recurse -File -Force))
            {
                [void] $results.Add($file.FullName.Substring($prefix.Length).TrimStart('\').Replace('\', '/'))
            }
        }
        return ,([string[]] $results.ToArray())
    }

    $absolute = Join-Path $prefix ($Pattern -replace '/', '\')
    foreach ($file in (Get-ChildItem -Path $absolute -File -Force -ErrorAction SilentlyContinue))
    {
        [void] $results.Add($file.FullName.Substring($prefix.Length).TrimStart('\').Replace('\', '/'))
    }
    return ,([string[]] $results.ToArray())
}

function Get-TrackDIgnoredPaths
{
    <#
    .SYNOPSIS
        The subset of the given repository-relative paths that git is configured to ignore.

    .DESCRIPTION
        Read-only, and used to tell two very different failures apart.

        A declared input that git has merely not been told about yet is a missing commit: commit it
        and a fresh clone reproduces the release. A declared input that git is CONFIGURED to ignore
        can never become tracked at all, so declaring it is a manifest error and no amount of
        committing will fix it - which is exactly the failure that reads as "untracked release
        input" and sends the reader off to commit generated output.

        git check-ignore does not report tracked paths, so anything it names here is genuinely
        unreachable from a clone.
    #>
    param(
        [Parameter(Mandatory)][string] $RepoRoot,
        [AllowEmptyCollection()][string[]] $Paths = @()
    )

    if ($null -eq $Paths -or $Paths.Count -eq 0) { return ,([string[]] @()) }

    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) { return ,([string[]] @()) }

    $ignored = $Paths | & git -C $RepoRoot check-ignore --stdin 2>$null

    # 0 = at least one path is ignored, 1 = none is, anything else means git could not answer and
    # the diagnostic is simply unavailable. The untracked check below still fires either way.
    if ($LASTEXITCODE -gt 1) { return ,([string[]] @()) }

    return ,([string[]] @($ignored | Where-Object { $_ } | ForEach-Object { $_.Replace('\', '/') }))
}

function Get-TrackDDeclaredReleaseInputs
{
    <#
    .SYNOPSIS
        Expands the release-source manifest into the single list of declared release inputs.

    .DESCRIPTION
        Exclusions are applied HERE, during enumeration, which is what makes the count, the git
        comparison and the clean copy agree with each other: all three read this one list.

        Filtered files are returned rather than dropped, so the report can say what was excluded and
        by which rule. -TrackedFiles is a parameter rather than an internal `git ls-files` call so
        the self-test can drive this against a synthetic tree with a known tracking state.
    #>
    param(
        [Parameter(Mandatory)][string] $RepoRoot,
        [Parameter(Mandatory)] $Manifest,
        [AllowEmptyCollection()][string[]] $TrackedFiles,
        [switch] $SkipHash
    )

    $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

    if (-not $PSBoundParameters.ContainsKey('TrackedFiles'))
    {
        $TrackedFiles = Get-TrackDTrackedFiles $RepoRoot
    }

    # Case-insensitive, because Windows paths are. Populated by Add rather than by the collection
    # constructor: New-Object cannot bind the (IEnumerable, IEqualityComparer) overload cleanly.
    $trackedSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($tracked in $TrackedFiles) { [void] $trackedSet.Add($tracked.Replace('\', '/')) }

    $exclusions = @()
    if ($Manifest.Contains('Excluded') -and $Manifest['Excluded']) { $exclusions = @($Manifest['Excluded']) }

    $files         = New-Object System.Collections.ArrayList
    $filtered      = New-Object System.Collections.ArrayList
    $missing       = New-Object System.Collections.ArrayList
    $fileIndex     = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $filteredIndex = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)

    foreach ($group in @('Required', 'Validation'))
    {
        if (-not $Manifest.Contains($group)) { continue }

        foreach ($entry in @($Manifest[$group]))
        {
            $kept = 0
            $excludedHere = 0

            foreach ($relative in (Expand-TrackDDeclaredPattern -RepoRoot $RepoRoot -Pattern $entry.Path))
            {
                $exclusion = Get-TrackDPathExclusion -Path $relative -Exclusions $exclusions
                if ($exclusion)
                {
                    ++$excludedHere
                    if ($filteredIndex.Add($relative))
                    {
                        [void] $filtered.Add([ordered] @{
                            Path       = $relative
                            Pattern    = $entry.Path
                            ExcludedBy = $exclusion.Pattern
                            Reason     = $exclusion.Reason
                        })
                    }
                    continue
                }

                if ($fileIndex.Add($relative))
                {
                    $absolute = Join-Path $RepoRoot ($relative -replace '/', '\')
                    [void] $files.Add([ordered] @{
                        Path       = $relative
                        Group      = $group
                        Category   = if ($entry.Contains('Category')) { $entry.Category } else { $null }
                        Pattern    = $entry.Path
                        SizeBytes  = (Get-Item -LiteralPath $absolute).Length
                        Sha256     = if ($SkipHash) { $null } else { Get-TrackDSha256 $absolute }
                        GitTracked = $trackedSet.Contains($relative)
                    })
                }
                ++$kept
            }

            if ($kept -eq 0)
            {
                [void] $missing.Add([ordered] @{
                    Group          = $group
                    Pattern        = $entry.Path
                    Note           = if ($entry.Contains('Note')) { $entry.Note } else { $null }
                    ExcludedMatches = $excludedHere
                })
            }
        }
    }

    $untracked = @($files | Where-Object { -not $_.GitTracked })

    return [ordered] @{
        Files           = $files.ToArray()
        FilteredFiles   = $filtered.ToArray()
        MissingPatterns = $missing.ToArray()
        FileCount       = $files.Count
        FilteredCount   = $filtered.Count
        TrackedCount    = ($files.Count - $untracked.Count)
        UntrackedCount  = $untracked.Count
        UntrackedFiles  = $untracked
    }
}

function New-TrackDCleanCopy
{
    <#
    .SYNOPSIS
        Copies exactly the given declared inputs into an empty destination tree.

    .DESCRIPTION
        Takes the enumerated list rather than re-walking the repository, which is the whole point:
        the copy cannot contain anything the count and the git comparison did not also see.
    #>
    param(
        [Parameter(Mandatory)][string] $RepoRoot,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]] $Files,
        [Parameter(Mandatory)][string] $Destination
    )

    if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null

    foreach ($file in $Files)
    {
        $source = Join-Path $RepoRoot ($file.Path -replace '/', '\')
        $target = Join-Path $Destination ($file.Path -replace '/', '\')
        $targetDir = Split-Path -Parent $target
        if (-not (Test-Path -LiteralPath $targetDir)) { New-Item -ItemType Directory -Path $targetDir -Force | Out-Null }
        Copy-Item -LiteralPath $source -Destination $target -Force
    }

    return (Resolve-Path -LiteralPath $Destination).Path
}

function Get-TrackDCleanCopyLeaks
{
    <#
    .SYNOPSIS
        Files in a built clean copy that an exclusion says must not be there.

    .DESCRIPTION
        Walks the copy and tests every file against the same matcher the enumeration used, instead
        of probing each excluded pattern with a filesystem wildcard. PowerShell's -Path globbing has
        no recursive '**', so a probe for '**/.claude/settings.local.json' finds nothing and reports
        a clean result for a check it never actually performed.
    #>
    param(
        [Parameter(Mandatory)][string] $Root,
        [AllowEmptyCollection()][object[]] $Exclusions = @()
    )

    $leaks = New-Object System.Collections.ArrayList
    $rootPath = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')

    foreach ($file in (Get-ChildItem -LiteralPath $rootPath -Recurse -File -Force -ErrorAction SilentlyContinue))
    {
        $relative = $file.FullName.Substring($rootPath.Length).TrimStart('\').Replace('\', '/')
        $exclusion = Get-TrackDPathExclusion -Path $relative -Exclusions $Exclusions
        if ($exclusion)
        {
            [void] $leaks.Add([ordered] @{ Path = $relative; Pattern = $exclusion.Pattern; Reason = $exclusion.Reason })
        }
    }

    return ,($leaks.ToArray())
}

# ---------------------------------------------------------------------------------------------
# Release-source self-test
# ---------------------------------------------------------------------------------------------

function Test-TrackDReleaseSourceFiltering
{
    <#
    .SYNOPSIS
        Proves the release-source filtering rules on a synthetic tree. Returns the problems found.

    .DESCRIPTION
        Deterministic and offline: it builds its own miniature repository in the temp directory,
        so it asserts on the RULES rather than on whatever happens to be in the working folder
        today, and it uses the real manifest's Excluded list so the rules it proves are the ones
        that ship.

        What it pins down:

          - a tracked documentation source under Docs/ is an input;
          - a generated Docs/*.pdf is not, and never reaches the clean copy;
          - a nested .claude/settings.local.json is not, while a tracked CLAUDE.md beside it still
            is - the machine-local file is excluded, not the directory;
          - excluded files are absent from the count, from the untracked diagnostics and from the
            copy, which are the three places that must agree with each other;
          - a genuinely required file that is untracked STILL fails, so this filtering cannot be
            mistaken for a way to make the fresh-clone check quieter.
    #>
    param(
        [string] $ManifestPath = (Join-Path $PSScriptRoot 'ReleaseSources.psd1'),
        [string] $WorkRoot = (Join-Path ([System.IO.Path]::GetTempPath()) 'spoutplugin-trackd-selftest')
    )

    $problems = New-Object System.Collections.ArrayList

    if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf))
    {
        [void] $problems.Add("release source manifest not found: $ManifestPath")
        return ,([string[]] $problems.ToArray())
    }

    $realManifest = Import-PowerShellDataFile -LiteralPath $ManifestPath
    $exclusions = @($realManifest.Excluded)

    # --- pattern vocabulary ---------------------------------------------------------------------
    # Table-driven so a regression names the exact pattern and path that changed meaning.
    $matcherCases = @(
        @{ Pattern = 'Docs/*.pdf';                     Path = 'Docs/manual.pdf';                             Expect = $true }
        @{ Pattern = 'Docs/*.pdf';                     Path = 'Docs/2026-06-05-SpoutPlugin-User-Manual.pdf'; Expect = $true }
        @{ Pattern = 'Docs/*.pdf';                     Path = 'Docs/GUIDE.md';                               Expect = $false }
        @{ Pattern = 'Docs/*.pdf';                     Path = 'Docs/sub/manual.pdf';                          Expect = $false }
        @{ Pattern = '**/.claude/settings.local.json'; Path = '.claude/settings.local.json';                  Expect = $true }
        @{ Pattern = '**/.claude/settings.local.json'; Path = 'Docs/spout_web/.claude/settings.local.json';   Expect = $true }
        @{ Pattern = '**/.claude/settings.local.json'; Path = 'Docs/spout_web/.claude/settings.json';         Expect = $false }
        @{ Pattern = '**/.claude/settings.local.json'; Path = 'Docs/spout_web/CLAUDE.md';                     Expect = $false }
        @{ Pattern = 'Artifacts/...';                  Path = 'Artifacts/TrackD/run/report.json';             Expect = $true }
        @{ Pattern = 'Artifacts/...';                  Path = 'ArtifactsExtra/report.json';                   Expect = $false }
        @{ Pattern = 'TRACK_A_*.md';                   Path = 'TRACK_A_DEVIATIONS.md';                        Expect = $true }
        @{ Pattern = 'TRACK_A_*.md';                   Path = 'Docs/TRACK_A_DEVIATIONS.md';                   Expect = $false }
        @{ Pattern = 'CLAUDE.md';                      Path = 'Docs/spout_web/CLAUDE.md';                     Expect = $false }
    )
    foreach ($case in $matcherCases)
    {
        $actual = Test-TrackDPathMatchesPattern -Path $case.Path -Pattern $case.Pattern
        if ($actual -ne $case.Expect)
        {
            [void] $problems.Add("pattern '$($case.Pattern)' vs '$($case.Path)': expected $($case.Expect), got $actual")
        }
    }

    # --- enumeration, on a synthetic tree ---------------------------------------------------------
    $root = Join-Path $WorkRoot 'repo'
    $copy = Join-Path $WorkRoot 'copy'

    try
    {
        if (Test-Path -LiteralPath $WorkRoot) { Remove-Item -LiteralPath $WorkRoot -Recurse -Force }

        $included = @(
            'Source/Runtime.cpp'
            'Docs/GUIDE.md'
            'Docs/build_user_manual.py'
            'Docs/spout_web/index.html'
            'Docs/spout_web/assets/logo.png'
            'Docs/spout_web/CLAUDE.md'
        )
        $excludedFixtures = @(
            'Docs/2026-06-05-Manual.pdf'
            'Docs/spout_web/.claude/settings.local.json'
            'Source/Intermediate/Build/stale.obj'
            'Artifacts/TrackD/run/report.json'
        )

        foreach ($relative in ($included + $excludedFixtures))
        {
            $absolute = Join-Path $root ($relative -replace '/', '\')
            $directory = Split-Path -Parent $absolute
            if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
            Set-Content -LiteralPath $absolute -Value "fixture: $relative" -Encoding UTF8
        }

        $syntheticManifest = @{
            SchemaVersion = $realManifest.SchemaVersion
            Required   = @(@{ Path = 'Source/...'; Category = 'source'; Note = 'synthetic' })
            Validation = @(@{ Path = 'Docs/...';   Category = 'docs';   Note = 'synthetic' })
            Excluded   = $exclusions
        }

        # Case A: everything that SHOULD be an input is tracked, and nothing else exists in git.
        $inputs = Get-TrackDDeclaredReleaseInputs -RepoRoot $root -Manifest $syntheticManifest `
            -TrackedFiles ([string[]] $included) -SkipHash

        $declaredPaths = @($inputs.Files | ForEach-Object { $_.Path })

        foreach ($expected in $included)
        {
            if ($declaredPaths -notcontains $expected)
            {
                [void] $problems.Add("'$expected' should be a declared release input but was filtered out")
            }
        }

        $mustBeFiltered = @{
            'Docs/2026-06-05-Manual.pdf'                 = 'Docs/*.pdf'
            'Docs/spout_web/.claude/settings.local.json' = '**/.claude/settings.local.json'
        }
        foreach ($path in $mustBeFiltered.Keys)
        {
            if ($declaredPaths -contains $path)
            {
                [void] $problems.Add("'$path' must not be counted as a declared release input")
            }

            $record = @($inputs.FilteredFiles | Where-Object { $_.Path -eq $path })
            if ($record.Count -ne 1)
            {
                [void] $problems.Add("'$path' should be reported once as filtered, was reported $($record.Count) time(s)")
            }
            elseif ($record[0].ExcludedBy -ne $mustBeFiltered[$path])
            {
                [void] $problems.Add("'$path' was excluded by '$($record[0].ExcludedBy)', expected '$($mustBeFiltered[$path])'")
            }
        }

        foreach ($path in @('Source/Intermediate/Build/stale.obj', 'Artifacts/TrackD/run/report.json'))
        {
            if ($declaredPaths -contains $path)
            {
                [void] $problems.Add("build output '$path' must not be counted as a declared release input")
            }
        }

        if ($inputs.FileCount -ne $included.Count)
        {
            [void] $problems.Add("declared input count is $($inputs.FileCount), expected $($included.Count)")
        }
        if ($inputs.UntrackedCount -ne 0)
        {
            [void] $problems.Add("untracked diagnostics named $($inputs.UntrackedCount) file(s) when every input was tracked: $(@($inputs.UntrackedFiles | ForEach-Object { $_.Path }) -join ', ')")
        }
        if ($inputs.MissingPatterns.Count -ne 0)
        {
            [void] $problems.Add("$($inputs.MissingPatterns.Count) declared pattern(s) matched nothing in the synthetic tree")
        }

        # --- the copy contains what the count contains, and nothing else ------------------------
        [void] (New-TrackDCleanCopy -RepoRoot $root -Files $inputs.Files -Destination $copy)

        foreach ($path in $mustBeFiltered.Keys)
        {
            if (Test-Path -LiteralPath (Join-Path $copy ($path -replace '/', '\')))
            {
                [void] $problems.Add("'$path' reached the clean copy")
            }
        }
        foreach ($path in $included)
        {
            if (-not (Test-Path -LiteralPath (Join-Path $copy ($path -replace '/', '\'))))
            {
                [void] $problems.Add("'$path' is missing from the clean copy")
            }
        }

        $leaks = Get-TrackDCleanCopyLeaks -Root $copy -Exclusions $exclusions
        if ($leaks.Count -ne 0)
        {
            [void] $problems.Add("the clean copy leaked $($leaks.Count) excluded file(s): $(@($leaks | ForEach-Object { $_.Path }) -join ', ')")
        }

        # --- Case B: a genuinely required input that is untracked must still fail ---------------
        $withoutSource = @($included | Where-Object { $_ -ne 'Source/Runtime.cpp' })
        $regression = Get-TrackDDeclaredReleaseInputs -RepoRoot $root -Manifest $syntheticManifest `
            -TrackedFiles ([string[]] $withoutSource) -SkipHash

        $regressionUntracked = @($regression.UntrackedFiles | ForEach-Object { $_.Path })
        if ($regressionUntracked.Count -ne 1 -or $regressionUntracked[0] -ne 'Source/Runtime.cpp')
        {
            [void] $problems.Add("an untracked required input must still fail validation; got [$($regressionUntracked -join ', ')]")
        }
    }
    catch
    {
        [void] $problems.Add("release-source self-test threw: $($_.Exception.Message)")
    }
    finally
    {
        if (Test-Path -LiteralPath $WorkRoot) { Remove-Item -LiteralPath $WorkRoot -Recurse -Force -ErrorAction SilentlyContinue }
    }

    return ,([string[]] $problems.ToArray())
}

function Test-TrackDBuildContentionClassifier
{
    <#
    .SYNOPSIS
        Proves the build-log classifiers on captured log text. Returns the problems found.

    .DESCRIPTION
        Deterministic and offline: it feeds recorded log excerpts to the classifiers rather than
        waiting for the machine to reproduce a race.

        A classifier that recognises only the phrasing UBT uses WITHOUT -waitmutex is blind to every
        automation build, since each one passes -waitmutex: the failure mode it is meant to absorb
        would go unretried, and the manifest would record it as though the plugin itself had failed
        to compile.

        A classifier that answers "no" to everything is indistinguishable from a working one until
        it matters, so both the positive and the negative cases are pinned here.
    #>

    $problems = New-Object System.Collections.ArrayList

    # Verbatim from a UE 5.4 automation-stage build-editor.log.
    $waitMutexLogRotation = @'
Running UnrealBuildTool: dotnet "..\..\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll" SpoutHostTestEditor Win64 Development -Project=C:\Temp\SpoutHostTest.uproject -waitmutex
Log file: C:\Users\x\AppData\Local\UnrealBuildTool\Log.txt
Total execution time: 0.27 seconds
Unhandled exception: System.IO.IOException: The process cannot access the file because it is being used by another process.
   at System.IO.FileSystem.MoveFile(String sourceFullPath, String destFullPath, Boolean overwrite)
   at EpicGames.Core.Log.BackupLogFile(FileReference outputFile) in D:\UE\UE_5.4\Engine\Source\Programs\Shared\EpicGames.Core\Log.cs:line 257
   at EpicGames.Core.Log.AddFileWriter(String name, FileReference outputFile) in D:\UE\UE_5.4\Engine\Source\Programs\Shared\EpicGames.Core\Log.cs:line 272
'@

    # Verbatim from a UE 5.4 BuildPlugin log. A different shared file from the one above,
    # contended the same way by the previous engine's UnrealBuildTool.
    $sharedRulesAssembly = @'
Running: dotnet "UnrealBuildTool.dll" UnrealEditor Win64 Development -plugin="SpoutPlugin.uplugin"
Total execution time: 0.54 seconds
Unhandled exception: System.IO.IOException: The process cannot access the file 'C:\Users\x\AppData\Local\UnrealEngine\Intermediate\Build\BuildRules\MarketplaceRules.dll' because it is being used by another process.
   at UnrealBuildTool.DynamicCompilation.CompileAssembly(FileReference OutputAssemblyPath, IEnumerable`1 SourceFileNames, ILogger Logger) in DynamicCompilation.cs:line 258
   at UnrealBuildTool.RulesCompiler.CreateMarketplaceRulesAssembly(...) in RulesCompiler.cs:line 226
AutomationTool exiting with ExitCode=6 (6)
'@

    # Verbatim from a UE 5.8 host packaging log. Same contention as $sharedRulesAssembly, but
    # UE 5.8 prints the SHORT exception type name and the async frame, so a match that requires
    # 'System.IO.IOException' finds the UE 5.4 form and misses this one.
    $sharedRulesAssemblyShortName = @'
Creating makefile for SpoutHostTestEditor (no existing makefile)
UbaServer - Listening on 0.0.0.0:1345
Unhandled exception: IOException: The process cannot access the file 'C:\Users\x\AppData\Local\UnrealEngine\Intermediate\Build\BuildRules\MarketplaceRules.dll' because it is being used by another process.
   at System.IO.File.Open(String path, FileMode mode)
   at UnrealBuildTool.DynamicCompilation.CompileAssemblyAsync(FileReference OutputAssemblyPath, IEnumerable`1 SourceFileNames, ILogger Logger)
Unhandled 1 aggregate exceptions
Result: Failed (OtherCompilationError)
AutomationTool exiting with ExitCode=6 (6)
'@

    $classifierCases = @(
        @{ Name = 'waitmutex log rotation'; Text = $waitMutexLogRotation; Expect = $true }
        @{ Name = 'shared rules assembly locked'; Text = $sharedRulesAssembly; Expect = $true }
        @{ Name = 'shared rules assembly, short exception name'; Text = $sharedRulesAssemblyShortName; Expect = $true }
        @{ Name = 'plain conflicting instance'
           Text = "A conflicting instance of UnrealBuildTool is already running.`nTook 0.29s to run dotnet.exe, ExitCode=6"
           Expect = $true }
        @{ Name = 'mutex wait timeout'
           Text = "Timed out waiting for another instance of UnrealBuildTool to finish"
           Expect = $true }
        @{ Name = 'genuine compile error is not contention'
           Text = "SpoutSender.cpp(120): error C2065: 'Foo': undeclared identifier`nERROR: UBT ERROR: Failed to produce item"
           Expect = $false }
        @{ Name = 'unrelated IOException is not contention'
           Text = "Unhandled exception: System.IO.IOException: There is not enough space on the disk.`n   at UnrealBuildTool.Program.Main()"
           Expect = $false }
        @{ Name = 'compiler memory exhaustion is a different class'
           Text = "c1xx: error C3859: Failed to create virtual memory for PCH"
           Expect = $false }
        @{ Name = 'clean build is not contention'
           Text = "[15/15] WriteMetadata SpoutHostTestEditor.target`nTotal execution time: 25.50 seconds"
           Expect = $false }
    )

    $probeRoot = Join-Path ([System.IO.Path]::GetTempPath()) 'spoutplugin-trackd-logprobe'

    try
    {
        if (Test-Path -LiteralPath $probeRoot) { Remove-Item -LiteralPath $probeRoot -Recurse -Force }
        New-Item -ItemType Directory -Path $probeRoot -Force | Out-Null

        $index = 0
        foreach ($case in $classifierCases)
        {
            $probe = Join-Path $probeRoot ("case-{0}.log" -f $index)
            ++$index
            Set-Content -LiteralPath $probe -Value $case.Text -Encoding UTF8

            $actual = Test-TrackDLogForBuildMutex $probe
            if ($actual -ne $case.Expect)
            {
                [void] $problems.Add("build contention '$($case.Name)': expected $($case.Expect), got $actual")
            }
        }

        # The three environmental classes must stay distinguishable, because each is counted
        # separately in every manifest and a reader uses that split to tell a contended machine from
        # a loaded one from a restarting service. Cross-checked both ways: each classifier must
        # recognise its own class and reject the other two.
        $pchProbe = Join-Path $probeRoot 'pch.log'
        Set-Content -LiteralPath $pchProbe -Value 'c1xx: fatal error C1076: compiler limit: internal heap limit reached' -Encoding UTF8
        if (-not (Test-TrackDLogForResourceExhaustion $pchProbe))
        {
            [void] $problems.Add('resource-exhaustion classifier no longer recognises C1076')
        }
        if (Test-TrackDLogForBuildMutex $pchProbe)
        {
            [void] $problems.Add('resource exhaustion was misclassified as build-tool contention')
        }
        if (Test-TrackDLogForZenStoreFailure $pchProbe)
        {
            [void] $problems.Add('resource exhaustion was misclassified as a Zen Store failure')
        }

        # Verbatim from a UE 5.8 Development packaged-host row.
        $zenProbe = Join-Path $probeRoot 'zen.log'
        Set-Content -LiteralPath $zenProbe -Encoding UTF8 -Value @'
LogWindows: Error: === Critical error: ===
LogWindows: Error: Zen project 'SpoutHostTest.9f03d224' get FAILED: Failed: Could not connect to server (7) Url: /prj/SpoutHostTest.9f03d224
LogWindows: Error: Fatal error: [File:D:\build\++UE5\Sync\Engine\Source\Developer\IoStoreUtilities\Private\ZenStoreHttpClient.cpp] [Line: 300]
Took 38.26s to run UnrealPak.exe, ExitCode=3
AutomationTool exiting with ExitCode=3 (Error_UnknownCommand)
'@
        if (-not (Test-TrackDLogForZenStoreFailure $zenProbe))
        {
            [void] $problems.Add('Zen Store classifier does not recognise a captured Zen connection failure')
        }
        if (Test-TrackDLogForBuildMutex $zenProbe)
        {
            [void] $problems.Add('a Zen Store failure was misclassified as build-tool contention')
        }
        if (Test-TrackDLogForResourceExhaustion $zenProbe)
        {
            [void] $problems.Add('a Zen Store failure was misclassified as resource exhaustion')
        }

        # An ordinary pak failure must still fail the row on its first attempt.
        $pakProbe = Join-Path $probeRoot 'pak.log'
        Set-Content -LiteralPath $pakProbe -Value 'UnrealPak: Error: Failed to open file for writing. ExitCode=3' -Encoding UTF8
        if (Test-TrackDLogForZenStoreFailure $pakProbe)
        {
            [void] $problems.Add('an ordinary pak failure was misclassified as a Zen Store failure')
        }

        # A file nothing holds must read as free, or the pre-flight settle would wait out its whole
        # timeout before every single build.
        $freeProbe = Join-Path $probeRoot 'free.log'
        Set-Content -LiteralPath $freeProbe -Value 'not held by anyone' -Encoding UTF8
        if (Test-TrackDBuildToolStateBusy -Path $freeProbe)
        {
            [void] $problems.Add('an unlocked log file was reported as busy')
        }

        # And a file something does hold must read as busy, or the settle is a no-op that silently
        # never waits - the failure mode this whole check exists to prevent.
        $heldProbe = Join-Path $probeRoot 'held.log'
        Set-Content -LiteralPath $heldProbe -Value 'held' -Encoding UTF8
        $handle = [System.IO.File]::Open($heldProbe, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::Read)
        try
        {
            if (-not (Test-TrackDBuildToolStateBusy -Path $heldProbe))
            {
                [void] $problems.Add('a locked log file was reported as free')
            }
        }
        finally
        {
            $handle.Dispose()
        }

        # A path that does not exist yet is not contention either: on a machine that has never run
        # UBT there is no log to rotate.
        if (Test-TrackDBuildToolStateBusy -Path (Join-Path $probeRoot 'no-such-file.log'))
        {
            [void] $problems.Add('a missing log file was reported as busy')
        }

        # One locked file among several must still read as busy: the probe covers a set of shared
        # paths, and a loop that stopped at the first readable one would miss the locked rules
        # assembly entirely.
        $setProbe = @((Join-Path $probeRoot 'free.log'), (Join-Path $probeRoot 'held.log'))
        $handle2 = [System.IO.File]::Open((Join-Path $probeRoot 'held.log'), [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::Read)
        try
        {
            if (-not (Test-TrackDBuildToolStateBusy -Path $setProbe))
            {
                [void] $problems.Add('a locked file later in the probe set was not detected')
            }
        }
        finally
        {
            $handle2.Dispose()
        }

        # The real path set must at least name the UBT log, or the settle probes nothing.
        $statePaths = Get-TrackDBuildToolStatePaths
        if (-not (@($statePaths) -match 'UnrealBuildTool'))
        {
            [void] $problems.Add('the shared build-tool state path set does not include the UnrealBuildTool log')
        }
    }
    catch
    {
        [void] $problems.Add("build-contention self-test threw: $($_.Exception.Message)")
    }
    finally
    {
        if (Test-Path -LiteralPath $probeRoot) { Remove-Item -LiteralPath $probeRoot -Recurse -Force -ErrorAction SilentlyContinue }
    }

    return ,([string[]] $problems.ToArray())
}

# ---------------------------------------------------------------------------------------------
# Exit codes
# ---------------------------------------------------------------------------------------------

function Get-TrackDExitCode
{
    <#
    .SYNOPSIS
        0 only when there are no failures AND no required row was skipped.

    .DESCRIPTION
        The single place any validation script decides success. A skipped required row is a failure
        here, which is what stops a row that was never run from being printed as a pass.
    #>
    param(
        [int] $FailureCount = 0,
        [int] $SkippedRequiredCount = 0
    )

    if ($FailureCount -gt 0 -or $SkippedRequiredCount -gt 0) { return 1 }
    return 0
}

. (Join-Path $PSScriptRoot 'TrackD.Identity.ps1')
. (Join-Path $PSScriptRoot 'TrackD.Performance.ps1')

Export-ModuleMember -Function @(
    'Test-TrackDPerformanceResult',
    'Expand-TrackDArchiveCandidate',
    'Build-TrackDControlHost',
    'Build-TrackDNativeTools',
    'Get-TrackDSourceIdentity',
    'New-TrackDEvidenceRun',
    'New-TrackDLaunchIdentity',
    'Get-TrackDIdentityArguments',
    'Test-TrackDLaunchIdentity',
    'New-TrackDArtifactIdentity',
    'Read-TrackDArtifactIdentity',
    'Get-TrackDDeclaredEngineVersions',
    'Get-TrackDPluginName',
    'Get-TrackDHostProjectName',
    'Expand-TrackDList',
    'Write-TrackDSection',
    'Write-TrackDStep',
    'Write-TrackDGood',
    'Write-TrackDBad',
    'Write-TrackDWarn',
    'Get-TrackDSha256',
    'Get-TrackDFileFacts',
    'Get-TrackDPluginVersion',
    'Get-TrackDGitInfo',
    'Get-TrackDTrackedFiles',
    'Get-TrackDHostSummary',
    'Resolve-TrackDEnginePath',
    'Get-TrackDEngineFacts',
    'Test-TrackDUnrealBuildToolBusy',
    'Get-TrackDBuildToolStatePaths',
    'Test-TrackDBuildToolStateBusy',
    'Wait-TrackDBuildToolIdle',
    'Test-TrackDLogForBuildMutex',
    'Test-TrackDLogForResourceExhaustion',
    'Test-TrackDLogForZenStoreFailure',
    'New-TrackDRunContext',
    'Write-TrackDJson',
    'Invoke-TrackDProcess',
    'Copy-TrackDTree',
    'New-TrackDHostStaging',
    'Read-TrackDSmokeResult',
    'Test-TrackDViewportEvidence',
    'Get-TrackDExcludedPathSegments',
    'ConvertTo-TrackDPathRegex',
    'Test-TrackDPathMatchesPattern',
    'Get-TrackDPathExclusion',
    'Expand-TrackDDeclaredPattern',
    'Get-TrackDIgnoredPaths',
    'Get-TrackDDeclaredReleaseInputs',
    'New-TrackDCleanCopy',
    'Get-TrackDCleanCopyLeaks',
    'Test-TrackDReleaseSourceFiltering',
    'Test-TrackDBuildContentionClassifier',
    'Get-TrackDExitCode'
)
