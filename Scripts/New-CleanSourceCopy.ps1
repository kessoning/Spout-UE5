<#
.SYNOPSIS
    Builds a clean copy of the declared release inputs and runs the release from it.

.DESCRIPTION
    Answers two separate questions that are easy to confuse, and reports them separately.

      1. CLEAN-COPY REPRODUCIBILITY. Does producing a release need anything beyond the inputs
         declared in Scripts/ReleaseSources.psd1? The copy contains only declared files; if the
         release succeeds from it, nothing undeclared in the author's working folder was required.

      2. FRESH-CLONE REPRODUCIBILITY. Is every declared input actually tracked by git? A release
         can be perfectly reproducible from a clean copy of THIS working tree and still be
         impossible to reproduce from a fresh clone, because the inputs were never committed.
         That state is invisible unless the two claims are checked separately.

    Git is used read-only, and only to answer question 2. The copy itself is built by explicit file
    copy from the declared manifest - never by a checkout, a clean or a stash - so this script
    cannot disturb the working tree it is inspecting.

    Both verdicts must pass for the exit code to be 0.

.EXAMPLE
    .\Scripts\New-CleanSourceCopy.ps1 -EvidenceRoot .\Artifacts\TrackD\run

.EXAMPLE
    .\Scripts\New-CleanSourceCopy.ps1 -EngineVersions 5.3,5.8 -EvidenceRoot .\Artifacts\TrackD\run
#>
[CmdletBinding()]
param(
    [string] $RepoRoot = '',

    [Parameter(Mandatory = $true)]
    [string] $EvidenceRoot,

    # Engines to build from the clean copy. Empty means the full declared matrix.
    [string[]] $EngineVersions = @(),

    [string] $EngineRoot = 'D:\UE',
    [string[]] $EnginePaths = @(),

    [string] $WorkRoot = "$env:TEMP\SpoutD",

    # Build the copy and check it, but do not run the release from it. Useful for a fast check of
    # the manifest itself without paying for a full BuildPlugin matrix.
    [switch] $NoBuild,

    [int] $BuildTimeoutSeconds = 10800
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

Import-Module (Join-Path $PSScriptRoot 'TrackD.psm1') -Force

if ([string]::IsNullOrWhiteSpace($RepoRoot)) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

$EngineVersions = Expand-TrackDList $EngineVersions
$EnginePaths = Expand-TrackDList $EnginePaths
if ($EngineVersions.Count -eq 0) { $EngineVersions = Get-TrackDDeclaredEngineVersions }

$cleanSourceEvidence = Join-Path $EvidenceRoot 'clean-source'
foreach ($dir in @($EvidenceRoot, $cleanSourceEvidence))
{
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
}
$cleanSourceEvidence = (Resolve-Path -LiteralPath $cleanSourceEvidence).Path

$manifestPath = Join-Path $PSScriptRoot 'ReleaseSources.psd1'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf))
{
    throw "Release source manifest not found: $manifestPath"
}
$sourceManifest = Import-PowerShellDataFile -LiteralPath $manifestPath

Write-TrackDSection 'Clean-source-copy reproducibility'
Write-TrackDStep "Repository : $RepoRoot"
Write-TrackDStep "Manifest   : $manifestPath (schema $($sourceManifest.SchemaVersion))"
Write-TrackDStep "Engines    : $($EngineVersions -join ', ')"

# -------------------------------------------------------------------------------------------------
# Expand the declared patterns against the working tree
#
# One list, produced once by the module, used for all three things this script does with it: the
# declared-input count, the `git ls-files` comparison and the clean copy. Exclusions are applied
# inside that enumeration, so all three agree by construction. Consulting the exclusion list only
# after the copy exists does not achieve that: generated and machine-local files would still be
# counted and reported as required tracked inputs.
# -------------------------------------------------------------------------------------------------

$inputs = Get-TrackDDeclaredReleaseInputs -RepoRoot $RepoRoot -Manifest $sourceManifest

$declaredFiles = $inputs.Files
$filteredFiles = $inputs.FilteredFiles
$missingPatterns = $inputs.MissingPatterns
$untrackedDeclared = @($inputs.UntrackedFiles)

foreach ($missing in $missingPatterns)
{
    $suffix = if ($missing.ExcludedMatches -gt 0) { " ($($missing.ExcludedMatches) match(es), all excluded)" } else { '' }
    Write-TrackDBad "declared release input '$($missing.Pattern)' matched no file$suffix"
}

Write-TrackDStep "Declared inputs : $($inputs.FileCount) file(s)"
Write-TrackDStep "Git-tracked     : $($inputs.TrackedCount)"
Write-TrackDStep "Untracked       : $($inputs.UntrackedCount)"
Write-TrackDStep "Excluded by rule: $($inputs.FilteredCount) file(s) matched a declared pattern but are not release inputs"

# A declared input that git IGNORES is not a missing commit; it is a manifest error. Committing it
# is impossible without overriding the ignore rules, so reporting it as "untracked, please commit"
# sends the reader to do the wrong thing. Named separately for that reason.
# No @() around the call: these helpers return an array through the comma operator so that an EMPTY
# result survives the pipeline, and wrapping that in @() would produce a one-element array whose
# single element is the empty array.
$ignoredDeclared = Get-TrackDIgnoredPaths -RepoRoot $RepoRoot -Paths ([string[]] @($declaredFiles | ForEach-Object { $_.Path }))

if ($ignoredDeclared.Count -gt 0)
{
    foreach ($ignored in $ignoredDeclared)
    {
        Write-TrackDBad "declared release input '$ignored' is excluded by .gitignore, so it can never be tracked"
    }
    Write-TrackDStep "Resolution: declare it in ReleaseSources.psd1 Excluded, or stop ignoring it in .gitignore."
}

# -------------------------------------------------------------------------------------------------
# Build the clean copy
# -------------------------------------------------------------------------------------------------

$cleanCopyRoot = New-TrackDCleanCopy -RepoRoot $RepoRoot -Files $declaredFiles `
    -Destination (Join-Path $WorkRoot 'clean\R')

Write-TrackDGood "clean copy built at $cleanCopyRoot"

# Nothing from an excluded pattern may have arrived by another route. Checked by walking the copy
# and testing each file against the same matcher the enumeration used, so the check cannot pass by
# looking in the wrong place.
$leaked = Get-TrackDCleanCopyLeaks -Root $cleanCopyRoot -Exclusions @($sourceManifest.Excluded)
foreach ($leak in $leaked)
{
    Write-TrackDBad "excluded '$($leak.Pattern)' leaked into the clean copy: $($leak.Path)"
}
if ($leaked.Count -eq 0)
{
    Write-TrackDGood "no excluded input leaked into the clean copy"
}

# -------------------------------------------------------------------------------------------------
# Run the release from the clean copy
# -------------------------------------------------------------------------------------------------

$releaseResult = $null
$releaseOutputRoot = Join-Path $WorkRoot 'clean\O'
$archives = @()

if (-not $NoBuild)
{
    if (Test-Path -LiteralPath $releaseOutputRoot) { Remove-Item -LiteralPath $releaseOutputRoot -Recurse -Force }
    New-Item -ItemType Directory -Path $releaseOutputRoot -Force | Out-Null

    $cleanBuildScript = Join-Path $cleanCopyRoot 'build.ps1'
    if (-not (Test-Path -LiteralPath $cleanBuildScript -PathType Leaf))
    {
        throw "build.ps1 was not copied into the clean copy; it is not declared as a release input."
    }

    $releaseLog = Join-Path $cleanSourceEvidence 'clean-source-release.log'

    # Deliberately -Partial even for the full set: a full-matrix run from a clean copy still has to
    # name what it built, and the manifest records the requested list either way.
    $releaseArgs = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $cleanBuildScript,
        '-Partial',
        '-EngineVersions', ($EngineVersions -join ','),
        '-OutputRoot', $releaseOutputRoot,
        '-EngineRoot', $EngineRoot,
        '-PluginRoot', $cleanCopyRoot
    )
    foreach ($pair in $EnginePaths) { $releaseArgs += @('-EnginePaths', $pair) }

    Write-TrackDStep "Running the release from the clean copy"
    $releaseResult = Invoke-TrackDProcess -FilePath 'pwsh' -ArgumentList $releaseArgs `
        -StdOutPath $releaseLog -TimeoutSeconds $BuildTimeoutSeconds -WorkingDirectory $cleanCopyRoot

    if ($releaseResult.ExitCode -eq 0)
    {
        Write-TrackDGood "the release completed from declared inputs alone"
    }
    else
    {
        Write-TrackDBad "the release failed from the clean copy (exit $($releaseResult.ExitCode)); see $releaseLog"
    }

    $archives = @(Get-ChildItem -LiteralPath $releaseOutputRoot -Filter '*.zip' -File -Recurse -ErrorAction SilentlyContinue |
        ForEach-Object {
            [ordered] @{
                Path      = $_.FullName.Substring($releaseOutputRoot.Length).TrimStart('\').Replace('\', '/')
                SizeBytes = $_.Length
                Sha256    = Get-TrackDSha256 $_.FullName
            }
        })

    # Copy the release manifests the clean-copy build produced, so the evidence tree holds them
    # rather than only a temp directory that the next run deletes.
    foreach ($generated in (Get-ChildItem -LiteralPath $releaseOutputRoot -Filter 'release-manifest-*.json' -File -Recurse -ErrorAction SilentlyContinue))
    {
        Copy-Item -LiteralPath $generated.FullName -Destination (Join-Path $cleanSourceEvidence $generated.Name) -Force
    }
}
else
{
    Write-TrackDWarn "-NoBuild: the clean copy was built and checked, but no release was produced from it"
}

# -------------------------------------------------------------------------------------------------
# Verdicts
# -------------------------------------------------------------------------------------------------

$problems = New-Object System.Collections.ArrayList

if ($missingPatterns.Count -gt 0)
{
    [void] $problems.Add("$($missingPatterns.Count) declared release input pattern(s) matched no file")
}
if ($leaked.Count -gt 0)
{
    [void] $problems.Add("$($leaked.Count) excluded input(s) leaked into the clean copy")
}

$cleanCopyReproducible = $null
if (-not $NoBuild)
{
    $cleanCopyReproducible = ($releaseResult.ExitCode -eq 0)
    if (-not $cleanCopyReproducible)
    {
        [void] $problems.Add('the release could not be produced from the declared inputs alone')
    }
}

$freshCloneReproducible = ($untrackedDeclared.Count -eq 0)
if (-not $freshCloneReproducible)
{
    [void] $problems.Add("$($untrackedDeclared.Count) declared release input(s) are not tracked by git, so a fresh clone of HEAD cannot reproduce this release")
}

if ($ignoredDeclared.Count -gt 0)
{
    [void] $problems.Add("$($ignoredDeclared.Count) declared release input(s) are excluded by .gitignore and can never be tracked; declare them in ReleaseSources.psd1 Excluded, or stop ignoring them")
}

$report = [ordered] @{
    Schema       = 'spoutplugin-trackd-clean-source/1'
    GeneratedUtc = (Get-Date).ToUniversalTime().ToString('o')
    RepoRoot     = $RepoRoot
    ManifestPath = $manifestPath
    ManifestSchemaVersion = $sourceManifest.SchemaVersion
    CleanCopyRoot = $cleanCopyRoot
    ReleaseOutputRoot = $releaseOutputRoot
    Git          = Get-TrackDGitInfo $RepoRoot
    Host         = Get-TrackDHostSummary
    RequestedEngines = $EngineVersions

    Verdicts     = [ordered] @{
        CleanCopyReproducible  = $cleanCopyReproducible
        FreshCloneReproducible = $freshCloneReproducible
        NoExcludedInputLeaked  = ($leaked.Count -eq 0)
        AllDeclaredPatternsMatched = ($missingPatterns.Count -eq 0)
        NoGitIgnoredInputDeclared  = ($ignoredDeclared.Count -eq 0)
    }

    InputManifest = [ordered] @{
        FileCount      = $inputs.FileCount
        TrackedCount   = $inputs.TrackedCount
        UntrackedCount = $inputs.UntrackedCount
        FilteredCount  = $inputs.FilteredCount
        Files          = $declaredFiles
    }

    # What a declared pattern matched but an exclusion removed, with the rule that removed it. Kept
    # in the report so an excluded file is visibly excluded rather than silently missing.
    ExcludedInputs = $filteredFiles

    UntrackedDeclaredInputs = @($untrackedDeclared | ForEach-Object { $_.Path })
    GitIgnoredDeclaredInputs = $ignoredDeclared
    MissingPatterns = $missingPatterns
    LeakedExclusions = $leaked

    OutputManifest = [ordered] @{
        ReleaseExitCode = if ($releaseResult) { $releaseResult.ExitCode } else { $null }
        ReleaseLog      = if ($releaseResult) { $releaseResult.StdOutPath } else { $null }
        ArchiveCount    = $archives.Count
        Archives        = $archives
    }

    Problems = $problems.ToArray()
    Success  = ($problems.Count -eq 0)
}

$reportPath = Join-Path $cleanSourceEvidence 'clean-source-report.json'
Write-TrackDJson -InputObject $report -Path $reportPath

Write-Host ''
Write-Host "Clean-copy reproducible  : $(if ($null -eq $cleanCopyReproducible) { 'not evaluated (-NoBuild)' } elseif ($cleanCopyReproducible) { 'YES' } else { 'NO' })"
Write-Host "Fresh-clone reproducible : $(if ($freshCloneReproducible) { 'YES' } else { "NO - $($untrackedDeclared.Count) declared input(s) are untracked" })"

if (-not $freshCloneReproducible)
{
    $ignoredSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($ignored in $ignoredDeclared) { [void] $ignoredSet.Add($ignored) }

    Write-Host ''
    Write-TrackDWarn 'These declared release inputs exist in the working tree but are not tracked by git:'
    foreach ($file in ($untrackedDeclared | Select-Object -First 25))
    {
        # Two different fixes, so they are never printed as one. Committing a git-ignored file is
        # not possible without overriding the ignore rules, and should not be attempted.
        $note = if ($ignoredSet.Contains($file.Path)) { '   <- git-ignored; this one needs a manifest exclusion, NOT a commit' } else { '' }
        Write-Host "    $($file.Path)$note" -ForegroundColor Yellow
    }
    if ($untrackedDeclared.Count -gt 25)
    {
        Write-Host "    ... and $($untrackedDeclared.Count - 25) more (full list in $reportPath)" -ForegroundColor Yellow
    }
    Write-Host ''
    Write-Host '  Resolution: commit them. Until they are tracked, a clone of HEAD builds a different' -ForegroundColor Yellow
    Write-Host '  plugin from the one validated here.' -ForegroundColor Yellow
}

Write-Host ''
Write-Host "Clean-source report: $reportPath" -ForegroundColor Cyan

if ($problems.Count -eq 0)
{
    Write-Host ('=' * 78) -ForegroundColor Green
    Write-Host 'CLEAN-SOURCE VALIDATION PASSED' -ForegroundColor Green
    Write-Host ('=' * 78) -ForegroundColor Green
}
else
{
    Write-Host ('=' * 78) -ForegroundColor Red
    Write-Host 'CLEAN-SOURCE VALIDATION FAILED' -ForegroundColor Red
    foreach ($problem in $problems) { Write-Host "  - $problem" -ForegroundColor Red }
    Write-Host ('=' * 78) -ForegroundColor Red
}

exit (Get-TrackDExitCode -FailureCount $problems.Count)
