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
    [int] $Seconds = 60,
    [int] $WarmupSeconds = 5,
    [int] $Streams = 4,

    [switch] $Trace,
    [switch] $RhiValidation,
    [switch] $D3DDebug,

    # Reuses an existing packaged host instead of building one. The packaged-host matrix already
    # produces exactly the binary these baselines should measure.
    [string] $ExistingExecutable = '',

    [int] $PackageTimeoutSeconds = 5400,
    [int] $ScenarioTimeoutMarginSeconds = 300
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
    @{ Name = 'sender1080p';     Title = '1080p60 single sender';        Covers = '1080p60 baseline (send)' }
    @{ Name = 'receiver1080p';   Title = '1080p60 single receiver';      Covers = '1080p60 baseline (receive)' }
    @{ Name = 'sender4k';        Title = '4K60 single sender';           Covers = '4K60 baseline (send)' }
    @{ Name = 'receiver4k';      Title = '4K60 single receiver';         Covers = '4K60 baseline (receive)' }
    @{ Name = 'multistream';     Title = 'Multiple simultaneous streams'; Covers = 'Multi-stream baseline' }
    @{ Name = 'cpufallback';     Title = 'CPU receiver fallback';        Covers = 'CPU fallback profile (r.Spout.GPUReceiver=0)' }
    @{ Name = 'idlereceiver';    Title = 'Receiver with no new frame';   Covers = 'No-new-frame receiver cost' }
    @{ Name = 'resizechurn';     Title = 'Repeated resize and reconnect'; Covers = 'Resize/reconnect spikes' }
    @{ Name = 'startupshutdown'; Title = 'Repeated stream start/stop';   Covers = 'Startup and shutdown spikes' }
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

# ECrashExitCodes::CrashReporterCrashed, from GenericPlatformCrashContext.h.
#
# With -trace active, a packaged host that shuts down completely cleanly - engine exited, plugin
# unloaded, every marker emitted, "LogExit: Exiting." in the log - still reports this as its process
# exit code, because Unreal's out-of-process crash monitor is what supplies the code and it cannot
# read the real one once the trace session has torn down.
#
# It is exempted ONLY when -Trace was requested, ONLY for this exact value, and ONLY as a separate
# decision from every other check - the result file must still exist, still say passed, and still
# contain measured frames, or the scenario fails regardless. The exemption is recorded in the row so
# a reader can see it was applied rather than having a clean exit code silently assumed.
$TraceExitCodeExemption = 777003

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

    $uatResult = Invoke-TrackDProcess -FilePath $engine.RunUAT -ArgumentList $uatArgs `
        -StdOutPath $uatLog -TimeoutSeconds $PackageTimeoutSeconds -WorkingDirectory $WorkRoot

    if ($uatResult.ExitCode -ne 0)
    {
        throw "Could not package a host for the baselines: BuildCookRun exited $($uatResult.ExitCode). See $uatLog"
    }

    $binariesDir = Join-Path $archiveDir ('Windows\{0}\Binaries\Win64' -f $hostProjectName)
    $candidates = @(Get-ChildItem -LiteralPath $binariesDir -Filter '*.exe' -File -ErrorAction SilentlyContinue |
        Where-Object { $_.BaseName -like "$hostProjectName*" -and $_.BaseName -notlike '*CrashReport*' })
    if ($candidates.Count -eq 0)
    {
        throw "No packaged executable was produced under $binariesDir"
    }
    $executable = ($candidates | Sort-Object Length -Descending | Select-Object -First 1).FullName
}

if (-not (Test-Path -LiteralPath $executable -PathType Leaf))
{
    throw "Packaged host executable not found: $executable"
}
Write-TrackDGood "host: $executable"

# --- Run each scenario ---------------------------------------------------------------------------
$rows = New-Object System.Collections.ArrayList

foreach ($scenario in $AllScenarios)
{
    $name = $scenario.Name
    Write-TrackDSection "Scenario: $name - $($scenario.Title)"

    $scenarioEvidence = Join-Path $performanceEvidence $name
    New-Item -ItemType Directory -Path $scenarioEvidence -Force | Out-Null

    $stdOutLog = Join-Path $scenarioEvidence 'stdout.log'
    $absLog = Join-Path $scenarioEvidence 'engine.log'

    $launchArgs = @(
        "-SpoutPerf=$name",
        "-SpoutResultDir=$scenarioEvidence",
        "-SpoutPerfSeconds=$Seconds",
        "-SpoutPerfWarmupSeconds=$WarmupSeconds",
        "-SpoutPerfStreams=$Streams",
        '-unattended', '-nopause', '-nosplash', '-NoSound',
        '-windowed', '-ResX=640', '-ResY=360',
        '-stdout', "-abslog=$absLog"
    )

    $tracePath = $null
    if ($Trace)
    {
        $tracePath = Join-Path $insightsEvidence ("{0}-{1}-{2}.utrace" -f $name, $EngineVersion, $Configuration)
        $launchArgs += "-trace=cpu,gpu,frame,bookmark,log"
        $launchArgs += "-tracefile=$tracePath"
        $launchArgs += '-statnamedevents'
    }
    if ($RhiValidation) { $launchArgs += '-rhivalidation' }
    if ($D3DDebug) { $launchArgs += '-d3ddebug' }

    $timeout = $Seconds + $WarmupSeconds + $ScenarioTimeoutMarginSeconds

    $launchResult = Invoke-TrackDProcess -FilePath $executable -ArgumentList $launchArgs `
        -StdOutPath $stdOutLog -TimeoutSeconds $timeout -WorkingDirectory (Split-Path -Parent $executable)

    $resultFile = Join-Path $scenarioEvidence ("perf-{0}.json" -f $name)

    $row = [ordered] @{
        Scenario        = $name
        Title           = $scenario.Title
        Covers          = $scenario.Covers
        EngineVersion   = $EngineVersion
        Configuration   = $Configuration
        Executable      = $executable
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
        ExitCodeExemption = $null
        Status          = 'NotRun'
        Problems        = @()
    }

    $problems = New-Object System.Collections.ArrayList

    if ($launchResult.TimedOut)
    {
        [void] $problems.Add("the scenario did not finish within $timeout s and was terminated")
    }
    if ($null -eq $launchResult.ExitCode -or $launchResult.ExitCode -ne 0)
    {
        if ($Trace -and $launchResult.ExitCode -eq $TraceExitCodeExemption)
        {
            $row.ExitCodeExemption = "exit code $TraceExitCodeExemption (ECrashExitCodes::CrashReporterCrashed) accepted because -Trace was active; see the comment on `$TraceExitCodeExemption. Every other check still applied."
            Write-TrackDWarn $row.ExitCodeExemption
        }
        else
        {
            [void] $problems.Add("the host exited with code $($launchResult.ExitCode)")
        }
    }

    if (Test-Path -LiteralPath $resultFile -PathType Leaf)
    {
        try
        {
            $json = Get-Content -LiteralPath $resultFile -Raw | ConvertFrom-Json
            $row.Results = $json
            if (-not $json.passed)
            {
                [void] $problems.Add("the host reported a failure: $($json.failureReason)")
            }
            elseif ([int] $json.results.frameCount -le 0)
            {
                # A "pass" with no measured frames is not a baseline.
                [void] $problems.Add('the scenario measured zero frames')
            }
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
    PluginVersion = Get-TrackDPluginVersion $RepoRoot
    EngineVersion = $EngineVersion
    EngineBuildId = $engine.BuildId
    Configuration = $Configuration
    Executable    = $executable
    MeasuredSeconds = $Seconds
    WarmupSeconds = $WarmupSeconds
    StreamCount   = $Streams
    TraceEnabled  = [bool] $Trace
    RhiValidation = [bool] $RhiValidation
    D3DDebug      = [bool] $D3DDebug
    ExitCodeExemptions = @($rows | Where-Object { $_.ExitCodeExemption } | ForEach-Object {
        [ordered] @{ Scenario = $_.Scenario; ExitCode = $_.ExitCode; Reason = $_.ExitCodeExemption } })
    Git           = Get-TrackDGitInfo $RepoRoot
    Host          = Get-TrackDHostSummary
    Scenarios     = $rows.ToArray()
    Failures      = $failures.ToArray()
    Success       = ($failures.Count -eq 0)
}

$aggregatePath = Join-Path $performanceEvidence 'performance-baselines.json'
Write-TrackDJson -InputObject $aggregate -Path $aggregatePath

# --- Markdown summary ----------------------------------------------------------------------------
$summary = New-Object System.Text.StringBuilder
[void] $summary.AppendLine('# Spout performance baselines')
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
    $gpu = @($aggregate.Host.Gpus)[0]
    [void] $summary.AppendLine("| GPU | $($gpu.Name) (driver $($gpu.DriverVersion)) |")
}
[void] $summary.AppendLine("| OS | $($aggregate.Host.OperatingSystem) $($aggregate.Host.OsVersion) |")
[void] $summary.AppendLine("| CPU | $($aggregate.Host.ProcessorName) |")
[void] $summary.AppendLine("| Unreal Insights trace | $(if ($Trace) { 'captured' } else { 'not captured' }) |")
if (@($aggregate.ExitCodeExemptions).Count -gt 0)
{
    [void] $summary.AppendLine("| Exit-code exemptions | $(@($aggregate.ExitCodeExemptions).Count) scenario(s), code $TraceExitCodeExemption under -Trace (see below) |")
}
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
if (@($aggregate.ExitCodeExemptions).Count -gt 0)
{
    [void] $summary.AppendLine('## Exit-code exemptions applied')
    [void] $summary.AppendLine()
    [void] $summary.AppendLine("With ``-Trace`` active, a packaged host that shuts down completely cleanly still reports exit code $TraceExitCodeExemption (``ECrashExitCodes::CrashReporterCrashed``): Unreal's out-of-process crash monitor supplies the code and cannot read the real one once the trace session has torn down. Verified by running the same scenario with and without ``-Trace`` - identical output, exit 0 without it.")
    [void] $summary.AppendLine()
    [void] $summary.AppendLine('The exemption applies only to that exact code, only under `-Trace`, and only as a separate decision from every other check: the result file must still exist, still report a pass, and still contain measured frames.')
    [void] $summary.AppendLine()
    foreach ($exemption in $aggregate.ExitCodeExemptions)
    {
        [void] $summary.AppendLine("- $($exemption.Scenario): exit code $($exemption.ExitCode)")
    }
    [void] $summary.AppendLine()
}
if ($failures.Count -gt 0)
{
    [void] $summary.AppendLine('## Scenarios that did not produce a baseline')
    [void] $summary.AppendLine()
    foreach ($failure in $failures) { [void] $summary.AppendLine("- $failure") }
    [void] $summary.AppendLine()
}

$summaryPath = Join-Path $performanceEvidence 'performance-baselines.md'
Set-Content -LiteralPath $summaryPath -Value $summary.ToString() -Encoding UTF8

Write-Host ''
Write-Host "Performance manifest: $aggregatePath" -ForegroundColor Cyan
Write-Host "Performance summary : $summaryPath" -ForegroundColor Cyan

exit (Get-TrackDExitCode -FailureCount $failures.Count)
