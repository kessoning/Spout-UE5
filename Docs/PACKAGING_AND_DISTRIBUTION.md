# Packaging and Distribution

How the plugin is built for release and what must ship with it.

## Spout.dll staging

`SpoutPlugin.Build.cs` registers `Spout.dll` as a runtime dependency staged to
`$(BinaryOutputDir)/Spout.dll`. `$(BinaryOutputDir)` is the folder of the binary the module
is linked into, so it resolves to two different places depending on the build type:

- **Editor / modular builds** (normal project builds, `RunUAT BuildPlugin`, precompiled
  release zips): the plugin's own `Binaries/Win64`, beside `UnrealEditor-SpoutPlugin.dll`.
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
"Could not find definition for module 'SpoutPlugin'". The prebuilt `Binaries/` still let
Blueprint-only users with a matching engine skip compilation; do not strip `Source/` to make the
download smaller.

## Building releases (RunUAT)

Releases are produced with `RunUAT BuildPlugin` per engine version. The repository's `build.ps1`
loops over engine versions 5.3 through 5.8, runs:

```
RunUAT BuildPlugin -Plugin="<...>/SpoutPlugin.uplugin" -Package="<out>" -TargetPlatforms=Win64 -Rocket
```

then strips `.pdb` files and zips a `SpoutPlugin/` folder per version (for example
`SpoutPlugin_0.0.9_UE5.7_Win64.zip`). `build.bat` is a one-line launcher for `build.ps1`.

`Intermediate/Build` is kept in the zips on purpose (since 0.0.9). `BuildPlugin` emits
precompiled `UnrealGame` Development and Shipping objects plus `.precompiled` manifests
there. When the plugin is installed under `Engine/Plugins/Marketplace/SpoutPlugin/`, those
let a Blueprint-only project package **without Visual Studio**. Stripping them (as releases
up to 0.0.8 did) makes no-compiler packaging impossible.

## Packaging a project that uses the plugin

- **Plugin in `<Project>/Plugins/` (the usual setup):** Unreal Build Tool always recompiles
  project plugins from source when packaging, so Visual Studio (with the C++ toolchain and a
  matching Windows SDK) is required. This is a UBT rule, not a plugin choice.
- **Blueprint-only project, no Visual Studio:** install the plugin into the engine instead,
  at `Engine/Plugins/Marketplace/SpoutPlugin/`, using a release zip that still contains
  `Intermediate/` (0.0.9 or later). Packaging then links the shipped precompiled objects.
  Only Development and Shipping configurations are covered; DebugGame still needs a compiler.
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
