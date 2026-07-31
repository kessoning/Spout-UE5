# Installation

This page covers installing SpoutPlugin into an Unreal Engine 5 project, both from a
precompiled binary release and from source.

SpoutPlugin is a project-local plugin. Install it into your project's `Plugins/` folder,
not into the engine's plugin folder.

## Requirements

- Windows 64-bit.
- Unreal Engine 5 (5.3 through 5.8).
- The project must run on the DirectX 12 RHI. The plugin creates a D3D11-on-D3D12 interop
  device and logs `SpoutPlugin requires DX12 RHI.` and stops if the active RHI is not D3D12.
- For source builds: Visual Studio 2022 with the C++ toolchain and a matching Windows SDK.
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

1. Copy the `SpoutPlugin` folder into your project's `Plugins/` directory.
2. Set the project RHI to DirectX 12.
3. Right-click the `.uproject` and choose Generate Visual Studio project files.
4. Build the project from Visual Studio or from the Unreal Editor when it prompts to compile.

`Spout.dll` is staged automatically. `SpoutPlugin.Build.cs` registers it as a runtime
dependency to `$(BinaryOutputDir)/Spout.dll`, which is the plugin's own `Binaries/Win64`
folder. This is the same location used for normal project builds, for `RunUAT BuildPlugin`
packaging, and for precompiled release zips, and it matches where the startup diagnostics
look. Do not manually copy `Spout.dll` elsewhere.

## Set the project RHI to DirectX 12

1. Edit > Project Settings > Platforms > Windows.
2. Under Default RHI, select DirectX 12.
3. Restart the editor when prompted.

## Enable the plugin

The plugin descriptor sets `EnabledByDefault: true`, so it is usually active once present.
To confirm: Edit > Plugins, search for "Spout Plugin", and verify it is enabled. Restart the
editor if you change the state.

## Verify the plugin loaded

Open `Project/Saved/Logs/ProjectName.log` and look for `Spout Plugin Loaded` from the
`LogSpoutPlugin` category. On first use of any node you should also see
`SpoutDX11on12 Initialized.`. If `Spout.dll` is missing, the plugin logs an explicit error or
warning at startup naming the paths it checked.

See [PACKAGING_AND_DISTRIBUTION.md](PACKAGING_AND_DISTRIBUTION.md) for the clean-rebuild steps
after an engine upgrade, and [TROUBLESHOOTING.md](TROUBLESHOOTING.md) if the module fails to
load.
