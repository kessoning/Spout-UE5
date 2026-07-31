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
        BuildVersion  = $null
    }

    $buildVersionPath = Join-Path $EnginePath 'Engine\Build\Build.version'
    if (Test-Path -LiteralPath $buildVersionPath -PathType Leaf)
    {
        try
        {
            $buildVersion = Get-Content -LiteralPath $buildVersionPath -Raw | ConvertFrom-Json
            $facts.BuildVersion = "$($buildVersion.MajorVersion).$($buildVersion.MinorVersion).$($buildVersion.PatchVersion)"
            if ($buildVersion.PSObject.Properties.Name -contains 'BuildId')
            {
                $facts.BuildId = [string] $buildVersion.BuildId
            }
        }
        catch { }
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
    #>

    $names = @('UnrealBuildTool', 'AutomationTool', 'UnrealHeaderTool')
    foreach ($name in $names)
    {
        $running = Get-Process -Name $name -ErrorAction SilentlyContinue
        if ($running) { return $true }
    }
    return $false
}

function Test-TrackDLogForBuildMutex
{
    <#
    .SYNOPSIS
        True when a captured log shows the environmental UBT mutex rather than a real build failure.
    #>
    param([string] $LogPath)

    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) { return $false }
    $text = Get-Content -LiteralPath $LogPath -Raw -ErrorAction SilentlyContinue
    if (-not $text) { return $false }

    return ($text -match 'conflicting instance' -or
            $text -match 'Another instance of UnrealBuildTool is already running' -or
            $text -match 'Timed out waiting for another instance')
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
        $RunId = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
    }
    if ([string]::IsNullOrWhiteSpace($ArtifactsRoot))
    {
        $ArtifactsRoot = Join-Path $RepoRoot 'Artifacts\TrackD'
    }

    $runRoot = Join-Path $ArtifactsRoot $RunId

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
        [int] $TimeoutSeconds = 900,
        [string] $WorkingDirectory
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
    $psi.CreateNoWindow = $false
    if ($WorkingDirectory) { $psi.WorkingDirectory = $WorkingDirectory }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $psi

    $stdOutBuilder = New-Object System.Text.StringBuilder
    $stdErrBuilder = New-Object System.Text.StringBuilder

    $outEvent = Register-ObjectEvent -InputObject $process -EventName OutputDataReceived -MessageData $stdOutBuilder -Action {
        if ($null -ne $EventArgs.Data) { [void] $Event.MessageData.AppendLine($EventArgs.Data) }
    }
    $errEvent = Register-ObjectEvent -InputObject $process -EventName ErrorDataReceived -MessageData $stdErrBuilder -Action {
        if ($null -ne $EventArgs.Data) { [void] $Event.MessageData.AppendLine($EventArgs.Data) }
    }

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false

    try
    {
        [void] $process.Start()
        $process.BeginOutputReadLine()
        $process.BeginErrorReadLine()

        if (-not $process.WaitForExit($TimeoutSeconds * 1000))
        {
            $timedOut = $true
            try
            {
                # Entire tree: Unreal's children keep running (and keep the log open) otherwise.
                $process.Kill($true)
            }
            catch
            {
                try { $process.Kill() } catch { }
            }
            # Give the kill a moment to land so the captured output is complete.
            [void] $process.WaitForExit(15000)
        }

        # Flush the async readers before reading the builders.
        $process.WaitForExit()
    }
    finally
    {
        $stopwatch.Stop()
        Unregister-Event -SourceIdentifier $outEvent.Name -ErrorAction SilentlyContinue
        Unregister-Event -SourceIdentifier $errEvent.Name -ErrorAction SilentlyContinue
        Remove-Job -Id $outEvent.Id -Force -ErrorAction SilentlyContinue
        Remove-Job -Id $errEvent.Id -Force -ErrorAction SilentlyContinue
    }

    Set-Content -LiteralPath $StdOutPath -Value $stdOutBuilder.ToString() -Encoding UTF8
    Set-Content -LiteralPath $StdErrPath -Value $stdErrBuilder.ToString() -Encoding UTF8

    $exitCode = $null
    try { $exitCode = $process.ExitCode } catch { $exitCode = $null }
    $process.Dispose()

    return [ordered] @{
        FilePath        = $FilePath
        Arguments       = $ArgumentList
        CommandLine     = ('"{0}" {1}' -f $FilePath, ($ArgumentList -join ' '))
        ExitCode        = $exitCode
        TimedOut        = $timedOut
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

Export-ModuleMember -Function @(
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
    'Test-TrackDLogForBuildMutex',
    'Test-TrackDLogForResourceExhaustion',
    'New-TrackDRunContext',
    'Write-TrackDJson',
    'Invoke-TrackDProcess',
    'Copy-TrackDTree',
    'New-TrackDHostStaging',
    'Read-TrackDSmokeResult',
    'Get-TrackDExitCode'
)
