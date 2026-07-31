<#
.SYNOPSIS
    Cooks, packages, inspects and launches the Spout host test project for every requested
    engine/configuration row.

.DESCRIPTION
    One row = one engine version and one build configuration. For each row:

      1. an isolated staging copy of Tests/SpoutHostTest is built with the plugin installed inside it;
      2. RunUAT BuildCookRun builds, cooks, stages, paks and archives it;
      3. the archived layout is inspected - executable, plugin descriptor, cooked maps, Spout.dll;
      4. Spout.dll's location and SHA-256 are recorded and compared against the repository copy;
      5. the whole packaged tree is searched for OTHER copies of Spout.dll;
      6. the executable is launched, repeatedly, with the smoke sequence and a hard timeout;
      7. the host's markers and result.json are read, and the DLL path the host reports having
         loaded is compared against the path this script expects.

    What this deliberately does NOT accept as evidence:

      - BuildPlugin succeeding. A plugin that compiles says nothing about a packaged host.
      - The executable existing. A packaged binary on disk proves only that packaging ran.
      - The process starting, or exiting 0. Both are necessary and neither is sufficient; the
        required markers must all be present as well.
      - Spout.dll being present at the expected path. Presence is staging; the host reporting that
        the loader resolved THAT path is loading.

    Shipping is built with NO_LOGGING, so its log file is empty by design. The host writes markers
    and result.json to a directory this script owns, which is why a Shipping row produces exactly
    the same machine-readable evidence as a Development one.

.EXAMPLE
    .\Scripts\Invoke-PackagedHostMatrix.ps1 -EngineVersions 5.3,5.8 -Configurations Development,Shipping -EvidenceRoot .\Artifacts\TrackD\run
#>
[CmdletBinding()]
param(
    [string] $RepoRoot = '',

    [Parameter(Mandatory = $true)]
    [string[]] $EngineVersions,

    # Validated in the body rather than with [ValidateSet], because a caller launching this through
    # a batch file passes the whole list as one "Development,Shipping" string and ValidateSet would
    # reject it before the body could split it.
    [string[]] $Configurations = @('Development', 'Shipping'),

    [string] $EngineRoot = 'D:\UE',
    [string[]] $EnginePaths = @(),

    [Parameter(Mandatory = $true)]
    [string] $EvidenceRoot,

    [string] $WorkRoot = "$env:TEMP\SpoutD",

    # How many times each packaged host is launched. More than one is the "repeated application
    # launch/exit" case: a lifecycle defect that only bites on a second run in the same session
    # would otherwise never be seen.
    [int] $LaunchRepeats = 3,

    [int] $SenderCycles = 10,
    [int] $ReceiverCycles = 10,
    [int] $TravelCycles = 5,

    [int] $PackageTimeoutSeconds = 5400,
    [int] $LaunchTimeoutSeconds = 900,

    [int] $MutexRetries = 3,

    # How long to wait for ANOTHER build to release UnrealBuildTool's machine-global state before
    # attempting anyway. Sized for a foreign build rather than for this pipeline racing itself: an
    # unrelated Unreal build on the same machine can hold that state for many minutes, and waiting
    # it out costs nothing on an idle machine.
    [int] $BuildToolWaitSeconds = 1800,

    # Retries reserved for the compiler running out of memory on a busy machine. Environmental,
    # like the mutex, and counted separately from it in the manifest.
    [int] $ResourceRetries = 2,

    # Keeps the packaged output on disk after the row passes. Off by default: six engines times two
    # configurations of a packaged UE project is tens of gigabytes.
    [switch] $KeepPackages
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

Import-Module (Join-Path $PSScriptRoot 'TrackD.psm1') -Force

if ([string]::IsNullOrWhiteSpace($RepoRoot)) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

$EngineVersions = Expand-TrackDList $EngineVersions
$Configurations = Expand-TrackDList $Configurations
$EnginePaths = Expand-TrackDList $EnginePaths

foreach ($configuration in $Configurations)
{
    if ($configuration -notin @('Development', 'Shipping'))
    {
        throw "Unsupported configuration '$configuration'. Valid values are Development and Shipping."
    }
}

$explicitMap = @{}
foreach ($pair in $EnginePaths)
{
    if ([string]::IsNullOrWhiteSpace($pair)) { continue }
    $separator = $pair.IndexOf('=')
    if ($separator -lt 1) { throw "Malformed -EnginePaths entry '$pair'. Expected 'version=path'." }
    $explicitMap[$pair.Substring(0, $separator).Trim()] = $pair.Substring($separator + 1).Trim()
}

foreach ($dir in @($EvidenceRoot))
{
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
}
$EvidenceRoot = (Resolve-Path -LiteralPath $EvidenceRoot).Path

$packagedHostsEvidence = Join-Path $EvidenceRoot 'packaged-hosts'
$smokeEvidence         = Join-Path $EvidenceRoot 'smoke-tests'
$lifecycleEvidence     = Join-Path $EvidenceRoot 'lifecycle'
$buildLogEvidence      = Join-Path $EvidenceRoot 'build-logs'
$manifestEvidence      = Join-Path $EvidenceRoot 'manifests'
$hashEvidence          = Join-Path $EvidenceRoot 'hashes'

foreach ($dir in @($packagedHostsEvidence, $smokeEvidence, $lifecycleEvidence, $buildLogEvidence, $manifestEvidence, $hashEvidence))
{
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
}

$pluginName = Get-TrackDPluginName
$hostProjectName = Get-TrackDHostProjectName
$pluginVersion = Get-TrackDPluginVersion $RepoRoot

$repoSpoutDll = Join-Path $RepoRoot 'ThirdParty\Spout\lib\amd64\Spout.dll'
$repoSpoutDllHash = Get-TrackDSha256 $repoSpoutDll
if (-not $repoSpoutDllHash)
{
    throw "The repository Spout.dll is missing at $repoSpoutDll; there is nothing to compare packaged copies against."
}

# The maps the host travels between. Passed explicitly to BuildCookRun so a packaged host is never
# at the mercy of ini merge order.
$SmokeMapA = '/Engine/Maps/Entry'
$SmokeMapB = '/Engine/Maps/Templates/Template_Default'

# Every marker the smoke sequence must emit. Missing any one fails the row.
$RequiredSmokeMarkers = @(
    'SPOUT_SMOKE_BEGIN',
    'SPOUT_PLUGIN_MODULE_LOADED',
    'SPOUT_DLL_LOADED',
    'SPOUT_DLL_PATH',
    'SPOUT_PLUGIN_API_INIT_PASS',
    'SPOUT_SENDER_CREATE_PASS',
    'SPOUT_SAME_NAME_RESTART_PASS',
    'SPOUT_RECEIVER_CREATE_PASS',
    'SPOUT_LEVEL_TRAVEL_PASS',
    'SPOUT_NO_STALE_SESSION_PASS',
    'SPOUT_SHUTDOWN_BEGIN',
    'SPOUT_SHUTDOWN_COMPLETE',
    'SPOUT_SMOKE_PASS'
)

$rows = New-Object System.Collections.ArrayList
$failures = New-Object System.Collections.ArrayList

function Find-PackagedExecutable
{
    <#
    .SYNOPSIS
        Locates the real game executable inside an archived Windows package.

    .DESCRIPTION
        Globbed rather than assumed: Development stages <Project>.exe while Shipping stages
        <Project>-Win64-Shipping.exe, and the root-level launcher shim next to Engine/ is not the
        binary whose loader state is inspected.
    #>
    param([string] $ArchiveDir, [string] $ProjectName)

    $binariesDir = Join-Path $ArchiveDir ('Windows\{0}\Binaries\Win64' -f $ProjectName)
    if (-not (Test-Path -LiteralPath $binariesDir -PathType Container))
    {
        return $null
    }

    $candidates = @(Get-ChildItem -LiteralPath $binariesDir -Filter '*.exe' -File -ErrorAction SilentlyContinue |
        Where-Object { $_.BaseName -like "$ProjectName*" -and $_.BaseName -notlike '*CrashReport*' })

    if ($candidates.Count -eq 0) { return $null }
    return ($candidates | Sort-Object Length -Descending | Select-Object -First 1).FullName
}

Write-TrackDSection "Packaged-host matrix - plugin $pluginVersion"
Write-TrackDStep "Repository     : $RepoRoot"
Write-TrackDStep "Evidence       : $EvidenceRoot"
Write-TrackDStep "Engines        : $($EngineVersions -join ', ')"
Write-TrackDStep "Configurations : $($Configurations -join ', ')"
Write-TrackDStep "Launch repeats : $LaunchRepeats"
Write-TrackDStep "Repo Spout.dll : $repoSpoutDllHash"

foreach ($version in $EngineVersions)
{
    $enginePath = Resolve-TrackDEnginePath -Version $version -EngineRoot $EngineRoot -ExplicitMap $explicitMap
    $engine = Get-TrackDEngineFacts -Version $version -EnginePath $enginePath

    foreach ($configuration in $Configurations)
    {
        $rowLabel = "UE $version / $configuration"
        Write-TrackDSection $rowLabel

        $shortVersion = $version.Replace('.', '')
        $shortConfig = if ($configuration -eq 'Shipping') { 's' } else { 'd' }
        $rowKey = "UE$version-$configuration"

        $stagingDir = Join-Path $WorkRoot ("p{0}{1}\P" -f $shortVersion, $shortConfig)
        $archiveDir = Join-Path $WorkRoot ("p{0}{1}\A" -f $shortVersion, $shortConfig)

        $rowEvidence = Join-Path $packagedHostsEvidence $rowKey
        New-Item -ItemType Directory -Path $rowEvidence -Force | Out-Null

        $row = [ordered] @{
            Schema              = 'spoutplugin-trackd-packaged-host-row/1'
            RowKey              = $rowKey
            EngineVersion       = $version
            EnginePath          = $enginePath
            EngineBuildId       = $engine.BuildId
            EngineBuildVersion  = $engine.BuildVersion
            Configuration       = $configuration
            Platform            = 'Win64'
            PluginVersion       = $pluginVersion
            StagingProjectDir   = $stagingDir
            ArchiveDir          = $archiveDir
            UatCommand          = $null
            UatExitCode         = $null
            UatTimedOut         = $null
            UatLogPath          = $null
            UatDurationSeconds  = $null
            MutexRetries        = 0
            ResourceRetries     = 0
            BuildToolWaitSeconds = 0
            ExecutablePath      = $null
            ExecutableSha256    = $null
            PluginDescriptorStaged = $null
            SpoutDllStagedAsNonUfs = $null
            StagedPluginEntries = @()
            CookedMaps          = @()
            ExpectedSpoutDllPath = $null
            ActualSpoutDllPath  = $null
            SpoutDllSha256      = $null
            SpoutDllSizeBytes   = $null
            SpoutDllMatchesRepository = $null
            DuplicateSpoutDlls  = @()
            Launches            = @()
            Status              = 'NotRun'
            Problems            = @()
        }

        $problems = New-Object System.Collections.ArrayList

        # Every exit from this row - including the early ones - goes through here, so a row that
        # failed still leaves a manifest saying how far it got and why. A failed row that leaves no
        # evidence is the precise gap this pipeline exists to close, and it is just as easy to
        # introduce in the validation tooling as in the thing being validated.
        $rowManifestPath = Join-Path $manifestEvidence "packaged-host-$rowKey.json"
        function Complete-Row
        {
            param($Row, $Problems, $Label, $ManifestPath)
            $Row.Problems = $Problems.ToArray()
            $Row.Status = if ($Problems.Count -eq 0) { 'Passed' } else { 'Failed' }
            Write-TrackDJson -InputObject $Row -Path $ManifestPath
            return $Row.Status
        }

        if (-not $engine.Found)
        {
            [void] $problems.Add("engine not installed at $enginePath")
            [void] (Complete-Row -Row $row -Problems $problems -Label $rowLabel -ManifestPath $rowManifestPath)
            [void] $rows.Add($row)
            [void] $failures.Add("$rowLabel : engine not installed")
            Write-TrackDBad "engine not installed at $enginePath"
            continue
        }

        # --- Stage ------------------------------------------------------------------------------
        Write-TrackDStep "Staging project : $stagingDir"
        $staging = New-TrackDHostStaging -RepoRoot $RepoRoot -StagingProjectDir $stagingDir -Force
        if ($staging.MissingItems.Count -gt 0)
        {
            [void] $problems.Add("plugin staging is missing: $($staging.MissingItems -join ', ')")
        }

        if (Test-Path -LiteralPath $archiveDir) { Remove-Item -LiteralPath $archiveDir -Recurse -Force }
        New-Item -ItemType Directory -Path $archiveDir -Force | Out-Null

        # --- Cook and package -------------------------------------------------------------------
        $uatLog = Join-Path $rowEvidence 'buildcookrun.log'
        $row.UatLogPath = $uatLog

        $uatArgs = @(
            'BuildCookRun',
            "-project=$($staging.ProjectFile)",
            '-noP4',
            '-platform=Win64',
            '-targetplatform=Win64',
            "-clientconfig=$configuration",
            "-map=$SmokeMapA+$SmokeMapB",
            '-build',
            '-cook',
            '-stage',
            '-pak',
            '-archive',
            "-archivedirectory=$archiveDir",
            '-nodebuginfo',
            '-utf8output',
            '-unattended',
            '-nocompileuat'
        )

        $row.UatCommand = ('"{0}" {1}' -f $engine.RunUAT, ($uatArgs -join ' '))
        Write-TrackDStep "Running BuildCookRun ($configuration)"

        $uatResult = $null
        while ($true)
        {
            # Settle before packaging. Rows run back to back, and an unrelated Unreal build may also be
            # running on the machine; either way, a row started while another process holds
            # UnrealBuildTool's machine-global state fails almost at once, having compiled nothing.
            $settled = Wait-TrackDBuildToolIdle -TimeoutSeconds $BuildToolWaitSeconds
            if ($settled -gt 0)
            {
                $row.BuildToolWaitSeconds += $settled
                Write-TrackDStep "waited ${settled}s for another build to release the shared UnrealBuildTool state"
            }

            $uatResult = Invoke-TrackDProcess -FilePath $engine.RunUAT -ArgumentList $uatArgs `
                -StdOutPath $uatLog -TimeoutSeconds $PackageTimeoutSeconds -WorkingDirectory $WorkRoot

            if ($uatResult.ExitCode -eq 0) { break }

            # Two environmental conditions are retried, and only these two. Both are properties of
            # the machine rather than of the plugin, and both are counted separately in the manifest
            # so an environmental retry is never mistaken for a flaky build.
            if ((Test-TrackDLogForBuildMutex $uatLog) -and $row.MutexRetries -lt $MutexRetries)
            {
                $row.MutexRetries++
                Write-TrackDWarn "UnrealBuildTool contention; waiting for the other build to finish"
                [void] (Wait-TrackDBuildToolIdle -TimeoutSeconds $BuildToolWaitSeconds -PollSeconds 10)
                continue
            }

            if ((Test-TrackDLogForResourceExhaustion $uatLog) -and $row.ResourceRetries -lt $ResourceRetries)
            {
                # MSVC could not get virtual memory for a PCH. Waiting lets whatever else on this
                # machine was holding memory release it; the same source compiles cleanly once it
                # has. Nothing is killed to make room.
                $row.ResourceRetries++
                Write-TrackDWarn "the compiler ran out of memory (C3859/C1076); waiting before retrying"
                Start-Sleep -Seconds 60
                continue
            }

            break
        }

        $row.UatExitCode = $uatResult.ExitCode
        $row.UatTimedOut = $uatResult.TimedOut
        $row.UatDurationSeconds = $uatResult.DurationSeconds

        # Keep the engine's own AutomationTool log too; the captured stdout does not carry per-target
        # compiler detail.
        $engineLogSource = Join-Path $env:APPDATA 'Unreal Engine\AutomationTool\Logs'
        if (Test-Path -LiteralPath $engineLogSource)
        {
            $engineLogDest = Join-Path $buildLogEvidence $rowKey
            New-Item -ItemType Directory -Path $engineLogDest -Force | Out-Null
            Copy-Item -Path (Join-Path $engineLogSource '*') -Destination $engineLogDest -Recurse -Force -ErrorAction SilentlyContinue
        }

        if ($uatResult.TimedOut)
        {
            [void] $problems.Add("BuildCookRun did not finish within $PackageTimeoutSeconds s and was terminated")
        }
        if ($uatResult.ExitCode -ne 0)
        {
            $classification = if (Test-TrackDLogForResourceExhaustion $uatLog) { ' (host ran out of compiler memory)' }
                elseif (Test-TrackDLogForBuildMutex $uatLog) { ' (UnrealBuildTool mutex conflict)' }
                else { '' }

            [void] $problems.Add("BuildCookRun failed with exit code $($uatResult.ExitCode)$classification; see $uatLog")
            [void] (Complete-Row -Row $row -Problems $problems -Label $rowLabel -ManifestPath $rowManifestPath)
            [void] $rows.Add($row)
            [void] $failures.Add("$rowLabel : BuildCookRun failed (exit $($uatResult.ExitCode))$classification")
            Write-TrackDBad "BuildCookRun failed (exit $($uatResult.ExitCode))$classification"
            continue
        }
        Write-TrackDGood "BuildCookRun exit 0 ($($uatResult.DurationSeconds)s)"

        # --- Inspect the packaged layout --------------------------------------------------------
        $executablePath = Find-PackagedExecutable -ArchiveDir $archiveDir -ProjectName $hostProjectName
        $row.ExecutablePath = $executablePath

        if (-not $executablePath)
        {
            [void] $problems.Add("no packaged executable was produced under $archiveDir")
        }
        else
        {
            $row.ExecutableSha256 = Get-TrackDSha256 $executablePath
            Write-TrackDGood "executable: $executablePath"
        }

        $stagedProjectRoot = Join-Path $archiveDir ('Windows\{0}' -f $hostProjectName)

        # The plugin descriptor must be staged; without it the packaged game does not mount the
        # plugin's content at all.
        #
        # Read from UAT's own staging manifests rather than by looking for a loose file. With -pak
        # every UFS file, the .uplugin included, ends up inside the .pak and nothing remains on disk
        # to find - an on-disk check reports a missing descriptor for a package that staged it
        # perfectly well. The manifests are the authoritative record of what was staged and work for
        # both pak'd and loose layouts.
        $ufsManifest = Join-Path $archiveDir 'Windows\Manifest_UFSFiles_Win64.txt'
        $nonUfsManifest = Join-Path $archiveDir 'Windows\Manifest_NonUFSFiles_Win64.txt'

        $stagedUfsEntries = @()
        $stagedNonUfsEntries = @()
        if (Test-Path -LiteralPath $ufsManifest -PathType Leaf)
        {
            $stagedUfsEntries = @(Get-Content -LiteralPath $ufsManifest | ForEach-Object { ($_ -split "`t")[0] })
        }
        else
        {
            [void] $problems.Add("UAT staging manifest is missing at $ufsManifest")
        }
        if (Test-Path -LiteralPath $nonUfsManifest -PathType Leaf)
        {
            $stagedNonUfsEntries = @(Get-Content -LiteralPath $nonUfsManifest | ForEach-Object { ($_ -split "`t")[0] })
        }
        else
        {
            [void] $problems.Add("UAT staging manifest is missing at $nonUfsManifest")
        }

        $descriptorEntry = '{0}/Plugins/{1}/{1}.uplugin' -f $hostProjectName, $pluginName
        $loosDescriptor = Join-Path $stagedProjectRoot ('Plugins\{0}\{0}.uplugin' -f $pluginName)
        $row.PluginDescriptorStaged = ($stagedUfsEntries -contains $descriptorEntry) -or
                                      (Test-Path -LiteralPath $loosDescriptor -PathType Leaf)
        $row.StagedPluginEntries = @($stagedUfsEntries + $stagedNonUfsEntries | Where-Object { $_ -like "*$pluginName*" -or $_ -like '*Spout*' })

        if (-not $row.PluginDescriptorStaged)
        {
            [void] $problems.Add("plugin descriptor '$descriptorEntry' appears in neither staging manifest nor on disk")
        }
        else
        {
            Write-TrackDGood "plugin descriptor staged ($descriptorEntry)"
        }

        # The runtime DLL must be staged as a NonUFS file: it is loaded by the OS loader, which
        # cannot read out of a .pak. A Spout.dll that landed in the UFS set would be inside the pak
        # and invisible to LoadLibrary.
        $dllEntry = '{0}/Binaries/Win64/Spout.dll' -f $hostProjectName
        $row.SpoutDllStagedAsNonUfs = ($stagedNonUfsEntries -contains $dllEntry)
        if (-not $row.SpoutDllStagedAsNonUfs)
        {
            [void] $problems.Add("Spout.dll is not listed as a NonUFS staged file ('$dllEntry'); the OS loader cannot read it from inside a pak")
        }
        else
        {
            Write-TrackDGood "Spout.dll staged as a NonUFS file"
        }

        # Cooked maps: proof the smoke sequence has somewhere to travel to.
        $cookedContentDir = Join-Path $stagedProjectRoot 'Content\Paks'
        $pakFiles = @(Get-ChildItem -LiteralPath $cookedContentDir -Filter '*.pak' -File -ErrorAction SilentlyContinue)
        if ($pakFiles.Count -eq 0)
        {
            [void] $problems.Add("no .pak was produced under $cookedContentDir")
        }
        $row.CookedMaps = @($SmokeMapA, $SmokeMapB)

        # --- Spout.dll: the layout claim under test ---------------------------------------------
        # In a monolithic packaged game the plugin module is linked into the game executable, so the
        # Build.cs RuntimeDependencies target $(BinaryOutputDir) resolves to the PROJECT's
        # Binaries/Win64 next to the exe - NOT the plugin's own Binaries folder, which is not staged
        # at all in this layout.
        $expectedDll = Join-Path $stagedProjectRoot 'Binaries\Win64\Spout.dll'
        $row.ExpectedSpoutDllPath = $expectedDll

        if (-not (Test-Path -LiteralPath $expectedDll -PathType Leaf))
        {
            [void] $problems.Add("Spout.dll is missing from the packaged host at $expectedDll")
        }
        else
        {
            $row.ActualSpoutDllPath = $expectedDll
            $row.SpoutDllSha256 = Get-TrackDSha256 $expectedDll
            $row.SpoutDllSizeBytes = (Get-Item -LiteralPath $expectedDll).Length
            $row.SpoutDllMatchesRepository = ($row.SpoutDllSha256 -eq $repoSpoutDllHash)

            if (-not $row.SpoutDllMatchesRepository)
            {
                [void] $problems.Add("packaged Spout.dll hash $($row.SpoutDllSha256) does not match the repository copy $repoSpoutDllHash")
            }
            else
            {
                Write-TrackDGood "Spout.dll staged at the expected path (sha256 $($row.SpoutDllSha256))"
            }
        }

        # Any OTHER Spout.dll inside the package is a conflicting copy: the loader could bind to
        # either, and which one it picked would depend on search order rather than on staging.
        $allDlls = @(Get-ChildItem -LiteralPath $archiveDir -Filter 'Spout.dll' -File -Recurse -ErrorAction SilentlyContinue)
        $duplicates = @($allDlls | Where-Object { $_.FullName -ne $expectedDll } | ForEach-Object {
            [ordered] @{ Path = $_.FullName; Sha256 = (Get-TrackDSha256 $_.FullName) }
        })
        $row.DuplicateSpoutDlls = $duplicates
        if ($duplicates.Count -gt 0)
        {
            foreach ($duplicate in $duplicates)
            {
                [void] $problems.Add("a second Spout.dll exists in the package at $($duplicate.Path)")
            }
        }
        else
        {
            Write-TrackDGood "exactly one Spout.dll in the package"
        }

        # --- Launch -----------------------------------------------------------------------------
        if ($executablePath -and $problems.Count -eq 0)
        {
            $launches = New-Object System.Collections.ArrayList

            for ($attempt = 1; $attempt -le $LaunchRepeats; $attempt++)
            {
                $launchEvidence = Join-Path $smokeEvidence ("{0}\launch{1}" -f $rowKey, $attempt)
                New-Item -ItemType Directory -Path $launchEvidence -Force | Out-Null

                $hostLog = Join-Path $launchEvidence 'host-stdout.log'
                $absLog = Join-Path $launchEvidence 'host-engine.log'

                $launchArgs = @(
                    '-SpoutSmoke',
                    "-SpoutResultDir=$launchEvidence",
                    "-SpoutSenderCycles=$SenderCycles",
                    "-SpoutReceiverCycles=$ReceiverCycles",
                    "-SpoutTravelCycles=$TravelCycles",
                    "-SpoutSmokeMapA=$SmokeMapA",
                    "-SpoutSmokeMapB=$SmokeMapB",
                    "-SpoutSmokeTimeoutSeconds=$([Math]::Max(60, $LaunchTimeoutSeconds - 60))",
                    '-unattended',
                    '-nopause',
                    '-nosplash',
                    '-NoSound',
                    '-windowed',
                    '-ResX=640',
                    '-ResY=360',
                    '-stdout',
                    "-abslog=$absLog"
                )

                Write-TrackDStep "Launch $attempt/$LaunchRepeats"
                $launchResult = Invoke-TrackDProcess -FilePath $executablePath -ArgumentList $launchArgs `
                    -StdOutPath $hostLog -TimeoutSeconds $LaunchTimeoutSeconds `
                    -WorkingDirectory (Split-Path -Parent $executablePath)

                $verdict = Read-TrackDSmokeResult -ResultDir $launchEvidence `
                    -RequiredMarkers $RequiredSmokeMarkers -ProcessResult $launchResult

                # The decisive check: the path the host says the loader resolved must be the path
                # this script staged. Anything else means the process bound to a different copy.
                $reportedDllPath = $null
                if ($verdict.HostFields -and ($verdict.HostFields.PSObject.Properties.Name -contains 'runtimeDllPath'))
                {
                    $reportedDllPath = [string] $verdict.HostFields.runtimeDllPath
                }

                $dllPathMatches = $false
                if ($reportedDllPath -and $row.ExpectedSpoutDllPath)
                {
                    $normalisedReported = $reportedDllPath.Replace('/', '\').TrimEnd('\')
                    $normalisedExpected = $row.ExpectedSpoutDllPath.Replace('/', '\').TrimEnd('\')
                    $dllPathMatches = [string]::Equals($normalisedReported, $normalisedExpected, [System.StringComparison]::OrdinalIgnoreCase)
                }

                $launchProblems = @($verdict.Problems)
                if (-not $reportedDllPath)
                {
                    $launchProblems += 'the host did not report which Spout.dll it loaded'
                }
                elseif (-not $dllPathMatches)
                {
                    $launchProblems += "the host loaded Spout.dll from '$reportedDllPath' but the packaged copy is at '$($row.ExpectedSpoutDllPath)'"
                }

                $launchRecord = [ordered] @{
                    Attempt          = $attempt
                    EvidenceDir      = $launchEvidence
                    CommandLine      = $launchResult.CommandLine
                    ExitCode         = $launchResult.ExitCode
                    TimedOut         = $launchResult.TimedOut
                    DurationSeconds  = $launchResult.DurationSeconds
                    StdOutPath       = $launchResult.StdOutPath
                    EngineLogPath    = $absLog
                    ObservedMarkers  = $verdict.ObservedMarkers
                    MissingMarkers   = $verdict.MissingMarkers
                    HostReportedPass = $verdict.HostReportedPass
                    HostFailureReason = $verdict.HostFailureReason
                    ReportedDllPath  = $reportedDllPath
                    DllPathMatchesPackagedCopy = $dllPathMatches
                    Problems         = $launchProblems
                    Passed           = ($launchProblems.Count -eq 0)
                }

                [void] $launches.Add($launchRecord)

                if ($launchRecord.Passed)
                {
                    Write-TrackDGood "launch $attempt passed (exit 0, all markers, DLL path verified)"
                }
                else
                {
                    foreach ($problem in $launchProblems)
                    {
                        Write-TrackDBad "launch $attempt : $problem"
                        [void] $problems.Add("launch $attempt : $problem")
                    }
                }
            }

            $row.Launches = $launches.ToArray()
        }
        elseif ($executablePath)
        {
            [void] $problems.Add('the packaged layout failed inspection, so the host was not launched')
        }

        [void] (Complete-Row -Row $row -Problems $problems -Label $rowLabel -ManifestPath $rowManifestPath)

        if ($row.Status -ne 'Passed')
        {
            [void] $failures.Add("$rowLabel : $($problems.Count) problem(s)")
        }
        else
        {
            Write-TrackDGood "$rowLabel PASSED"
        }

        # Free the packaged tree unless asked to keep it. Evidence lives in the evidence directory;
        # the package itself is tens of gigabytes across the full matrix.
        if (-not $KeepPackages -and $row.Status -eq 'Passed')
        {
            if (Test-Path -LiteralPath $archiveDir) { Remove-Item -LiteralPath $archiveDir -Recurse -Force -ErrorAction SilentlyContinue }
            if (Test-Path -LiteralPath $stagingDir) { Remove-Item -LiteralPath $stagingDir -Recurse -Force -ErrorAction SilentlyContinue }
        }

        [void] $rows.Add($row)
    }
}

# --- DLL staging/loading matrix ------------------------------------------------------------------
$dllMatrix = New-Object System.Collections.ArrayList
foreach ($row in $rows)
{
    $reported = $null
    $verified = $false
    if ($row.Launches -and @($row.Launches).Count -gt 0)
    {
        $first = @($row.Launches)[0]
        $reported = $first.ReportedDllPath
        $verified = [bool] $first.DllPathMatchesPackagedCopy
    }

    [void] $dllMatrix.Add([ordered] @{
        Layout               = "$($row.Configuration) packaged host"
        EngineVersion        = $row.EngineVersion
        Configuration        = $row.Configuration
        ExpectedPath         = $row.ExpectedSpoutDllPath
        ActualPath           = $row.ActualSpoutDllPath
        SizeBytes            = $row.SpoutDllSizeBytes
        Sha256               = $row.SpoutDllSha256
        MatchesRepositoryCopy = $row.SpoutDllMatchesRepository
        LoadedPathReportedByHost = $reported
        LoadedFromExpectedPath = $verified
        DuplicateCopies      = $row.DuplicateSpoutDlls
    })
}

Write-TrackDJson -InputObject ([ordered] @{
    Schema       = 'spoutplugin-trackd-dll-matrix/1'
    GeneratedUtc = (Get-Date).ToUniversalTime().ToString('o')
    RepositorySpoutDll = Get-TrackDFileFacts $repoSpoutDll
    Rows         = $dllMatrix.ToArray()
}) -Path (Join-Path $hashEvidence 'dll-staging-matrix.json')

$manifest = [ordered] @{
    Schema         = 'spoutplugin-trackd-packaged-host-matrix/1'
    GeneratedUtc   = (Get-Date).ToUniversalTime().ToString('o')
    RepoRoot       = $RepoRoot
    PluginName     = $pluginName
    PluginVersion  = $pluginVersion
    Git            = Get-TrackDGitInfo $RepoRoot
    Host           = Get-TrackDHostSummary
    RequestedEngines = $EngineVersions
    Configurations = $Configurations
    LaunchRepeats  = $LaunchRepeats
    RequiredMarkers = $RequiredSmokeMarkers
    RepositorySpoutDllSha256 = $repoSpoutDllHash
    Rows           = $rows.ToArray()
    Failures       = $failures.ToArray()
    Success        = ($failures.Count -eq 0)
}

$manifestPath = Join-Path $manifestEvidence 'packaged-host-matrix.json'
Write-TrackDJson -InputObject $manifest -Path $manifestPath

# The lifecycle evidence is a projection of the same rows, kept separately because lifecycle and
# level travel are tracked as their own required outcome.
$lifecycleRows = New-Object System.Collections.ArrayList
foreach ($row in $rows)
{
    foreach ($launch in @($row.Launches))
    {
        [void] $lifecycleRows.Add([ordered] @{
            EngineVersion    = $row.EngineVersion
            Configuration    = $row.Configuration
            Attempt          = $launch.Attempt
            SenderCycles     = $SenderCycles
            ReceiverCycles   = $ReceiverCycles
            TravelCycles     = $TravelCycles
            SameNameRestart  = ($launch.ObservedMarkers -contains 'SPOUT_SAME_NAME_RESTART_PASS')
            LevelTravel      = ($launch.ObservedMarkers -contains 'SPOUT_LEVEL_TRAVEL_PASS')
            NoStaleSession   = ($launch.ObservedMarkers -contains 'SPOUT_NO_STALE_SESSION_PASS')
            ShutdownComplete = ($launch.ObservedMarkers -contains 'SPOUT_SHUTDOWN_COMPLETE')
            ExitCode         = $launch.ExitCode
            TimedOut         = $launch.TimedOut
            Passed           = $launch.Passed
        })
    }
}
Write-TrackDJson -InputObject ([ordered] @{
    Schema       = 'spoutplugin-trackd-lifecycle/1'
    GeneratedUtc = (Get-Date).ToUniversalTime().ToString('o')
    Rows         = $lifecycleRows.ToArray()
}) -Path (Join-Path $lifecycleEvidence 'lifecycle-results.json')

Write-Host ''
if ($failures.Count -eq 0)
{
    Write-Host ('=' * 78) -ForegroundColor Green
    Write-Host "PACKAGED-HOST MATRIX PASSED - $($rows.Count) row(s)" -ForegroundColor Green
    Write-Host ('=' * 78) -ForegroundColor Green
}
else
{
    Write-Host ('=' * 78) -ForegroundColor Red
    Write-Host 'PACKAGED-HOST MATRIX FAILED' -ForegroundColor Red
    foreach ($failure in $failures) { Write-Host "  - $failure" -ForegroundColor Red }
    Write-Host ('=' * 78) -ForegroundColor Red
}
Write-Host "Manifest: $manifestPath" -ForegroundColor Cyan

exit (Get-TrackDExitCode -FailureCount $failures.Count)
