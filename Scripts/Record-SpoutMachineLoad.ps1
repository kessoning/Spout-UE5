# Copyright (c) 2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
# Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

<#
.SYNOPSIS
    Records what else uses the machine while timed measurements run.

.DESCRIPTION
    Started by Invoke-PerformanceBaselines.ps1 -RecordMachineLoad. Every interval it appends one JSON line with:
      - cpuTotalPct: whole-machine CPU use;
      - cpu: every process above 0.5% of the whole machine (plus the Spout hosts and this recorder), in % of the
        whole machine, from the change in each process's CPU time;
      - gpu: per process and engine type, the Windows "GPU Engine" utilisation above 0.5% (plus the Spout hosts);
      - gpuMem: per process, dedicated GPU memory above 256 MB (plus the Spout hosts), in MB.
    It keeps the PC and the display awake while it runs, and stops when the stop file appears.

    It only records; it judges nothing. The analysis compares each scenario's window with the disturbance limit
    written in the run's criteria before the run.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $OutPath,
    [Parameter(Mandatory = $true)][string] $StopFile,
    [ValidateRange(1, 60)][int] $IntervalSeconds = 2,
    [ValidateRange(1, 48)][int] $MaxHours = 12
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -Namespace SpoutLoad -Name Power -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint esFlags);'
# ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED, for this thread's lifetime.
[void] [SpoutLoad.Power]::SetThreadExecutionState([uint32] 2147483651)

$cores = [Environment]::ProcessorCount
$hostPattern = 'SpoutHostTest*'
$previousCpu = @{}
$previousTicks = [Diagnostics.Stopwatch]::GetTimestamp()
$deadline = (Get-Date).AddHours($MaxHours)
$counters = @('\Processor(_Total)\% Processor Time', '\GPU Engine(*)\Utilization Percentage', '\GPU Process Memory(*)\Dedicated Usage')

$writer = [IO.StreamWriter]::new($OutPath, $true, [Text.UTF8Encoding]::new($false))
$writer.AutoFlush = $true
try
{
    while (-not (Test-Path -LiteralPath $StopFile) -and (Get-Date) -lt $deadline)
    {
        $loopStart = Get-Date

        # Counters first: Get-Counter takes two raw samples about a second apart for rate counters.
        $total = $null
        $gpuByKey = @{}
        $memByPid = @{}
        try
        {
            $samples = (Get-Counter -Counter $counters -ErrorAction SilentlyContinue).CounterSamples
            foreach ($sample in $samples)
            {
                $path = $sample.Path
                if ($path -like '*\processor(_total)\*') { $total = [math]::Round($sample.CookedValue, 2); continue }
                $instance = $sample.InstanceName
                if ($instance -notmatch '^pid_(\d+)_') { continue }
                $processId = [int] $Matches[1]
                if ($path -like '*\gpu engine(*') {
                    $engine = if ($instance -match 'engtype_(.+)$') { $Matches[1] } else { 'unknown' }
                    $key = "$processId|$engine"
                    if (-not $gpuByKey.ContainsKey($key)) { $gpuByKey[$key] = 0.0 }
                    $gpuByKey[$key] += $sample.CookedValue
                }
                elseif ($path -like '*\gpu process memory(*') {
                    if (-not $memByPid.ContainsKey($processId)) { $memByPid[$processId] = 0.0 }
                    $memByPid[$processId] += $sample.CookedValue
                }
            }
        }
        catch { }

        $nowTicks = [Diagnostics.Stopwatch]::GetTimestamp()
        $elapsedMs = ($nowTicks - $previousTicks) * 1000.0 / [Diagnostics.Stopwatch]::Frequency
        $previousTicks = $nowTicks

        $names = @{}
        $currentCpu = @{}
        $cpu = New-Object System.Collections.ArrayList
        foreach ($process in [Diagnostics.Process]::GetProcesses())
        {
            $names[$process.Id] = $process.ProcessName
            try { $cpuMs = $process.TotalProcessorTime.TotalMilliseconds } catch { continue }
            $currentCpu[$process.Id] = $cpuMs
            if ($previousCpu.ContainsKey($process.Id) -and $elapsedMs -gt 0)
            {
                $pct = ($cpuMs - $previousCpu[$process.Id]) / $elapsedMs * 100.0 / $cores
                if ($pct -ge 0.5 -or $process.ProcessName -like $hostPattern -or $process.Id -eq $PID)
                {
                    [void] $cpu.Add([ordered]@{ pid = $process.Id; name = $process.ProcessName; pct = [math]::Round($pct, 2) })
                }
            }
        }
        $previousCpu = $currentCpu

        $gpu = New-Object System.Collections.ArrayList
        foreach ($key in $gpuByKey.Keys)
        {
            $parts = $key.Split('|')
            $processId = [int] $parts[0]
            $name = if ($names.ContainsKey($processId)) { $names[$processId] } else { '?' }
            if ($gpuByKey[$key] -ge 0.5 -or $name -like $hostPattern)
            {
                [void] $gpu.Add([ordered]@{ pid = $processId; name = $name; engine = $parts[1]; pct = [math]::Round($gpuByKey[$key], 2) })
            }
        }

        $gpuMem = New-Object System.Collections.ArrayList
        foreach ($processId in $memByPid.Keys)
        {
            $name = if ($names.ContainsKey($processId)) { $names[$processId] } else { '?' }
            $mb = $memByPid[$processId] / 1MB
            if ($mb -ge 256 -or $name -like $hostPattern)
            {
                [void] $gpuMem.Add([ordered]@{ pid = $processId; name = $name; dedicatedMB = [math]::Round($mb, 1) })
            }
        }

        $line = [ordered]@{
            t           = (Get-Date).ToUniversalTime().ToString('o')
            intervalMs  = [math]::Round($elapsedMs, 1)
            cpuTotalPct = $total
            cpu         = $cpu.ToArray()
            gpu         = $gpu.ToArray()
            gpuMem      = $gpuMem.ToArray()
        }
        $writer.WriteLine(($line | ConvertTo-Json -Compress -Depth 4))

        $remaining = $IntervalSeconds - ((Get-Date) - $loopStart).TotalSeconds
        if ($remaining -gt 0) { Start-Sleep -Milliseconds ([int] ($remaining * 1000)) }
    }
}
finally
{
    $writer.Dispose()
    [void] [SpoutLoad.Power]::SetThreadExecutionState([uint32] 2147483648)
}
