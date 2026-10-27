# Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
# Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

<#
.SYNOPSIS
    Runs the Spout performance baseline scenarios in a packaged host and collects the results.

.DESCRIPTION
    Each scenario runs as its own packaged-host process, so one scenario cannot influence the next
    through accumulated state. The host measures and writes its own JSON; this script launches the
    scenarios, gathers those files, and produces an aggregate JSON plus a Markdown summary.

    Numbers are never synthesised here. If a scenario cannot genuinely run - the host refuses it,
    the process crashes, no frames are measured - the row is recorded as failed with its reason and
    the aggregate says so. An empty result is reported as empty.

    Optional Unreal Insights capture: -Trace adds -trace=cpu,gpu,frame,log and -tracefile=<path> so
    each scenario leaves a .utrace next to its JSON. The trace is retained as evidence; nothing in
    this script claims to have inspected one.

    Optional RHI validation: -RhiValidation adds -rhivalidation, and -D3DDebug adds -d3ddebug. Both
    change what is being measured (they add substantial overhead), so they are off by default and
    the result files record which were on.

.EXAMPLE
    .\Scripts\Invoke-PerformanceBaselines.ps1 -EngineVersion 5.8 -EvidenceRoot .\Artifacts\TrackD\run

.EXAMPLE
    .\Scripts\Invoke-PerformanceBaselines.ps1 -EngineVersion 5.8 -Scenarios receiver1080p,cpufallback -Trace -EvidenceRoot .\Artifacts\TrackD\run
#>
[CmdletBinding()]
param(
    [string] $RepoRoot = '',

    # One engine. Baselines are a measurement, not a compatibility matrix: running them on six
    # engines would produce six sets of numbers that differ for engine reasons rather than plugin
    # ones, which is not what a baseline is for.
    [string] $EngineVersion = '5.8',

    [ValidateSet('Development', 'Shipping')]
    [string] $Configuration = 'Development',

    [string] $EngineRoot = 'D:\UE',
    [string[]] $EnginePaths = @(),

    [Parameter(Mandatory = $true)]
    [string] $EvidenceRoot,

    [string] $WorkRoot = "$env:TEMP\SpoutD",

    # Empty means every scenario.
    [string[]] $Scenarios = @(),

    # 60 s per scenario by default: long enough for frame times to stabilise and for percentiles
    # over several thousand frames to mean something.
    [ValidateRange(1,86400)][int] $Seconds = 60,
    [ValidateRange(0,3600)][int] $WarmupSeconds = 5,
    [ValidateRange(1,32)][int] $Streams = 4,

    # Explicit diagnostic override; zero preserves the plugin's configured default.
    [ValidateRange(0,65536)][int] $RetainedBudgetMiB = 0,

    [switch] $Trace,
    [switch] $RhiValidation,
    [switch] $D3DDebug,

    # Reuses an existing packaged host instead of building one. The packaged-host matrix already
    # produces exactly the binary these baselines should measure.
    [string] $ExistingExecutable = '',

    [int] $PackageTimeoutSeconds = 5400,
    [int] $ScenarioTimeoutMarginSeconds = 300,

    # Extra arguments appended to every scenario launch. Empty by default. Development coverage only (for example
    # '-ini:Engine:[ConsoleVariables]:r.Spout.Dev.AsyncSend=0', the synchronous render-target send; the asynchronous send
    # is the default); the summary records them.
    [string[]] $ExtraLaunchArguments = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

Import-Module (Join-Path $PSScriptRoot 'TrackD.psm1') -Force

if ([string]::IsNullOrWhiteSpace($RepoRoot)) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

$Scenarios = Expand-TrackDList $Scenarios
$EnginePaths = Expand-TrackDList $EnginePaths

# The required performance scenarios, plus two that catch regressions the throughput numbers alone
# would hide (no-new-frame cost and resize/reconnect churn).
$AllScenarios = @(
    @{ Name = 'noplugin'; Title = 'Independent host with Spout disabled'; Covers = 'No-plugin control' }
    @{ Name = 'baseline'; Title = 'Idle host without stream work'; Covers = 'Host baseline for comparison' }
    @{ Name = 'sender1080p';     Title = '1080p60 single sender';        Covers = '1080p60 baseline (send)' }
    @{ Name = 'receiver1080p';   Title = '1080p60 single receiver';      Covers = '1080p60 baseline (receive)' }
    @{ Name = 'sender4k';        Title = '4K60 single sender';           Covers = '4K60 baseline (send)' }
    @{ Name = 'receiver4k';      Title = '4K60 single receiver';         Covers = '4K60 baseline (receive)' }
    @{ Name = 'multistream';     Title = 'Multiple simultaneous streams'; Covers = 'Multi-stream baseline' }
    @{ Name = 'cpufallback';     Title = 'CPU receiver fallback';        Covers = 'CPU fallback profile (r.Spout.GPUReceiver=0)' }
    @{ Name = 'idlereceiver';    Title = 'Receiver with no new frame';   Covers = 'No-new-frame receiver cost' }
    @{ Name = 'resizechurn';     Title = 'Repeated resize and reconnect'; Covers = 'Resize/reconnect spikes' }
    @{ Name = 'startupshutdown'; Title = 'Repeated stream start/stop';   Covers = 'Startup and shutdown spikes' }
    @{ Name = 'restartpaced';    Title = 'Paced stream start/stop (2 s)'; Covers = 'Qualified close/reopen workload at the default retention budget' }
)

if ($Scenarios.Count -gt 0)
{
    $known = @($AllScenarios | ForEach-Object { $_.Name })
    foreach ($requested in $Scenarios)
    {
        if ($known -notcontains $requested)
        {
            throw "Unknown scenario '$requested'. Known scenarios: $($known -join ', ')"
        }
    }
    $AllScenarios = @($AllScenarios | Where-Object { $Scenarios -contains $_.Name })
}

$performanceEvidence = Join-Path $EvidenceRoot 'performance'
$insightsEvidence = Join-Path $EvidenceRoot 'insights'
foreach ($dir in @($EvidenceRoot, $performanceEvidence, $insightsEvidence))
{
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
}
$EvidenceRoot = (Resolve-Path -LiteralPath $EvidenceRoot).Path
$performanceEvidence = (Resolve-Path -LiteralPath $performanceEvidence).Path
$insightsEvidence = (Resolve-Path -LiteralPath $insightsEvidence).Path
$invocation = New-TrackDEvidenceRun -Parent $performanceEvidence -Label 'measurement'
$runInputs = Get-TrackDSourceIdentity -Root $RepoRoot
$nativeTools = Build-TrackDNativeTools -RepoRoot $RepoRoot -WorkRoot $WorkRoot -EvidenceRoot $invocation.Directory

$hostProjectName = Get-TrackDHostProjectName

$explicitMap = @{}
foreach ($pair in $EnginePaths)
{
    if ([string]::IsNullOrWhiteSpace($pair)) { continue }
    $separator = $pair.IndexOf('=')
    if ($separator -lt 1) { throw "Malformed -EnginePaths entry '$pair'. Expected 'version=path'." }
    $explicitMap[$pair.Substring(0, $separator).Trim()] = $pair.Substring($separator + 1).Trim()
}

$enginePath = Resolve-TrackDEnginePath -Version $EngineVersion -EngineRoot $EngineRoot -ExplicitMap $explicitMap
$engine = Get-TrackDEngineFacts -Version $EngineVersion -EnginePath $enginePath

Write-TrackDSection "Performance baselines - UE $EngineVersion $Configuration"
Write-TrackDStep "Scenarios : $(($AllScenarios | ForEach-Object { $_.Name }) -join ', ')"
Write-TrackDStep "Duration  : ${Seconds}s measured after ${WarmupSeconds}s warmup"
Write-TrackDStep "Evidence  : $performanceEvidence"

$failures = New-Object System.Collections.ArrayList

# All nonzero process exits fail, including traced shutdowns.

# --- Obtain a packaged host --------------------------------------------------------------------
$executable = $ExistingExecutable
if ([string]::IsNullOrWhiteSpace($executable))
{
    if (-not $engine.Found)
    {
        throw "UE $EngineVersion is not installed at $enginePath, and no -ExistingExecutable was supplied."
    }

    $shortVersion = $EngineVersion.Replace('.', '')
    $stagingDir = Join-Path $WorkRoot ("f{0}\P" -f $shortVersion)
    $archiveDir = Join-Path $WorkRoot ("f{0}\A" -f $shortVersion)

    Write-TrackDStep "Packaging a host for the baselines"
    $staging = New-TrackDHostStaging -RepoRoot $RepoRoot -StagingProjectDir $stagingDir -Force
    $hostBuildInputs = Get-TrackDSourceIdentity -Root $staging.ProjectDir -Kind Host
    if (Test-Path -LiteralPath $archiveDir) { Remove-Item -LiteralPath $archiveDir -Recurse -Force }
    New-Item -ItemType Directory -Path $archiveDir -Force | Out-Null

    $uatLog = Join-Path $performanceEvidence 'package-host.log'
    $uatArgs = @(
        'BuildCookRun',
        "-project=$($staging.ProjectFile)",
        '-noP4', '-platform=Win64', '-targetplatform=Win64',
        "-clientconfig=$Configuration",
        '-map=/Engine/Maps/Entry+/Engine/Maps/Templates/Template_Default',
        '-build', '-cook', '-stage', '-pak', '-archive',
        "-archivedirectory=$archiveDir",
        '-nodebuginfo', '-utf8output', '-unattended', '-nocompileuat'
    )

    # Settle and retry around the packaging build, the same way every other build in the pipeline
    # does. A stage-level settle is not sufficient on its own, because an unrelated build elsewhere on
    # the machine can reacquire UnrealBuildTool's shared state in the gap between the settle and the
    # build starting; the build then fails at once with an IOException on the shared rules assembly,
    # having compiled nothing, where a per-attempt retry comes through the same conditions intact.
    $packageAttempt = 0
    $uatResult = $null
    while ($true)
    {
        $packageAttempt++

        $settled = Wait-TrackDBuildToolIdle
        if ($settled -gt 0)
        {
            Write-TrackDStep "waited ${settled}s for another build to release the shared UnrealBuildTool state"
        }

        $uatResult = Invoke-TrackDProcess -FilePath $engine.RunUAT -ArgumentList $uatArgs `
            -StdOutPath $uatLog -TimeoutSeconds $PackageTimeoutSeconds -WorkingDirectory $WorkRoot

        if ($uatResult.ExitCode -eq 0) { break }

        # Only environmental contention is retried; a genuine packaging failure still throws on its
        # first attempt with its log intact.
        if ($packageAttempt -le 3 -and (Test-TrackDLogForBuildMutex $uatLog))
        {
            Write-TrackDWarn "shared UnrealBuildTool state was locked by another build; retrying the host packaging"
            continue
        }

        break
    }

    if ($uatResult.ExitCode -ne 0)
    {
        throw "Could not package a host for the baselines: BuildCookRun exited $($uatResult.ExitCode) after $packageAttempt attempt(s). See $uatLog"
    }
    if ($packageAttempt -gt 1)
    {
        Write-TrackDStep "host packaged on attempt $packageAttempt; earlier attempts hit shared build-tool contention"
    }

    $binariesDir = Join-Path $archiveDir ('Windows\{0}\Binaries\Win64' -f $hostProjectName)
    $candidates = @(Get-ChildItem -LiteralPath $binariesDir -Filter '*.exe' -File -ErrorAction SilentlyContinue |
        Where-Object { $_.BaseName -like "$hostProjectName*" -and $_.BaseName -notlike '*CrashReport*' })
    if ($candidates.Count -eq 0)
    {
        throw "No packaged executable was produced under $binariesDir"
    }
    $executable = ($candidates | Sort-Object Length -Descending | Select-Object -First 1).FullName
    if ((Get-TrackDSourceIdentity -Root $staging.PluginDir).Digest -ne $runInputs.Digest) { throw 'Source changed during performance host staging' }
    $artifactIdentity = New-TrackDArtifactIdentity -Artifact $executable -SourceIdentity $runInputs -Engine $engine -Configuration $Configuration -Git (Get-TrackDGitInfo $RepoRoot) -HostSourceIdentity $hostBuildInputs -CompanionRoot $archiveDir
}
else { $artifactIdentity = Read-TrackDArtifactIdentity -Artifact $executable }
if ($artifactIdentity.Engine.Version -ne $EngineVersion -or $artifactIdentity.Configuration -ne $Configuration) { throw 'Requested engine/configuration differs from the original artifact build' }

if (-not (Test-Path -LiteralPath $executable -PathType Leaf))
{
    throw "Packaged host executable not found: $executable"
}
Write-TrackDGood "host: $executable"

$control = $null
if (@($AllScenarios | Where-Object { $_.Name -eq 'noplugin' }).Count -gt 0) {
    Write-TrackDStep 'Packaging independent no-plugin control'
    $control = Build-TrackDControlHost -RepoRoot $RepoRoot -WorkRoot $WorkRoot -EvidenceRoot $invocation.Directory -Engine $engine -Configuration $Configuration -TimeoutSeconds $PackageTimeoutSeconds
}

# --- Run each scenario ---------------------------------------------------------------------------
$rows = New-Object System.Collections.ArrayList

foreach ($scenario in $AllScenarios)
{
    $name = $scenario.Name
    $scenarioExecutable = if ($name -eq 'noplugin') { $control.Executable } else { $executable }
    $scenarioIdentity = if ($name -eq 'noplugin') { $control.BuildIdentity } else { $artifactIdentity }
    Write-TrackDSection "Scenario: $name - $($scenario.Title)"

    $scenarioEvidence = Join-Path $invocation.Directory $name
    New-Item -ItemType Directory -Path $scenarioEvidence -Force | Out-Null

    $stdOutLog = Join-Path $scenarioEvidence 'stdout.log'
    $absLog = Join-Path $scenarioEvidence 'engine.log'

    $launchArgs = @(
        "-SpoutPerf=$name",
        "-SpoutResultDir=$scenarioEvidence",
        "-SpoutPerfSeconds=$Seconds",
        "-SpoutPerfWarmupSeconds=$WarmupSeconds",
        "-SpoutPerfStreams=$Streams",
        "-SpoutNativePeer=$($nativeTools.Executable)",
        '-unattended', '-nopause', '-nosplash', '-NoSound',
        '-windowed', '-ResX=640', '-ResY=360',
        '-stdout', "-abslog=$absLog"
    )

    $tracePath = $null
    if ($RetainedBudgetMiB -gt 0) { $launchArgs += "-ExecCmds=r.Spout.MaxRetainedMiB $RetainedBudgetMiB" }
    if ($Trace)
    {
        $tracePath = Join-Path $scenarioEvidence 'capture.utrace'
        $launchArgs += "-trace=cpu,gpu,frame,bookmark,log"
        $launchArgs += "-tracefile=$tracePath"
        $launchArgs += '-statnamedevents'
    }
    if ($RhiValidation) { $launchArgs += '-rhivalidation' }
    if ($D3DDebug) { $launchArgs += '-d3ddebug' }
    if ($ExtraLaunchArguments.Count -gt 0) { $launchArgs += $ExtraLaunchArguments }

    $timeout = $Seconds + $WarmupSeconds + $ScenarioTimeoutMarginSeconds
    $identity = New-TrackDLaunchIdentity -Directory $scenarioEvidence -Executable $scenarioExecutable -ArtifactIdentity $scenarioIdentity
    $launchArgs += Get-TrackDIdentityArguments $identity

    $launchResult = Invoke-TrackDProcess -FilePath $scenarioExecutable -ArgumentList $launchArgs `
        -StdOutPath $stdOutLog -TimeoutSeconds $timeout -WorkingDirectory (Split-Path -Parent $scenarioExecutable)

    $resultFile = Join-Path $scenarioEvidence ("perf-{0}.json" -f $name)

    $row = [ordered] @{
        Scenario        = $name
        Title           = $scenario.Title
        Covers          = $scenario.Covers
        EngineVersion   = $EngineVersion
        Configuration   = $Configuration
        Executable      = $scenarioExecutable
        CommandLine     = $launchResult.CommandLine
        ExitCode        = $launchResult.ExitCode
        TimedOut        = $launchResult.TimedOut
        DurationSeconds = $launchResult.DurationSeconds
        StdOutPath      = $stdOutLog
        EngineLogPath   = $absLog
        TracePath       = $tracePath
        TraceProduced   = ($tracePath -and (Test-Path -LiteralPath $tracePath -PathType Leaf))
        RhiValidation   = [bool] $RhiValidation
        D3DDebug        = [bool] $D3DDebug
        ResultFile      = $resultFile
        Results         = $null
        Status          = 'NotRun'
        Problems        = @()
        Identity        = Test-TrackDLaunchIdentity $identity $launchResult
        BuildIdentity   = $scenarioIdentity
    }

    $problems = New-Object System.Collections.ArrayList
    foreach ($problem in $row.Identity.Problems) { [void] $problems.Add($problem) }

    if ($launchResult.TimedOut)
    {
        [void] $problems.Add("the scenario did not finish within $timeout s and was terminated")
    }
    if ($null -eq $launchResult.ExitCode -or $launchResult.ExitCode -ne 0)
    {
        [void] $problems.Add("the host exited with code $($launchResult.ExitCode)")
    }

    if (Test-Path -LiteralPath $resultFile -PathType Leaf)
    {
        try
        {
            $json = Get-Content -LiteralPath $resultFile -Raw | ConvertFrom-Json
            $row.Results = $json
            $measurement = Test-TrackDPerformanceResult -Result $json -Scenario $name -RunId $identity.RunId -Seconds $Seconds -Streams $Streams
            foreach ($problem in $measurement.Problems) { [void] $problems.Add($problem) }
        }
        catch
        {
            [void] $problems.Add("the result file could not be parsed: $($_.Exception.Message)")
        }
    }
    else
    {
        [void] $problems.Add("no result file was written at $resultFile")
    }

    if ($Trace -and -not $row.TraceProduced)
    {
        [void] $problems.Add("Unreal Insights capture was requested but no trace was written to $tracePath")
    }

    $row.Problems = $problems.ToArray()
    $row.Status = if ($problems.Count -eq 0) { 'Passed' } else { 'Failed' }

    if ($row.Status -eq 'Passed')
    {
        $r = $row.Results.results
        Write-TrackDGood ("{0} frames in {1:N1}s, mean {2:N3} ms, p95 {3:N3} ms, p99 {4:N3} ms" -f `
            $r.frameCount, $r.measuredSeconds, $r.frameTimeMeanMs, $r.frameTimeP95Ms, $r.frameTimeP99Ms)
    }
    else
    {
        foreach ($problem in $problems) { Write-TrackDBad $problem }
        [void] $failures.Add("$name : $($problems -join '; ')")
    }

    [void] $rows.Add($row)
}

# --- Aggregate -----------------------------------------------------------------------------------
$aggregate = [ordered] @{
    Schema        = 'spoutplugin-trackd-performance/1'
    GeneratedUtc  = (Get-Date).ToUniversalTime().ToString('o')
    RepoRoot      = $RepoRoot
    PluginVersion = 'See immutable BuildIdentity.SourceIdentity descriptor hash'
    RunId         = $invocation.RunId
    BuildIdentity = $artifactIdentity
    ControlBuild = $control
    NativeTools = $nativeTools
    EngineVersion = $EngineVersion
    EngineBuildId = $engine.BuildId
    Configuration = $Configuration
    ExtraLaunchArguments = @($ExtraLaunchArguments)
    Executable    = $executable
    MeasuredSeconds = $Seconds
    WarmupSeconds = $WarmupSeconds
    StreamCount   = $Streams
    TraceEnabled  = [bool] $Trace
    RhiValidation = [bool] $RhiValidation
    D3DDebug      = [bool] $D3DDebug
    ValidationInvocationGit = Get-TrackDGitInfo $RepoRoot
    Host          = Get-TrackDHostSummary
    Scenarios     = $rows.ToArray()
    Failures      = $failures.ToArray()
    Success       = ($failures.Count -eq 0)
}

$aggregatePath = Join-Path $performanceEvidence 'performance-baselines.json'
Write-TrackDJson -InputObject $aggregate -Path (Join-Path $invocation.Directory 'performance-baselines.json')
Write-TrackDJson -InputObject $aggregate -Path $aggregatePath

# --- Markdown summary ----------------------------------------------------------------------------
$summary = New-Object System.Text.StringBuilder
[void] $summary.AppendLine('# Spout performance baselines')
[void] $summary.AppendLine('Timing includes a serialized pixel observer and queue drain. observerSeconds includes waiting for queued plugin work and cannot be subtracted as isolated validation overhead. Compare with the idle host baseline and separate no-plugin control; delivery latency is reported separately. These numbers do not represent unobserved asynchronous throughput.')
[void] $summary.AppendLine()
[void] $summary.AppendLine("Generated: $($aggregate.GeneratedUtc)")
[void] $summary.AppendLine()
[void] $summary.AppendLine("| Field | Value |")
[void] $summary.AppendLine("|---|---|")
[void] $summary.AppendLine("| Plugin version | $($aggregate.PluginVersion) |")
[void] $summary.AppendLine("| Engine | UE $EngineVersion |")
[void] $summary.AppendLine("| Configuration | $Configuration |")
[void] $summary.AppendLine("| Measured window | ${Seconds}s after ${WarmupSeconds}s warmup |")
if ($aggregate.Host.Gpus -and @($aggregate.Host.Gpus).Count -gt 0)
{
    $inventory = @($aggregate.Host.Gpus | ForEach-Object { "$($_.Name) (driver $($_.DriverVersion))" }) -join '; '
    [void] $summary.AppendLine("| GPU inventory (see engine log for selected adapter) | $inventory |")
}
[void] $summary.AppendLine("| OS | $($aggregate.Host.OperatingSystem) $($aggregate.Host.OsVersion) |")
[void] $summary.AppendLine("| CPU | $($aggregate.Host.ProcessorName) |")
[void] $summary.AppendLine("| Unreal Insights trace | $(if ($Trace) { 'captured' } else { 'not captured' }) |")

[void] $summary.AppendLine()
[void] $summary.AppendLine('| Scenario | Res | Streams | Mode | Frames | Mean ms | p95 ms | p99 ms | Peak ms | Game ms | Render ms | GPU ms | Peak WS MB | Result |')
[void] $summary.AppendLine('|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|')

foreach ($row in $rows)
{
    if ($row.Status -ne 'Passed' -or -not $row.Results)
    {
        [void] $summary.AppendLine("| $($row.Scenario) | - | - | - | - | - | - | - | - | - | - | - | - | **FAILED**: $($row.Problems -join '; ') |")
        continue
    }

    $c = $row.Results.configuration
    $r = $row.Results.results
    $gpuText = if ($null -eq $r.gpuMeanMs) { 'n/a' } else { ('{0:N3}' -f $r.gpuMeanMs) }
    $peakWs = [Math]::Round(($r.peakWorkingSetBytes / 1MB), 1)

    [void] $summary.AppendLine(
        ("| {0} | {1}x{2} | {3} | {4} | {5} | {6:N3} | {7:N3} | {8:N3} | {9:N3} | {10:N3} | {11:N3} | {12} | {13} | pass |" -f `
            $row.Scenario, $c.width, $c.height, $c.streamCount, $c.receiverMode,
            $r.frameCount, $r.frameTimeMeanMs, $r.frameTimeP95Ms, $r.frameTimeP99Ms, $r.frameTimePeakMs,
            $r.gameThreadMeanMs, $r.renderThreadMeanMs, $gpuText, $peakWs))
}

[void] $summary.AppendLine()

if ($failures.Count -gt 0)
{
    [void] $summary.AppendLine('## Scenarios that did not produce a baseline')
    [void] $summary.AppendLine()
    foreach ($failure in $failures) { [void] $summary.AppendLine("- $failure") }
    [void] $summary.AppendLine()
}

$summaryPath = Join-Path $performanceEvidence 'performance-baselines.md'
Set-Content -LiteralPath (Join-Path $invocation.Directory 'performance-baselines.md') -Value $summary.ToString() -Encoding UTF8
Set-Content -LiteralPath $summaryPath -Value $summary.ToString() -Encoding UTF8

Write-Host ''
Write-Host "Performance manifest: $aggregatePath" -ForegroundColor Cyan
Write-Host "Performance summary : $summaryPath" -ForegroundColor Cyan

exit (Get-TrackDExitCode -FailureCount $failures.Count)
