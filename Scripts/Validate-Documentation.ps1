<#
.SYNOPSIS
    Validates the documentation set against the implementation, and fails on drift.

.DESCRIPTION
    Documentation that is merely correct today goes stale silently. This script is what stops that:
    it re-derives the facts from source on every run and fails when they disagree.

    Every fact it checks is DERIVED from tracked source and then looked for in the documentation,
    rather than being restated here:

      - plugin version              <- SpoutPlugin.uplugin VersionName
      - supported engine range      <- build.ps1 $DeclaredEngineVersions and Scripts/TrackD.psm1
      - module name                 <- SpoutPlugin.uplugin Modules[].Name
      - runtime DLL name            <- SpoutPlugin.Build.cs PublicDelayLoadDLLs
      - supported pixel formats     <- SpoutFormatPolicy.cpp's supported-format table
      - public Blueprint functions  <- UFUNCTION declarations in SpoutBPFunctionLibrary.h
      - release artifact name       <- build.ps1's archive-name format string
      - licensing files             <- LICENSE, THIRD_PARTY_NOTICES.md and Config/FilterPlugin.ini

    It also checks that a set of known-false claims stays ABSENT from the current documentation:
    the removed gamma pipeline, the removed d3dcompiler dependency, the false claim that the
    render-target factory roots its result, the false claim that RuntimeDependencies puts Spout.dll
    into raw BuildPlugin output, and the false claim that release Intermediate data is stripped.

    Historical records are exempt from the forbidden-claim checks, and deliberately so: they QUOTE
    the false claims in order to record them. They are listed explicitly rather than skipped by
    pattern, so adding a new exemption is a visible decision.

    Nothing is ever rewritten. -ShowDetail prints every check; without it only problems are printed.

.EXAMPLE
    .\Scripts\Validate-Documentation.ps1

.EXAMPLE
    .\Scripts\Validate-Documentation.ps1 -ShowDetail -ReportPath .\Artifacts\TrackD\run\documentation\report.json
#>
[CmdletBinding()]
param(
    [string] $RepoRoot = '',
    [string] $ReportPath = '',
    [switch] $ShowDetail,

    # Generated documentation (the PDF manual and the website) is checked for version freshness.
    # -SkipGeneratedDocs excludes it, for a caller that has deliberately taken it out of scope.
    [switch] $SkipGeneratedDocs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'TrackD.psm1') -Force

if ([string]::IsNullOrWhiteSpace($RepoRoot)) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

$checks = New-Object System.Collections.ArrayList
$problems = New-Object System.Collections.ArrayList

function Add-Check
{
    param(
        [string] $Name,
        [bool] $Passed,
        [string] $Detail,
        [string] $Expected = '',
        [string] $Actual = ''
    )

    [void] $checks.Add([ordered] @{
        Name     = $Name
        Passed   = $Passed
        Detail   = $Detail
        Expected = $Expected
        Actual   = $Actual
    })

    if ($Passed)
    {
        if ($ShowDetail) { Write-TrackDGood "$Name : $Detail" }
    }
    else
    {
        Write-TrackDBad "$Name : $Detail"
        [void] $problems.Add("$Name : $Detail")
    }
}

function Get-TextOrEmpty
{
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    return (Get-Content -LiteralPath $Path -Raw)
}

Write-TrackDSection 'Documentation validation'

# =================================================================================================
# Derive the facts from source
# =================================================================================================

$descriptorPath = Join-Path $RepoRoot 'SpoutPlugin.uplugin'
$descriptor = Get-Content -LiteralPath $descriptorPath -Raw | ConvertFrom-Json
$pluginVersion = [string] $descriptor.VersionName
$moduleNames = @($descriptor.Modules | ForEach-Object { [string] $_.Name })

$buildCsPath = Join-Path $RepoRoot 'Source\SpoutPlugin\SpoutPlugin.Build.cs'
$buildCsText = Get-TextOrEmpty $buildCsPath

$buildPs1Path = Join-Path $RepoRoot 'build.ps1'
$buildPs1Text = Get-TextOrEmpty $buildPs1Path

$formatPolicyPath = Join-Path $RepoRoot 'Source\SpoutPlugin\Private\SpoutFormatPolicy.cpp'
$formatPolicyText = Get-TextOrEmpty $formatPolicyPath

$bpHeaderPath = Join-Path $RepoRoot 'Source\SpoutPlugin\Public\SpoutBPFunctionLibrary.h'
$bpHeaderText = Get-TextOrEmpty $bpHeaderPath

$readmePath = Join-Path $RepoRoot 'README.md'
$readmeText = Get-TextOrEmpty $readmePath

$changelogPath = Join-Path $RepoRoot 'CHANGELOG.md'
$changelogText = Get-TextOrEmpty $changelogPath

# Runtime DLL name, from the delay-load declaration that actually governs it.
$dllName = $null
if ($buildCsText -match 'PublicDelayLoadDLLs\.Add\("([^"]+)"\)')
{
    $dllName = $Matches[1]
}

# Engine matrix, from build.ps1's declared list.
$buildScriptEngines = @()
if ($buildPs1Text -match "(?m)^\s*\`$DeclaredEngineVersions\s*=\s*@\(([^\)]*)\)")
{
    $buildScriptEngines = @([regex]::Matches($Matches[1], "'([0-9]+\.[0-9]+)'") | ForEach-Object { $_.Groups[1].Value })
}
$moduleEngines = Get-TrackDDeclaredEngineVersions

# Supported pixel formats, from the format policy's UE-side table.
$supportedPixelFormats = @([regex]::Matches($formatPolicyText, 'PF_[A-Za-z0-9]+') |
    ForEach-Object { $_.Value } |
    Where-Object { $_ -ne 'PF_Unknown' } |
    Sort-Object -Unique)

# Public Blueprint entry points, from the UFUNCTION declarations.
$blueprintFunctions = @()
$ufunctionMatches = [regex]::Matches(
    $bpHeaderText,
    'UFUNCTION\s*\(\s*BlueprintCallable[\s\S]*?\)\s*[\r\n]+\s*static\s+[A-Za-z0-9_:\*&<>\s]+?\b(\w+)\s*\(')
foreach ($match in $ufunctionMatches)
{
    $blueprintFunctions += $match.Groups[1].Value
}
$blueprintFunctions = @($blueprintFunctions | Sort-Object -Unique)

# Release archive name, from the format string build.ps1 builds it with.
$archiveNamePattern = $null
if ($buildPs1Text -match '\$zipName\s*=\s*"([^"]+)"')
{
    $archiveNamePattern = $Matches[1]
}

Write-TrackDStep "Plugin version    : $pluginVersion"
Write-TrackDStep "Modules           : $($moduleNames -join ', ')"
Write-TrackDStep "Runtime DLL       : $dllName"
Write-TrackDStep "Engine matrix     : $($buildScriptEngines -join ', ')"
Write-TrackDStep "Pixel formats     : $($supportedPixelFormats -join ', ')"
Write-TrackDStep "Blueprint API     : $($blueprintFunctions -join ', ')"

# =================================================================================================
# 1. Source-to-source agreement
# =================================================================================================

Add-Check -Name 'version.descriptor' -Passed (-not [string]::IsNullOrWhiteSpace($pluginVersion)) `
    -Detail "SpoutPlugin.uplugin declares VersionName '$pluginVersion'"

Add-Check -Name 'module.name' -Passed ($moduleNames -contains 'SpoutPlugin') `
    -Detail "descriptor declares the SpoutPlugin module" -Expected 'SpoutPlugin' -Actual ($moduleNames -join ', ')

Add-Check -Name 'dll.name' -Passed ($dllName -eq 'Spout.dll') `
    -Detail "SpoutPlugin.Build.cs delay-loads '$dllName'" -Expected 'Spout.dll' -Actual $dllName

Add-Check -Name 'engines.buildScriptDeclared' -Passed ($buildScriptEngines.Count -gt 0) `
    -Detail "build.ps1 declares $($buildScriptEngines.Count) engine version(s)"

# The two lists are maintained in different files and would drift silently otherwise: build.ps1
# decides which engines a release must cover, TrackD.psm1 decides which ones validation must cover.
$engineListsAgree = ($buildScriptEngines.Count -eq $moduleEngines.Count) -and
    (@(Compare-Object $buildScriptEngines $moduleEngines -SyncWindow 0).Count -eq 0)
Add-Check -Name 'engines.releaseAndValidationAgree' -Passed $engineListsAgree `
    -Detail 'build.ps1 and Scripts/TrackD.psm1 declare the same engine matrix' `
    -Expected ($buildScriptEngines -join ', ') -Actual ($moduleEngines -join ', ')

Add-Check -Name 'formats.policyHasATable' -Passed ($supportedPixelFormats.Count -ge 3) `
    -Detail "the format policy names $($supportedPixelFormats.Count) supported pixel format(s)" `
    -Actual ($supportedPixelFormats -join ', ')

Add-Check -Name 'blueprint.apiDiscovered' -Passed ($blueprintFunctions.Count -ge 5) `
    -Detail "the public header declares $($blueprintFunctions.Count) BlueprintCallable function(s)" `
    -Actual ($blueprintFunctions -join ', ')

Add-Check -Name 'release.archiveNameDerived' -Passed (-not [string]::IsNullOrWhiteSpace($archiveNamePattern)) `
    -Detail "build.ps1 builds archive names from '$archiveNamePattern'"

# =================================================================================================
# 2. Licensing
# =================================================================================================

$licensePath = Join-Path $RepoRoot 'LICENSE'
$noticePath = Join-Path $RepoRoot 'THIRD_PARTY_NOTICES.md'
$filterPath = Join-Path $RepoRoot 'Config\FilterPlugin.ini'
$filterText = Get-TextOrEmpty $filterPath

Add-Check -Name 'licensing.licenseExists' -Passed (Test-Path -LiteralPath $licensePath -PathType Leaf) `
    -Detail 'a root LICENSE exists (AUD-011)'
Add-Check -Name 'licensing.noticesExist' -Passed (Test-Path -LiteralPath $noticePath -PathType Leaf) `
    -Detail 'THIRD_PARTY_NOTICES.md exists (AUD-011)'

foreach ($entry in @('/LICENSE', '/THIRD_PARTY_NOTICES.md', '/README.md', '/ThirdParty/...'))
{
    Add-Check -Name "licensing.filterShips$($entry -replace '[^A-Za-z]','')" `
        -Passed ($filterText -match [regex]::Escape($entry)) `
        -Detail "Config/FilterPlugin.ini ships $entry into every archive" -Expected $entry
}

Add-Check -Name 'licensing.readmeReferencesLicense' -Passed ($readmeText -match '\[LICENSE\]\(LICENSE\)') `
    -Detail 'README links to the LICENSE file'
Add-Check -Name 'licensing.readmeReferencesNotices' -Passed ($readmeText -match 'THIRD_PARTY_NOTICES\.md') `
    -Detail 'README links to THIRD_PARTY_NOTICES.md'

# =================================================================================================
# 3. Documentation reflects the derived facts
# =================================================================================================

$lowestEngine = ($buildScriptEngines | Sort-Object { [version] $_ } | Select-Object -First 1)
$highestEngine = ($buildScriptEngines | Sort-Object { [version] $_ } | Select-Object -Last 1)

Add-Check -Name 'readme.statesEngineRange' `
    -Passed (($readmeText -match [regex]::Escape($lowestEngine)) -and ($readmeText -match [regex]::Escape($highestEngine))) `
    -Detail "README names both ends of the supported range ($lowestEngine and $highestEngine)"

# Every supported format must be findable in the README, in the friendly spelling users see in the
# render-target picker rather than the internal PF_ name.
$formatAliases = @{
    'PF_B8G8R8A8'    = @('B8G8R8A8', 'BGRA8')
    'PF_R8G8B8A8'    = @('R8G8B8A8', 'RGBA8')
    'PF_A2B10G10R10' = @('A2B10G10R10', 'RGB10A2')
    'PF_FloatRGBA'   = @('FloatRGBA', 'RGBA16F')
}
foreach ($format in $supportedPixelFormats)
{
    if (-not $formatAliases.ContainsKey($format)) { continue }
    $aliases = $formatAliases[$format]
    $found = $false
    foreach ($alias in $aliases)
    {
        if ($readmeText -match [regex]::Escape($alias)) { $found = $true; break }
    }
    Add-Check -Name "readme.documentsFormat.$format" -Passed $found `
        -Detail "README mentions $format (as one of: $($aliases -join ', '))"
}

# Every public entry point is checked, so a newly added one cannot ship undocumented.
$blueprintApiDocPath = Join-Path $RepoRoot 'Docs\BLUEPRINT_API.md'
$blueprintApiText = Get-TextOrEmpty $blueprintApiDocPath
$haveApiDoc = -not [string]::IsNullOrWhiteSpace($blueprintApiText)

foreach ($function in $blueprintFunctions)
{
    # Blueprint shows "Close Receiver" for CloseReceiver, so both spellings count.
    $spaced = [regex]::Replace($function, '(?<!^)([A-Z])', ' $1')
    $inReadme = ($readmeText -match [regex]::Escape($function)) -or ($readmeText -match [regex]::Escape($spaced))
    $inApiDoc = $haveApiDoc -and (($blueprintApiText -match [regex]::Escape($function)) -or ($blueprintApiText -match [regex]::Escape($spaced)))

    Add-Check -Name "docs.documentsApi.$function" -Passed ($inReadme -or $inApiDoc) `
        -Detail "$function is documented in README.md or Docs/BLUEPRINT_API.md"
}

# The changelog must have an entry for the version the descriptor declares, or the release ships
# with no record of what changed.
Add-Check -Name 'changelog.hasCurrentVersion' -Passed ($changelogText -match [regex]::Escape($pluginVersion)) `
    -Detail "CHANGELOG.md has an entry for $pluginVersion"

# =================================================================================================
# 4. Known-false claims must be absent
# =================================================================================================

# Historical records quote the false claims deliberately, to record them. Exempting them by name
# rather than by pattern keeps each exemption a visible decision.
$historicalRecords = @(
    'Docs/2026-07-27-AUDIT.md',
    'Docs/2026-07-30-PRODUCTION-READINESS-AUDIT.md',
    'Docs/TRACK_B_DEVIATIONS.md',
    'Docs/TRACK_B_IMPLEMENTATION_PROGRESS.md',
    'Docs/TRACK_B_MANUAL_VALIDATION_CHECKLIST.md',
    'Docs/TRACK_B_SAME_NAME_RECEIVER_REVIEW.md',
    'Docs/TRACK_B_TEST_RESULTS.md',
    'Docs/TRACK_C_DEVIATIONS.md',
    'Docs/TRACK_C_IMPLEMENTATION_PROGRESS.md',
    'Docs/TRACK_C_MANUAL_VALIDATION_CHECKLIST.md',
    'Docs/TRACK_C_TEST_RESULTS.md',
    'Docs/TRACK_D_IMPLEMENTATION_REPORT.md',
    'Docs/TRACK_D_VALIDATION_MATRIX.md',
    'Docs/TRACK_D_PERFORMANCE_BASELINES.md',
    'Docs/TRACK_D_RELEASE_EVIDENCE.md',
    'Docs/TRACK_D_DEVIATIONS.md',
    'TRACK_A_DEVIATIONS.md',
    'TRACK_A_IMPLEMENTATION_PROGRESS.md',
    'TRACK_A_TEST_RESULTS.md',
    'CHANGELOG.md',
    'UPDATES_NOTES.md',
    'Docs/CHANGELOG.md'
)

$forbiddenClaims = @(
    @{ Pattern = 'd3dcompiler';                                    Finding = 'AUD-012'; Detail = 'the d3dcompiler dependency was removed from SpoutPlugin.Build.cs' }
    @{ Pattern = 'EnsureGammaResources';                           Finding = 'AUD-012'; Detail = 'the sender gamma pipeline was removed' }
    @{ Pattern = 'SanitizeGamma';                                  Finding = 'AUD-012'; Detail = 'the sender gamma pipeline was removed' }
    @{ Pattern = 'gamma pipeline';                                 Finding = 'AUD-012'; Detail = 'the reserved sender gamma pipeline no longer exists' }
    @{ Pattern = 'RuntimeDependencies[^.]{0,80}automatically[^.]{0,80}BuildPlugin'; Finding = 'AUD-012'; Detail = 'raw BuildPlugin output does NOT receive Spout.dll from RuntimeDependencies; build.ps1 stages it explicitly' }
    @{ Pattern = 'Intermediate[^.]{0,40}(is |are )?stripped';      Finding = 'AUD-012'; Detail = 'release archives deliberately KEEP Intermediate/Build for no-compiler packaging' }
)

$docFiles = New-Object System.Collections.ArrayList
foreach ($candidate in @(Get-ChildItem -LiteralPath $RepoRoot -Filter '*.md' -File))
{
    [void] $docFiles.Add($candidate.FullName)
}
$docsDir = Join-Path $RepoRoot 'Docs'
if (Test-Path -LiteralPath $docsDir -PathType Container)
{
    foreach ($candidate in @(Get-ChildItem -LiteralPath $docsDir -Filter '*.md' -File -Recurse))
    {
        [void] $docFiles.Add($candidate.FullName)
    }
}

foreach ($claim in $forbiddenClaims)
{
    $offenders = New-Object System.Collections.ArrayList
    foreach ($file in $docFiles)
    {
        $relative = $file.Substring($RepoRoot.Length).TrimStart('\').Replace('\', '/')
        if ($historicalRecords -contains $relative) { continue }

        $text = Get-TextOrEmpty $file
        if ($text -match $claim.Pattern)
        {
            [void] $offenders.Add($relative)
        }
    }

    Add-Check -Name "staleClaim.$($claim.Pattern -replace '[^A-Za-z]','')" -Passed ($offenders.Count -eq 0) `
        -Detail "$($claim.Finding): $($claim.Detail)$(if ($offenders.Count -gt 0) { " - found in: $($offenders -join ', ')" })"
}

# The render-target factory does NOT root its result, and documentation that says otherwise makes
# callers leak or crash.
$rootingClaimOffenders = New-Object System.Collections.ArrayList
foreach ($file in $docFiles)
{
    $relative = $file.Substring($RepoRoot.Length).TrimStart('\').Replace('\', '/')
    if ($historicalRecords -contains $relative) { continue }

    $text = Get-TextOrEmpty $file
    if ($text -match 'CreateTextureRenderTarget2D[\s\S]{0,300}?(AddToRoot|adds it to the root|is rooted|automatically rooted)')
    {
        [void] $rootingClaimOffenders.Add($relative)
    }
}
Add-Check -Name 'staleClaim.renderTargetRooting' -Passed ($rootingClaimOffenders.Count -eq 0) `
    -Detail "AUD-012: CreateTextureRenderTarget2D does NOT root its result$(if ($rootingClaimOffenders.Count -gt 0) { " - claimed in: $($rootingClaimOffenders -join ', ')" })"

# =================================================================================================
# 5. DLL staging layouts must be distinguished
# =================================================================================================

# Documentation must distinguish five distinct DLL layouts. Each is checked by a phrase that only
# the corresponding section would contain.
$layoutMarkers = @(
    @{ Name = 'sourcePlugin';   Pattern = 'ThirdParty/Spout/lib/amd64';                     Detail = 'the source-plugin layout' }
    @{ Name = 'buildPluginRaw'; Pattern = 'RunUAT BuildPlugin|raw .?BuildPlugin';           Detail = 'raw BuildPlugin output' }
    @{ Name = 'releaseArchive'; Pattern = 'release archive|release ZIP|release zip';        Detail = 'the release archive layout' }
    @{ Name = 'projectPlugin';  Pattern = 'Plugins/SpoutPlugin/Binaries/Win64';             Detail = 'the plugin-installed-in-a-project layout' }
    @{ Name = 'packagedGame';   Pattern = '(?i)packaged (game|host|build)';                 Detail = 'the packaged-game layout' }
)

foreach ($marker in $layoutMarkers)
{
    Add-Check -Name "dllStaging.$($marker.Name)" -Passed ($readmeText -match $marker.Pattern) `
        -Detail "README describes $($marker.Detail)"
}

# =================================================================================================
# 6. Generated documentation freshness
# =================================================================================================

$generatedDocs = @()
if (-not $SkipGeneratedDocs)
{
    $generatorPath = Join-Path $RepoRoot 'Docs\build_user_manual.py'
    $manualSourcePath = Join-Path $RepoRoot 'Docs\USER_MANUAL_SOURCE.md'
    $webDataPath = Join-Path $RepoRoot 'Docs\spout_web\assets\spout-data.js'

    if (Test-Path -LiteralPath $generatorPath -PathType Leaf)
    {
        $generatorText = Get-TextOrEmpty $generatorPath

        # The generator must DERIVE the version, not restate it: a hard-coded VERSION = "x.y.z" lets the
        # manual drift from the version the descriptor declares.
        $hardCoded = ($generatorText -match '(?m)^\s*VERSION\s*=\s*["'']\d+\.\d+\.\d+["'']')
        Add-Check -Name 'generatedDocs.generatorDerivesVersion' -Passed (-not $hardCoded) `
            -Detail 'Docs/build_user_manual.py reads the version from SpoutPlugin.uplugin instead of hard-coding it'

        $generatedDocs += [ordered] @{ Path = 'Docs/build_user_manual.py'; Kind = 'generator'; DerivesVersion = (-not $hardCoded) }
    }

    foreach ($generated in @(
        @{ Path = $manualSourcePath;  Relative = 'Docs/USER_MANUAL_SOURCE.md';             Kind = 'manual source' },
        @{ Path = $webDataPath;       Relative = 'Docs/spout_web/assets/spout-data.js';    Kind = 'website data' }))
    {
        if (-not (Test-Path -LiteralPath $generated.Path -PathType Leaf)) { continue }

        $text = Get-TextOrEmpty $generated.Path

        # Any OTHER x.y.z version string that is not the descriptor's is drift.
        $versions = @([regex]::Matches($text, '\b0\.\d+\.\d+\b') | ForEach-Object { $_.Value } | Sort-Object -Unique)
        $stale = @($versions | Where-Object { $_ -ne $pluginVersion })

        Add-Check -Name "generatedDocs.version.$($generated.Kind -replace '[^A-Za-z]','')" -Passed ($stale.Count -eq 0) `
            -Detail "$($generated.Relative) states version $pluginVersion$(if ($stale.Count -gt 0) { " - also found: $($stale -join ', ')" })" `
            -Expected $pluginVersion -Actual ($versions -join ', ')

        $generatedDocs += [ordered] @{ Path = $generated.Relative; Kind = $generated.Kind; StaleVersions = $stale }
    }

    $pdfPath = Join-Path $RepoRoot 'Docs\2026-06-05-SpoutPlugin-User-Manual.pdf'
    if (Test-Path -LiteralPath $pdfPath -PathType Leaf)
    {
        # A rendered PDF cannot be diffed, so freshness is judged the only way that is reliable
        # without parsing it: it must be newer than every input it is generated from.
        $pdfWrite = (Get-Item -LiteralPath $pdfPath).LastWriteTimeUtc
        $inputs = @($generatorPath, $manualSourcePath, $descriptorPath) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
        $newestInput = ($inputs | ForEach-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc } | Sort-Object -Descending | Select-Object -First 1)

        $fresh = ($null -eq $newestInput) -or ($pdfWrite -ge $newestInput)
        Add-Check -Name 'generatedDocs.pdfFreshness' -Passed $fresh `
            -Detail "Docs/2026-06-05-SpoutPlugin-User-Manual.pdf is newer than its inputs (regenerate with: python Docs/build_user_manual.py)" `
            -Expected "newer than $newestInput" -Actual "$pdfWrite"

        $generatedDocs += [ordered] @{ Path = 'Docs/2026-06-05-SpoutPlugin-User-Manual.pdf'; Kind = 'pdf'; Fresh = $fresh }
    }
}

# =================================================================================================
# Report
# =================================================================================================

$report = [ordered] @{
    Schema        = 'spoutplugin-trackd-documentation/1'
    GeneratedUtc  = (Get-Date).ToUniversalTime().ToString('o')
    RepoRoot      = $RepoRoot
    DerivedFacts  = [ordered] @{
        PluginVersion         = $pluginVersion
        ModuleNames           = $moduleNames
        RuntimeDllName        = $dllName
        EngineMatrixFromBuildScript = $buildScriptEngines
        EngineMatrixFromModule      = $moduleEngines
        SupportedPixelFormats = $supportedPixelFormats
        BlueprintFunctions    = $blueprintFunctions
        ArchiveNamePattern    = $archiveNamePattern
    }
    GeneratedDocs = $generatedDocs
    Checks        = $checks.ToArray()
    Problems      = $problems.ToArray()
    Success       = ($problems.Count -eq 0)
}

if (-not [string]::IsNullOrWhiteSpace($ReportPath))
{
    Write-TrackDJson -InputObject $report -Path $ReportPath
    Write-Host ''
    Write-Host "Documentation report: $ReportPath" -ForegroundColor Cyan
}

Write-Host ''
$passedCount = @($checks | Where-Object { $_.Passed }).Count
if ($problems.Count -eq 0)
{
    Write-Host ('=' * 78) -ForegroundColor Green
    Write-Host "DOCUMENTATION VALIDATION PASSED - $passedCount/$($checks.Count) checks" -ForegroundColor Green
    Write-Host ('=' * 78) -ForegroundColor Green
}
else
{
    Write-Host ('=' * 78) -ForegroundColor Red
    Write-Host "DOCUMENTATION VALIDATION FAILED - $($problems.Count) of $($checks.Count) checks" -ForegroundColor Red
    Write-Host ('=' * 78) -ForegroundColor Red
}

exit (Get-TrackDExitCode -FailureCount $problems.Count)
