<#
.SYNOPSIS
    Builds, stages, validates and hashes SpoutPlugin release archives for one or more Unreal
    Engine versions.

.DESCRIPTION
    Every path is a parameter, the release version is read from SpoutPlugin.uplugin, and every
    failure terminates the run with a nonzero exit code. A success banner is printed only when
    every requested engine and every generated artifact passed validation.

    Two modes:
      -FullMatrix  Every engine in $DeclaredEngineVersions must be installed and must pass.
                   A missing engine is fatal.
      -Partial     Only the engines named by -EngineVersions are expected. The manifest records
                   the release as partial and lists the omitted versions.

    Exactly one of -FullMatrix or -Partial is required, so a partial release can never be
    produced by accident.

    Every generated ZIP is reopened and its contents validated: a single plugin root, the
    descriptor with the expected version, Source and Build.cs, the module binary, Spout.dll with
    a hash matching the repository copy, the precompiled Intermediate/Build metadata, LICENSE and
    THIRD_PARTY_NOTICES.md, no PDBs, and no staging leftovers. A machine-readable manifest with
    SHA-256 hashes, UAT exit codes, log paths, commit and dirty state is written next to the
    artifacts.

.EXAMPLE
    .\build.ps1 -FullMatrix -OutputRoot "E:\Patreon\UEPLUGINS\SpoutBuild"

.EXAMPLE
    .\build.ps1 -Partial -EngineVersions 5.3,5.8 -OutputRoot "D:\staging"

.EXAMPLE
    .\build.ps1 -Partial -EngineVersions 5.4 -EnginePaths @{ '5.4' = 'X:\Engines\UE_5.4' } -OutputRoot "D:\staging"

.EXAMPLE
    .\build.ps1 -Partial -EngineVersions 5.3 -OutputRoot "D:\staging" -ValidateOnly
#>
[CmdletBinding()]
param(
    # Plugin repository root. Defaults to the directory holding this script, resolved in the body
    # rather than here: $PSScriptRoot is not reliably populated as a param() default when the script
    # is launched via "powershell -File" from a nested process, which is exactly how build.bat
    # invokes it. Left empty here so the fallback chain below can run.
    [string] $PluginRoot = '',

    # Where release output is written. A per-version subdirectory is created underneath.
    [Parameter(Mandatory = $true)]
    [string] $OutputRoot,

    # Root containing UE_<version> installations, used when -EnginePaths does not cover a version.
    [string] $EngineRoot = 'D:\UE',

    # Explicit "version=path" pairs, which take precedence over -EngineRoot for those versions.
    # Expressed as strings rather than a hashtable so they survive being passed through build.bat
    # and any other non-PowerShell launcher.
    # Example: -EnginePaths 5.3=D:\UE\UE_5.3,5.8=X:\Engines\UE_5.8
    [string[]] $EnginePaths = @(),

    # Engine versions to build. Required with -Partial; defaults to the declared matrix otherwise.
    [string[]] $EngineVersions,

    # Every declared engine must exist and pass. Any missing engine or failure is fatal.
    [switch] $FullMatrix,

    # Only the explicitly requested engines are expected. The manifest is marked partial.
    [switch] $Partial,

    # Remove any pre-existing per-version build directory before building.
    [switch] $CleanStaging,

    # Keep the intermediate staging tree for inspection instead of deleting it after zipping.
    [switch] $KeepStaging,

    # Copy the engine's own AutomationTool/UBT logs alongside the captured UAT log.
    [switch] $KeepLogs,

    # Skip building and staging; only reopen and validate artifacts that already exist.
    [switch] $ValidateOnly
)

# ---------------------------------------------------------------------------------------------
# Failure policy. Any cmdlet error, any uninitialized variable, and any unhandled exception must
# terminate the release rather than let the loop continue to a success banner.
# ---------------------------------------------------------------------------------------------
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# The engine matrix this plugin declares support for. -FullMatrix requires all of them.
$DeclaredEngineVersions = @('5.3', '5.4', '5.5', '5.6', '5.7', '5.8')

# Target configurations produced by a single "RunUAT BuildPlugin -Rocket" invocation.
$TargetConfigurations = @(
    'UnrealEditor Win64 Development',
    'UnrealGame Win64 Development',
    'UnrealGame Win64 Shipping'
)

$PluginName = 'SpoutPlugin'

# Accumulated state, read by the manifest writer and the final banner.
$script:Failures = New-Object System.Collections.ArrayList
$script:EngineResults = New-Object System.Collections.ArrayList
$script:ManifestPath = $null

# Pre-initialized because the finally block reads them even when the run aborts before they are
# computed, and Set-StrictMode makes touching an undefined variable an error.
$requestedVersions = @()
$omittedVersions = @()
$releaseVersion = 'unknown'
$buildMode = 'Unspecified'

function Write-Section
{
    param([string] $Text)
    Write-Host ''
    Write-Host ('=' * 78) -ForegroundColor Cyan
    Write-Host $Text -ForegroundColor Cyan
    Write-Host ('=' * 78) -ForegroundColor Cyan
}

function Write-Step
{
    param([string] $Text)
    Write-Host "  $Text"
}

function Write-Good
{
    param([string] $Text)
    Write-Host "  [ok]   $Text" -ForegroundColor Green
}

function Write-Bad
{
    param([string] $Text)
    Write-Host "  [FAIL] $Text" -ForegroundColor Red
}

# Records a failure and throws. Every failure path funnels through here so nothing can be
# reported as a warning and then silently forgotten.
function Fail
{
    param([string] $Message)
    [void] $script:Failures.Add($Message)
    Write-Bad $Message
    throw $Message
}

# Records a failure without throwing, for validation checks that should all be reported before
# the release aborts.
function Add-Failure
{
    param([string] $Message)
    [void] $script:Failures.Add($Message)
    Write-Bad $Message
}

# SHA-256 via .NET rather than Get-FileHash. Get-FileHash and Compress-Archive live in optional
# modules, and a PSModulePath inherited from a different PowerShell edition (which happens whenever
# pwsh launches this through build.bat's "powershell" fallback) can make them unresolvable. The
# release must not fail for that reason, so nothing here depends on a module beyond the core cmdlets.
function Get-Sha256
{
    param([string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf))
    {
        return $null
    }

    $stream = $null
    $sha = $null
    try
    {
        $stream = [System.IO.File]::OpenRead($Path)
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $bytes = $sha.ComputeHash($stream)
        return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
    }
    finally
    {
        if ($sha) { $sha.Dispose() }
        if ($stream) { $stream.Dispose() }
    }
}

# Zips $SourceDirectory's contents so that its immediate children become the archive's top-level
# entries, matching what "Compress-Archive -Path <dir>\*" produced.
function New-ZipArchive
{
    param([string] $SourceDirectory, [string] $DestinationZip)

    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue

    if (Test-Path -LiteralPath $DestinationZip)
    {
        Remove-Item -LiteralPath $DestinationZip -Force
    }

    $parent = Split-Path -Parent $DestinationZip
    if ($parent -and -not (Test-Path -LiteralPath $parent))
    {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }

    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $SourceDirectory,
        $DestinationZip,
        [System.IO.Compression.CompressionLevel]::Optimal,
        $false)
}

# Reads VersionName from the plugin descriptor. The release version is never hard-coded.
function Get-PluginVersion
{
    param([string] $DescriptorPath)

    if (-not (Test-Path -LiteralPath $DescriptorPath -PathType Leaf))
    {
        Fail "Plugin descriptor not found: $DescriptorPath"
    }

    try
    {
        $descriptor = Get-Content -LiteralPath $DescriptorPath -Raw | ConvertFrom-Json
    }
    catch
    {
        Fail "Plugin descriptor is not valid JSON: $DescriptorPath ($($_.Exception.Message))"
    }

    $versionName = $descriptor.VersionName
    if ([string]::IsNullOrWhiteSpace($versionName))
    {
        Fail "Plugin descriptor has no VersionName: $DescriptorPath"
    }

    return [string] $versionName
}

# Parses the -EnginePaths "version=path" pairs once, so a malformed pair is a hard error rather than
# a version that silently falls back to -EngineRoot.
function Get-ExplicitEnginePathMap
{
    $map = @{}
    foreach ($pair in $EnginePaths)
    {
        if ([string]::IsNullOrWhiteSpace($pair))
        {
            continue
        }

        $separator = $pair.IndexOf('=')
        if ($separator -lt 1 -or $separator -eq ($pair.Length - 1))
        {
            Fail "Malformed -EnginePaths entry '$pair'. Expected 'version=path', for example 5.3=D:\UE\UE_5.3"
        }

        $version = $pair.Substring(0, $separator).Trim()
        $path = $pair.Substring($separator + 1).Trim()
        $map[$version] = $path
    }
    return $map
}

function Resolve-EnginePath
{
    param([string] $Version, [hashtable] $ExplicitMap)

    if ($ExplicitMap.ContainsKey($Version))
    {
        return [string] $ExplicitMap[$Version]
    }
    return (Join-Path $EngineRoot "UE_$Version")
}

# Best-effort engine build identity, for the manifest. Absent on some installs, which is not fatal.
function Get-EngineBuildId
{
    param([string] $EnginePath)

    $buildVersionPath = Join-Path $EnginePath 'Engine\Build\Build.version'
    if (-not (Test-Path -LiteralPath $buildVersionPath -PathType Leaf))
    {
        return $null
    }

    try
    {
        $buildVersion = Get-Content -LiteralPath $buildVersionPath -Raw | ConvertFrom-Json
        $parts = New-Object System.Collections.ArrayList
        [void] $parts.Add("$($buildVersion.MajorVersion).$($buildVersion.MinorVersion).$($buildVersion.PatchVersion)")
        if ($buildVersion.PSObject.Properties.Name -contains 'BuildId' -and $buildVersion.BuildId)
        {
            [void] $parts.Add("BuildId=$($buildVersion.BuildId)")
        }
        if ($buildVersion.PSObject.Properties.Name -contains 'BranchName' -and $buildVersion.BranchName)
        {
            [void] $parts.Add("Branch=$($buildVersion.BranchName)")
        }
        return ($parts -join ' ')
    }
    catch
    {
        return $null
    }
}

function Get-GitInfo
{
    param([string] $RepoRoot)

    $info = [ordered] @{
        Commit = $null
        IsDirty = $null
        Branch  = $null
    }

    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git)
    {
        return $info
    }

    # Git failures here are informational only; they must not abort a release, so $LASTEXITCODE is
    # inspected instead of letting a nonzero native exit propagate.
    $commit = & git -C $RepoRoot rev-parse HEAD 2>$null
    if ($LASTEXITCODE -eq 0) { $info.Commit = ($commit | Out-String).Trim() }

    $branch = & git -C $RepoRoot rev-parse --abbrev-ref HEAD 2>$null
    if ($LASTEXITCODE -eq 0) { $info.Branch = ($branch | Out-String).Trim() }

    $status = & git -C $RepoRoot status --porcelain 2>$null
    if ($LASTEXITCODE -eq 0)
    {
        $info.IsDirty = -not [string]::IsNullOrWhiteSpace(($status | Out-String).Trim())
    }

    return $info
}

# ---------------------------------------------------------------------------------------------
# Pre-flight: everything a release needs from the repository, checked before any engine runs so a
# missing license or DLL costs seconds instead of a full matrix.
# ---------------------------------------------------------------------------------------------
function Test-SourceDependencies
{
    param([string] $Root)

    Write-Section 'Pre-flight: repository dependencies'

    $required = [ordered] @{
        'plugin descriptor'      = Join-Path $Root 'SpoutPlugin.uplugin'
        'module build rules'     = Join-Path $Root 'Source\SpoutPlugin\SpoutPlugin.Build.cs'
        'module source'          = Join-Path $Root 'Source\SpoutPlugin\Private'
        'module public headers'  = Join-Path $Root 'Source\SpoutPlugin\Public'
        'packaging filter'       = Join-Path $Root 'Config\FilterPlugin.ini'
        'Spout headers'          = Join-Path $Root 'ThirdParty\Spout\include'
        'Spout import library'   = Join-Path $Root 'ThirdParty\Spout\lib\amd64\Spout.lib'
        'Spout runtime library'  = Join-Path $Root 'ThirdParty\Spout\lib\amd64\Spout.dll'
        'project license'        = Join-Path $Root 'LICENSE'
        'third-party notices'    = Join-Path $Root 'THIRD_PARTY_NOTICES.md'
        'shipped readme'         = Join-Path $Root 'README.md'
    }

    $missing = $false
    foreach ($label in $required.Keys)
    {
        $path = $required[$label]
        if (Test-Path -LiteralPath $path)
        {
            Write-Good "$label : $path"
        }
        else
        {
            Add-Failure "Missing $label : $path"
            $missing = $true
        }
    }

    # The filter must ship ThirdParty and the licensing files, or BuildPlugin output will be
    # missing them and every ZIP validation below would fail after a full build.
    $filterPath = Join-Path $Root 'Config\FilterPlugin.ini'
    if (Test-Path -LiteralPath $filterPath -PathType Leaf)
    {
        $filter = Get-Content -LiteralPath $filterPath -Raw
        foreach ($entry in @('/ThirdParty/...', '/LICENSE', '/THIRD_PARTY_NOTICES.md', '/README.md'))
        {
            if ($filter -notmatch [regex]::Escape($entry))
            {
                Add-Failure "Config\FilterPlugin.ini does not list '$entry'; packaged output would omit it."
                $missing = $true
            }
            else
            {
                Write-Good "FilterPlugin.ini lists $entry"
            }
        }
    }

    if ($missing)
    {
        throw 'Pre-flight dependency check failed.'
    }
}

# ---------------------------------------------------------------------------------------------
# ZIP validation. Reopens the archive and asserts its exact expected contents.
# ---------------------------------------------------------------------------------------------
function Test-ReleaseArchive
{
    param(
        [string] $ZipPath,
        [string] $ExpectedVersion,
        [string] $ExpectedSpoutDllHash,
        [string] $Label
    )

    $result = [ordered] @{
        Label       = $Label
        Path        = $ZipPath
        Exists      = $false
        Opened      = $false
        Valid       = $false
        Sha256      = $null
        SizeBytes   = $null
        EntryCount  = $null
        SpoutDllSha256 = $null
        Problems    = @()
    }

    $problems = New-Object System.Collections.ArrayList

    if (-not (Test-Path -LiteralPath $ZipPath -PathType Leaf))
    {
        [void] $problems.Add("archive does not exist")
        $result.Problems = $problems.ToArray()
        return $result
    }

    $result.Exists = $true
    $result.SizeBytes = (Get-Item -LiteralPath $ZipPath).Length
    $result.Sha256 = Get-Sha256 $ZipPath

    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue

    $archive = $null
    try
    {
        # Reopening is the check: a truncated or corrupt archive throws here rather than being
        # published as a valid release.
        $archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
        $result.Opened = $true

        $entries = @($archive.Entries | ForEach-Object { $_.FullName -replace '\\', '/' })
        $result.EntryCount = $entries.Count

        if ($entries.Count -eq 0)
        {
            [void] $problems.Add('archive is empty')
        }

        # Exactly one plugin root, named after the plugin.
        $roots = @($entries | ForEach-Object { ($_ -split '/')[0] } | Sort-Object -Unique)
        if ($roots.Count -ne 1)
        {
            [void] $problems.Add("expected exactly one root directory, found $($roots.Count): $($roots -join ', ')")
        }
        elseif ($roots[0] -ne $PluginName)
        {
            [void] $problems.Add("root directory is '$($roots[0])', expected '$PluginName'")
        }

        function Test-Entry
        {
            param([string[]] $Entries, [string] $Relative)
            $target = "$PluginName/$Relative"
            return ($Entries -contains $target)
        }

        function Test-EntryPrefix
        {
            param([string[]] $Entries, [string] $Prefix)
            $target = "$PluginName/$Prefix"
            foreach ($e in $Entries)
            {
                if ($e.StartsWith($target, [System.StringComparison]::OrdinalIgnoreCase)) { return $true }
            }
            return $false
        }

        # Descriptor present, and its VersionName matches the release being built.
        $descriptorEntryName = "$PluginName/SpoutPlugin.uplugin"
        $descriptorEntry = $archive.Entries | Where-Object { ($_.FullName -replace '\\', '/') -eq $descriptorEntryName } | Select-Object -First 1
        if (-not $descriptorEntry)
        {
            [void] $problems.Add("missing $descriptorEntryName")
        }
        else
        {
            $reader = $null
            try
            {
                $reader = New-Object System.IO.StreamReader($descriptorEntry.Open())
                $descriptor = $reader.ReadToEnd() | ConvertFrom-Json
                if ([string] $descriptor.VersionName -ne $ExpectedVersion)
                {
                    [void] $problems.Add("descriptor VersionName is '$($descriptor.VersionName)', expected '$ExpectedVersion'")
                }
            }
            catch
            {
                [void] $problems.Add("could not read descriptor from archive: $($_.Exception.Message)")
            }
            finally
            {
                if ($reader) { $reader.Dispose() }
            }
        }

        # Source tree, so consuming projects can compile and package against the plugin.
        if (-not (Test-Entry $entries 'Source/SpoutPlugin/SpoutPlugin.Build.cs'))
        {
            [void] $problems.Add('missing Source/SpoutPlugin/SpoutPlugin.Build.cs')
        }
        if (-not (Test-EntryPrefix $entries 'Source/SpoutPlugin/Private/'))
        {
            [void] $problems.Add('missing Source/SpoutPlugin/Private/')
        }
        if (-not (Test-EntryPrefix $entries 'Source/SpoutPlugin/Public/'))
        {
            [void] $problems.Add('missing Source/SpoutPlugin/Public/')
        }

        # Module binary.
        $moduleBinaries = @($entries | Where-Object {
            $_ -like "$PluginName/Binaries/Win64/*.dll" -and $_ -notlike '*/Spout.dll'
        })
        if ($moduleBinaries.Count -eq 0)
        {
            [void] $problems.Add('missing plugin module binary in Binaries/Win64')
        }

        # Runtime DLL, verified byte-for-byte against the repository copy.
        $spoutEntryName = "$PluginName/Binaries/Win64/Spout.dll"
        $spoutEntry = $archive.Entries | Where-Object { ($_.FullName -replace '\\', '/') -eq $spoutEntryName } | Select-Object -First 1
        if (-not $spoutEntry)
        {
            [void] $problems.Add("missing $spoutEntryName")
        }
        else
        {
            $stream = $null
            $memory = $null
            try
            {
                $stream = $spoutEntry.Open()
                $memory = New-Object System.IO.MemoryStream
                $stream.CopyTo($memory)
                [void] $memory.Seek(0, [System.IO.SeekOrigin]::Begin)
                $sha = [System.Security.Cryptography.SHA256]::Create()
                try
                {
                    $hashBytes = $sha.ComputeHash($memory)
                }
                finally
                {
                    $sha.Dispose()
                }
                $zipDllHash = (($hashBytes | ForEach-Object { $_.ToString('x2') }) -join '')
                $result.SpoutDllSha256 = $zipDllHash
                if ($ExpectedSpoutDllHash -and $zipDllHash -ne $ExpectedSpoutDllHash)
                {
                    [void] $problems.Add("Spout.dll hash in archive ($zipDllHash) does not match the repository copy ($ExpectedSpoutDllHash)")
                }
            }
            catch
            {
                [void] $problems.Add("could not hash Spout.dll from archive: $($_.Exception.Message)")
            }
            finally
            {
                if ($memory) { $memory.Dispose() }
                if ($stream) { $stream.Dispose() }
            }
        }

        # Precompiled metadata. These are what let Blueprint-only users package without a compiler,
        # so their absence is a release defect, not a cleanup opportunity.
        if (-not (Test-EntryPrefix $entries 'Intermediate/Build/'))
        {
            [void] $problems.Add('missing Intermediate/Build precompiled metadata')
        }
        $precompiledManifests = @($entries | Where-Object { $_ -like '*.precompiled' })
        if ($precompiledManifests.Count -eq 0)
        {
            [void] $problems.Add('missing .precompiled manifest under Intermediate/Build')
        }

        # ThirdParty tree, required by SpoutPlugin.Build.cs at compile time.
        foreach ($tp in @('ThirdParty/Spout/lib/amd64/Spout.lib', 'ThirdParty/Spout/lib/amd64/Spout.dll'))
        {
            if (-not (Test-Entry $entries $tp))
            {
                [void] $problems.Add("missing $tp")
            }
        }
        if (-not (Test-EntryPrefix $entries 'ThirdParty/Spout/include/'))
        {
            [void] $problems.Add('missing ThirdParty/Spout/include/')
        }

        # Licensing and shipped documentation.
        foreach ($required in @('LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md'))
        {
            if (-not (Test-Entry $entries $required))
            {
                [void] $problems.Add("missing $required")
            }
        }

        # PDB policy: release archives ship no debug symbols.
        $pdbs = @($entries | Where-Object { $_ -like '*.pdb' })
        if ($pdbs.Count -gt 0)
        {
            [void] $problems.Add("contains $($pdbs.Count) PDB file(s): $(($pdbs | Select-Object -First 3) -join ', ')")
        }

        # Staging leftovers must never be published.
        $staging = @($entries | Where-Object { $_ -match '(^|/)ZipTemp|(^|/)Staging_' })
        if ($staging.Count -gt 0)
        {
            [void] $problems.Add("contains staging leftovers: $(($staging | Select-Object -First 3) -join ', ')")
        }

        # Internal material must never reach a customer. BuildPlugin's default filter does not copy
        # these directories, so finding one means the filter or the staging step changed - which is
        # worth failing the release over rather than discovering in a published ZIP.
        $forbiddenPrefixes = @(
            @{ Prefix = 'Artifacts/'; What = 'Track D validation evidence' },
            @{ Prefix = 'Scripts/';   What = 'release and validation tooling' },
            @{ Prefix = 'Tests/';     What = 'the host test project' },
            @{ Prefix = 'Docs/';      What = 'internal documentation and audit material' },
            @{ Prefix = '.git/';      What = 'version-control metadata' },
            @{ Prefix = '.vs/';       What = 'IDE scratch' }
        )
        foreach ($forbidden in $forbiddenPrefixes)
        {
            $hits = @($entries | Where-Object { $_ -like "$PluginName/$($forbidden.Prefix)*" })
            if ($hits.Count -gt 0)
            {
                [void] $problems.Add("contains $($forbidden.What) ($($forbidden.Prefix)): $(($hits | Select-Object -First 3) -join ', ')")
            }
        }

        # Individual files that are internal records rather than shipped documentation.
        $forbiddenFiles = @('CLAUDE.md', 'UPDATES_NOTES.md', 'build.ps1', 'build.bat')
        foreach ($forbidden in $forbiddenFiles)
        {
            if (Test-Entry $entries $forbidden)
            {
                [void] $problems.Add("contains internal file $forbidden")
            }
        }
        $trackRecords = @($entries | Where-Object { $_ -match "^$([regex]::Escape($PluginName))/TRACK_[A-Z]_" })
        if ($trackRecords.Count -gt 0)
        {
            [void] $problems.Add("contains internal track records: $(($trackRecords | Select-Object -First 3) -join ', ')")
        }

        # Local absolute paths must not be published inside the shipped documentation.
        $readmeEntryName = "$PluginName/README.md"
        $readmeEntry = $archive.Entries | Where-Object { ($_.FullName -replace '\\', '/') -eq $readmeEntryName } | Select-Object -First 1
        if ($readmeEntry)
        {
            $reader = $null
            try
            {
                $reader = New-Object System.IO.StreamReader($readmeEntry.Open())
                $readmeText = $reader.ReadToEnd()
                if ($readmeText -match '[A-Za-z]:\\Users\\')
                {
                    [void] $problems.Add('the shipped README contains a machine-specific user path')
                }
            }
            catch
            {
                [void] $problems.Add("could not read README.md from archive: $($_.Exception.Message)")
            }
            finally
            {
                if ($reader) { $reader.Dispose() }
            }
        }
    }
    catch
    {
        [void] $problems.Add("could not open archive: $($_.Exception.Message)")
    }
    finally
    {
        if ($archive) { $archive.Dispose() }
    }

    $result.Problems = $problems.ToArray()
    $result.Valid = ($problems.Count -eq 0)
    return $result
}

# ---------------------------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------------------------
$exitCode = 1
try
{
    # --- List-parameter normalization ------------------------------------------------------------
    # "-EngineVersions 5.3,5.8" does not always bind as two array elements. Under
    # "pwsh -File build.ps1", which is exactly how build.bat invokes this script, and from cmd or any
    # other non-PowerShell launcher, it arrives as the single string "5.3,5.8" -- and the script then
    # looks for an engine installed at "<EngineRoot>\UE_5.3,5.8" and fails with a confusing
    # "engine not found". Splitting here makes both forms mean the same thing, which matters because
    # the comma form is what this script's own usage examples tell people to type.
    $EngineVersions = @(@($EngineVersions) |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        ForEach-Object { $_ -split ',' } |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ })

    $EnginePaths = @(@($EnginePaths) |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ })

    # --- Mode resolution -----------------------------------------------------------------------
    if ($FullMatrix -and $Partial)
    {
        Fail 'Specify only one of -FullMatrix or -Partial.'
    }
    if (-not $FullMatrix -and -not $Partial)
    {
        Fail 'Specify either -FullMatrix (every declared engine must pass) or -Partial (only the engines named by -EngineVersions are expected).'
    }

    # --- Plugin root resolution ----------------------------------------------------------------
    if ([string]::IsNullOrWhiteSpace($PluginRoot))
    {
        # Try each source of this script's own location in turn. Under "powershell -File" launched
        # from build.bat, $PSScriptRoot can be empty at param-binding time while $PSCommandPath is
        # populated, so the fallbacks are load-bearing rather than defensive noise.
        foreach ($candidate in @($PSScriptRoot, (Split-Path -Parent -Path $PSCommandPath -ErrorAction SilentlyContinue), (Split-Path -Parent -Path $MyInvocation.MyCommand.Path -ErrorAction SilentlyContinue)))
        {
            if (-not [string]::IsNullOrWhiteSpace($candidate))
            {
                $PluginRoot = $candidate
                break
            }
        }
    }

    if ([string]::IsNullOrWhiteSpace($PluginRoot) -or -not (Test-Path -LiteralPath $PluginRoot -PathType Container))
    {
        Fail "Could not determine the plugin root. Pass -PluginRoot explicitly. (resolved to '$PluginRoot')"
    }

    $PluginRoot = (Resolve-Path -LiteralPath $PluginRoot).Path

    if ($FullMatrix)
    {
        if ($EngineVersions -and $EngineVersions.Count -gt 0)
        {
            # Requesting a subset while claiming a full matrix is contradictory; refuse rather than
            # silently produce a partial release labelled as full.
            $unexpected = @($EngineVersions | Where-Object { $DeclaredEngineVersions -notcontains $_ })
            if ($unexpected.Count -gt 0)
            {
                Fail "-FullMatrix cannot be combined with engine versions outside the declared matrix: $($unexpected -join ', ')"
            }
            if ($EngineVersions.Count -ne $DeclaredEngineVersions.Count)
            {
                Fail "-FullMatrix builds the complete declared matrix ($($DeclaredEngineVersions -join ', ')). Use -Partial to build a subset."
            }
        }
        $requestedVersions = $DeclaredEngineVersions
        $buildMode = 'FullMatrix'
    }
    else
    {
        if (-not $EngineVersions -or $EngineVersions.Count -eq 0)
        {
            Fail '-Partial requires -EngineVersions with at least one version.'
        }
        $requestedVersions = @($EngineVersions | Sort-Object -Unique)
        $buildMode = 'Partial'
    }

    $omittedVersions = @($DeclaredEngineVersions | Where-Object { $requestedVersions -notcontains $_ })

    # --- Version and layout --------------------------------------------------------------------
    $descriptorPath = Join-Path $PluginRoot 'SpoutPlugin.uplugin'
    $releaseVersion = Get-PluginVersion $descriptorPath

    $versionOutputRoot = Join-Path $OutputRoot $releaseVersion
    $fabRoot = Join-Path $versionOutputRoot 'Fab'
    $patreonRoot = Join-Path $versionOutputRoot 'Patreon'
    $logRoot = Join-Path $versionOutputRoot 'Logs'

    Write-Section "SpoutPlugin release $releaseVersion  [$buildMode]"
    Write-Step "Plugin root  : $PluginRoot"
    Write-Step "Output root  : $versionOutputRoot"
    Write-Step "Engine root  : $EngineRoot"
    Write-Step "Requested    : $($requestedVersions -join ', ')"
    if ($omittedVersions.Count -gt 0)
    {
        Write-Step "Omitted      : $($omittedVersions -join ', ')"
    }
    Write-Step "Validate only: $($ValidateOnly.IsPresent)"

    if (-not $ValidateOnly)
    {
        Test-SourceDependencies $PluginRoot
    }

    $repoSpoutDll = Join-Path $PluginRoot 'ThirdParty\Spout\lib\amd64\Spout.dll'
    $repoSpoutDllHash = Get-Sha256 $repoSpoutDll
    $repoSpoutLib = Join-Path $PluginRoot 'ThirdParty\Spout\lib\amd64\Spout.lib'
    $repoSpoutLibHash = Get-Sha256 $repoSpoutLib

    foreach ($dir in @($versionOutputRoot, $fabRoot, $patreonRoot, $logRoot))
    {
        if (-not (Test-Path -LiteralPath $dir))
        {
            New-Item -ItemType Directory -Path $dir -Force | Out-Null
        }
    }

    # --- Engine resolution ---------------------------------------------------------------------
    # Resolve every engine before building anything, so a missing engine in full-matrix mode fails
    # immediately instead of after an hour of successful builds.
    $explicitEnginePaths = Get-ExplicitEnginePathMap

    $enginePlan = New-Object System.Collections.ArrayList
    foreach ($version in $requestedVersions)
    {
        $enginePath = Resolve-EnginePath -Version $version -ExplicitMap $explicitEnginePaths
        $runUat = Join-Path $enginePath 'Engine\Build\BatchFiles\RunUAT.bat'
        $found = Test-Path -LiteralPath $runUat -PathType Leaf

        [void] $enginePlan.Add([ordered] @{
            Version    = $version
            EnginePath = $enginePath
            RunUAT     = $runUat
            Found      = $found
        })

        if ($found)
        {
            Write-Good "UE $version : $enginePath"
        }
        else
        {
            # Fatal in both modes: in full-matrix mode because the matrix is incomplete, and in
            # partial mode because the operator explicitly asked for this version.
            Add-Failure "UE $version : engine not found at $enginePath (RunUAT.bat missing)"
        }
    }

    if (@($enginePlan | Where-Object { -not $_.Found }).Count -gt 0)
    {
        if ($FullMatrix)
        {
            throw 'Full-matrix release aborted: one or more declared engines are not installed.'
        }
        throw 'Release aborted: one or more explicitly requested engines are not installed.'
    }

    # --- Per-engine build, stage, zip, validate -------------------------------------------------
    foreach ($engine in $enginePlan)
    {
        $version = $engine.Version
        Write-Section "UE $version"

        $buildOut = Join-Path $versionOutputRoot "UE$version"
        $zipName = "${PluginName}_${releaseVersion}_UE${version}_Win64.zip"
        $rootZip = Join-Path $versionOutputRoot $zipName
        $fabZip = Join-Path $fabRoot $zipName
        $patreonZip = Join-Path $patreonRoot $zipName
        $uatLog = Join-Path $logRoot "UE$version-BuildPlugin.log"

        $engineResult = [ordered] @{
            EngineVersion        = $version
            EnginePath           = $engine.EnginePath
            EngineBuildId        = (Get-EngineBuildId $engine.EnginePath)
            TargetConfigurations = $TargetConfigurations
            UatCommand           = $null
            UatExitCode          = $null
            UatLogPath           = $uatLog
            EngineLogDirectory   = $null
            Artifacts            = @()
            Success              = $false
        }

        if ($ValidateOnly)
        {
            Write-Step 'Validate-only: skipping build and staging.'
        }
        else
        {
            if ($CleanStaging -and (Test-Path -LiteralPath $buildOut))
            {
                Write-Step "Removing previous build output: $buildOut"
                Remove-Item -LiteralPath $buildOut -Recurse -Force
            }

            foreach ($zip in @($rootZip, $fabZip, $patreonZip))
            {
                if (Test-Path -LiteralPath $zip)
                {
                    Remove-Item -LiteralPath $zip -Force
                }
            }

            # --- UAT -------------------------------------------------------------------------
            $uatArgs = @(
                'BuildPlugin',
                "-Plugin=$descriptorPath",
                "-Package=$buildOut",
                '-TargetPlatforms=Win64',
                '-Rocket'
            )
            $engineResult.UatCommand = ('"{0}" {1}' -f $engine.RunUAT, ($uatArgs -join ' '))

            Write-Step "Running: $($engineResult.UatCommand)"
            Write-Step "Log    : $uatLog"

            # Tee so the log is retained and the console still shows progress. $LASTEXITCODE is
            # captured immediately: a nonzero UAT status is a release failure, not a warning.
            & $engine.RunUAT @uatArgs 2>&1 | Tee-Object -FilePath $uatLog | Out-Null
            $uatExit = $LASTEXITCODE
            $engineResult.UatExitCode = $uatExit

            if ($uatExit -ne 0)
            {
                [void] $script:EngineResults.Add($engineResult)
                Fail "UE $version : RunUAT BuildPlugin failed with exit code $uatExit. See $uatLog"
            }
            Write-Good "UAT exit code 0"

            if ($KeepLogs)
            {
                # The engine's own AutomationTool/UBT logs give per-target detail that the single
                # combined UAT capture above does not.
                $engineLogSource = Join-Path $env:APPDATA "Unreal Engine\AutomationTool\Logs"
                if (Test-Path -LiteralPath $engineLogSource)
                {
                    $engineLogDest = Join-Path $logRoot "UE$version-EngineLogs"
                    New-Item -ItemType Directory -Path $engineLogDest -Force | Out-Null
                    Copy-Item -Path (Join-Path $engineLogSource '*') -Destination $engineLogDest -Recurse -Force
                    $engineResult.EngineLogDirectory = $engineLogDest
                    Write-Good "Engine logs copied to $engineLogDest"
                }
            }

            # --- Packaged-output checks ------------------------------------------------------
            # BuildPlugin only copies /ThirdParty/... when Config/FilterPlugin.ini asks for it, and
            # consuming projects compile from the shipped Source/, where SpoutPlugin.Build.cs reads
            # all three of these from the plugin root. UBT hard-fails if any is missing.
            $requiredInOutput = @(
                (Join-Path $buildOut 'ThirdParty\Spout\include'),
                (Join-Path $buildOut 'ThirdParty\Spout\lib\amd64\Spout.lib'),
                (Join-Path $buildOut 'ThirdParty\Spout\lib\amd64\Spout.dll'),
                (Join-Path $buildOut 'Source\SpoutPlugin\SpoutPlugin.Build.cs'),
                (Join-Path $buildOut 'SpoutPlugin.uplugin'),
                (Join-Path $buildOut 'LICENSE'),
                (Join-Path $buildOut 'THIRD_PARTY_NOTICES.md'),
                (Join-Path $buildOut 'README.md'),
                (Join-Path $buildOut 'Intermediate\Build')
            )

            $outputMissing = $false
            foreach ($path in $requiredInOutput)
            {
                if (Test-Path -LiteralPath $path)
                {
                    Write-Good "packaged: $path"
                }
                else
                {
                    Add-Failure "UE $version : missing from packaged output: $path"
                    $outputMissing = $true
                }
            }
            if ($outputMissing)
            {
                [void] $script:EngineResults.Add($engineResult)
                throw "UE $version : packaged output is incomplete."
            }

            # RunUAT BuildPlugin does NOT carry the RuntimeDependencies Spout.dll into the -Package
            # output; the packaged Binaries/Win64 contains only the module dll. Stage it explicitly
            # so releases actually ship it - without it the plugin fails on the first Spout call
            # (0xC06D007E, delay-load module not found).
            $binariesWin64 = Join-Path $buildOut 'Binaries\Win64'
            if (-not (Test-Path -LiteralPath $binariesWin64))
            {
                New-Item -ItemType Directory -Path $binariesWin64 -Force | Out-Null
            }
            Copy-Item -LiteralPath $repoSpoutDll -Destination $binariesWin64 -Force

            $stagedSpoutDll = Join-Path $binariesWin64 'Spout.dll'
            if (-not (Test-Path -LiteralPath $stagedSpoutDll -PathType Leaf))
            {
                [void] $script:EngineResults.Add($engineResult)
                Fail "UE $version : Spout.dll is missing from $binariesWin64 after staging."
            }
            $stagedHash = Get-Sha256 $stagedSpoutDll
            if ($stagedHash -ne $repoSpoutDllHash)
            {
                [void] $script:EngineResults.Add($engineResult)
                Fail "UE $version : staged Spout.dll hash ($stagedHash) does not match the repository copy ($repoSpoutDllHash)."
            }
            Write-Good "Spout.dll staged into Binaries/Win64 (sha256 $stagedHash)"

            # PDB policy: no debug symbols in release archives.
            Get-ChildItem -LiteralPath $binariesWin64 -Filter '*.pdb' -ErrorAction SilentlyContinue |
                Remove-Item -Force

            # Keep Intermediate\Build: it holds the precompiled UnrealGame Development/Shipping
            # objects (+ SpoutPlugin.precompiled manifests and UnrealGame\Inc generated headers)
            # that BuildPlugin produces. Installed under Engine\Plugins\Marketplace, they let
            # Blueprint-only users package a project WITHOUT Visual Studio; deleting them makes
            # such packaging impossible.

            # --- Stage and zip ---------------------------------------------------------------
            # Both variants ship identical contents (including Source/, without which UBT cannot
            # resolve the module). They are separate files so each distribution channel gets its
            # own artifact and hash.
            foreach ($variant in @(
                @{ Name = 'Fab'; Zip = $fabZip },
                @{ Name = 'Patreon'; Zip = $patreonZip }
            ))
            {
                $stagingRoot = Join-Path $versionOutputRoot ("Staging_{0}_UE{1}" -f $variant.Name, $version)
                $stagingPluginDir = Join-Path $stagingRoot $PluginName

                if (Test-Path -LiteralPath $stagingRoot)
                {
                    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
                }
                New-Item -ItemType Directory -Path $stagingPluginDir -Force | Out-Null

                Copy-Item -Path (Join-Path $buildOut '*') -Destination $stagingPluginDir -Recurse -Force

                Get-ChildItem -LiteralPath $stagingPluginDir -Filter '*.pdb' -Recurse -ErrorAction SilentlyContinue |
                    Remove-Item -Force

                Write-Step "Creating $($variant.Name) archive: $($variant.Zip)"
                New-ZipArchive -SourceDirectory $stagingRoot -DestinationZip $variant.Zip

                if (-not (Test-Path -LiteralPath $variant.Zip -PathType Leaf))
                {
                    [void] $script:EngineResults.Add($engineResult)
                    Fail "UE $version : $($variant.Name) archive was not created at $($variant.Zip)."
                }

                if (-not $KeepStaging)
                {
                    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
                }
            }

            # Root ZIP mirror of the Patreon archive.
            Copy-Item -LiteralPath $patreonZip -Destination $rootZip -Force
        }

        # --- Validate every artifact ---------------------------------------------------------
        $artifacts = New-Object System.Collections.ArrayList
        foreach ($candidate in @(
            @{ Label = 'Fab'; Path = $fabZip },
            @{ Label = 'Patreon'; Path = $patreonZip },
            @{ Label = 'Root'; Path = $rootZip }
        ))
        {
            Write-Step "Validating $($candidate.Label) archive: $($candidate.Path)"
            $validation = Test-ReleaseArchive -ZipPath $candidate.Path -ExpectedVersion $releaseVersion `
                -ExpectedSpoutDllHash $repoSpoutDllHash -Label $candidate.Label

            [void] $artifacts.Add($validation)

            if ($validation.Valid)
            {
                Write-Good "$($candidate.Label): $($validation.EntryCount) entries, sha256 $($validation.Sha256)"
            }
            else
            {
                foreach ($problem in $validation.Problems)
                {
                    Add-Failure "UE $version $($candidate.Label) archive: $problem"
                }
            }
        }

        $engineResult.Artifacts = $artifacts.ToArray()
        $engineResult.Success = (@($artifacts | Where-Object { -not $_.Valid }).Count -eq 0)
        [void] $script:EngineResults.Add($engineResult)

        if (-not $engineResult.Success)
        {
            throw "UE $version : artifact validation failed."
        }

        Write-Good "UE $version completed"
    }

    $exitCode = 0
}
catch
{
    if ($script:Failures.Count -eq 0)
    {
        [void] $script:Failures.Add($_.Exception.Message)
    }
    Write-Host ''
    Write-Host "RELEASE FAILED: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
}
finally
{
    # The manifest is written on success AND on failure, so a failed release still leaves auditable
    # evidence of exactly how far it got and why it stopped.
    try
    {
        if ($OutputRoot)
        {
            $manifestVersion = $releaseVersion
            $manifestDir = Join-Path $OutputRoot $manifestVersion
            if (-not (Test-Path -LiteralPath $manifestDir))
            {
                New-Item -ItemType Directory -Path $manifestDir -Force | Out-Null
            }

            # The run may have aborted before the plugin root was resolved (for example a malformed
            # argument). Guard every plugin-root-derived field so the manifest still gets written and
            # records the failure, instead of the manifest writer itself becoming the reported error.
            $haveRoot = (-not [string]::IsNullOrWhiteSpace($PluginRoot)) -and (Test-Path -LiteralPath $PluginRoot)

            $gitInfoForManifest = $null
            if ($haveRoot)
            {
                try { $gitInfoForManifest = Get-GitInfo $PluginRoot } catch { $gitInfoForManifest = $null }
            }

            $bundledDllPath = $null
            $bundledDllHash = $null
            $bundledLibPath = $null
            $bundledLibHash = $null
            $hasLicenseFile = $false
            $hasNoticeFile = $false
            if ($haveRoot)
            {
                $bundledDllPath = Join-Path $PluginRoot 'ThirdParty\Spout\lib\amd64\Spout.dll'
                $bundledDllHash = Get-Sha256 $bundledDllPath
                $bundledLibPath = Join-Path $PluginRoot 'ThirdParty\Spout\lib\amd64\Spout.lib'
                $bundledLibHash = Get-Sha256 $bundledLibPath
                $hasLicenseFile = (Test-Path -LiteralPath (Join-Path $PluginRoot 'LICENSE'))
                $hasNoticeFile = (Test-Path -LiteralPath (Join-Path $PluginRoot 'THIRD_PARTY_NOTICES.md'))
            }

            $succeeded = ($exitCode -eq 0)

            # Precomputed rather than inlined into the hashtable, so the literal stays valid on
            # Windows PowerShell 5.1 as well as PowerShell 7.
            $manifestCommit = $null
            $manifestBranch = $null
            $manifestDirty = $null
            if ($gitInfoForManifest)
            {
                $manifestCommit = $gitInfoForManifest.Commit
                $manifestBranch = $gitInfoForManifest.Branch
                $manifestDirty = $gitInfoForManifest.IsDirty
            }

            $manifest = [ordered] @{
                Schema               = 'spoutplugin-release-manifest/1'
                GeneratedUtc         = (Get-Date).ToUniversalTime().ToString('o')
                Success              = $succeeded
                BuildMode            = $buildMode
                ValidateOnly         = $ValidateOnly.IsPresent
                PluginName           = $PluginName
                PluginVersion        = $manifestVersion
                PluginRoot           = $PluginRoot
                OutputRoot           = $manifestDir
                LogDirectory         = (Join-Path $manifestDir 'Logs')
                Commit               = $manifestCommit
                Branch               = $manifestBranch
                WorkingTreeDirty     = $manifestDirty
                DeclaredEngines      = $DeclaredEngineVersions
                RequestedEngines     = $requestedVersions
                OmittedEngines       = $omittedVersions
                TargetConfigurations = $TargetConfigurations
                BundledSpoutDll      = [ordered] @{
                    Path   = $bundledDllPath
                    Sha256 = $bundledDllHash
                }
                BundledSpoutLib      = [ordered] @{
                    Path   = $bundledLibPath
                    Sha256 = $bundledLibHash
                }
                LicenseValidation    = [ordered] @{
                    RepositoryLicense           = $hasLicenseFile
                    RepositoryThirdPartyNotices = $hasNoticeFile
                    PresentInEveryArchive       = $succeeded
                }
                Engines              = $script:EngineResults.ToArray()
                Failures             = $script:Failures.ToArray()
            }

            # Partial releases must be unmistakable in the manifest.
            if ($Partial)
            {
                $manifest['PartialReleaseNotice'] = "PARTIAL RELEASE. Only these engine versions were built and validated: $($manifest.RequestedEngines -join ', '). Omitted: $($manifest.OmittedEngines -join ', ')."
            }

            $script:ManifestPath = Join-Path $manifestDir ("release-manifest-{0}.json" -f (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss'))
            $manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $script:ManifestPath -Encoding UTF8

            Write-Host ''
            Write-Host "Manifest: $($script:ManifestPath)" -ForegroundColor Cyan
        }
    }
    catch
    {
        # A manifest that cannot be written is itself a release failure.
        Write-Host "Manifest generation failed: $($_.Exception.Message)" -ForegroundColor Red
        $exitCode = 1
    }

    Write-Host ''
    if ($exitCode -eq 0 -and $script:Failures.Count -eq 0)
    {
        Write-Host ('=' * 78) -ForegroundColor Green
        if ($Partial)
        {
            Write-Host "PARTIAL RELEASE COMPLETED - engines built and validated: $($requestedVersions -join ', ')" -ForegroundColor Green
            Write-Host "Omitted (not built, not validated): $($omittedVersions -join ', ')" -ForegroundColor Yellow
        }
        else
        {
            Write-Host "FULL MATRIX RELEASE COMPLETED - all engines built and validated" -ForegroundColor Green
        }
        Write-Host ('=' * 78) -ForegroundColor Green
    }
    else
    {
        Write-Host ('=' * 78) -ForegroundColor Red
        Write-Host 'RELEASE FAILED' -ForegroundColor Red
        foreach ($failure in $script:Failures)
        {
            Write-Host "  - $failure" -ForegroundColor Red
        }
        Write-Host ('=' * 78) -ForegroundColor Red
        if ($exitCode -eq 0) { $exitCode = 1 }
    }
}

exit $exitCode
