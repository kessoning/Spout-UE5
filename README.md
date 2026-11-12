# Spout Texture Sharing for Unreal Engine 5

![Fab: coming soon](https://img.shields.io/badge/Fab-coming%20soon-lightgrey)
![GitHub Releases: v1.0.0 coming soon](https://img.shields.io/badge/GitHub%20Releases-v1.0.0%20coming%20soon-lightgrey)
![Unreal Engine 5.3 to 5.8](https://img.shields.io/badge/Unreal%20Engine-5.3%20to%205.8-313131)
![Windows 64-bit, DirectX 12](https://img.shields.io/badge/Windows%2064--bit-DirectX%2012-0078D6)
![License: MIT](https://img.shields.io/badge/License-MIT-green)

Hi, I'm Kesson, a new media artist and creative technologist. I built this plugin because my own work needed Unreal Engine to exchange live images with tools like TouchDesigner, and I found few practical, up-to-date ways to do that with Unreal's DirectX 12 renderer. It began as a fork of [zuyi53/Spout-UE5](https://github.com/zuyi53/Spout-UE5), an existing open-source Spout plugin, and grew from there. I share it freely because I believe creative technology should be accessible, and that tools like this get better when people build on them together.

The plugin shares live GPU textures between Unreal Engine and other Windows applications through [Spout](https://spout.zeal.co/), with no disk or network transfer: send Unreal's game view or any render target out, and bring other applications' streams in.

**Version 1.0.0** · Unreal Engine 5.3 to 5.8 · Windows 64-bit · DirectX 12 · MIT License

In Unreal the plugin appears as **Spout Plugin** (module `SpoutPlugin`, category Rendering).

## Features

- **Drag-and-drop actors:** a Spout Sender and a Spout Receiver in the Place Actors panel. No Blueprint needed.
- **Send** the game view, with or without the editor around it in Play In Editor, or any render target.
- **Receive** into a texture, your own material, a render target, or the receiver actor's built-in screen.
- **Colour control:** a per-receiver Color Space setting (Auto, sRGB, Linear), including exact decoding of Unreal viewport streams.
- **Formats:** BGRA8, RGBA8, RGB10A2 and RGBA16F.
- **GPU to GPU:** the plugin bridges Unreal's DirectX 12 renderer and Spout's DirectX 11 shared textures.
- **Blueprint nodes and a C++ API**, with example content and a full user manual (PDF) in every download.

## What's new in 1.0.0

1.0.0 is the first major release. Compared with the 0.0.x builds:

- Drag-and-drop **Spout Sender** and **Spout Receiver** actors.
- **Game Viewport Only**, the new default sender source.
- **Asynchronous sending and receiving** by default, so Unreal's render thread does not wait for the copies.
- **A new interop path**, with a time limit on every wait.
- **Correct colours for Unreal viewport streams** received with Color Space sRGB.
- **Fixes:** no crash when resizing a window while sending the viewport; the Spout Demo Receiver and other OpenGL receivers now accept streams from 8-bit sRGB render targets; Close Receiver never destroys a texture or material it did not create.
- **Automated release qualification** on every supported engine version, and the user manual.

**Upgrading from 0.0.x or a test build:**

- Spout Sender nodes left on Game Viewport switch to Game Viewport Only when their Blueprint loads. To keep sending the whole window, select Game Viewport (Whole Window) again and save.
- Receive nodes whose Async Receive box was left unchecked turn it on.

The full list is in the [changelog](https://github.com/kessoning/Spout-UE5/blob/main/CHANGELOG.md).

## Status and testing

1.0.0 was tested on every supported engine version, with automated tests and packaged Development and Shipping test games. The two newest additions (the actors and Game Viewport Only) received focused checks. Testing used Spout's own demo applications and an independent native test sender on NVIDIA GPUs.

Not covered by the automated release tests:

- other GPU vendors and multi-GPU machines;
- driver resets;
- external applications such as TouchDesigner, Resolume or OBS;
- installation into the engine's own plugin folder, which is how Fab installs plugins.

Test the plugin with your own engine version, hardware and applications before relying on it for production or a live show. The details are in [Release readiness](Docs/RELEASE_READINESS.md) and [Limitations](Docs/LIMITATIONS.md).

## Download

The plugin is free.

| Option | Best for | Where |
|---|---|---|
| **Fab** | Installing from the marketplace into your engine | Coming soon: the listing is awaiting Fab's review. Its link will be added here once it is live. |
| **GitHub Releases** | Free precompiled ZIPs, one per engine version | Coming soon: the v1.0.0 ZIPs will be attached to the v1.0.0 release on the repository's Releases page. |
| **Source** | Custom builds and modifications | https://github.com/kessoning/Spout-UE5 |

A precompiled ZIP only works with the engine version it was built for. Download the one that matches your Unreal Engine version exactly (for example `SpoutPlugin_1.0.0_UE5.7_Win64.zip` for UE 5.7).

## Installation

### Requirements

- Windows 64-bit, Unreal Engine 5.3 to 5.8, and the **DirectX 12** RHI. The plugin does not start on any other RHI and logs `SpoutPlugin requires DX12 RHI.`
- Both applications sharing a stream must run on the same GPU.
- To build from source or package a game: Visual Studio 2022 with the *Game development with C++* workload, its MSVC toolchain and a Windows SDK supported by your engine version. Opening a project with the precompiled plugin in a matching editor needs no compiler.

### Precompiled ZIP

1. Extract the `SpoutPlugin` folder into your project's `Plugins` folder (`MyProject/Plugins/SpoutPlugin`).
2. Check that `Plugins/SpoutPlugin/Binaries/Win64/Spout.dll` is there (see [`Spout.dll`](#spoutdll) below).
3. Set Edit > Project Settings > Platforms > Windows > Default RHI to **DirectX 12**, then restart the editor.
4. Open the project. A plugin in the project's `Plugins` folder is enabled by default (Edit > Plugins, search "Spout"). If Unreal offers to rebuild the plugin, the ZIP does not match your engine version.

### Fab

Install the plugin to your engine version from Fab, then enable **Spout Plugin** in Edit > Plugins and set the project to DirectX 12 as above. Fab builds the plugin from the same source.

### Build from source

1. Use a C++ project. For a Blueprint-only project, first add a C++ class (Tools > New C++ Class).
2. Copy or clone the repository into `MyProject/Plugins/SpoutPlugin`.
3. Set the project to DirectX 12, generate Visual Studio project files from the `.uproject`, and build.

`Spout.dll` is staged automatically; do not copy it by hand.

### Packaging a game

Packaging a game that uses the plugin requires the Visual Studio setup above: the plugin is compiled into the game executable from its `Source` folder.

**Blueprint-only projects:** with the plugin in the project's `Plugins` folder, a UE 5.8 packaged game built during release testing did not contain the plugin, and it reported at startup that module `SpoutPlugin` could not be found. Add a C++ class to the project (Tools > New C++ Class) and package again. Enabling the plugin in the `.uproject` does not help. The packaged game must also run on DirectX 12.

### `Spout.dll`

The bundled `Spout.dll` is staged automatically, and its location depends on the layout: the source (`Source/ThirdParty/Spout/lib/amd64`), raw `RunUAT BuildPlugin` output, a release ZIP, a project install (`Plugins/SpoutPlugin/Binaries/Win64`) or a packaged game (beside the game executable). At startup the plugin logs the file it loaded (`Loaded Spout.dll from '<path>'`). If the plugin does not load, the full table is in [Packaging and distribution](https://github.com/kessoning/Spout-UE5/blob/main/Docs/PACKAGING_AND_DISTRIBUTION.md#spoutdll-staging) and the fixes are under Troubleshooting in the [user manual](Docs/Manual/SpoutPlugin_User_Manual.pdf).

## Quick start

### Drag-and-drop actors

1. Open **Place Actors** (Window > Place Actors) and choose the **Spout** category. If the panel is short, it is under **More**; searching "Spout" also finds both actors.
2. Drag **Spout Sender** and/or **Spout Receiver** into the level and press **Play**. Nothing streams outside Play.
3. Adjust them in the **Details** panel (category Spout).

- **Spout Sender** sends Game Viewport Only by default, under a unique name per placement (`UE Spout Sender`, `UE Spout Sender 2`, and so on). It never takes over a name another sender already publishes.
- **Spout Receiver** with an empty **Sender Name** connects to a sender from another application and stays with it, holding the last frame if the sender stops. Type a name, or use **Choose Running Sender**, to receive a specific sender. For a stream coming from an Unreal viewport, set **Color Space** to **sRGB**.
- The receiver's screen is an unlit surface in the level, so the level's exposure and tone mapping still affect what you see. For exact colours, use its Output Render Target.

### Blueprints

- **Send:** call **Spout Sender** on Tick or a timer, with a **Spout Name** and a **Send Texture From** source. **Close Sender** stops it.
- **Receive:** create a Dynamic Material Instance once, then call **Spout Receiver With Material** on Tick with the same Spout Name. It returns the received texture and binds it to the texture parameter you name (for example `SpoutTexture`). **Close Receiver** ends the stream.
- **Status and discovery:** **Spout Info**, **Get Sender Info**, **Get Spout Stream Status** and **Get Spout Runtime Status**.

The example Blueprints `BP_SpoutSenderExample_` and `BP_SpoutReceiverExample` are in the plugin's content (enable Show Plugin Content in the Content Browser). Every node, and C++ use, is described in the [Blueprint API reference](Docs/BLUEPRINT_API.md) and the [user manual](Docs/Manual/SpoutPlugin_User_Manual.pdf).

## Documentation

Shipped with the plugin:

- [User manual (PDF)](Docs/Manual/SpoutPlugin_User_Manual.pdf): everything below in one document, including the C++ API reference.
- [Blueprint API reference](Docs/BLUEPRINT_API.md).
- [Limitations](Docs/LIMITATIONS.md): platform, interop, formats and colour, operational limits, performance.
- [Release readiness](Docs/RELEASE_READINESS.md): how this release was tested, and what was not covered.

In the repository: [Installation](https://github.com/kessoning/Spout-UE5/blob/main/Docs/INSTALLATION.md), [Quick start](https://github.com/kessoning/Spout-UE5/blob/main/Docs/QUICK_START.md), [Sender](https://github.com/kessoning/Spout-UE5/blob/main/Docs/SENDER.md), [Receiver](https://github.com/kessoning/Spout-UE5/blob/main/Docs/RECEIVER.md), [Render targets and formats](https://github.com/kessoning/Spout-UE5/blob/main/Docs/RENDER_TARGETS_AND_FORMATS.md), [SceneCapture2D](https://github.com/kessoning/Spout-UE5/blob/main/Docs/SCENECAPTURE2D.md), [Packaging and distribution](https://github.com/kessoning/Spout-UE5/blob/main/Docs/PACKAGING_AND_DISTRIBUTION.md) (including building and validating releases), [Troubleshooting](https://github.com/kessoning/Spout-UE5/blob/main/Docs/TROUBLESHOOTING.md), [C++ API](https://github.com/kessoning/Spout-UE5/blob/main/Docs/CPP_API.md) and the [changelog](https://github.com/kessoning/Spout-UE5/blob/main/CHANGELOG.md).

## Support

Bug reports and questions are welcome through the GitHub repository. If the plugin is useful to you and you would like to support its development, you can. It is entirely optional; the plugin is and stays free.

- Patreon (ongoing support and development updates): https://www.patreon.com/c/kesson_lab
- Ko-fi (one-time donation): https://ko-fi.com/kesson

If you build something with the plugin and would like to share it, get in touch: a showcase section may follow.

## Credits and license

- **License:** MIT License, see [LICENSE](LICENSE). You may use, modify and redistribute the plugin, including commercially. Every copy or substantial portion must keep the copyright and license notices in `LICENSE`, and redistributions of the bundled Spout SDK must keep its BSD notice from [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Crediting this project is appreciated but not required.
- **Original work:** based on [zuyi53/Spout-UE5](https://github.com/zuyi53/Spout-UE5) by RockmanZuyi, whose copyright is preserved in the license. Developed and maintained by Giovanni Muzio (Kesson).
- **Spout:** the bundled Spout2 SDK (headers, `Spout.dll`, `Spout.lib`) is by Lynn Jarvis, under the BSD 2-Clause license. Both license files ship in every release archive.
- **Development tools:** parts of the plugin were refined with AI-assisted tools, used for debugging, refactoring and documentation. The overall architecture, the implementation and most of the code are the author's.
