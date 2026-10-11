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
  own `Binaries` and `ThirdParty` folders are not staged at all in a packaged game.

The startup loader (0.0.9+) checks, in order: the plugin's `Binaries/Win64`, the plugin's
`ThirdParty/Spout/lib/amd64`, the project's `Binaries/Win64`, the executable directory, and
finally the OS search order. The DLL is also delay-loaded (`PublicDelayLoadDLLs`) so the
module itself loads even when the DLL resolves late. Do not commit or copy `Spout.dll` into
any other location.

Source layout for the DLL and import library:

```
ThirdParty/Spout/lib/amd64/Spout.dll
ThirdParty/Spout/lib/amd64/Spout.lib
```

## What ships in a release

A packaged plugin folder contains:

```
SpoutPlugin/
  Binaries/        (includes Spout.dll beside UnrealEditor-SpoutPlugin.dll)
  Content/
  Intermediate/    (precompiled UnrealGame objects; see below - do not strip)
  Resources/
  Source/
  ThirdParty/
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
loops over engine versions 5.3 through 5.8, runs:

```
RunUAT BuildPlugin -Plugin="<...>/SpoutPlugin.uplugin" -Package="<out>" -TargetPlatforms=Win64 -Rocket
```

then strips `.pdb` files and zips a `SpoutPlugin/` folder per version (for example
`SpoutPlugin_1.0.0_UE5.7_Win64.zip`). `build.bat` is a one-line launcher for `build.ps1`.

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

- Tested across UE 5.3 to 5.8 (Win64).
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

Prebuilt binaries are distributed through Patreon with free download. Source is available on
GitHub (`https://github.com/kessoning/Spout-UE5`).

## License and attribution

- Licensed under the MIT License (see the README license section; if a standalone `LICENSE`
  file is not present in your copy, the MIT terms in the README apply).
- Based on the original repository by zuyi53 (`https://github.com/zuyi53/Spout-UE5`).
- Spout: `http://spout.zeal.co/`.
- Redistributing the plugin as-is and selling it as a standalone product goes against the
  spirit of the project, even though the MIT license permits commercial use.
