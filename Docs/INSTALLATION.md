# Installation

This page covers installing SpoutPlugin into an Unreal Engine 5 project, both from a
precompiled binary release and from source, and what packaging a game requires.

Three tasks have different requirements:

- **Installing the prebuilt plugin and opening a project in the editor** needs only the archive
  that matches your exact engine version. No compiler is needed to open a Blueprint-only project
  with the prebuilt plugin in a matching editor.
- **Building or modifying the source** needs Visual Studio 2022 with the *Game development with
  C++* workload, its MSVC toolchain and a Windows SDK supported by your engine version.
- **Packaging a game that uses the plugin** is supported with the same Visual Studio setup. See
  [Packaging a game](#packaging-a-game).

## Requirements

- Windows 64-bit.
- Unreal Engine 5 (5.3 through 5.8).
- The project must run on the DirectX 12 RHI. The plugin creates a D3D11-on-D3D12 interop
  device and logs `SpoutPlugin requires DX12 RHI.` and stops if the active RHI is not D3D12.
- For source builds and for packaging: Visual Studio 2022 with the C++ workload and toolchain and
  a Windows SDK supported by your engine version.
- `Spout.dll` (bundled under `ThirdParty/Spout/lib/amd64/` and staged to the plugin's
  `Binaries/Win64/` at build time).

## Option A - Precompiled binary release

Prebuilt binaries are distributed through Patreon with free download.

1. Download the ZIP that matches your exact Unreal Engine version. A binary built for one
   engine version will not load in another.
2. Extract the `SpoutPlugin` folder into your project's `Plugins/` directory, for example
   `MyProject/Plugins/SpoutPlugin/`.
3. Confirm `Spout.dll` is present at `Plugins/SpoutPlugin/Binaries/Win64/Spout.dll`, beside
   `UnrealEditor-SpoutPlugin.dll`.
4. Set the project RHI to DirectX 12 (see below).
5. Open the project. If Unreal offers to rebuild the plugin because it was compiled with a
   different engine version, the binary does not match your engine - use the matching ZIP or
   build from source instead.

## Option B - Build from source

1. Use a C++ project, which is the supported source-build workflow (for a Blueprint-only
   project, add a C++ class first: Tools > New C++ Class). Copy the `SpoutPlugin` folder into your
   project's `Plugins/` directory.
2. Set the project RHI to DirectX 12.
3. Right-click the `.uproject` and choose Generate Visual Studio project files.
4. Build the project from Visual Studio or from the Unreal Editor when it prompts to compile.

`Spout.dll` is staged automatically. `SpoutPlugin.Build.cs` registers it as a runtime
dependency to `$(BinaryOutputDir)/Spout.dll`. For a source build in your project's `Plugins/`
folder that resolves to the plugin's own `Binaries/Win64`, which is where the startup
diagnostics look first. In a packaged (monolithic) game the same rule stages the DLL next to
your game executable instead. Note that raw `RunUAT BuildPlugin` output does not include the
DLL; the release pipeline stages it into the precompiled zips explicitly. See
[PACKAGING_AND_DISTRIBUTION.md](PACKAGING_AND_DISTRIBUTION.md) for every layout. Do not
manually copy `Spout.dll` elsewhere.

## Set the project RHI to DirectX 12

1. Edit > Project Settings > Platforms > Windows.
2. Under Default RHI, select DirectX 12.
3. Restart the editor when prompted.

## Enable the plugin

A plugin in the project's `Plugins/` folder is enabled by default, so it is usually active once
present. The descriptor inside the release archives does not set `EnabledByDefault`; if the
plugin is installed into the engine instead (for example by Fab), enable it for each project.
To confirm: Edit > Plugins, search for "Spout Plugin", and verify it is enabled. Restart the
editor if you change the state.

## Packaging a game

The supported packaging workflow uses Visual Studio 2022 with the C++ workload, its MSVC
toolchain and a supported Windows SDK, even when the prebuilt plugin loads in the editor without
compiling. Packaging without a compiler is not supported. The game executable has to contain the
plugin, and the prebuilt editor binaries do not provide that.

- **C++ project, plugin in the project's `Plugins/` folder:** Unreal compiles the plugin from its
  `Source/` folder and links it into the game. This is the tested configuration (UE 5.8,
  Development and Shipping). In that test, building the project also rebuilt the plugin's editor
  binaries from `Source/`, replacing the prebuilt ones from the archive.
- **Blueprint-only project, plugin in the project's `Plugins/` folder:** in our UE 5.8 test the
  package used Unreal's prebuilt game executable without the plugin, and the game reported at
  startup that module `SpoutPlugin` could not be found. Enabling the plugin explicitly in the
  `.uproject` did not change that. The verified workaround is to add a C++ class to the project
  (Tools > New C++ Class) and package again.
- In the packaged game, `Spout.dll` is staged next to the game executable
  (`<Project>/Binaries/Win64/`). The game must run on the DirectX 12 RHI.

## Verify the plugin loaded

Open `Project/Saved/Logs/ProjectName.log` and look for `Spout Plugin Loaded` from the
`LogSpoutPlugin` category. On first use of any node you should also see
`SpoutDX11on12 Initialized.`. If `Spout.dll` is missing, the plugin logs an explicit error or
warning at startup naming the paths it checked.

See [PACKAGING_AND_DISTRIBUTION.md](PACKAGING_AND_DISTRIBUTION.md) for the clean-rebuild steps
after an engine upgrade, and [TROUBLESHOOTING.md](TROUBLESHOOTING.md) if the module fails to
load.
