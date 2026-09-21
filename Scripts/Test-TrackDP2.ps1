#requires -Version 7.2
# Copyright (c) 2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
[CmdletBinding()]
param([string]$EvidenceRoot = (Join-Path (Split-Path $PSScriptRoot) 'Artifacts/P2/script-tests'))
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'TrackD.psm1') -Force
$run = New-TrackDEvidenceRun -Parent $EvidenceRoot -Label 'regression'
$checks = @()
function Assert-Case([string]$Name,[bool]$Passed) {
    if (-not $Passed) { throw "FAILED: $Name" }
    $script:checks += $Name
}
$pwsh = (Get-Process -Id $PID).Path
$result = Invoke-TrackDProcess -FilePath $pwsh -ArgumentList @('-NoProfile','-Command', '[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); [Console]::Write("x" * 1048576); [Console]::Error.WriteLine("tail-α"); exit 7') -StdOutPath (Join-Path $run.Directory 'output.log') -TimeoutSeconds 20
Assert-Case 'Large output streams completely and preserves exit code' ($result.ExitCode -eq 7 -and (Get-Item $result.StdOutPath).Length -eq 1048576 -and (Get-Content $result.StdErrPath -Raw).Contains('tail-α'))
$result = Invoke-TrackDProcess -FilePath $pwsh -ArgumentList @('-NoProfile','-Command','[Console]::WriteLine("before-timeout"); Start-Sleep 60') -StdOutPath (Join-Path $run.Directory 'timeout.log') -TimeoutSeconds 2 -TerminationTimeoutSeconds 2 -OutputDrainTimeoutSeconds 2
Assert-Case 'Timeout kills child with a bounded complete output drain' ($result.TimedOut -and -not $result.CleanupFailed -and $result.DurationSeconds -lt 8 -and (Get-Content $result.StdOutPath -Raw).Contains('before-timeout'))
$result = $null
try {
    $result = Invoke-TrackDProcess -FilePath $pwsh -ArgumentList @('-NoProfile','-Command','[Console]::WriteLine("live-output"); Start-Sleep 60') -StdOutPath (Join-Path $run.Directory 'refused.log') -TimeoutSeconds 2 -TerminationTimeoutSeconds 0 -OutputDrainTimeoutSeconds 1 -TerminateProcess { param($OwnedProcess) throw 'Injected termination refusal' }
    Assert-Case 'Refused termination returns failure without unbounded wait' ($result.TimedOut -and $result.CleanupFailed -and $null -eq $result.ExitCode -and $result.DurationSeconds -lt 7 -and $result.TerminationErrors.Count -gt 0)
    Assert-Case 'Output is visible before the surviving process exits' ((Get-Content $result.StdOutPath -Raw).Contains('live-output'))
} finally {
    # This PID is the child just created by this test, never an enumerated unrelated build.
    if ($result -and $result.ProcessId) {
        $child = Get-Process -Id $result.ProcessId -ErrorAction SilentlyContinue
        if ($child) { try { $child.Kill($true); [void]$child.WaitForExit(5000) } finally { $child.Dispose() } }
    }
}
$fixture = Join-Path $run.Directory 'arguments.ps1'
$orphanFixture = Join-Path $run.Directory 'orphan.ps1'
$orphanPidFile = Join-Path $run.Directory 'orphan.pid'
@'
param([string]$ChildExecutable,[string]$PidFile)
$start = [Diagnostics.ProcessStartInfo]::new($ChildExecutable)
$start.UseShellExecute=$false
$start.CreateNoWindow=$true
foreach ($argument in @('-NoProfile','-Command','[Console]::WriteLine("orphan-output"); Start-Sleep 60')) { $start.ArgumentList.Add($argument) }
$child=[Diagnostics.Process]::Start($start)
[IO.File]::WriteAllText($PidFile,[string]$child.Id)
[Console]::WriteLine('parent-exited')
[Environment]::Exit(0)
'@ | Set-Content $orphanFixture
try {
    $result = Invoke-TrackDProcess -FilePath $pwsh -ArgumentList @('-NoProfile','-File',$orphanFixture,'-ChildExecutable',$pwsh,'-PidFile',$orphanPidFile) -StdOutPath (Join-Path $run.Directory 'orphan.log') -TimeoutSeconds 10 -OutputDrainTimeoutSeconds 2
    Assert-Case 'Inherited orphan pipe cannot block output capture indefinitely' (-not $result.TimedOut -and $result.OutputDrainTimedOut -and $result.CleanupFailed -and $null -eq $result.ExitCode -and $result.DurationSeconds -lt 8)
} finally {
    if (Test-Path $orphanPidFile) {
        $child = Get-Process -Id ([int](Get-Content $orphanPidFile)) -ErrorAction SilentlyContinue
        if ($child) { try { $child.Kill($true); [void]$child.WaitForExit(5000) } finally { $child.Dispose() } }
    }
}
Set-Content $fixture 'param([string[]]$EnginePaths=@()); @($EnginePaths | ForEach-Object { $_ -split '','' }) | ConvertTo-Json -Compress'
foreach ($paths in @(@{Values=@()}, @{Values=@('5.3=C:\Engine With Space\UE_5.3')}, @{Values=@('5.3=C:\Engine With Space\UE_5.3','5.8=D:\Other Engine\UE_5.8')})) {
    $arguments = @('-NoProfile','-File',$fixture)
    if ($paths.Values.Count) { $arguments += @('-EnginePaths',($paths.Values -join ',')) }
    $result = Invoke-TrackDProcess -FilePath $pwsh -ArgumentList $arguments -StdOutPath (Join-Path $run.Directory "arguments-$($paths.Values.Count).log") -TimeoutSeconds 20
    $text = Get-Content $result.StdOutPath -Raw
    $actual = if ([string]::IsNullOrWhiteSpace($text)) { @() } else { @($text | ConvertFrom-Json) }
    Assert-Case "Engine path forwarding preserves $($paths.Values.Count) overrides including spaces" ($result.ExitCode -eq 0 -and ($actual -join '|') -eq ($paths.Values -join '|'))
}
$legacy = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$result = Invoke-TrackDProcess -FilePath $legacy -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path (Split-Path $PSScriptRoot) 'build.ps1'),'-Help') -StdOutPath (Join-Path $run.Directory 'legacy.log') -TimeoutSeconds 20
Assert-Case 'Windows PowerShell 5.1 is refused before release actions' ($result.ExitCode -ne 0 -and (Get-Content $result.StdErrPath -Raw).Contains('7.2'))
Write-TrackDJson @{Passed=$true;Checks=$checks;Count=$checks.Count} (Join-Path $run.Directory 'results.json')
Write-Output "Passed $($checks.Count) P2 process/prerequisite/argument checks. Evidence: $($run.Directory)"
