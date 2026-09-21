# Copyright (c) 2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
# Negative integration tests: these deliberately make a provenance-checked packaged host fail.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$EvidenceRoot,
    [Parameter(Mandatory)][string]$NativePeer,
    [string]$RepoRoot=(Split-Path $PSScriptRoot))
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'TrackD.psm1') -Force
$artifact=Read-TrackDArtifactIdentity $Executable
$run=New-TrackDEvidenceRun -Parent $EvidenceRoot -Label 'negative-delivery'
$peerHash=Get-TrackDSha256 $NativePeer
if(-not $peerHash){throw 'Native pixel oracle is missing'}
$rows=@()
foreach($case in @(
    @{Name='stopped-transfer';Flag='-SpoutPerfSuppressTransfers';Reason='stopped making pixel progress|measured delivery gate failed'},
    @{Name='corrupt-pixel';Flag='-SpoutInjectPixelFailure';Reason='native receiver rejected'})) {
    $directory=Join-Path $run.Directory $case.Name
    $identity=New-TrackDLaunchIdentity -Directory $directory -Executable $Executable -ArtifactIdentity $artifact
    $argsRun=@('-SpoutPerf=sender1080p','-SpoutPerfSeconds=3','-SpoutPerfWarmupSeconds=2',
        "-SpoutResultDir=$directory","-SpoutNativePeer=$NativePeer",$case.Flag,
        '-unattended','-nosplash','-NoSound','-windowed','-ResX=640','-ResY=360','-d3d12','-stdout',"-abslog=$directory/engine.log")+(Get-TrackDIdentityArguments $identity)
    $processResult=Invoke-TrackDProcess -FilePath $Executable -ArgumentList $argsRun -StdOutPath (Join-Path $directory 'stdout.log') -TimeoutSeconds 120 -WorkingDirectory (Split-Path $Executable)
    $result=Get-Content (Join-Path $directory 'perf-sender1080p.json') -Raw | ConvertFrom-Json
    $identityCheck=Test-TrackDLaunchIdentity $identity $processResult
    $passed=$identityCheck.Passed -and -not $processResult.TimedOut -and $processResult.ExitCode -eq 5 -and -not $result.passed -and $result.failureReason -match $case.Reason
    $rows+=@{Case=$case.Name;NegativeTestPassed=$passed;Process=$processResult;Result=$result;Identity=$identityCheck}
    Write-TrackDJson $rows (Join-Path $run.Directory 'results.json')
    Write-Host "$($case.Name): rejected correctly = $passed; process exit $($processResult.ExitCode); identity $($identityCheck.Passed); $($result.failureReason)"
    if(-not $passed){throw 'The negative case did not fail for its intended delivery defect'}
}
$directory=Join-Path $run.Directory 'packaged-smoke-corrupt-pixel'
$identity=New-TrackDLaunchIdentity -Directory $directory -Executable $Executable -ArtifactIdentity $artifact
$argsRun=@('-SpoutSmoke','-SpoutSenderCycles=1','-SpoutReceiverCycles=1','-SpoutTravelCycles=1',
    '-SpoutSmokeMapA=/Engine/Maps/Entry','-SpoutSmokeMapB=/Engine/Maps/Templates/Template_Default',
    '-SpoutSmokeTimeoutSeconds=120','-SpoutInjectPixelFailure',"-SpoutResultDir=$directory","-SpoutNativePeer=$NativePeer",
    '-unattended','-nosplash','-NoSound','-windowed','-ResX=640','-ResY=360','-d3d12','-stdout',"-abslog=$directory/engine.log")+(Get-TrackDIdentityArguments $identity)
$processResult=Invoke-TrackDProcess -FilePath $Executable -ArgumentList $argsRun -StdOutPath (Join-Path $directory 'stdout.log') -TimeoutSeconds 180 -WorkingDirectory (Split-Path $Executable)
# Read the literal required-marker contract from the matrix, without executing its build flow.
$tokens=$null; $parseErrors=$null
$matrixAst=[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Invoke-PackagedHostMatrix.ps1'),[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'Cannot parse packaged matrix marker contract'}
$markerAssignment=$matrixAst.Find({param($node) $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '$RequiredSmokeMarkers'},$true)
$requiredMarkers=@($markerAssignment.Right.FindAll({param($node) $node -is [System.Management.Automation.Language.StringConstantExpressionAst]},$true) | ForEach-Object Value)
if($requiredMarkers -notcontains 'SPOUT_EXTERNAL_PIXELS_PASS' -or $requiredMarkers -notcontains 'SPOUT_SMOKE_PASS'){throw 'Packaged matrix is missing required pixel/smoke markers'}
$verdict=Read-TrackDSmokeResult -ResultDir $directory -RequiredMarkers $requiredMarkers -ProcessResult $processResult
$identityCheck=Test-TrackDLaunchIdentity $identity $processResult
$passed=$identityCheck.Passed -and -not $processResult.TimedOut -and $processResult.ExitCode -eq 3 -and -not $verdict.Passed -and
    $verdict.HostFailureReason -match 'native receiver rejected' -and $verdict.MissingMarkers -contains 'SPOUT_EXTERNAL_PIXELS_PASS'
$rows+=@{Case='packaged-smoke-corrupt-pixel';NegativeTestPassed=$passed;Process=$processResult;Result=$verdict;Identity=$identityCheck}
Write-TrackDJson $rows (Join-Path $run.Directory 'results.json')
Write-Host "packaged smoke: native corruption rejected by matrix validator = $passed; exit $($processResult.ExitCode); $($verdict.HostFailureReason)"
if(-not $passed){throw 'Packaged matrix validator did not reject the native pixel failure for its intended reason'}
if((Get-TrackDSha256 $NativePeer) -ne $peerHash){throw 'Native oracle changed during the test'}
Write-TrackDJson @{Passed=$true;NativePeerSHA256=$peerHash;Rows=$rows} (Join-Path $run.Directory 'results.json')
