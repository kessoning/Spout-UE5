# Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
# Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

<#
.SYNOPSIS
    Single entry point for the release validation pipeline.

.DESCRIPTION
    Runs the validation stages cheapest-first, so a
    documentation typo fails in seconds instead of after an hour of packaging - and writes one
    aggregate manifest describing every stage that ran, every stage that was skipped, and why.

    Stage order:

        1. Script self-test and manifest-schema check
        2. Documentation validation
        3. Automation suite on the engine floors
        4. BuildPlugin matrix                       (-BoundaryOnly / -FullMatrix)
        5. Packaged-host matrix                     (-BoundaryOnly / -FullMatrix)
        6. Performance baselines                    (-Performance)
        7. Clean-source-copy reproducibility        (-CleanSource)

    A skipped stage is never reported as a pass. The aggregate manifest records each stage as
    Passed, Failed or Skipped with its reason, and the exit code is non-zero if ANY stage failed.

    Modes:
        (none)         fast: stages 1-3 on the two engine floors only
        -BoundaryOnly  stages 1-5 on UE 5.3 and UE 5.8, both configurations
        -FullMatrix    stages 1-5 on the whole declared matrix
        -Performance   adds stage 6
        -CleanSource   adds stage 7
        -All           everything

.EXAMPLE
    .\Scripts\Invoke-TrackD.ps1

.EXAMPLE
    .\Scripts\Invoke-TrackD.ps1 -FullMatrix

.EXAMPLE
    .\Scripts\Invoke-TrackD.ps1 -All -RunId release-check
#>
[CmdletBinding()]
param(
    [string] $RepoRoot = '',

    [switch] $BoundaryOnly,
    [switch] $FullMatrix,
    [switch] $Performance,
    [switch] $CleanSource,
    [switch] $All,

    [string] $EngineRoot = 'D:\UE',
    [string[]] $EnginePaths = @(),

    [string] $RunId = '',
    [string] $ArtifactsRoot = '',
    [string] $WorkRoot = "$env:TEMP\SpoutD",

    # Where build.ps1 writes release archives during the BuildPlugin matrix stage.
    [string] $ReleaseOutputRoot = '',

    [int] $LaunchRepeats = 3,
    [int] $PerformanceSeconds = 60,

    # How long any stage waits for ANOTHER build to release UnrealBuildTool's machine-global state
    # before attempting anyway. An unrelated Unreal build on the same machine can hold that state
    # for many minutes; waiting costs nothing when nothing else is building.
    [int] $BuildToolWaitSeconds = 1800,

    [switch] $Trace
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

Import-Module (Join-Path $PSScriptRoot 'TrackD.psm1') -Force

if ([string]::IsNullOrWhiteSpace($RepoRoot)) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

$EnginePaths = Expand-TrackDList $EnginePaths

if ($All)
{
    $FullMatrix = $true
    $Performance = $true
    $CleanSource = $true
}

$declaredEngines = Get-TrackDDeclaredEngineVersions
$boundaryEngines = @($declaredEngines | Select-Object -First 1) + @($declaredEngines | Select-Object -Last 1)

$context = New-TrackDRunContext -RepoRoot $RepoRoot -RunId $RunId -ArtifactsRoot $ArtifactsRoot
if ([string]::IsNullOrWhiteSpace($ReleaseOutputRoot))
{
    $ReleaseOutputRoot = Join-Path $WorkRoot ('rel\{0}' -f $context.RunId)
}

$pluginVersion = Get-TrackDPluginVersion $RepoRoot

Write-TrackDSection "Track D pipeline - SpoutPlugin $pluginVersion"
Write-TrackDStep "Run id     : $($context.RunId)"
Write-TrackDStep "Evidence   : $($context.RunRoot)"
Write-TrackDStep "Mode       : $(if ($All) { 'All' } elseif ($FullMatrix) { 'FullMatrix' } elseif ($BoundaryOnly) { 'BoundaryOnly' } else { 'Fast' })"

$stages = New-Object System.Collections.ArrayList

function Invoke-Stage
{
    <#
    .SYNOPSIS
        Runs one stage script and records its outcome.

    .DESCRIPTION
        -Skip records the stage as Skipped WITH a reason rather than omitting it. A stage missing
        from the manifest and a stage that was deliberately not run look identical to a reader
        otherwise, and that is precisely how a partial validation gets reported as a complete one.
    #>
    param(
        [string] $Name,
        [string] $ScriptPath,
        [string[]] $Arguments = @(),
        [switch] $Skip,
        [string] $SkipReason = '',
        [int] $TimeoutSeconds = 21600
    )

    if ($Skip)
    {
        Write-TrackDWarn "SKIPPED $Name - $SkipReason"
        [void] $stages.Add([ordered] @{
            Name = $Name; Status = 'Skipped'; Reason = $SkipReason
            ExitCode = $null; DurationSeconds = $null; LogPath = $null
        })
        return $true
    }

    Write-TrackDSection "Stage: $Name"

    # Stages run back to back, and the one that just finished may still be releasing the
    # machine-global UnrealBuildTool log and mutex - an editor shutting down after the automation
    # suite is enough. A build started inside that window dies before it reads its own arguments,
    # which is how a healthy matrix produces a wall of instant failures. Wait it out first; never
    # kill anything to make room.
    $settled = Wait-TrackDBuildToolIdle -TimeoutSeconds $BuildToolWaitSeconds
    if ($settled -gt 0)
    {
        Write-TrackDStep "waited ${settled}s for another build to release the shared UnrealBuildTool state"
    }

    $logPath = Join-Path $context.RunRoot ("stage-{0}.log" -f ($Name -replace '[^A-Za-z0-9]', '-'))

    $psArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $ScriptPath) + $Arguments
    $result = Invoke-TrackDProcess -FilePath 'pwsh' -ArgumentList $psArgs `
        -StdOutPath $logPath -TimeoutSeconds $TimeoutSeconds -WorkingDirectory $RepoRoot

    # Echo the stage's own output so a single console log tells the whole story.
    if (Test-Path -LiteralPath $logPath -PathType Leaf)
    {
        Get-Content -LiteralPath $logPath | ForEach-Object { Write-Host $_ }
    }

    $passed = ($result.ExitCode -eq 0) -and (-not $result.TimedOut)

    [void] $stages.Add([ordered] @{
        Name            = $Name
        Status          = if ($passed) { 'Passed' } else { 'Failed' }
        Reason          = if ($result.TimedOut) { "timed out after $TimeoutSeconds s" } elseif (-not $passed) { "exit code $($result.ExitCode)" } else { $null }
        ExitCode        = $result.ExitCode
        DurationSeconds = $result.DurationSeconds
        LogPath         = $logPath
        CommandLine     = $result.CommandLine
    })

    if ($passed) { Write-TrackDGood "$Name passed" } else { Write-TrackDBad "$Name FAILED" }
    return $passed
}

# =================================================================================================
# Stage 1 - script self-test and manifest schema
# =================================================================================================

Write-TrackDSection 'Stage: Self-test'

$selfTestProblems = New-Object System.Collections.ArrayList

# Every script the pipeline dispatches to must exist and parse. A syntax error found here costs a
# second; found at stage 5 it costs an hour of packaging first.
$pipelineScripts = @(
    'TrackD.psm1',
    'Invoke-AutomationTests.ps1',
    'Invoke-PackagedHostMatrix.ps1',
    'Invoke-PerformanceBaselines.ps1',
    'Validate-Documentation.ps1',
    'New-CleanSourceCopy.ps1',
    'Invoke-TrackD.ps1'
)
foreach ($script in $pipelineScripts)
{
    $path = Join-Path $PSScriptRoot $script
    if (-not (Test-Path -LiteralPath $path -PathType Leaf))
    {
        [void] $selfTestProblems.Add("pipeline script missing: $script")
        continue
    }

    $parseErrors = $null
    $tokens = $null
    [void] [System.Management.Automation.Language.Parser]::ParseFile($path, [ref] $tokens, [ref] $parseErrors)
    if ($parseErrors -and $parseErrors.Count -gt 0)
    {
        [void] $selfTestProblems.Add("$script has $($parseErrors.Count) parse error(s): $($parseErrors[0].Message)")
    }
}

# The release source manifest must load and declare the fields the clean-source stage reads.
$sourceManifestPath = Join-Path $PSScriptRoot 'ReleaseSources.psd1'
if (-not (Test-Path -LiteralPath $sourceManifestPath -PathType Leaf))
{
    [void] $selfTestProblems.Add('Scripts/ReleaseSources.psd1 is missing')
}
else
{
    try
    {
        $sourceManifest = Import-PowerShellDataFile -LiteralPath $sourceManifestPath
        foreach ($key in @('SchemaVersion', 'Required', 'Validation', 'Excluded'))
        {
            if (-not $sourceManifest.ContainsKey($key))
            {
                [void] $selfTestProblems.Add("ReleaseSources.psd1 has no '$key' section")
            }
        }
        foreach ($entry in $sourceManifest.Required)
        {
            foreach ($field in @('Path', 'Category', 'Note'))
            {
                if (-not $entry.ContainsKey($field))
                {
                    [void] $selfTestProblems.Add("a Required entry is missing '$field'")
                    break
                }
            }
        }
    }
    catch
    {
        [void] $selfTestProblems.Add("ReleaseSources.psd1 could not be loaded: $($_.Exception.Message)")
    }

    # The rules that decide what counts as a release input, proved on a synthetic tree before an
    # hour of packaging depends on them. Offline and deterministic: it asserts on the rules, not on
    # whatever happens to be sitting in the working folder today.
    foreach ($filterProblem in (Test-TrackDReleaseSourceFiltering -ManifestPath $sourceManifestPath))
    {
        [void] $selfTestProblems.Add("release-source filtering: $filterProblem")
    }
}

# The classifiers that decide whether a failed build was the machine's fault or the plugin's. A
# classifier that silently answers "no" to everything looks exactly like a working one until a real
# conflict arrives, so both the positive and negative cases are proved here on recorded log text.
foreach ($contentionProblem in (Test-TrackDBuildContentionClassifier))
{
    [void] $selfTestProblems.Add("build contention: $contentionProblem")
}

# build.ps1 and build.bat are release inputs; their absence is a release failure, not a warning.
foreach ($required in @('build.ps1', 'build.bat', 'SpoutPlugin.uplugin', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md'))
{
    if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot $required) -PathType Leaf))
    {
        [void] $selfTestProblems.Add("required repository file missing: $required")
    }
}

# The host test project must be present, or stages 5 and 6 have nothing to package.
$hostProject = Join-Path $RepoRoot ('Tests\{0}\{0}.uproject' -f (Get-TrackDHostProjectName))
if (-not (Test-Path -LiteralPath $hostProject -PathType Leaf))
{
    [void] $selfTestProblems.Add("host test project missing: $hostProject")
}

# A round trip through the manifest writer, so a malformed manifest is caught before a six-engine
# run tries to write one.
$schemaProbePath = Join-Path $context.Manifests 'self-test-schema-probe.json'
try
{
    Write-TrackDJson -InputObject ([ordered] @{ Schema = 'spoutplugin-trackd-selftest/1'; Value = 1 }) -Path $schemaProbePath
    $roundTrip = Get-Content -LiteralPath $schemaProbePath -Raw | ConvertFrom-Json
    if ($roundTrip.Schema -ne 'spoutplugin-trackd-selftest/1')
    {
        [void] $selfTestProblems.Add('manifest round trip did not preserve the schema field')
    }
}
catch
{
    [void] $selfTestProblems.Add("manifest writer failed: $($_.Exception.Message)")
}

foreach ($problem in $selfTestProblems) { Write-TrackDBad $problem }
if ($selfTestProblems.Count -eq 0) { Write-TrackDGood "self-test passed ($($pipelineScripts.Count) scripts parsed, manifest schema valid, release-source filtering proved)" }

[void] $stages.Add([ordered] @{
    Name = 'Self-test'
    Status = if ($selfTestProblems.Count -eq 0) { 'Passed' } else { 'Failed' }
    Reason = if ($selfTestProblems.Count -gt 0) { $selfTestProblems -join '; ' } else { $null }
    ExitCode = $selfTestProblems.Count
    DurationSeconds = $null
    LogPath = $null
})

# =================================================================================================
# Stage 2 - documentation
# =================================================================================================

[void] (Invoke-Stage -Name 'Documentation' -ScriptPath (Join-Path $PSScriptRoot 'Validate-Documentation.ps1') `
    -Arguments @('-RepoRoot', $RepoRoot, '-ReportPath', (Join-Path $context.Documentation 'documentation-report.json')) `
    -TimeoutSeconds 600)

# =================================================================================================
# Stage 3 - automation suite
# =================================================================================================

$automationEngines = if ($FullMatrix) { $declaredEngines } else { $boundaryEngines }

$automationArgs = @(
    '-RepoRoot', $RepoRoot,
    '-EngineVersions', ($automationEngines -join ','),
    '-EvidenceRoot', $context.Automation,
    '-EngineRoot', $EngineRoot,
    '-WorkRoot', $WorkRoot
)
foreach ($pair in $EnginePaths) { $automationArgs += @('-EnginePaths', $pair) }

[void] (Invoke-Stage -Name 'Automation' -ScriptPath (Join-Path $PSScriptRoot 'Invoke-AutomationTests.ps1') `
    -Arguments $automationArgs -TimeoutSeconds 14400)

# =================================================================================================
# Stage 4 - BuildPlugin matrix
# =================================================================================================

$runPackaging = $BoundaryOnly -or $FullMatrix
$packagingEngines = if ($FullMatrix) { $declaredEngines } else { $boundaryEngines }

$buildPluginArgs = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $RepoRoot 'build.ps1'),
    '-PluginRoot', $RepoRoot,
    '-OutputRoot', $ReleaseOutputRoot,
    '-EngineRoot', $EngineRoot
)
if ($FullMatrix)
{
    $buildPluginArgs += '-FullMatrix'
}
else
{
    $buildPluginArgs += @('-Partial', '-EngineVersions', ($packagingEngines -join ','))
}
foreach ($pair in $EnginePaths) { $buildPluginArgs += @('-EnginePaths', $pair) }

if ($runPackaging)
{
    Write-TrackDSection 'Stage: BuildPlugin matrix'
    $buildPluginLog = Join-Path $context.BuildLogs 'buildplugin-matrix.log'
    $buildPluginResult = Invoke-TrackDProcess -FilePath 'pwsh' -ArgumentList $buildPluginArgs `
        -StdOutPath $buildPluginLog -TimeoutSeconds 21600 -WorkingDirectory $RepoRoot

    if (Test-Path -LiteralPath $buildPluginLog -PathType Leaf)
    {
        Get-Content -LiteralPath $buildPluginLog | Select-Object -Last 60 | ForEach-Object { Write-Host $_ }
    }

    # The archives and their manifests are the release evidence; copy them out of the temp output
    # root so they survive the next run.
    $archiveEvidence = Join-Path $context.ArchiveValidation 'release-manifests'
    New-Item -ItemType Directory -Path $archiveEvidence -Force | Out-Null
    foreach ($generated in (Get-ChildItem -LiteralPath $ReleaseOutputRoot -Filter 'release-manifest-*.json' -File -Recurse -ErrorAction SilentlyContinue))
    {
        Copy-Item -LiteralPath $generated.FullName -Destination (Join-Path $archiveEvidence $generated.Name) -Force
    }

    $archiveHashes = @(Get-ChildItem -LiteralPath $ReleaseOutputRoot -Filter '*.zip' -File -Recurse -ErrorAction SilentlyContinue |
        ForEach-Object {
            [ordered] @{
                Path      = $_.FullName
                Name      = $_.Name
                SizeBytes = $_.Length
                Sha256    = Get-TrackDSha256 $_.FullName
            }
        })
    Write-TrackDJson -InputObject ([ordered] @{
        Schema       = 'spoutplugin-trackd-archive-hashes/1'
        GeneratedUtc = (Get-Date).ToUniversalTime().ToString('o')
        OutputRoot   = $ReleaseOutputRoot
        Archives     = $archiveHashes
    }) -Path (Join-Path $context.Hashes 'release-archive-hashes.json')

    $buildPluginPassed = ($buildPluginResult.ExitCode -eq 0) -and (-not $buildPluginResult.TimedOut)
    [void] $stages.Add([ordered] @{
        Name = 'BuildPlugin matrix'
        Status = if ($buildPluginPassed) { 'Passed' } else { 'Failed' }
        Reason = if ($buildPluginResult.TimedOut) { 'timed out' } elseif (-not $buildPluginPassed) { "exit code $($buildPluginResult.ExitCode)" } else { $null }
        ExitCode = $buildPluginResult.ExitCode
        DurationSeconds = $buildPluginResult.DurationSeconds
        LogPath = $buildPluginLog
        CommandLine = $buildPluginResult.CommandLine
        ArchiveCount = $archiveHashes.Count
    })

    if ($buildPluginPassed) { Write-TrackDGood "BuildPlugin matrix passed ($($archiveHashes.Count) archive(s))" }
    else { Write-TrackDBad 'BuildPlugin matrix FAILED' }
}
else
{
    [void] (Invoke-Stage -Name 'BuildPlugin matrix' -ScriptPath '' -Skip `
        -SkipReason 'not requested; pass -BoundaryOnly or -FullMatrix')
}

# =================================================================================================
# Stage 5 - packaged-host matrix
# =================================================================================================

$packagedHostArgs = @(
    '-RepoRoot', $RepoRoot,
    '-EngineVersions', ($packagingEngines -join ','),
    '-Configurations', 'Development,Shipping',
    '-EvidenceRoot', $context.RunRoot,
    '-EngineRoot', $EngineRoot,
    '-WorkRoot', $WorkRoot,
    '-LaunchRepeats', $LaunchRepeats
)
foreach ($pair in $EnginePaths) { $packagedHostArgs += @('-EnginePaths', $pair) }

[void] (Invoke-Stage -Name 'Packaged-host matrix' -ScriptPath (Join-Path $PSScriptRoot 'Invoke-PackagedHostMatrix.ps1') `
    -Arguments $packagedHostArgs -Skip:(-not $runPackaging) `
    -SkipReason 'not requested; pass -BoundaryOnly or -FullMatrix' -TimeoutSeconds 36000)

# =================================================================================================
# Stage 6 - performance baselines
# =================================================================================================

$performanceArgs = @(
    '-RepoRoot', $RepoRoot,
    '-EngineVersion', ($declaredEngines | Select-Object -Last 1),
    '-EvidenceRoot', $context.RunRoot,
    '-EngineRoot', $EngineRoot,
    '-WorkRoot', $WorkRoot,
    '-Seconds', $PerformanceSeconds
)
if ($Trace) { $performanceArgs += '-Trace' }
foreach ($pair in $EnginePaths) { $performanceArgs += @('-EnginePaths', $pair) }

[void] (Invoke-Stage -Name 'Performance baselines' -ScriptPath (Join-Path $PSScriptRoot 'Invoke-PerformanceBaselines.ps1') `
    -Arguments $performanceArgs -Skip:(-not $Performance) `
    -SkipReason 'not requested; pass -Performance' -TimeoutSeconds 21600)

# =================================================================================================
# Stage 7 - clean-source copy
# =================================================================================================

$cleanSourceArgs = @(
    '-RepoRoot', $RepoRoot,
    '-EvidenceRoot', $context.RunRoot,
    '-EngineVersions', ($packagingEngines -join ','),
    '-EngineRoot', $EngineRoot,
    '-WorkRoot', $WorkRoot
)
foreach ($pair in $EnginePaths) { $cleanSourceArgs += @('-EnginePaths', $pair) }

[void] (Invoke-Stage -Name 'Clean-source copy' -ScriptPath (Join-Path $PSScriptRoot 'New-CleanSourceCopy.ps1') `
    -Arguments $cleanSourceArgs -Skip:(-not $CleanSource) `
    -SkipReason 'not requested; pass -CleanSource' -TimeoutSeconds 21600)

# =================================================================================================
# Aggregate manifest
# =================================================================================================

$failedStages = @($stages | Where-Object { $_.Status -eq 'Failed' })
$skippedStages = @($stages | Where-Object { $_.Status -eq 'Skipped' })

$aggregate = [ordered] @{
    Schema        = 'spoutplugin-trackd-run/1'
    GeneratedUtc  = (Get-Date).ToUniversalTime().ToString('o')
    RunId         = $context.RunId
    RepoRoot      = $RepoRoot
    EvidenceRoot  = $context.RunRoot
    PluginName    = Get-TrackDPluginName
    PluginVersion = $pluginVersion
    Mode          = if ($All) { 'All' } elseif ($FullMatrix) { 'FullMatrix' } elseif ($BoundaryOnly) { 'BoundaryOnly' } else { 'Fast' }
    DeclaredEngines = $declaredEngines
    EnginesExercised = $packagingEngines
    Git           = Get-TrackDGitInfo $RepoRoot
    Host          = Get-TrackDHostSummary
    ReleaseOutputRoot = $ReleaseOutputRoot
    Stages        = $stages.ToArray()
    FailedStages  = @($failedStages | ForEach-Object { $_.Name })
    SkippedStages = @($skippedStages | ForEach-Object { $_.Name })
    Success       = ($failedStages.Count -eq 0)
}

$aggregatePath = Join-Path $context.Manifests 'trackd-run.json'
Write-TrackDJson -InputObject $aggregate -Path $aggregatePath

Write-TrackDSection 'Track D summary'
foreach ($stage in $stages)
{
    $colour = switch ($stage.Status)
    {
        'Passed'  { 'Green' }
        'Skipped' { 'Yellow' }
        default   { 'Red' }
    }
    $suffix = if ($stage.Reason) { " - $($stage.Reason)" } else { '' }
    Write-Host ("  {0,-24} {1}{2}" -f $stage.Name, $stage.Status, $suffix) -ForegroundColor $colour
}

Write-Host ''
Write-Host "Aggregate manifest: $aggregatePath" -ForegroundColor Cyan
Write-Host "Evidence          : $($context.RunRoot)" -ForegroundColor Cyan
Write-Host ''

if ($failedStages.Count -eq 0)
{
    Write-Host ('=' * 78) -ForegroundColor Green
    if ($skippedStages.Count -gt 0)
    {
        # Never a bare success banner when something was not run.
        Write-Host "TRACK D STAGES PASSED - $($skippedStages.Count) stage(s) were NOT run: $(($skippedStages | ForEach-Object { $_.Name }) -join ', ')" -ForegroundColor Green
    }
    else
    {
        Write-Host 'TRACK D - EVERY STAGE PASSED' -ForegroundColor Green
    }
    Write-Host ('=' * 78) -ForegroundColor Green
}
else
{
    Write-Host ('=' * 78) -ForegroundColor Red
    Write-Host "TRACK D FAILED - $($failedStages.Count) stage(s): $(($failedStages | ForEach-Object { $_.Name }) -join ', ')" -ForegroundColor Red
    Write-Host ('=' * 78) -ForegroundColor Red
}

exit (Get-TrackDExitCode -FailureCount $failedStages.Count)
