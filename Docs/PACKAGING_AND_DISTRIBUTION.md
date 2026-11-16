# Packaging and Distribution

How the plugin is built for release and what must ship with it.

## Spout.dll staging

`SpoutPlugin.Build.cs` registers `Spout.dll` as a runtime dependency staged to
`$(BinaryOutputDir)/Spout.dll`. `$(BinaryOutputDir)` is the folder of the binary the module
is linked into, so it resolves to two different places depending on the build type:

- **Editor / modular builds** (normal project builds): the plugin's own `Binaries/Win64`,
  beside `UnrealEditor-SpoutPlugin.dll`. Raw `RunUAT BuildPlugin` output does not include the
  DLL — plugin packaging runs no target staging — so `build.ps1` stages it into that same
  location explicitly when it assembles the precompiled release zips.
- **Packaged (monolithic) games**: the plugin module is linked into the game executable, so
  the DLL is staged into the project's `Binaries/Win64` next to the game `.exe`. The plugin's
  own `Binaries` and `Source/ThirdParty` folders are not staged at all in a packaged game.

The startup loader (0.0.9+) checks, in order: the plugin's `Binaries/Win64`, the plugin's
`Source/ThirdParty/Spout/lib/amd64`, the project's `Binaries/Win64`, the executable directory, and
finally the OS search order. The DLL is also delay-loaded (`PublicDelayLoadDLLs`) so the
module itself loads even when the DLL resolves late; loading it by full path at startup makes the
delay-load thunks bind by name instead of raising `0xC06D007E` (`ERROR_MOD_NOT_FOUND`). The plugin
logs the file it loaded:

```
LogSpoutPlugin: Loaded Spout.dll from '<path>'.
```

If that line is missing, or names a path you did not expect, that is the problem. Do not commit or
copy `Spout.dll` into any other location.

Where the DLL is in each layout (a missing or misplaced DLL is the most common cause of
"module could not be loaded"):

| Layout | `Spout.dll` location | How it gets there |
|---|---|---|
| Source plugin (this repository) | `Source/ThirdParty/Spout/lib/amd64/Spout.dll` | Committed, with `Spout.lib` beside it. Every other layout is derived from this copy. |
| Raw `RunUAT BuildPlugin -Package` output (also what Fab builds) | `Source/ThirdParty/Spout/lib/amd64/Spout.dll` only | `RuntimeDependencies` does not carry the DLL into `Binaries/Win64` here; the startup loader falls back to this copy. |
| Release ZIP | `Binaries/Win64/Spout.dll`, plus the `Source/ThirdParty` copy | `build.ps1` copies it in after `BuildPlugin` and verifies that its SHA-256 matches the repository copy. |
| Plugin installed in a project (editor or other modular build) | `Plugins/SpoutPlugin/Binaries/Win64/Spout.dll` | `RuntimeDependencies` staging to `$(BinaryOutputDir)`, the plugin's own binaries folder. |
| Packaged game | `<YourProject>/Binaries/Win64/Spout.dll`, beside the game executable | The module is linked into the game executable, so `$(BinaryOutputDir)` is the project's binaries folder. The plugin's own `Binaries` folder is not staged. |

## What ships in a release

A packaged plugin folder contains:

```
SpoutPlugin/
  Binaries/        (includes Spout.dll beside UnrealEditor-SpoutPlugin.dll)
  Content/
  Intermediate/    (precompiled UnrealGame objects; see below - do not strip)
  Resources/
  Source/          (includes ThirdParty/Spout, the Spout SDK)
  SpoutPlugin.uplugin
```

Precompiled releases must include `Spout.dll` in `Binaries/Win64`. If it is missing, users hit
"module could not be loaded" at load time.

Every release variant must also keep the `Source/` folder (at minimum
`Source/SpoutPlugin/SpoutPlugin.Build.cs` and `Source/SpoutPlugin/Public`). Unreal Build Tool
reads the module's `.Build.cs` on every compile and package, even when it uses the prebuilt
binary, so a source-less zip cannot be compiled or packaged against and fails with
"Could not find definition for module 'SpoutPlugin'". The prebuilt `Binaries/` let a
Blueprint-only project open in a matching editor without compiling; do not strip `Source/` to make
the download smaller.

## Building releases (RunUAT)

Releases are produced with `RunUAT BuildPlugin` per engine version. The repository's `build.ps1`
loops over engine versions 5.3 through 5.9, runs:

```
RunUAT BuildPlugin -Plugin="<...>/SpoutPlugin.uplugin" -Package="<out>" -TargetPlatforms=Win64 -Rocket
```

then strips `.pdb` files and zips a `SpoutPlugin/` folder per version (for example
`SpoutPlugin_1.0.1_UE5.7_Win64.zip`). `build.bat` is a one-line launcher for `build.ps1`.

`Intermediate/Build` is kept in the zips on purpose (since 0.0.9). `BuildPlugin` emits
precompiled `UnrealGame` Development and Shipping objects, generated headers and `.precompiled`
manifests there, as Unreal's plugin packaging expects. Keeping them is not a supported way to
package a game without Visual Studio, and the tested project-folder installs did not reuse them
(see below).

## Packaging a project that uses the plugin

The supported packaging workflow uses Visual Studio 2022 with the C++ workload, its MSVC
toolchain and a Windows SDK supported by the engine version, even when the prebuilt editor
binaries load without compiling. Packaging without a compiler is not supported.

Four different things happen, and only the first one works without a compiler:

- **Loading the prebuilt editor module.** In a matching editor, the archive's
  `UnrealEditor-SpoutPlugin.dll` loads without compiling (verified on UE 5.8 in a Blueprint-only
  project).
- **Linking the game executable.** A packaged Win64 game is monolithic: the plugin module is linked
  into the game executable. A package that uses the engine's prebuilt `UnrealGame.exe` cannot
  contain it, whatever the archive ships.
- **Compiling the plugin source.** In every tested configuration that produced a game containing
  the plugin from `<Project>/Plugins/`, Unreal compiled the plugin from the shipped `Source/` with
  MSVC and the Windows SDK.
- **Reusing precompiled objects.** Those configurations did not reuse the archive's precompiled
  `UnrealGame` objects. Whether an engine-level install reuses them was not tested for 1.0.0.

What was tested (UE 5.8.2, a pre-release 1.0.0 archive in `<Project>/Plugins/`, Development):

| Project | Plugin enablement | Game build | Packaged game |
|---|---|---|---|
| C++ project | release descriptor, dropped in | project target; plugin compiled from `Source/` and linked (Shipping too) | runs, streams work |
| Blueprint-only | release descriptor, dropped in | none; engine's prebuilt `UnrealGame.exe`; no `Spout.dll` staged | module `SpoutPlugin` could not be found |
| Blueprint-only | as above plus `{"Name": "SpoutPlugin", "Enabled": true}` in the `.uproject` | none | same failure |
| Blueprint-only | descriptor edited to `"EnabledByDefault": true` (test copy) | none | same failure |
| Blueprint-only | descriptor edited to `"EnabledByDefault": false` (test copy) plus the `.uproject` entry | temporary target; plugin compiled from `Source/` with MSVC and linked; `Spout.dll` staged | runs, sender published |

Why: Unreal Build Tool gives a Blueprint-only project its own (temporary) game target only when
the code plugins the project enables differ from the ones enabled by default
(`RequiresTempTargetForCodePlugin` in `NativeProjects.cs`). A plugin in `<Project>/Plugins/` counts
as enabled by default unless its descriptor says `"EnabledByDefault": false`. The release
descriptor does not set the key, so the plugin is in both sets and no game target is built. The
last row only demonstrates that mechanism: it changes the shipped descriptor, which 1.0.0 does
not do, and it still compiles with MSVC. It is not a supported setup.

- **Supported:** a C++ project with the plugin in `<Project>/Plugins/`, packaged with Visual
  Studio. For a Blueprint-only project, the verified workaround is to add a C++ class (Tools > New
  C++ Class) and package again.
- **C++ project, editor:** in the same test, building the C++ project's editor target also rebuilt
  the plugin's editor binaries from `Source/`, replacing the prebuilt ones from the archive.
- **Packaged-game runtime:** `Spout.dll` is staged next to the game executable automatically;
  nothing needs to be copied by hand. The packaged game must run on the D3D12 RHI, exactly
  like the editor.

Note: a precompiled binary built for one engine version will not load in another. Ship one zip
per engine version and label it clearly.

## Engine version matching

- Tested across UE 5.3 to 5.9 (Win64).
- Distribute a separate build per engine version. Users must download the build matching their
  exact engine.

## Clean rebuild (after an engine upgrade or a corrupted build)

Delete these, then regenerate project files and rebuild from scratch:

```
Project/Binaries
Project/Intermediate
Plugins/SpoutPlugin/Binaries
Plugins/SpoutPlugin/Intermediate
```

## Distribution channel

- **Fab:** marketplace installation into the engine. Fab builds the plugin itself from the submitted source package.
- **GitHub Releases** (`https://github.com/kessoning/Spout-UE5/releases`): free precompiled ZIPs, one per engine
  version.
- **Source:** `https://github.com/kessoning/Spout-UE5`.

Patreon is optional support for the project, never required for a download.

## License and attribution

- Licensed under the MIT License; see `LICENSE`, which ships in every release archive together with
  `THIRD_PARTY_NOTICES.md` (the Spout2 SDK's BSD 2-Clause license). Use, modification and
  redistribution, including commercial redistribution, are permitted. Every copy must keep the
  copyright and license notices in `LICENSE`, and redistributions of the Spout SDK must keep its BSD
  notice. Crediting this project is appreciated but not required.
- Based on the original repository by zuyi53 (`https://github.com/zuyi53/Spout-UE5`).
- Spout: `http://spout.zeal.co/`.

## Building and validating a release (maintainers)

The developer workflows below require the full repository, **PowerShell 7.2 or newer**, CMake,
and the Windows C++ toolchain for the selected Unreal engine. `build.bat` requires `pwsh`;
Windows PowerShell 5.1 is rejected before any release action.

Everything below runs from the repository root and returns a non-zero exit code on any failure, so
a partial or failed run can never be mistaken for a successful one.

### Release archives

```bat
:: Every declared engine must be installed and must pass. Any missing engine is fatal.
build.bat -FullMatrix -OutputRoot "D:\staging"

:: Only the engines you name are expected. The manifest is marked partial and lists what was omitted.
build.bat -Partial -EngineVersions 5.3,5.8 -OutputRoot "D:\staging"

:: Re-open and re-validate archives that already exist, without rebuilding.
build.bat -Partial -EngineVersions 5.3 -OutputRoot "D:\staging" -ValidateOnly
```

Each run writes a machine-readable `release-manifest-*.json` next to the artifacts, containing the
commit, working-tree dirty state, engine build IDs, UAT exit codes, per-archive SHA-256 hashes and
the content-validation result for every ZIP.

### Validation pipeline

```bat
:: Fast pre-release check: documentation drift, then the automation suite on both engine floors.
build.bat -TrackD

:: Boundary validation: UE 5.3 and UE 5.8, packaged hosts included.
build.bat -TrackD -BoundaryOnly

:: The complete matrix: every declared engine, Development and Shipping packaged hosts.
build.bat -TrackD -FullMatrix

:: Performance baselines (1080p60, 4K60, multi-stream, CPU fallback, idle, resize and restart churn).
build.bat -TrackD -Performance

:: Prove the release needs nothing beyond its declared inputs.
build.bat -TrackD -CleanSource
```

Individual stages can also be run directly:

```powershell
.\Scripts\Validate-Documentation.ps1
.\Scripts\Invoke-AutomationTests.ps1     -EngineVersions 5.3,5.8 -EvidenceRoot .\Artifacts\TrackD\dev
.\Scripts\Invoke-PackagedHostMatrix.ps1  -EngineVersions 5.8 -Configurations Development -EvidenceRoot .\Artifacts\TrackD\dev
.\Scripts\Invoke-PerformanceBaselines.ps1 -EngineVersion 5.8 -EvidenceRoot .\Artifacts\TrackD\dev
.\Scripts\New-CleanSourceCopy.ps1        -EvidenceRoot .\Artifacts\TrackD\dev
```

The automation tests, their test hooks and the compiled source identity are built only when the
plugin's root folder holds the marker file `SpoutTests.enable`. The pipeline writes it into every
plugin it stages for testing. Release archives and Fab packages are built without it, so the tests
never compile into a user's editor. To run the suite in your own checkout, create the empty file
`SpoutTests.enable` next to `SpoutPlugin.uplugin` in a fresh build tree, then run
`Automation RunTests SpoutPlugin`.

Evidence (build logs, staging manifests, smoke-test markers, performance JSON, hashes and Unreal
Insights traces) is retained under `Artifacts/TrackD/<run-id>/`, which is git-ignored and never
included in a release archive.

The release gates create fresh invocation directories and require the launched process to report
its run ID and embedded plugin/host source hashes. Packaged smoke tests compare actual pixels before
receiver close and run a separate native peer in both directions, including the published format
(advertised and real), alpha and resizing. The scripts build that peer automatically; CMake and a
Windows C++ toolchain are required.

Performance runs publish at a target of 60 Hz (1 Hz in the idle-receiver case), observe pixel
sequences during measurement, and fail below 90% delivery or on corruption. Their timing includes a
serialized pixel observer and queue drain; observer time and delivery latency are reported
separately. Compare against `baseline` (Spout loaded without stream work) and `noplugin` (a
separately built host with Spout disabled and no Spout module/SDK dependency). The control uses the
same host configuration, window and pacing, and attests its own compiled host identity plus Spout's
absence. These measurements are not a claim about unobserved asynchronous throughput.
`observerSeconds` includes waiting for queued plugin work as well as reading pixels; it cannot be
subtracted to derive an isolated plugin cost. The observer keeps at most one frame per stream in
flight and reports observed loss, rather than estimating transfers from accepted API calls. The
release gates cover ordinary transfer, resize churn and a paced close/reopen workload (one 1080p
stream closed and reopened every two seconds). The twice-per-second close/reopen stress case
(`startupshutdown`) is kept as a record: at the default retention budget its new receiver outputs
are refused between garbage collections, so it fails its delivery gate by contract. See
[release readiness](RELEASE_READINESS.md) for what qualification covers.

Build artifacts carry immutable `.identity.json` sidecars. Keep those with the artifacts:
`-ValidateOnly` and `-ExistingExecutable` reject missing or mismatched provenance and preserve the
original build identity. Legacy artifacts require rebuilding. Manifest merging rejects duplicate
rows and mixed source builds; use `-RowManifestPaths` to select the exact attempts after retries.
