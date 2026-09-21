# Copyright (c) 2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
# Regression checks for evidence isolation and artifact provenance; no Unreal installation needed.
[CmdletBinding()]
param([string]$EvidenceRoot = (Join-Path (Split-Path $PSScriptRoot) 'Artifacts/P1/identity-tests'))
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'TrackD.psm1') -Force
$run=New-TrackDEvidenceRun -Parent $EvidenceRoot -Label 'regression'
$checks=@()
function Assert-Case([string]$Name,[bool]$Condition) {
    if (-not $Condition) { throw "FAILED: $Name" }
    $script:checks += $Name
}
$plugin=Join-Path $run.Directory 'plugin'
$hostRoot=Join-Path $run.Directory 'host'
New-Item -ItemType Directory -Path $plugin,$hostRoot | Out-Null
Set-Content -LiteralPath (Join-Path $plugin 'SpoutPlugin.uplugin') -Value '{"VersionName":"fixture"}'
Set-Content -LiteralPath (Join-Path $hostRoot 'SpoutHostTest.uproject') -Value '{}'
$source=Get-TrackDSourceIdentity $plugin
$hostInputs=Get-TrackDSourceIdentity $hostRoot -Kind Host
$binary=Join-Path $run.Directory 'fixture.bin'
Set-Content -LiteralPath $binary -Value 'immutable build bytes'
$artifact=New-TrackDArtifactIdentity -Artifact $binary -SourceIdentity $source -HostSourceIdentity $hostInputs -Engine @{Version='5.8'} -Configuration Development -Git @{Commit='original-build-commit'}
$sidecarBefore=Get-TrackDSha256 "$binary.identity.json"
Set-Content -LiteralPath (Join-Path $plugin 'SpoutPlugin.uplugin') -Value '{"VersionName":"changed checkout"}'
$read=Read-TrackDArtifactIdentity $binary
Assert-Case 'Validate-existing preserves original build commit and inputs' ($read.BuildGit.Commit -eq 'original-build-commit' -and $read.SourceIdentity.Digest -eq $source.Digest -and (Get-TrackDSha256 "$binary.identity.json") -eq $sidecarBefore)
$identity=New-TrackDLaunchIdentity -Directory $run.Directory -Executable $binary -ArtifactIdentity $artifact
$emptyResult=@{ProcessId=42}
Assert-Case 'Empty child cannot reuse an earlier result' (-not (Test-TrackDLaunchIdentity $identity $emptyResult).Passed)
$observed=@{runId='old-run';sourceId=$identity.SourceId;hostSourceId=$identity.HostSourceId;processId=42;executable=$binary;matched=$true}
Write-TrackDJson $observed $identity.AttestationPath
Assert-Case 'Seeded stale attestation is rejected' (-not (Test-TrackDLaunchIdentity $identity $emptyResult).Passed)
$observed.runId=$identity.RunId; $observed.processId=41
Write-TrackDJson $observed $identity.AttestationPath
Assert-Case 'Different child PID is rejected' (-not (Test-TrackDLaunchIdentity $identity $emptyResult).Passed)
$observed.processId=42
Write-TrackDJson $observed $identity.AttestationPath
Assert-Case 'Matching current binary and process accepted' ((Test-TrackDLaunchIdentity $identity $emptyResult).Passed)
$next=New-TrackDLaunchIdentity -Directory $run.Directory -Executable $binary -ArtifactIdentity $artifact
Assert-Case 'Every invocation gets a distinct empty output directory' ($next.RunId -ne $identity.RunId -and -not (Test-Path -LiteralPath $next.AttestationPath))
$context=New-TrackDRunContext -RepoRoot (Split-Path $PSScriptRoot) -RunId fixed-fixture -ArtifactsRoot $run.Directory
$rejected=$false
try { New-TrackDRunContext -RepoRoot (Split-Path $PSScriptRoot) -RunId fixed-fixture -ArtifactsRoot $run.Directory | Out-Null } catch { $rejected=$true }
Assert-Case 'Explicit repeated pipeline RunId cannot reuse old stage evidence' $rejected
Add-Content -LiteralPath $binary -Value 'tampered'
Assert-Case 'Changed executable rejected after launch' (-not (Test-TrackDLaunchIdentity $identity $emptyResult).Passed)
$rejected=$false
try { Read-TrackDArtifactIdentity $binary | Out-Null } catch { $rejected=$true }
Assert-Case 'Changed artifact cannot retain old provenance' $rejected
$missing=Join-Path $run.Directory 'legacy.bin'; Set-Content -LiteralPath $missing -Value 'old bytes'
$rejected=$false
try { Read-TrackDArtifactIdentity $missing | Out-Null } catch { $rejected=$true }
Assert-Case 'Legacy artifact cannot be attributed to current checkout' $rejected
$package=Join-Path $run.Directory 'package'
New-Item -ItemType Directory -Path (Join-Path $package 'bin') | Out-Null
$packageExe=Join-Path $package 'bin/app.bin'
Set-Content -LiteralPath $packageExe -Value 'executable fixture'
Set-Content -LiteralPath (Join-Path $package 'data.pak') -Value 'packaged data fixture'
New-TrackDArtifactIdentity -Artifact $packageExe -SourceIdentity $source -Engine @{Version='5.8'} -CompanionRoot $package | Out-Null
$relocated=Join-Path $run.Directory 'relocated'
Copy-Item -LiteralPath $package -Destination $relocated -Recurse
$relocatedExe=Join-Path $relocated 'bin/app.bin'
$relocatedIdentity=Read-TrackDArtifactIdentity $relocatedExe
Assert-Case 'Relocated package retains original provenance' ($relocatedIdentity.CompanionRoot -eq $package)
Add-Content -LiteralPath (Join-Path $relocated 'data.pak') -Value 'changed payload'
$rejected=$false
try { Read-TrackDArtifactIdentity $relocatedExe | Out-Null } catch { $rejected=$true }
Assert-Case 'Changed packaged payload is rejected even with unchanged executable' $rejected
$manifestDir=Join-Path $run.Directory 'manifests'
New-Item -ItemType Directory -Path $manifestDir | Out-Null
$row=@{RowKey='UE5.8-Development';SourceIdentity=$source;BuildIdentity=@{BuildId='synthetic-negative-fixture';SourceIdentity=$source;HostSourceIdentity=$hostInputs;SHA256='fixture-sha';Engine=@{Version='5.8';BuildId='fixture-engine'};Configuration='Development'};
    EngineVersion='5.8';EngineBuildId='fixture-engine';Configuration='Development';ExecutableSha256='fixture-sha';
    Launches=@(@{Identity=@{Passed=$true;Expected=@{SourceId=$source.Digest;HostSourceId=$hostInputs.Digest;ExecutableSHA256='fixture-sha'}}})}
$first=Join-Path $manifestDir 'first.json'; $second=Join-Path $manifestDir 'second.json'
Write-TrackDJson $row $first
foreach($case in @('duplicate','mixed')) {
    $other=$row | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    if($case -eq 'mixed') { $other.RowKey='UE5.8-Shipping';$other.Configuration='Shipping';$other.BuildIdentity.Configuration='Shipping';$other.SourceIdentity.Digest='different-source';$other.BuildIdentity.SourceIdentity.Digest='different-source';$other.Launches[0].Identity.Expected.SourceId='different-source' }
    Write-TrackDJson $other $second
    $result=Invoke-TrackDProcess -FilePath (Get-Process -Id $PID).Path -ArgumentList @('-NoProfile','-File',(Join-Path $PSScriptRoot 'Merge-PackagedHostManifests.ps1'),'-EvidenceRoot',$run.Directory,'-ExpectedEngines','5.8','-RowManifestPaths',("$first,$second")) -StdOutPath (Join-Path $run.Directory "$case.log") -TimeoutSeconds 30 -WorkingDirectory (Split-Path $PSScriptRoot)
    $text=(Get-Content $result.StdOutPath -Raw)+(Get-Content $result.StdErrPath -Raw)
    $reason=if($case -eq 'duplicate'){'Duplicate row'}else{'different plugin sources'}
    Assert-Case "$case merge fixture rejected for its provenance defect" ($result.ExitCode -ne 0 -and $text -match $reason)
}
Write-TrackDJson @{Passed=$true;Checks=$checks} (Join-Path $run.Directory 'results.json')
Write-Host "$($checks.Count) evidence identity regression checks passed: $($run.Directory)"
