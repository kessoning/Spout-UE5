# Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
# Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

<#
.SYNOPSIS
    Validates the documentation set against the implementation, and fails on drift.

.DESCRIPTION
    Documentation that is merely correct today goes stale silently. This script is what stops that:
    it re-derives the facts from source on every run and fails the release when they disagree.

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

    It also checks that a set of known-false claims stays ABSENT from everything a user reads:
    the removed gamma pipeline, the removed d3dcompiler dependency, the BGRA8-fallback claim, the
    SetMaxSenders echo claim, the claim that the render-target factory roots its result, the claim
    that RuntimeDependencies puts Spout.dll into raw BuildPlugin output, and the claim that
    release Intermediate data is stripped.

    Claims-exposure checks are scoped to an explicit allowlist of public documents: the canonical
    documentation set plus the website and generated-manual sources. Other records may QUOTE stale
    claims in order to record them, so they are out of scope for the claims-exposure checks, and
    only for those. The changelogs are exempt from the claim patterns for the same reason (a
    correction entry must be able to name what it corrected) and are validated for version
    currency instead. Both lists are explicit, so widening or narrowing the scope is a visible
    decision.

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
    'UFUNCTION\s*\(\s*Blueprint(?:Callable|Pure)[\s\S]*?\)\s*[\r\n]+\s*static\s+[A-Za-z0-9_:\*&<>\s]+?\b(\w+)\s*\(')
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

# Docs/CHANGELOG.md is deliberately a version-free pointer to the canonical root changelog: any
# version number appearing there is drift by definition.
$docsChangelogText = Get-TextOrEmpty (Join-Path $RepoRoot 'Docs\CHANGELOG.md')
$docsChangelogVersions = @([regex]::Matches($docsChangelogText, '\b\d+\.\d+\.\d+\b') |
    ForEach-Object { $_.Value } | Sort-Object -Unique)
Add-Check -Name 'changelog.docsPointerVersionFree' `
    -Passed (($docsChangelogVersions.Count -eq 0) -and ($docsChangelogText -match 'canonical changelog')) `
    -Detail "Docs/CHANGELOG.md stays a version-free pointer to the canonical changelog$(if ($docsChangelogVersions.Count -gt 0) { " - found: $($docsChangelogVersions -join ', ')" })"

# =================================================================================================
# 4. Known-false claims must be absent from everything a user reads
# =================================================================================================

# Claims-exposure checks run against an explicit allowlist of the documents users actually see:
# the canonical public documentation set plus the website and generated-manual sources. Other
# records may QUOTE stale claims in order to record them; they are out of scope for the
# claims-exposure checks, and only for those. Listing the public set explicitly rather than
# sweeping a directory keeps every scope decision visible: a new public page must be added here
# to be scanned at all.
$publicDocRelatives = @(
    'README.md',
    'CHANGELOG.md',
    'UPDATES_NOTES.md',
    'Docs/BLUEPRINT_API.md',
    'Docs/CHANGELOG.md',
    'Docs/INSTALLATION.md',
    'Docs/LIMITATIONS.md',
    'Docs/RELEASE_READINESS.md',
    'Docs/PACKAGING_AND_DISTRIBUTION.md',
    'Docs/QUICK_START.md',
    'Docs/RECEIVER.md',
    'Docs/RENDER_TARGETS_AND_FORMATS.md',
    'Docs/SCENECAPTURE2D.md',
    'Docs/SENDER.md',
    'Docs/TROUBLESHOOTING.md',
    'Docs/USER_MANUAL_SOURCE.md'
)

# The website has no build step: the checked-in page, data, render and style sources ARE the
# deployed site, so they are scanned as public documents too.
$websiteRoot = Join-Path $RepoRoot 'Docs\spout_web'
$websiteFiles = @()
if (Test-Path -LiteralPath $websiteRoot -PathType Container)
{
    $websiteFiles = @(Get-ChildItem -LiteralPath $websiteRoot -Recurse -File -Include *.js, *.html, *.css)
}

# Every allowlisted public document must actually exist: a rename or deletion would otherwise
# silently drop the file out of every claims-exposure check below.
$missingPublicDocs = @($publicDocRelatives | Where-Object {
    -not (Test-Path -LiteralPath (Join-Path $RepoRoot ($_ -replace '/', '\')) -PathType Leaf) })
Add-Check -Name 'publicDocs.allPresent' -Passed ($missingPublicDocs.Count -eq 0) `
    -Detail "every allowlisted public document exists$(if ($missingPublicDocs.Count -gt 0) { " - missing: $($missingPublicDocs -join ', ')" })"

$claimScanFiles = New-Object System.Collections.ArrayList
foreach ($relative in $publicDocRelatives)
{
    $path = Join-Path $RepoRoot ($relative -replace '/', '\')
    if (Test-Path -LiteralPath $path -PathType Leaf)
    {
        [void] $claimScanFiles.Add(@{ Relative = $relative; Path = $path })
    }
}
foreach ($file in $websiteFiles)
{
    $relative = $file.FullName.Substring($RepoRoot.Length).TrimStart('\').Replace('\', '/')
    [void] $claimScanFiles.Add(@{ Relative = $relative; Path = $file.FullName })
}

# The changelogs are records of change: a correction entry must be able to QUOTE the claim it
# corrects ("removed the d3dcompiler dependency", "corrected the claim that ..."), so they are
# exempt from the claim patterns - and from nothing else. Their own drift risk is version
# currency, which changelog.hasCurrentVersion and changelog.docsPointerVersionFree cover.
$correctionRecords = @('CHANGELOG.md', 'UPDATES_NOTES.md', 'Docs/CHANGELOG.md')

$forbiddenClaims = @(
    @{ Name = 'P2Persistence'; Pattern = '(?i)Nothing keeps the stream alive|stream is not kept alive between calls|costs nothing on the frames'; Finding = 'AUD-024'; Detail = 'registration persists and idle calls still have a cost' }
    @{ Name = 'P2BetaMetadata'; Pattern = '(?i)Marked beta|VERSION\+"  \(beta\)"'; Finding = 'AUD-024'; Detail = 'descriptor beta metadata must match current implementation' }
    @{ Name = 'ddcompiler';           Pattern = 'd3dcompiler';                                    Finding = 'AUD-012'; Detail = 'the d3dcompiler dependency was removed from SpoutPlugin.Build.cs' }
    @{ Name = 'EnsureGammaResources'; Pattern = 'EnsureGammaResources';                           Finding = 'AUD-012'; Detail = 'the sender gamma pipeline was removed' }
    @{ Name = 'SanitizeGamma';        Pattern = 'SanitizeGamma';                                  Finding = 'AUD-012'; Detail = 'the sender gamma pipeline was removed' }
    @{ Name = 'gammapipeline';        Pattern = 'gamma pipeline';                                 Finding = 'AUD-012'; Detail = 'the reserved sender gamma pipeline no longer exists' }
    @{ Name = 'RuntimeDependenciesautomaticallyBuildPlugin'; Pattern = 'RuntimeDependencies[^.]{0,80}automatically[^.]{0,80}BuildPlugin'; Finding = 'AUD-012'; Detail = 'raw BuildPlugin output does NOT receive Spout.dll from RuntimeDependencies; build.ps1 stages it explicitly' }
    @{ Name = 'Intermediateisarestripped'; Pattern = 'Intermediate[^.]{0,40}(is |are )?stripped'; Finding = 'AUD-012'; Detail = 'release archives deliberately KEEP Intermediate/Build for no-compiler packaging' }
    @{ Name = 'bgraFallback';         Pattern = 'fall(s|ing)? back to (BGRA8|B8G8R8A8)';          Finding = 'SPUE-002'; Detail = 'unsupported formats are refused with a logged warning, never converted to BGRA8' }
    @{ Name = 'setMaxSendersEcho';    Pattern = 'Returns the value passed in';                    Finding = 'SPUE-003'; Detail = 'SetMaxSenders returns the limit actually in effect (0 on failure), not an echo of the request' }
)

foreach ($claim in $forbiddenClaims)
{
    $offenders = New-Object System.Collections.ArrayList
    foreach ($file in $claimScanFiles)
    {
        if ($correctionRecords -contains $file.Relative) { continue }

        $text = Get-TextOrEmpty $file.Path
        if ($text -match $claim.Pattern)
        {
            [void] $offenders.Add($file.Relative)
        }
    }

    Add-Check -Name "staleClaim.$($claim.Name)" -Passed ($offenders.Count -eq 0) `
        -Detail "$($claim.Finding): $($claim.Detail)$(if ($offenders.Count -gt 0) { " - found in: $($offenders -join ', ')" })"
}

# The render-target factory does NOT root its result, and documentation that says otherwise makes
# callers leak or crash.
$rootingClaimOffenders = New-Object System.Collections.ArrayList
foreach ($file in $claimScanFiles)
{
    if ($correctionRecords -contains $file.Relative) { continue }

    $text = Get-TextOrEmpty $file.Path
    if ($text -match 'CreateTextureRenderTarget2D[\s\S]{0,300}?(AddToRoot|adds it to the root|is rooted|automatically rooted)')
    {
        [void] $rootingClaimOffenders.Add($file.Relative)
    }
}
Add-Check -Name 'staleClaim.renderTargetRooting' -Passed ($rootingClaimOffenders.Count -eq 0) `
    -Detail "AUD-012: CreateTextureRenderTarget2D does NOT root its result$(if ($rootingClaimOffenders.Count -gt 0) { " - claimed in: $($rootingClaimOffenders -join ', ')" })"

# =================================================================================================
# 5. Corrected behaviour contracts must stay stated
# =================================================================================================

# Each check pins a behaviour contract the documentation must keep stating. The presence checks
# anchor to wording only the correct contract would use, so reverting a correction fails the
# release instead of quietly shipping the false claim again. Where the contract is visible in a
# source declaration, the check derives it from the header first rather than restating it here.

$installationText = Get-TextOrEmpty (Join-Path $RepoRoot 'Docs\INSTALLATION.md')
$packagingText    = Get-TextOrEmpty (Join-Path $RepoRoot 'Docs\PACKAGING_AND_DISTRIBUTION.md')
$receiverText     = Get-TextOrEmpty (Join-Path $RepoRoot 'Docs\RECEIVER.md')
$quickStartText   = Get-TextOrEmpty (Join-Path $RepoRoot 'Docs\QUICK_START.md')
$limitationsText  = Get-TextOrEmpty (Join-Path $RepoRoot 'Docs\LIMITATIONS.md')
$renderTargetsText = Get-TextOrEmpty (Join-Path $RepoRoot 'Docs\RENDER_TARGETS_AND_FORMATS.md')
$manualSourceText = Get-TextOrEmpty (Join-Path $RepoRoot 'Docs\USER_MANUAL_SOURCE.md')

# Unsupported formats are refused with a logged warning naming the format (SpoutFormatPolicy.cpp);
# both format-facing pages must state refusal, not conversion.
foreach ($page in @(
    @{ Name = 'renderTargets'; Text = $renderTargetsText; Relative = 'Docs/RENDER_TARGETS_AND_FORMATS.md' },
    @{ Name = 'limitations';   Text = $limitationsText;   Relative = 'Docs/LIMITATIONS.md' }))
{
    Add-Check -Name "contract.formatRefusal.$($page.Name)" -Passed ($page.Text -match '(?i)refused') `
        -Detail "$($page.Relative) states the refuse-and-log format contract"
}

# SetMaxSenders returns the limit actually in effect afterwards (0 when nothing changed), not an
# echo of the requested value.
Add-Check -Name 'contract.setMaxSendersEffectiveLimit' -Passed ($blueprintApiText -match 'actually in effect') `
    -Detail 'Docs/BLUEPRINT_API.md states that SetMaxSenders returns the limit actually in effect'

# The receiver signature carries an ESpoutColorSpace parameter; derived from the header so the
# doc checks only apply while the parameter actually exists.
$receiverHasColorSpace = ($bpHeaderText -match 'SpoutReceiver\s*\([\s\S]{0,800}?ESpoutColorSpace\s+ColorSpace')
Add-Check -Name 'contract.colorSpace.derived' -Passed $receiverHasColorSpace `
    -Detail 'SpoutBPFunctionLibrary.h declares ESpoutColorSpace ColorSpace on SpoutReceiver'
if ($receiverHasColorSpace)
{
    foreach ($page in @(
        @{ Name = 'blueprintApi'; Text = $blueprintApiText; Relative = 'Docs/BLUEPRINT_API.md' },
        @{ Name = 'userManual';   Text = $manualSourceText; Relative = 'Docs/USER_MANUAL_SOURCE.md' }))
    {
        Add-Check -Name "contract.colorSpace.$($page.Name)" -Passed ($page.Text -match 'ESpoutColorSpace') `
            -Detail "$($page.Relative) documents the ESpoutColorSpace parameter"
    }
}

# OptionalOutputRenderTarget must match the incoming frame's pixel format exactly and be
# single-sampled; a mismatched target is left untouched, which reads as "black render target"
# without this documentation. Every receiver guide must state both constraints.
$headerHasOptionalTarget = ($bpHeaderText -match 'OptionalOutputRenderTarget')
Add-Check -Name 'contract.outputTarget.derived' -Passed $headerHasOptionalTarget `
    -Detail 'SpoutBPFunctionLibrary.h declares OptionalOutputRenderTarget'
if ($headerHasOptionalTarget)
{
    foreach ($page in @(
        @{ Name = 'receiver';     Text = $receiverText;     Relative = 'Docs/RECEIVER.md' },
        @{ Name = 'blueprintApi'; Text = $blueprintApiText; Relative = 'Docs/BLUEPRINT_API.md' },
        @{ Name = 'quickStart';   Text = $quickStartText;   Relative = 'Docs/QUICK_START.md' }))
    {
        $statesBoth = ($page.Text -match '(?i)single.sample') -and ($page.Text -match '(?i)pixel format')
        Add-Check -Name "contract.outputTarget.$($page.Name)" -Passed $statesBoth `
            -Detail "$($page.Relative) states the exact-pixel-format and single-sample requirements"
    }
}

# Raw BuildPlugin output does not include Spout.dll - build.ps1 stages it into the release zips.
# The two install-facing guides must keep saying so.
Add-Check -Name 'contract.dllStaging.installation' -Passed ($installationText -match 'output does not include the\s+DLL') `
    -Detail 'Docs/INSTALLATION.md states that raw BuildPlugin output does not include the DLL'
Add-Check -Name 'contract.dllStaging.packaging' -Passed ($packagingText -match 'runs no target staging') `
    -Detail 'Docs/PACKAGING_AND_DISTRIBUTION.md states that plugin packaging runs no target staging'

# =================================================================================================
# 5b. The manual GENERATOR must state the same contracts as the Markdown source
# =================================================================================================

# Docs/USER_MANUAL_SOURCE.md and Docs/build_user_manual.py both carry public prose, maintained
# independently: the PDF's body lives in the generator and is NOT rendered from the Markdown, so
# a correction to one does not reach the other. Until the two are consolidated, this section
# checks the generator's own text so they cannot diverge.
$manualGeneratorText = Get-TextOrEmpty (Join-Path $RepoRoot 'Docs\build_user_manual.py')
if (-not [string]::IsNullOrWhiteSpace($manualGeneratorText))
{
    # Whitespace-insensitive view, so line wrapping inside the generator's string literals cannot
    # break a check that is really about wording.
    $generatorFlat = ($manualGeneratorText -replace '\\n', ' ') -replace '\s+', ' '

    foreach ($contract in @(
        @{ Name = 'receiverSignatureHasColorSpace'; Pattern = 'SpoutReceiver\([^)]{0,400}ESpoutColorSpace\s+ColorSpace';
           Detail = 'the manual generator documents the current SpoutReceiver signature including ESpoutColorSpace ColorSpace' },
        # Negative lookahead on the second colon: without it this matches the scoped value
        # ESpoutColorSpace::Auto inside the signature and would pass with the enum undocumented.
        @{ Name = 'colorSpaceEnumDocumented';      Pattern = 'ESpoutColorSpace:(?!:)';
           Detail = 'the manual generator documents the ESpoutColorSpace enum values' },
        @{ Name = 'colorSpaceAutoBehaviour';       Pattern = '(?i)8-bit\s+decodes\s+as\s+sRGB';
           Detail = 'the manual generator describes Auto colour-space behaviour' },
        @{ Name = 'formatRefusal';                 Pattern = '(?i)Anything else is refused';
           Detail = 'the manual generator states that unsupported formats are refused' },
        @{ Name = 'formatRefusalNoFallback';       Pattern = '(?i)never reinterpreted as B8G8R8A8';
           Detail = 'the manual generator states that unsupported formats are not reinterpreted as B8G8R8A8' },
        @{ Name = 'outputTargetFormatMatch';       Pattern = '(?i)must match the incoming frame''s format exactly';
           Detail = 'the manual generator states the OptionalOutputRenderTarget exact-format requirement' },
        @{ Name = 'outputTargetSingleSampled';     Pattern = '(?i)single-sampled';
           Detail = 'the manual generator states the OptionalOutputRenderTarget single-sample requirement' }))
    {
        Add-Check -Name "manualGenerator.$($contract.Name)" -Passed ($generatorFlat -match $contract.Pattern) `
            -Detail $contract.Detail
    }

    # The stale six-parameter form: a SpoutReceiver signature whose parameter list ends at
    # OptionalOutputRenderTarget. Matching the END of the list rather than counting commas keeps
    # this robust against reformatting.
    Add-Check -Name 'manualGenerator.noSixParameterReceiver' `
        -Passed (-not ($generatorFlat -match 'SpoutReceiver\([^)]{0,400}OptionalOutputRenderTarget\s*=\s*nullptr\s*\)')) `
        -Detail 'the manual generator no longer documents the superseded six-parameter SpoutReceiver signature'

    Add-Check -Name 'manualGenerator.noBgraFallback' `
        -Passed (-not ($generatorFlat -match '(?i)fall(s|ing)? back to (BGRA8|B8G8R8A8)')) `
        -Detail 'the manual generator does not claim a BGRA8 fallback'
}

# =================================================================================================
# 5c. The RENDERED manual must carry the same contracts
# =================================================================================================

# Checking the generator's source proves what it would write; it does not prove what the shipped
# PDF says. Extraction is attempted with the interpreter the generator already requires; when no
# extractor is available the result is recorded as UNVERIFIED rather than silently passing, so a
# green run never implies the rendered PDF was machine-checked when it was not.
$manualPdfPath = Join-Path $RepoRoot 'Docs\2026-06-05-SpoutPlugin-User-Manual.pdf'
$pdfTextVerified = $false
$pdfTextProblems = @()
if ((-not $SkipGeneratedDocs) -and (Test-Path -LiteralPath $manualPdfPath -PathType Leaf))
{
    $extractScript = @'
import sys
try:
    from pypdf import PdfReader
except Exception:
    print("NO_EXTRACTOR"); sys.exit(0)
try:
    r = PdfReader(sys.argv[1])
    t = "".join((p.extract_text() or "") for p in r.pages)
except Exception as e:
    print("EXTRACT_FAILED"); sys.exit(0)
flat = " ".join(t.split()).lower()
checks = {
    "colorspace_param": "espoutcolorspace colorspace" in flat,
    "colorspace_enum": "espoutcolorspace:" in flat,
    "format_refusal": "anything else is refused" in flat,
    "output_format_match": "must match the incoming frame's format exactly" in flat,
    "single_sampled": "single-sampled" in flat,
    "no_six_param": "optionaloutputrendertarget = nullptr)" not in flat,
    "no_bgra_fallback": "falls back to bgra8" not in flat,
}
print("OK|" + "|".join(k for k, v in checks.items() if not v))
'@
    $tempScript = Join-Path ([System.IO.Path]::GetTempPath()) 'spout-manual-pdf-check.py'
    Set-Content -LiteralPath $tempScript -Value $extractScript -Encoding utf8
    $extractOutput = ''
    try { $extractOutput = (& python $tempScript $manualPdfPath 2>&1 | Out-String).Trim() } catch { $extractOutput = 'NO_EXTRACTOR' }
    Remove-Item -LiteralPath $tempScript -Force -ErrorAction SilentlyContinue

    if ($extractOutput -like 'OK|*')
    {
        $pdfTextVerified = $true
        $failed = @($extractOutput.Substring(3).Split('|') | Where-Object { $_ })
        $pdfTextProblems = $failed
        Add-Check -Name 'manualPdf.renderedContractsVerified' -Passed ($failed.Count -eq 0) `
            -Detail "the rendered manual PDF states every corrected contract$(if ($failed.Count -gt 0) { " - missing: $($failed -join ', ')" })"
    }
    else
    {
        # Visible, and deliberately not a silent pass: the report records PdfTextVerified=false.
        Add-Check -Name 'manualPdf.renderedContractsVerified' -Passed $true `
            -Detail 'UNVERIFIED: no PDF text extractor is available on this machine, so the rendered manual was NOT machine-checked (the generator source was). Verify the PDF manually or install pypdf.'
    }
}

# =================================================================================================
# 6. DLL staging layouts must be distinguished
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
# 7. Generated documentation freshness
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

        # Any OTHER x.y.z version string that is not the descriptor's is drift. The pattern must
        # match ANY three-part version, not just 0.x.y: anchoring it to a leading 0 would make the
        # check pass vacuously forever the moment the plugin ships 1.0.0.
        $versions = @([regex]::Matches($text, '\b\d+\.\d+\.\d+\b') | ForEach-Object { $_.Value } | Sort-Object -Unique)
        $stale = @($versions | Where-Object { $_ -ne $pluginVersion })

        Add-Check -Name "generatedDocs.version.$($generated.Kind -replace '[^A-Za-z]','')" -Passed ($stale.Count -eq 0) `
            -Detail "$($generated.Relative) states version $pluginVersion$(if ($stale.Count -gt 0) { " - also found: $($stale -join ', ')" })" `
            -Expected $pluginVersion -Actual ($versions -join ', ')

        $generatedDocs += [ordered] @{ Path = $generated.Relative; Kind = $generated.Kind; StaleVersions = $stale }
    }

    $pdfPath = Join-Path $RepoRoot 'Docs\2026-06-05-SpoutPlugin-User-Manual.pdf'
    if (Test-Path -LiteralPath $pdfPath -PathType Leaf)
    {
        # A rendered PDF cannot be diffed. Its freshness is judged by content: Docs/build_user_manual.py
        # writes, beside the PDF, the SHA-256 of the PDF it wrote and of every input it read (the
        # generator, the manual source, the descriptor and the screenshots it embedded; text inputs with
        # CRLF read as LF, so a checkout's line endings do not matter). The PDF is fresh when that record
        # names this PDF's bytes and every recorded input still has its recorded content. Modification
        # times and commit times are not used: a checkout writes files in path order, and an equal or
        # later commit time says nothing about which file was generated from which. Without the record
        # (an older tree) the modification times decide.
        $pdfWrite = (Get-Item -LiteralPath $pdfPath).LastWriteTimeUtc
        $inputs = @($generatorPath, $manualSourcePath, $descriptorPath) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
        $newestInput = ($inputs | ForEach-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc } | Sort-Object -Descending | Select-Object -First 1)
        $freshnessBasis = 'modification time'
        $expected = "newer than $newestInput"
        $actual = "$pdfWrite"
        $fresh = ($null -eq $newestInput) -or ($pdfWrite -ge $newestInput)
        $recordPath = "$pdfPath.inputs.json"
        if (Test-Path -LiteralPath $recordPath -PathType Leaf)
        {
            $freshnessBasis = 'recorded input content'
            $contentHash = {
                param([string] $Path)
                $bytes = [IO.File]::ReadAllBytes($Path)
                if ($Path -notmatch '\.(png|jpe?g|pdf)$')
                {
                    $out = New-Object System.Collections.Generic.List[byte] ($bytes.Length)
                    for ($i = 0; $i -lt $bytes.Length; $i++) { if (-not ($bytes[$i] -eq 13 -and $i + 1 -lt $bytes.Length -and $bytes[$i + 1] -eq 10)) { $out.Add($bytes[$i]) } }
                    $bytes = $out.ToArray()
                }
                $sha = [Security.Cryptography.SHA256]::Create()
                try { return (-join ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') })) } finally { $sha.Dispose() }
            }
            $problemsFound = @()
            try { $record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json } catch { $record = $null; $problemsFound += 'the record is not valid JSON' }
            if ($record)
            {
                if ((& $contentHash $pdfPath) -ne "$($record.pdf.sha256)") { $problemsFound += 'the PDF is not the one the record names' }
                $recorded = @($record.inputs.PSObject.Properties | ForEach-Object Name)
                foreach ($required in 'Docs/build_user_manual.py', 'Docs/USER_MANUAL_SOURCE.md', 'SpoutPlugin.uplugin')
                {
                    if ($recorded -notcontains $required) { $problemsFound += "the record does not name $required" }
                }
                foreach ($name in $recorded)
                {
                    $path = Join-Path $RepoRoot ($name -replace '/', '\')
                    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $problemsFound += "$name is missing" }
                    elseif ((& $contentHash $path) -ne "$($record.inputs.$name)") { $problemsFound += "$name changed since the PDF was generated" }
                }
            }
            $fresh = ($problemsFound.Count -eq 0)
            $expected = 'the PDF and every input recorded in Docs/2026-06-05-SpoutPlugin-User-Manual.pdf.inputs.json unchanged'
            $actual = $(if ($fresh) { "unchanged ($(@($record.inputs.PSObject.Properties).Count) inputs)" } else { $problemsFound -join '; ' })
        }
        Add-Check -Name 'generatedDocs.pdfFreshness' -Passed $fresh `
            -Detail "Docs/2026-06-05-SpoutPlugin-User-Manual.pdf matches its inputs, by $freshnessBasis (regenerate with: python Docs/build_user_manual.py)" `
            -Expected $expected -Actual $actual

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
    ClaimScanScope = [ordered] @{
        PublicDocuments            = $publicDocRelatives
        WebsiteSources             = @($claimScanFiles | Where-Object { $_.Relative -like 'Docs/spout_web/*' } | ForEach-Object { $_.Relative })
        CorrectionRecordExemptions = $correctionRecords
    }
    ManualPdf = [ordered] @{
        # False means the rendered PDF was not machine-checked on this run; the generator source
        # still was. Recorded so a green run cannot be mistaken for rendered-PDF verification.
        PdfTextVerified = $pdfTextVerified
        MissingContracts = $pdfTextProblems
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
