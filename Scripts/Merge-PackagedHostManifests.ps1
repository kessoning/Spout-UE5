# Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
# Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

<#
.SYNOPSIS
    Rebuilds the aggregate packaged-host manifest from the per-row manifests in an evidence tree.

.DESCRIPTION
    Re-running a single failed row is normal - a machine-wide UnrealBuildTool mutex or a compiler
    that could not get memory fails a row for reasons that have nothing to do with the plugin, and
    the correct response is to re-run that row rather than the whole matrix. But a single-row re-run
    writes a single-row aggregate manifest, which then describes less than the matrix actually
    covered.

    This rebuilds the aggregate from whatever per-row manifests are present, so the evidence tree
    ends up describing every row that was run, at the state each row finished in.

    It does not invent rows. A row with no manifest is reported as missing, and the aggregate says
    so rather than quietly covering eleven rows while claiming twelve.

    -RetriedRow records that a row needed an environmental re-run, and why. That belongs in the
    manifest: a row that passed on its second attempt is not the same evidence as one that passed on
    its first, and a reader deserves to see which.

.EXAMPLE
    .\Scripts\Merge-PackagedHostManifests.ps1 -EvidenceRoot .\Artifacts\TrackD\run

.EXAMPLE
    .\Scripts\Merge-PackagedHostManifests.ps1 -EvidenceRoot .\Artifacts\TrackD\run `
        -ExpectedEngines 5.3,5.4,5.5,5.6,5.7,5.8 -ExpectedConfigurations Development,Shipping `
        -RetriedRow "UE5.6-Development=host ran out of compiler memory (C3859/C1076) on the first attempt"
#>
[CmdletBinding()]
param(
    [string] $RepoRoot = '',

    [Parameter(Mandatory = $true)]
    [string] $EvidenceRoot,

    [string[]] $ExpectedEngines = @(),
    [string[]] $ExpectedConfigurations = @('Development', 'Shipping'),

    # "RowKey=reason" pairs, recorded verbatim in the aggregate.
    [string[]] $RetriedRow = @(),
    # Explicit selection is required when an evidence tree contains retries of the same row.
    [string[]] $RowManifestPaths = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'TrackD.psm1') -Force

if ([string]::IsNullOrWhiteSpace($RepoRoot)) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
$EvidenceRoot = (Resolve-Path -LiteralPath $EvidenceRoot).Path

$ExpectedEngines = Expand-TrackDList $ExpectedEngines
$ExpectedConfigurations = Expand-TrackDList $ExpectedConfigurations
$RowManifestPaths = Expand-TrackDList $RowManifestPaths
if ($ExpectedEngines.Count -eq 0) { $ExpectedEngines = Get-TrackDDeclaredEngineVersions }

$manifestDir = Join-Path $EvidenceRoot 'manifests'
if (-not (Test-Path -LiteralPath $manifestDir -PathType Container))
{
    throw "No manifests directory at $manifestDir"
}

$rows = New-Object System.Collections.ArrayList
$inputs = @()
$producerInputs = $null
function Get-RowProducerInputs($Row) {
    if ($Row.PSObject.Properties.Name -notcontains 'ArchiveCandidate' -or -not $Row.ArchiveCandidate) { return $Row.SourceIdentity }
    $candidate = $Row.ArchiveCandidate
    if ($candidate.InstalledSourceIdentity.Digest -ne $Row.SourceIdentity.Digest -or
        $candidate.SHA256 -ne $candidate.BuildIdentity.SHA256 -or
        $candidate.BuildIdentity.Engine.BuildId -ne $Row.EngineBuildId -or
        $candidate.BuildIdentity.Engine.Version -ne $Row.EngineVersion) { throw 'Inconsistent archive installation provenance' }
    $expected = @{}
    $packagingInputs = @('SpoutPlugin.uplugin','Config/FilterPlugin.ini')
    foreach ($item in $candidate.BuildIdentity.SourceIdentity.Files) { if ($item.Path -notin $packagingInputs) { $expected[$item.Path]=$item.SHA256 } }
    foreach ($item in $Row.SourceIdentity.Files) {
        if ($item.Path -in $packagingInputs) { continue }
        if (-not $expected.ContainsKey($item.Path) -or $expected[$item.Path] -ne $item.SHA256) { throw 'Archive contains different plugin sources' }
        $expected.Remove($item.Path)
    }
    if ($expected.Count) { throw 'Archive omits plugin build sources' }
    return $candidate.BuildIdentity.SourceIdentity
}
$selected = if ($RowManifestPaths.Count) { @($RowManifestPaths | ForEach-Object { Get-Item -LiteralPath $_ }) } else { @(Get-ChildItem -LiteralPath $manifestDir -Filter 'packaged-host-UE*.json' -File | Sort-Object Name) }
foreach ($file in $selected)
{
    $row = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    if (@($rows | Where-Object { $_.RowKey -eq $row.RowKey }).Count) { throw "Duplicate row $($row.RowKey). Select one immutable attempt with -RowManifestPaths." }
    if (-not $row.SourceIdentity.Digest -or -not $row.BuildIdentity.BuildId -or
        $row.BuildIdentity.SourceIdentity.Digest -ne $row.SourceIdentity.Digest -or
        $row.BuildIdentity.SHA256 -ne $row.ExecutableSha256 -or
        $row.BuildIdentity.Engine.Version -ne $row.EngineVersion -or
        $row.BuildIdentity.Engine.BuildId -ne $row.EngineBuildId -or
        $row.BuildIdentity.Configuration -ne $row.Configuration) { throw "Missing or inconsistent build provenance: $($file.FullName)" }
    $rowProducer = Get-RowProducerInputs $row
    if ($producerInputs -and $producerInputs.Digest -ne $rowProducer.Digest) { throw 'Cannot merge rows built from different plugin sources' }
    $producerInputs = $rowProducer
    if ($rows.Count -gt 0 -and $rows[0].BuildIdentity.HostSourceIdentity.Digest -ne $row.BuildIdentity.HostSourceIdentity.Digest) { throw 'Cannot merge different host harness builds' }
    if (@($row.Launches).Count -eq 0) { throw "No launch evidence in $($row.RowKey)" }
    foreach ($launch in @($row.Launches)) {
        if (-not $launch.Identity.Passed -or $launch.Identity.Expected.SourceId -ne $row.SourceIdentity.Digest -or
            $launch.Identity.Expected.HostSourceId -ne $row.BuildIdentity.HostSourceIdentity.Digest -or
            $launch.Identity.Expected.ExecutableSHA256 -ne $row.BuildIdentity.SHA256) { throw "Invalid process identity in $($row.RowKey)" }
    }
    $inputs += [ordered]@{ Path=$file.FullName; SHA256=(Get-TrackDSha256 $file.FullName) }
    [void] $rows.Add($row)
}

$retryNotes = New-Object System.Collections.ArrayList
foreach ($pair in $RetriedRow)
{
    if ([string]::IsNullOrWhiteSpace($pair)) { continue }
    $separator = $pair.IndexOf('=')
    if ($separator -lt 1) { throw "Malformed -RetriedRow entry '$pair'. Expected 'RowKey=reason'." }
    [void] $retryNotes.Add([ordered] @{
        RowKey = $pair.Substring(0, $separator).Trim()
        Reason = $pair.Substring($separator + 1).Trim()
        Classification = 'environmental'
    })
}

$expectedRowKeys = New-Object System.Collections.ArrayList
foreach ($engine in $ExpectedEngines)
{
    foreach ($configuration in $ExpectedConfigurations)
    {
        [void] $expectedRowKeys.Add("UE$engine-$configuration")
    }
}

$presentRowKeys = @($rows | ForEach-Object { $_.RowKey })
foreach ($key in $presentRowKeys) { if ($expectedRowKeys -notcontains $key) { throw "Unexpected row: $key" } }
$missingRowKeys = @($expectedRowKeys | Where-Object { $presentRowKeys -notcontains $_ })
$failedRows = @($rows | Where-Object { $_.Status -ne 'Passed' })

$failures = New-Object System.Collections.ArrayList
foreach ($missing in $missingRowKeys)
{
    [void] $failures.Add("$missing : no per-row manifest exists; the row was never run or did not complete")
}
foreach ($failed in $failedRows)
{
    [void] $failures.Add("$($failed.RowKey) : $(@($failed.Problems).Count) problem(s)")
}

$aggregate = [ordered] @{
    Schema         = 'spoutplugin-trackd-packaged-host-matrix/1'
    GeneratedUtc   = (Get-Date).ToUniversalTime().ToString('o')
    ConsolidatedFrom = 'per-row manifests'
    RepoRoot       = $RepoRoot
    PluginName     = Get-TrackDPluginName
    SourceIdentity = $producerInputs
    SourceIdentityScope = 'Producer inputs; each row separately identifies its installed inputs and any archive packaging transformations'
    InputManifests = $inputs
    ValidationInvocationGit = Get-TrackDGitInfo $RepoRoot
    Host           = Get-TrackDHostSummary
    ExpectedEngines = $ExpectedEngines
    ExpectedConfigurations = $ExpectedConfigurations
    ExpectedRowCount = $expectedRowKeys.Count
    PresentRowCount  = $rows.Count
    MissingRows      = $missingRowKeys
    EnvironmentalRetries = $retryNotes.ToArray()
    Rows           = $rows.ToArray()
    Failures       = $failures.ToArray()
    Success        = ($failures.Count -eq 0)
}

$aggregatePath = Join-Path $manifestDir 'packaged-host-matrix.json'
Write-TrackDJson -InputObject $aggregate -Path $aggregatePath
Write-TrackDJson -InputObject $aggregate -Path (Join-Path $manifestDir ("packaged-host-merge-{0}.json" -f [Guid]::NewGuid().ToString('N')))

# The DLL-staging matrix and the lifecycle results are projections of the same rows, written to
# their own files because they are tracked as separate required outcomes. A single-row re-run
# overwrites both with one row, so they are rebuilt here for the same reason the aggregate is.
$dllMatrix = New-Object System.Collections.ArrayList
$lifecycleRows = New-Object System.Collections.ArrayList

foreach ($row in $rows)
{
    $reported = $null
    $verified = $false
    if ($row.PSObject.Properties.Name -contains 'Launches' -and @($row.Launches).Count -gt 0)
    {
        $first = @($row.Launches)[0]
        $reported = $first.ReportedDllPath
        $verified = [bool] $first.DllPathMatchesPackagedCopy
    }

    [void] $dllMatrix.Add([ordered] @{
        Layout                   = "$($row.Configuration) packaged host"
        EngineVersion            = $row.EngineVersion
        Configuration            = $row.Configuration
        ExpectedPath             = $row.ExpectedSpoutDllPath
        ActualPath               = $row.ActualSpoutDllPath
        SizeBytes                = $row.SpoutDllSizeBytes
        Sha256                   = $row.SpoutDllSha256
        MatchesRepositoryCopy    = $row.SpoutDllMatchesRepository
        StagedAsNonUfs           = $row.SpoutDllStagedAsNonUfs
        LoadedPathReportedByHost = $reported
        LoadedFromExpectedPath   = $verified
        DuplicateCopies          = $row.DuplicateSpoutDlls
    })

    foreach ($launch in @($row.Launches))
    {
        [void] $lifecycleRows.Add([ordered] @{
            EngineVersion    = $row.EngineVersion
            Configuration    = $row.Configuration
            Attempt          = $launch.Attempt
            SameNameRestart  = ($launch.ObservedMarkers -contains 'SPOUT_SAME_NAME_RESTART_PASS')
            LevelTravel      = ($launch.ObservedMarkers -contains 'SPOUT_LEVEL_TRAVEL_PASS')
            NoStaleSession   = ($launch.ObservedMarkers -contains 'SPOUT_NO_STALE_SESSION_PASS')
            ColorDecode      = ($launch.ObservedMarkers -contains 'SPOUT_COLOR_DECODE_PASS')
            ViewportSender   = ($launch.ObservedMarkers -contains 'SPOUT_VIEWPORT_SENDER_PASS')
            ShutdownComplete = ($launch.ObservedMarkers -contains 'SPOUT_SHUTDOWN_COMPLETE')
            ExitCode         = $launch.ExitCode
            TimedOut         = $launch.TimedOut
            Passed           = $launch.Passed
        })
    }
}

Write-TrackDJson -InputObject ([ordered] @{
    Schema       = 'spoutplugin-trackd-dll-matrix/1'
    GeneratedUtc = (Get-Date).ToUniversalTime().ToString('o')
    ConsolidatedFrom = 'per-row manifests'
    RepositorySpoutDll = Get-TrackDFileFacts (Join-Path $RepoRoot 'ThirdParty\Spout\lib\amd64\Spout.dll')
    Rows         = $dllMatrix.ToArray()
}) -Path (Join-Path $EvidenceRoot 'hashes\dll-staging-matrix.json')

Write-TrackDJson -InputObject ([ordered] @{
    Schema       = 'spoutplugin-trackd-lifecycle/1'
    GeneratedUtc = (Get-Date).ToUniversalTime().ToString('o')
    ConsolidatedFrom = 'per-row manifests'
    LaunchCount  = $lifecycleRows.Count
    Rows         = $lifecycleRows.ToArray()
}) -Path (Join-Path $EvidenceRoot 'lifecycle\lifecycle-results.json')

Write-TrackDSection 'Consolidated packaged-host matrix'
foreach ($row in $rows)
{
    $retried = @($retryNotes | Where-Object { $_.RowKey -eq $row.RowKey })
    $suffix = if ($retried.Count -gt 0) { "  (environmental retry: $($retried[0].Reason))" } else { '' }
    if ($row.Status -eq 'Passed') { Write-TrackDGood ("{0,-22} {1}{2}" -f $row.RowKey, $row.Status, $suffix) }
    else { Write-TrackDBad ("{0,-22} {1}{2}" -f $row.RowKey, $row.Status, $suffix) }
}
foreach ($missing in $missingRowKeys) { Write-TrackDBad "$missing : missing" }

Write-Host ''
Write-Host "Rows: $($rows.Count) present of $($expectedRowKeys.Count) expected" -ForegroundColor Cyan
Write-Host "Aggregate manifest: $aggregatePath" -ForegroundColor Cyan

exit (Get-TrackDExitCode -FailureCount $failures.Count -SkippedRequiredCount $missingRowKeys.Count)
