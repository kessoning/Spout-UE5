#requires -Version 7.2
# Copyright (c) 2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
[CmdletBinding()]
param([string]$EvidenceRoot = (Join-Path (Split-Path $PSScriptRoot) 'Artifacts/TechnicalDebt/validator-tests'))
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'TrackD.psm1') -Force
$run = New-TrackDEvidenceRun -Parent $EvidenceRoot -Label regression
$checks = [Collections.Generic.List[string]]::new()
function Assert-Case([string]$Name, [bool]$Condition) {
    if (-not $Condition) { throw "FAILED: $Name" }
    $checks.Add($Name)
}

$engine = Join-Path $run.Directory 'engine'
New-Item -ItemType Directory -Path "$engine/Engine/Build/BatchFiles","$engine/Engine/Binaries/Win64" | Out-Null
Set-Content "$engine/Engine/Build/BatchFiles/RunUAT.bat" '@exit /b 0'
Set-Content "$engine/Engine/Build/Build.version" '{"MajorVersion":5,"MinorVersion":8,"PatchVersion":2,"Changelist":12345,"CompatibleChangelist":12000,"BranchName":"fixture"}'
Set-Content "$engine/Engine/Binaries/Win64/UnrealEditor.modules" '{"BuildId":"editor-module-id","Modules":{}}'
$facts = Get-TrackDEngineFacts -Version 5.8 -EnginePath $engine
Assert-Case 'Installed engine ID comes from module manifest' ($facts.BuildId -eq 'editor-module-id' -and $facts.BuildIdSource -eq 'Engine/Binaries/Win64/UnrealEditor.modules')
Assert-Case 'Engine version and changelist remain distinct from module ID' ($facts.BuildVersion -eq '5.8.2' -and $facts.BuildVersionData.Changelist -eq 12345 -and $facts.BuildVersionData.CompatibleChangelist -eq 12000)
Assert-Case 'Engine metadata hashes identify the bytes read' ($facts.BuildVersionSHA256 -eq (Get-TrackDSha256 "$engine/Engine/Build/Build.version") -and $facts.EditorModulesSHA256 -eq (Get-TrackDSha256 "$engine/Engine/Binaries/Win64/UnrealEditor.modules"))
Set-Content "$engine/Engine/Build/Build.version" '{"MajorVersion":5,"MinorVersion":8,"PatchVersion":2,"BuildId":"legacy-id"}'
Assert-Case 'Module compatibility ID takes precedence over legacy field' ((Get-TrackDEngineFacts -Version 5.8 -EnginePath $engine).BuildId -eq 'editor-module-id')
Set-Content "$engine/Engine/Binaries/Win64/UnrealEditor.modules" 'invalid JSON'
$facts = Get-TrackDEngineFacts -Version 5.8 -EnginePath $engine
Assert-Case 'Unreadable module metadata is visible and retains legacy fallback' ($facts.BuildId -eq 'legacy-id' -and $facts.MetadataWarnings.Count -eq 1 -and $facts.EditorModulesSHA256)
Set-Content "$engine/Engine/Build/Build.version" 'invalid JSON'
$facts = Get-TrackDEngineFacts -Version 5.8 -EnginePath $engine
Assert-Case 'Invalid metadata is not silently treated as an identified engine' ($null -eq $facts.BuildId -and $null -eq $facts.BuildVersion -and $facts.MetadataWarnings.Count -eq 2)
$facts = Get-TrackDEngineFacts -Version 5.8 -EnginePath (Join-Path $run.Directory 'missing')
Assert-Case 'Absent engine facts remain unavailable' (-not $facts.Found -and $null -eq $facts.BuildId -and $null -eq $facts.BuildVersionSHA256)
[IO.File]::WriteAllText("$engine/Engine/Build/Build.version", '{"MajorVersion":5,"MinorVersion":8,"PatchVersion":2}', [Text.UTF8Encoding]::new($true))
Assert-Case 'UTF-8 BOM is accepted without changing byte hash' ((Get-TrackDEngineFacts -Version 5.8 -EnginePath $engine).BuildVersion -eq '5.8.2')
$mismatch = ''
try { Get-TrackDEngineFacts -Version 5.3 -EnginePath $engine | Out-Null } catch { $mismatch=$_.Exception.Message }
Assert-Case 'Mislabelled engine override is refused' ($mismatch -match 'Engine version mismatch: requested UE 5.3' -and $mismatch -match 'UE 5.8.2')

# Exercise the actual release entry point with a fake UAT that must never run.
Set-Content "$engine/Engine/Build/BatchFiles/RunUAT.bat" '@echo unexpected> "%~dp0unexpected-build.txt"'
$repoRoot = Split-Path $PSScriptRoot
$releaseProbe = Invoke-TrackDProcess -FilePath (Get-Process -Id $PID).Path -ArgumentList @(
    '-NoProfile','-File',(Join-Path $repoRoot 'build.ps1'),'-Partial','-EngineVersions','5.3',
    '-EnginePaths',"5.3=$engine",'-OutputRoot',(Join-Path $run.Directory 'release-probe')) `
    -StdOutPath (Join-Path $run.Directory 'release-probe.log') -TimeoutSeconds 60 -WorkingDirectory $repoRoot
Assert-Case 'Release refuses wrong engine before invoking UAT' ($releaseProbe.ExitCode -ne 0 -and -not $releaseProbe.TimedOut -and
    -not (Test-Path "$engine/Engine/Build/BatchFiles/unexpected-build.txt") -and
    (Get-Content $releaseProbe.StdOutPath -Raw) -match 'Engine version mismatch')

$fixture = @{
    schema='spouthosttest-performance/1'; scenario='receiver1080p'; passed=$true; failureReason='';
    commandLine='-SpoutRunId=fixture-run -unattended'; maxNoProgressSeconds=0.01;
    configuration=@{streamCount=1}; results=@{frameCount=3600;measuredSeconds=60.001};
    delivery=@{expected=3600;observed=3600;ratio=1;corrupt=0;targetHz=60}
}
function Test-MeasurementCase([string]$Name, [scriptblock]$Mutate, [bool]$Pass, [string]$Reason = '') {
    $sample = $fixture | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    & $Mutate $sample
    $verdict = Test-TrackDPerformanceResult -Result $sample -Scenario receiver1080p -RunId fixture-run -Seconds 60
    Assert-Case $Name ($verdict.Passed -eq $Pass -and ($Pass -or ($verdict.Problems -join '; ') -match $Reason))
}
Test-MeasurementCase 'Full-rate current evidence passes' {} $true
Test-MeasurementCase 'Host pass cannot conceal a below-target workload' {param($s) $s.delivery.expected=3000;$s.delivery.observed=3000} $false 'target rate'
Test-MeasurementCase 'Reported ratio cannot conceal dropped frames' {param($s) $s.delivery.observed=1800} $false 'ratio disagrees'
Test-MeasurementCase 'Short measurement cannot qualify requested window' {param($s) $s.results.measuredSeconds=3} $false 'shorter'
Test-MeasurementCase 'Prefix-matching run ID is not current evidence' {param($s) $s.commandLine='-SpoutRunId=fixture-run-stale'} $false 'different invocation'
Test-MeasurementCase 'Missing fields fail closed' {param($s) $s.delivery.PSObject.Properties.Remove('observed')} $false 'invalid performance evidence'
Test-MeasurementCase 'NaN ratio fails closed' {param($s) $s.delivery.ratio=[double]::NaN} $false 'finite'
Test-MeasurementCase 'Infinite duration fails closed' {param($s) $s.results.measuredSeconds=[double]::PositiveInfinity} $false 'finite'
Test-MeasurementCase 'String counter cannot be coerced into a number' {param($s) $s.delivery.observed='3600'} $false 'JSON number'
Test-MeasurementCase 'Boolean counter cannot be coerced into a number' {param($s) $s.delivery.observed=$true} $false 'JSON number'
Test-MeasurementCase 'Fractional delivery count is rejected' {param($s) $s.delivery.observed=3599.5} $false 'fractional'
Test-MeasurementCase 'Negative delivery count is rejected' {param($s) $s.delivery.observed=-1} $false 'nonnegative'
Test-MeasurementCase 'Observed count cannot exceed opportunities' {param($s) $s.delivery.observed=3601} $false 'exceed'
Test-MeasurementCase 'Delivery opportunities cannot exceed measured work' {param($s) $s.results.frameCount=120} $false 'exceed the measured work'
Test-MeasurementCase 'Reduced stream count cannot qualify requested workload' {param($s) $s.configuration.streamCount=0} $false 'stream count'
Test-MeasurementCase 'Lower reported target does not weaken gate' {param($s) $s.delivery.targetHz=30} $false 'target rate'
Test-MeasurementCase 'Stopped stream rejected despite host pass' {param($s) $s.maxNoProgressSeconds=1} $false 'stopped making'
Test-MeasurementCase 'Corruption rejected despite host pass' {param($s) $s.delivery.corrupt=1} $false 'corrupt'
Test-MeasurementCase 'Host failure remains a failure' {param($s) $s.passed=$false;$s.failureReason='injected failure'} $false 'injected failure'
Test-MeasurementCase 'Zero-frame baseline cannot qualify' {param($s) $s.results.frameCount=0} $false 'zero frames'
Test-MeasurementCase 'Rounded ratio at the threshold remains valid' {param($s) $s.results.frameCount=3601;$s.delivery.expected=3601;$s.delivery.observed=3241;$s.delivery.ratio=0.900028} $true
foreach ($scenario in @('baseline','noplugin','idlereceiver','multistream')) {
    $sample = $fixture | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $sample.scenario=$scenario
    switch ($scenario) {
        {$_ -in @('baseline','noplugin')} {
            $sample.delivery.expected=0;$sample.delivery.observed=0;$sample.configuration.streamCount=0
            if ($scenario -eq 'noplugin') { $sample | Add-Member runId fixture-run; $sample | Add-Member noPlugin $true }
        }
        idlereceiver { $sample.delivery.expected=60;$sample.delivery.observed=60;$sample.maxNoProgressSeconds=1.2 }
        multistream { $sample.delivery.expected=14400;$sample.delivery.observed=14400;$sample.configuration.streamCount=4 }
    }
    Assert-Case "$scenario contract accepted" (Test-TrackDPerformanceResult $sample $scenario fixture-run 60).Passed
}
Write-TrackDJson @{Passed=$true;Checks=@($checks.ToArray())} (Join-Path $run.Directory 'results.json')
Write-Output "Passed $($checks.Count) technical-debt regression checks. Evidence: $($run.Directory)"
