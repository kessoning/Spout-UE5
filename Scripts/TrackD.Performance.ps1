# Copyright (c) 2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
# Pure result validation, shared by live measurements and offline regression/replay checks.

function Get-TrackDPerformanceNumber {
    param($Value, [string]$Name, [switch]$Integer)
    $numericTypes = @('Byte','SByte','Int16','UInt16','Int32','UInt32','Int64','UInt64','Single','Double','Decimal')
    if ($null -eq $Value -or [Type]::GetTypeCode($Value.GetType()).ToString() -notin $numericTypes) {
        throw "$Name must be a JSON number"
    }
    $number = [double]$Value
    if (-not [double]::IsFinite($number) -or $number -lt 0 -or ($Integer -and $number -ne [math]::Floor($number))) {
        throw "$Name must be finite and nonnegative$(if ($Integer) { ' with no fractional part' })"
    }
    return $number
}

function Test-TrackDPerformanceResult {
    param(
        [Parameter(Mandatory)]$Result,
        [Parameter(Mandatory)][string]$Scenario,
        [Parameter(Mandatory)][string]$RunId,
        [Parameter(Mandatory)][ValidateRange(1,86400)][int]$Seconds,
        [ValidateRange(1,32)][int]$Streams = 4
    )
    $problems = [Collections.Generic.List[string]]::new()
    try {
        if ($Result.schema -ne 'spouthosttest-performance/1' -or $Result.scenario -ne $Scenario) {
            $problems.Add('Performance schema/scenario does not match the requested workload')
        }
        $matchesRun = if ($Scenario -eq 'noplugin') { $Result.runId -ceq $RunId } else {
            $Result.commandLine -cmatch ('(?:^|\s)-SpoutRunId=' + [regex]::Escape($RunId) + '(?=\s|$)')
        }
        if (-not $matchesRun) { $problems.Add('Performance result belongs to a different invocation') }
        if ($Result.passed -isnot [bool] -or -not $Result.passed) {
            $problems.Add("The host reported a failure: $($Result.failureReason)")
        }
        if ($Scenario -eq 'noplugin' -and ($Result.noPlugin -isnot [bool] -or -not $Result.noPlugin)) {
            $problems.Add('Control host did not confirm Spout was absent')
        }

        $frames = Get-TrackDPerformanceNumber $Result.results.frameCount 'frameCount' -Integer
        $duration = Get-TrackDPerformanceNumber $Result.results.measuredSeconds 'measuredSeconds'
        if ($frames -le 0) { $problems.Add('The scenario measured zero frames') }
        # Host JSON rounds seconds to three decimals. Permit that rounding, not a shorter run.
        if ($duration -lt $Seconds - 0.001) { $problems.Add('Measured duration is shorter than the requested window') }

        $expected = Get-TrackDPerformanceNumber $Result.delivery.expected 'delivery.expected' -Integer
        $observed = Get-TrackDPerformanceNumber $Result.delivery.observed 'delivery.observed' -Integer
        $corrupt = Get-TrackDPerformanceNumber $Result.delivery.corrupt 'delivery.corrupt' -Integer
        $ratio = Get-TrackDPerformanceNumber $Result.delivery.ratio 'delivery.ratio'
        if ($corrupt -ne 0) { $problems.Add('Measured delivery contains corrupt observations') }
        if ($observed -gt $expected) { $problems.Add('Observed deliveries exceed published opportunities') }
        $actualRatio = if ($expected -gt 0) { $observed / $expected } else { 1.0 }
        if ([math]::Abs($ratio - $actualRatio) -gt 0.000001) {
            $problems.Add('Reported delivery ratio disagrees with the observed counters')
        }
        if ($Scenario -in @('baseline','noplugin')) {
            if ($expected -ne 0 -or $observed -ne 0) { $problems.Add('Idle control unexpectedly reports transfers') }
        } else {
            $streamCount = Get-TrackDPerformanceNumber $Result.configuration.streamCount 'streamCount' -Integer
            $wantedStreams = if ($Scenario -eq 'multistream') { $Streams } else { 1 }
            if ($streamCount -ne $wantedStreams) { $problems.Add('Measured stream count differs from the requested workload') }
            if ($expected -gt $frames * $wantedStreams) { $problems.Add('Published opportunities exceed the measured work frames and streams') }
            # targetHz describes the host's polling/work cadence. The idle receiver
            # still polls at 60 Hz but deliberately publishes only once per second.
            $deliveryHz = if ($Scenario -eq 'idlereceiver') { 1 } else { 60 }
            if ((Get-TrackDPerformanceNumber $Result.delivery.targetHz 'delivery.targetHz') -ne 60) {
                $problems.Add('Reported target rate differs from the workload contract')
            }
            if ($expected -le 0 -or $observed -le 0 -or $actualRatio -lt 0.9) {
                $problems.Add('Observed delivery is below 90% of published opportunities')
            }
            if ($observed -lt $Seconds * $deliveryHz * $wantedStreams * 0.9) {
                $problems.Add('Observed delivery is below 90% of the requested target rate')
            }
            $silence = Get-TrackDPerformanceNumber $Result.maxNoProgressSeconds 'maxNoProgressSeconds'
            $maxSilence = if ($Scenario -eq 'idlereceiver') { 1.5 } else { 0.5 }
            if ($silence -gt $maxSilence) { $problems.Add('Measured stream stopped making pixel progress before the run ended') }
        }
    } catch {
        $problems.Add("Incomplete or invalid performance evidence: $($_.Exception.Message)")
    }
    return [ordered]@{ Passed=($problems.Count -eq 0); Problems=@($problems.ToArray()) }
}
