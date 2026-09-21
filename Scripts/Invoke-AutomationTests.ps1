# Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
# Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

<#
.SYNOPSIS
    Builds the host test project and runs the SpoutPlugin automation suite on one or more engines.

.DESCRIPTION
    For each requested engine this script stages an isolated copy of Tests/SpoutHostTest with the
    plugin installed inside it, builds the editor target, and runs

        UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests SpoutPlugin;Quit"

    Pass and fail come from the automation framework's own index.json, never from scraping the log:
    a run that crashed before writing a report is a failure here, whereas a log-scraping check would
    simply find no "Failed" line and call it a pass.

    The real D3D12 RHI is used deliberately. Under -nullrhi the plugin's interop context logs an
    Error on every call and the automation framework counts logged errors as test failures, so
    -nullrhi would fail the suite for a reason that has nothing to do with the tests.

.EXAMPLE
    .\Scripts\Invoke-AutomationTests.ps1 -EngineVersions 5.3,5.8 -EvidenceRoot .\Artifacts\TrackD\run\automation
#>
[CmdletBinding()]
param(
    [string] $RepoRoot = '',

    # Install every byte of an exact release ZIP, including its shipped binaries/manifests.
    [string] $PluginArchive = '',

    [Parameter(Mandatory = $true)]
    [string[]] $EngineVersions,

    [string] $EngineRoot = 'D:\UE',

    [string[]] $EnginePaths = @(),

    # Where logs, reports and the result manifest are written.
    [Parameter(Mandatory = $true)]
    [string] $EvidenceRoot,

    # Short path root for the staging projects. Windows' 260-character action-path limit is reached
    # quickly once UBT appends its Intermediate tree, and UBT fails rather than truncating.
    [string] $WorkRoot = "$env:TEMP\SpoutD",

    # Automation filter passed to "Automation RunTests".
    [string] $TestFilter = 'SpoutPlugin',

    [string[]] $TestArguments = @(),

    [int] $BuildTimeoutSeconds = 3600,
    [int] $TestTimeoutSeconds = 1800,

    # Retries reserved for the machine-wide UnrealBuildTool mutex, which is an environmental
    # conflict rather than a plugin failure. Recorded separately in the manifest.
    [int] $MutexRetries = 3,

    # How long to wait for ANOTHER build to release UnrealBuildTool's machine-global state before
    # attempting anyway. Sized for a foreign build rather than for this pipeline racing itself: an
    # unrelated Unreal build on the same machine can hold that state for many minutes, and waiting
    # it out costs nothing on an idle machine.
    [int] $BuildToolWaitSeconds = 1800
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

Import-Module (Join-Path $PSScriptRoot 'TrackD.psm1') -Force

if ([string]::IsNullOrWhiteSpace($RepoRoot))
{
    $RepoRoot = Split-Path -Parent $PSScriptRoot
}
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

$invocation = New-TrackDEvidenceRun -Parent $EvidenceRoot -Label 'automation'
$archiveCandidate = $null
$pluginSourceRoot = $RepoRoot
if ($PluginArchive) {
    $versions = @(Expand-TrackDList $EngineVersions)
    if ($versions.Count -ne 1) { throw 'An archive qualification must name exactly one engine version' }
    $archiveCandidate = Expand-TrackDArchiveCandidate -ArchivePath $PluginArchive -DestinationParent $WorkRoot -EvidenceRoot $invocation.Directory
    if ($archiveCandidate.BuildIdentity.Engine.Version -ne $versions[0]) { throw 'Candidate archive belongs to a different engine' }
    $pluginSourceRoot = $archiveCandidate.PluginRoot
    if ((Get-TrackDSha256 (Join-Path $pluginSourceRoot 'ThirdParty/Spout/lib/amd64/Spout.dll')) -ne (Get-TrackDSha256 (Join-Path $RepoRoot 'ThirdParty/Spout/lib/amd64/Spout.dll'))) { throw 'Native peer SDK differs from the candidate SDK' }
}
$runInputs = Get-TrackDSourceIdentity -Root $pluginSourceRoot
$nativeTools = Build-TrackDNativeTools -RepoRoot $RepoRoot -WorkRoot $WorkRoot -EvidenceRoot $invocation.Directory

$EngineVersions = Expand-TrackDList $EngineVersions
$EnginePaths = Expand-TrackDList $EnginePaths

# "-EngineVersions ','" satisfies [Parameter(Mandatory)] yet expands to nothing, the per-engine
# loop then runs zero times, and the manifest would record Success=true with exit 0 - validation
# that validated nothing, presented as a pass. Refuse it before any work happens.
if ($EngineVersions.Count -eq 0)
{
    throw '-EngineVersions expanded to an empty engine list; a run that validates zero engines cannot be recorded as a pass.'
}

$explicitMap = @{}
foreach ($pair in $EnginePaths)
{
    if ([string]::IsNullOrWhiteSpace($pair)) { continue }
    $separator = $pair.IndexOf('=')
    if ($separator -lt 1) { throw "Malformed -EnginePaths entry '$pair'. Expected 'version=path'." }
    $explicitMap[$pair.Substring(0, $separator).Trim()] = $pair.Substring($separator + 1).Trim()
}

if (-not (Test-Path -LiteralPath $EvidenceRoot))
{
    New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null
}
$EvidenceRoot = (Resolve-Path -LiteralPath $EvidenceRoot).Path

$hostProjectName = Get-TrackDHostProjectName
$results = New-Object System.Collections.ArrayList
$failures = New-Object System.Collections.ArrayList

function Read-AutomationReport
{
    <#
    .SYNOPSIS
        Reads the framework's index.json into pass/fail counts and per-test detail.

    .DESCRIPTION
        The schema differs slightly across UE 5.3 to 5.8 (older reports use "state", newer ones also
        carry "errors"/"warnings" counts), so state is read defensively and any test whose state is
        not exactly Success counts as a failure. Unknown is never treated as a pass.
    #>
    param([string] $ReportDirectory)

    $indexPath = Join-Path $ReportDirectory 'index.json'
    $report = [ordered] @{
        IndexPath   = $indexPath
        Exists      = (Test-Path -LiteralPath $indexPath -PathType Leaf)
        Total       = 0
        Succeeded   = 0
        WithWarnings = 0
        Failed      = 0
        NotRun      = 0
        FailedTests = @()
        ParseError  = $null
    }

    if (-not $report.Exists) { return $report }

    try
    {
        $json = Get-Content -LiteralPath $indexPath -Raw | ConvertFrom-Json
    }
    catch
    {
        $report.ParseError = $_.Exception.Message
        return $report
    }

    $tests = @()
    if ($json.PSObject.Properties.Name -contains 'tests' -and $json.tests)
    {
        $tests = @($json.tests)
    }

    $failed = New-Object System.Collections.ArrayList
    foreach ($test in $tests)
    {
        $report.Total++

        $state = ''
        if ($test.PSObject.Properties.Name -contains 'state') { $state = [string] $test.state }

        $warnings = 0
        if ($test.PSObject.Properties.Name -contains 'warnings') { $warnings = [int] $test.warnings }

        switch ($state)
        {
            'Success'
            {
                $report.Succeeded++
                if ($warnings -gt 0) { $report.WithWarnings++ }
            }
            'NotRun'
            {
                $report.NotRun++
                [void] $failed.Add([ordered] @{
                    Name = [string] $test.fullTestPath
                    State = $state
                })
            }
            default
            {
                $report.Failed++
                [void] $failed.Add([ordered] @{
                    Name = [string] $test.fullTestPath
                    State = $state
                })
            }
        }
    }

    $report.FailedTests = $failed.ToArray()
    return $report
}

Write-TrackDSection "Automation tests - filter '$TestFilter'"
Write-TrackDStep "Repository : $RepoRoot"
Write-TrackDStep "Evidence   : $EvidenceRoot"
Write-TrackDStep "Engines    : $($EngineVersions -join ', ')"

foreach ($version in $EngineVersions)
{
    Write-TrackDSection "UE $version - automation"

    $enginePath = Resolve-TrackDEnginePath -Version $version -EngineRoot $EngineRoot -ExplicitMap $explicitMap
    $engine = Get-TrackDEngineFacts -Version $version -EnginePath $enginePath

    $row = [ordered] @{
        EngineVersion   = $version
        EnginePath      = $enginePath
        EngineBuildId   = $engine.BuildId
        EngineFound     = $engine.Found
        StagingDir      = $null
        BuildExitCode   = $null
        BuildLog        = $null
        TestExitCode    = $null
        TestLog         = $null
        ReportDirectory = $null
        Report          = $null
        MutexRetries    = 0
        BuildToolWaitSeconds = 0
        Status          = 'NotRun'
        Problems        = @()
    }

    $problems = New-Object System.Collections.ArrayList

    if (-not $engine.Found)
    {
        [void] $problems.Add("engine not installed at $enginePath")
        $row.Problems = $problems.ToArray()
        $row.Status = 'Failed'
        [void] $results.Add($row)
        [void] $failures.Add("UE $version : engine not installed at $enginePath")
        Write-TrackDBad "engine not installed at $enginePath"
        continue
    }

    if (-not $engine.EditorPresent)
    {
        [void] $problems.Add("UnrealEditor-Cmd.exe not found at $($engine.EditorCmd)")
        $row.Problems = $problems.ToArray()
        $row.Status = 'Failed'
        [void] $results.Add($row)
        [void] $failures.Add("UE $version : UnrealEditor-Cmd.exe missing")
        Write-TrackDBad "UnrealEditor-Cmd.exe not found"
        continue
    }

    # Short, per-engine staging root. Rebuilt from scratch every run: UHT writes engine-specific
    # generated code under Intermediate, and reusing another engine's tree makes one engine compile
    # the other's gen.cpp.
    $shortVersion = $version.Replace('.', '')
    $stagingDir = Join-Path $WorkRoot ("a{0}\P" -f $shortVersion)
    $row.StagingDir = $stagingDir

    Write-TrackDStep "Staging   : $stagingDir"
    $staging = New-TrackDHostStaging -RepoRoot $RepoRoot -StagingProjectDir $stagingDir -PluginSourceRoot $pluginSourceRoot -PreservePluginBuildOutput:([bool]$PluginArchive) -Force

    if ($staging.MissingItems.Count -gt 0)
    {
        [void] $problems.Add("plugin staging is missing: $($staging.MissingItems -join ', ')")
    }

    $engineEvidence = Join-Path $invocation.Directory "UE$version"
    New-Item -ItemType Directory -Path $engineEvidence -Force | Out-Null

    # --- Build the editor target ---------------------------------------------------------------
    $buildLog = Join-Path $engineEvidence 'build-editor.log'
    $row.BuildLog = $buildLog

    $buildArgs = @(
        "${hostProjectName}Editor",
        'Win64',
        'Development',
        "-Project=$($staging.ProjectFile)",
        '-waitmutex'
    )

    $attempt = 0
    $buildResult = $null
    while ($true)
    {
        $attempt++

        # Settle BEFORE building, not only after a failure. Engines are built one after another, and
        # the previous engine's editor holds the machine-global UnrealBuildTool log for a few seconds
        # after its tests report completion; a build started inside that window dies instantly, and
        # every remaining engine in the matrix dies with it. Waiting is not a retry - it prevents the
        # conflict rather than classifying one after the fact.
        $settled = Wait-TrackDBuildToolIdle -TimeoutSeconds $BuildToolWaitSeconds
        if ($settled -gt 0)
        {
            $row.BuildToolWaitSeconds += $settled
            Write-TrackDStep "waited ${settled}s for another build to release the shared UnrealBuildTool state"
        }

        Write-TrackDStep "Building editor target (attempt $attempt)"
        $buildResult = Invoke-TrackDProcess -FilePath $engine.BuildBat -ArgumentList $buildArgs `
            -StdOutPath $buildLog -TimeoutSeconds $BuildTimeoutSeconds -WorkingDirectory $WorkRoot

        if ($buildResult.ExitCode -eq 0) { break }

        # Only environmental build-tool contention is retried, and only once the conflict is gone.
        # Nothing is ever killed to make room.
        if ((Test-TrackDLogForBuildMutex $buildLog) -and $row.MutexRetries -lt $MutexRetries)
        {
            $row.MutexRetries++
            Write-TrackDWarn "UnrealBuildTool contention; waiting for the other build to finish"
            [void] (Wait-TrackDBuildToolIdle -TimeoutSeconds $BuildToolWaitSeconds -PollSeconds 10)
            continue
        }

        break
    }

    $row.BuildExitCode = $buildResult.ExitCode

    if ($buildResult.ExitCode -ne 0)
    {
        [void] $problems.Add("editor target build failed with exit code $($buildResult.ExitCode); see $buildLog")
        $row.Problems = $problems.ToArray()
        $row.Status = 'Failed'
        [void] $results.Add($row)
        [void] $failures.Add("UE $version : editor build failed")
        Write-TrackDBad "editor build failed (exit $($buildResult.ExitCode))"
        continue
    }
    Write-TrackDGood "editor target built"

    # --- Run the suite -------------------------------------------------------------------------
    $reportDir = Join-Path $engineEvidence 'report'
    New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
    $testLog = Join-Path $engineEvidence 'automation.log'
    $absLog = Join-Path $engineEvidence 'automation-engine.log'
    $row.ReportDirectory = $reportDir
    $row.TestLog = $testLog

    $testArgs = @(
        $staging.ProjectFile,
        "-ExecCmds=Automation RunTests $TestFilter;Quit",
        "-ReportExportPath=$reportDir",
        '-unattended',
        '-nopause',
        '-nosplash',
        '-NoSound',
        '-stdout',
        '-log',
        "-abslog=$absLog"
    )

    $identity = New-TrackDLaunchIdentity -Directory $engineEvidence -PluginRoot $staging.PluginDir -HostRoot $staging.ProjectDir -Executable $engine.EditorCmd
    if ($identity.SourceId -ne $runInputs.Digest) { throw 'Staged plugin differs from invocation inputs' }
    $testArgs += $TestArguments
    $testArgs += Get-TrackDIdentityArguments $identity
    $testArgs += @('-rhivalidation',"-SpoutNativePeer=$($nativeTools.Executable)",'-ini:Engine:[/Script/Engine.AutomationTestSettings]:DefaultInteractiveFramerate=1,DefaultInteractiveFramerateDuration=0.1,DefaultInteractiveFramerateWaitTime=5')
    Write-TrackDStep "Running automation suite"
    $testResult = Invoke-TrackDProcess -FilePath $engine.EditorCmd -ArgumentList $testArgs `
        -StdOutPath $testLog -TimeoutSeconds $TestTimeoutSeconds -WorkingDirectory $WorkRoot

    $row.TestExitCode = $testResult.ExitCode
    $identityCheck = Test-TrackDLaunchIdentity $identity $testResult
    $row['Identity'] = $identityCheck
    foreach ($problem in $identityCheck.Problems) { [void]$problems.Add($problem) }

    if ($testResult.TimedOut)
    {
        [void] $problems.Add("the automation run did not finish within $TestTimeoutSeconds s and was terminated")
    }

    # A passing report does not excuse a bad exit. An editor that writes an all-pass index.json and
    # then crashes during module teardown exits nonzero after the report is already complete on
    # disk. The packaged-host and performance stages fail on a bad exit code, and so does this one.
    if ($null -eq $testResult.ExitCode -or $testResult.ExitCode -ne 0)
    {
        $observedExit = if ($null -eq $testResult.ExitCode) { 'none' } else { [string] $testResult.ExitCode }
        [void] $problems.Add("the editor exited with code $observedExit; a nonzero or missing exit code fails the stage even when the test report passes")
    }

    $report = Read-AutomationReport -ReportDirectory $reportDir
    $row.Report = $report

    if (-not $report.Exists)
    {
        [void] $problems.Add("no automation report was written to $reportDir")
    }
    elseif ($report.ParseError)
    {
        [void] $problems.Add("automation report could not be parsed: $($report.ParseError)")
    }
    else
    {
        if ($report.Total -eq 0)
        {
            # An empty report means the filter matched nothing, which is not a pass - it is a suite
            # that silently stopped existing.
            [void] $problems.Add("the automation report contains no tests; filter '$TestFilter' matched nothing")
        }
        if ($report.Failed -gt 0 -or $report.NotRun -gt 0)
        {
            foreach ($failed in $report.FailedTests)
            {
                [void] $problems.Add("test $($failed.Name) reported state '$($failed.State)'")
            }
        }
    }

    $row.Problems = $problems.ToArray()
    $row.Status = if ($problems.Count -eq 0) { 'Passed' } else { 'Failed' }

    if ($row.Status -eq 'Passed')
    {
        Write-TrackDGood ("{0} tests, {1} succeeded ({2} with warnings), 0 failed" -f `
            $report.Total, $report.Succeeded, $report.WithWarnings)
    }
    else
    {
        foreach ($problem in $problems) { Write-TrackDBad $problem }
        [void] $failures.Add("UE $version : automation suite failed")
    }

    [void] $results.Add($row)
}

$manifest = [ordered] @{
    RunId        = $invocation.RunId
    ArchiveCandidate = $archiveCandidate
    SourceIdentity = $runInputs
    NativeTools = $nativeTools
    Schema       = 'spoutplugin-trackd-automation/1'
    GeneratedUtc = (Get-Date).ToUniversalTime().ToString('o')
    RepoRoot     = $RepoRoot
    TestFilter   = $TestFilter
    Git          = Get-TrackDGitInfo $RepoRoot
    Host         = Get-TrackDHostSummary
    Engines      = $results.ToArray()
    Failures     = $failures.ToArray()
    Success      = ($failures.Count -eq 0)
}

$manifestPath = Join-Path $EvidenceRoot 'automation-manifest.json'
Write-TrackDJson -InputObject $manifest -Path $manifestPath
Write-Host ''
Write-Host "Automation manifest: $manifestPath" -ForegroundColor Cyan

exit (Get-TrackDExitCode -FailureCount $failures.Count)
