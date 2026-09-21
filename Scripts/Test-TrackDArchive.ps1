#requires -Version 7.2
# Copyright (c) 2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
[CmdletBinding()]
param([string]$EvidenceRoot = (Join-Path (Split-Path $PSScriptRoot) 'Artifacts/P2/archive-tests'))
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'TrackD.psm1') -Force
$run=New-TrackDEvidenceRun -Parent $EvidenceRoot -Label regression
$producer=Join-Path $run.Directory 'producer'
New-Item -ItemType Directory -Path "$producer/Source","$producer/Config" | Out-Null
Set-Content "$producer/SpoutPlugin.uplugin" '{"VersionName":"fixture","EnabledByDefault":true}'
Set-Content "$producer/Source/fixture.cpp" 'original source'
Set-Content "$producer/Config/FilterPlugin.ini" '[FilterPlugin]'
$inputs=Get-TrackDSourceIdentity $producer
$checks=@()
foreach ($case in @('valid','changed-source','missing-source','unexpected-source','traversal','tampered-zip')) {
    $archive=Join-Path $run.Directory "$case.zip"
    $zip=[IO.Compression.ZipFile]::Open($archive,[IO.Compression.ZipArchiveMode]::Create)
    try {
        $entries=@{'SpoutPlugin/SpoutPlugin.uplugin'='{"VersionName":"fixture","Installed":true,"EngineVersion":"5.3.0"}'}
        if ($case -ne 'missing-source') { $entries['SpoutPlugin/Source/fixture.cpp']=Get-Content "$producer/Source/fixture.cpp" -Raw }
        if ($case -eq 'changed-source') { $entries['SpoutPlugin/Source/fixture.cpp']='changed' }
        if ($case -eq 'unexpected-source') { $entries['SpoutPlugin/Source/unexpected.cpp']='extra' }
        if ($case -eq 'traversal') { $entries['SpoutPlugin/../escaped.txt']='escape' }
        foreach ($entry in $entries.GetEnumerator()) {
            $writer=[IO.StreamWriter]::new($zip.CreateEntry($entry.Key).Open(),[Text.UTF8Encoding]::new($false))
            try { $writer.Write($entry.Value) } finally { $writer.Dispose() }
        }
    } finally { $zip.Dispose() }
    $null=New-TrackDArtifactIdentity -Artifact $archive -SourceIdentity $inputs -Engine @{Version='5.3'}
    if ($case -eq 'tampered-zip') { [IO.File]::AppendAllText($archive,'changed after identity') }
    $errorText=''
    try { $result=Expand-TrackDArchiveCandidate -ArchivePath $archive -DestinationParent $run.Directory -EvidenceRoot $run.Directory } catch { $errorText=$_.Exception.Message }
    if ($case -eq 'valid') {
        if ($errorText -or $result.InstalledSourceIdentity.Digest -eq $inputs.Digest -or $result.Files.Count -ne 2) { throw "Valid transformed archive was not accepted: $errorText" }
    } else {
        $reason=switch($case) {
            changed-source {'differs from its build identity'}
            missing-source {'missing build inputs'}
            unexpected-source {'differs from its build identity'}
            traversal {'Unsafe or unexpected archive entry'}
            tampered-zip {'Artifact/provenance mismatch'}
        }
        if ($errorText -notmatch $reason) { throw "Case $case did not reject for the expected reason: $errorText" }
    }
    $checks += $case
}
Write-TrackDJson @{Passed=$true;Checks=$checks} (Join-Path $run.Directory 'results.json')
Write-Output "Passed $($checks.Count) exact archive regression checks. Evidence: $($run.Directory)"
