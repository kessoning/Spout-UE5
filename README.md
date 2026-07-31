[![Patreon](https://img.shields.io/badge/Patreon-Support%20this%20project-FF424D?logo=patreon&logoColor=white)](https://www.patreon.com/c/kesson_lab)

# SpoutPlugin (UE5)

Windows-only Unreal Engine 5 plugin that sends and receives textures via Spout using a DX11-on-DX12 bridge.

## Introduction
This plugin started from a real production need rather than as a polished, ready-made solution.
While working with Unreal Engine 5, I noticed there were very few practical and up-to-date ways to integrate UE with other real-time tools such as TouchDesigner, especially when dealing with modern DX12 workflows.

So I decided to build my own solution. I strongly believe in the idea that sharing is caring, which is why I did not start entirely from scratch. Instead, I forked an existing open-source repository and focused on making it work again across multiple Unreal Engine 5 versions, adapting it to current engine constraints and workflows.

Although I am comfortable working with C++, DirectX 11, and DirectX 12, this tool was developed primarily from the perspective of an artist and technical user rather than a traditional software engineer. The focus is therefore on solving real workflow problems and enabling practical integration, rather than building a perfectly abstracted or enterprise-grade system.

The goal of this project is to provide a clear and debuggable bridge between Unreal Engine and Spout, without hiding complexity, especially when dealing with DX11-on-DX12 interop and GPU resource sharing.

This plugin should not be considered fully production-ready. However, depending on your use case and with enough testing, it can be reliable enough. At least, it has been for my own projects.

I also started developing this plugin with future integration into generative AI workflows in mind.

Thanks to zuyi53, the original author of the repository this work is based on: [[link](https://github.com/zuyi53/Spout-UE5)].
If you find bugs or issues, please report them.
If you need a production-ready, paid solution with more features beyond Spout, consider the Off World Live plugin: [[link](https://offworld.live/)].

## Features

- **Spout Sender**
  - Send textures to external applications either from the **game viewport** or from a `UTextureRenderTarget2D`
  - Automatic creation and update of the Spout sender stream

- **Spout Receiver**
  - Receive Spout streams into a transient `UTexture2D`
  - Optional output to a user-provided `UTextureRenderTarget2D`
  - Provides a dynamic material instance ready to be applied to meshes

- **Rendering and Interop**
  - D3D11-on-DX12 interoperability layer built on top of Unreal Engine 5’s D3D12 RHI
  - GPU-based texture sharing via Spout, without disk or network transfer

## Requirements
- Windows 64-bit
- Unreal Engine 5 with D3D12 RHI enabled (Spout requires D3D11 interop)
- Spout runtime (bundled `Spout.dll` and `Spout.lib` under `ThirdParty/Spout`)

## Build

This plugin is intended to be built as part of a **project**, not as an engine plugin.

## Supported Unreal Engine versions

**UE 5.3, 5.4, 5.5, 5.6, 5.7 and 5.8 — all supported, all validated.**

Every release is built for all six with `RunUAT BuildPlugin -Rocket` (Editor Development,
UnrealGame Development and UnrealGame Shipping), and every generated archive is reopened and
content-checked before it is published.

Beyond compiling, each supported version is validated by packaging a real host project, launching
it unattended, and requiring it to prove that the plugin module loaded, that `Spout.dll` was
resolved from the packaged path, and that a full sender/receiver/level-travel lifecycle ran and
shut down cleanly. Both Development and Shipping hosts are checked. The evidence is retained under
`Artifacts/TrackD/`.

An earlier version of this README said support started at UE 5.5 because 5.3 and 5.4 failed to
build against an installed engine. That is no longer the case and has not been since 0.0.8.

> A plugin binary built for one engine version will not load in another. Use the archive that
> matches your engine, or build from source.

Pre-compiled binaries are also available on Patreon:
👉 https://www.patreon.com/posts/spout-plugin-for-146572687

---

## Build steps

1. Copy the `SpoutPlugin` folder into your project’s `Plugins/` directory.
2. Make sure the plugin is enabled in your project settings.
3. Ensure your project is using the **D3D12 RHI** (required).
4. Generate project files if needed.
5. Build the project using your IDE or Unreal Editor.

The bundled `Spout.dll` is staged automatically during the build. Exactly *where* it lands depends
on how the plugin is being used — see [Where `Spout.dll` ends up](#where-spoutdll-ends-up) below.

---

## Where `Spout.dll` ends up

There are five distinct layouts, and confusing them is the single most common cause of
`Plugin 'SpoutPlugin' failed to load` reports. They are not interchangeable.

| # | Layout | `Spout.dll` location | How it gets there |
|---|---|---|---|
| 1 | **Source plugin** in this repository | `ThirdParty/Spout/lib/amd64/Spout.dll` | Committed. This is the master copy every other layout is derived from. |
| 2 | **Raw `RunUAT BuildPlugin -Package` output** | `ThirdParty/Spout/lib/amd64/Spout.dll` **only** | `RuntimeDependencies` does **not** carry the DLL into `Binaries/Win64` here. This surprises people; it is why layout 3 exists. |
| 3 | **Release archive** (the ZIP you download) | `Binaries/Win64/Spout.dll`, plus the `ThirdParty` copy | `build.ps1` copies it in explicitly after `BuildPlugin`, then verifies the copy's SHA-256 matches the repository copy. |
| 4 | **Plugin installed in a project**, editor or other modular build | `Plugins/SpoutPlugin/Binaries/Win64/Spout.dll` | `RuntimeDependencies` staging `$(BinaryOutputDir)`, which for a modular build is the plugin's own binaries folder. |
| 5 | **Packaged game** | `<YourProject>/Binaries/Win64/Spout.dll`, beside the game executable | The plugin module is linked *into* the game executable in a monolithic build, so the same `$(BinaryOutputDir)` resolves to the **project's** binaries folder. The plugin's own `Binaries` folder is not staged at all in a packaged game — do not go looking for it there. |

In every layout the DLL is delay-loaded, and the module additionally loads it by full path at
startup so the delay-load thunks bind by name instead of raising `0xC06D007E`
(`ERROR_MOD_NOT_FOUND`). The plugin logs the path it resolved:

```
LogSpoutPlugin: Loaded Spout.dll from '<path>'.
```

If that line is missing or names a path you did not expect, that is the problem.

Release archives also deliberately **keep** `Intermediate/Build`. It carries the precompiled
UnrealGame Development and Shipping objects and the `.precompiled` manifests that let
Blueprint-only users package a project without Visual Studio. Deleting it makes such packaging
impossible.

---

## Notes

- This plugin is **Windows-only**
- A working C++ toolchain is required (Visual Studio with the appropriate Windows SDK)
- If you encounter build errors after upgrading Unreal Engine versions:
  - Clean the project
  - Regenerate project files
  - Rebuild from scratch



## Installation

Pre-built versions of this plugin are distributed through Patreon **with free download**.

👉 **Download access and updates are available here:**  
https://www.patreon.com/posts/spout-plugin-for-146572687

If you prefer building from source, clone this repository and follow the build steps above.

In some cases, you may also be able to drop the plugin into your project as-is.  
When opening the project, Unreal may ask to rebuild the plugin because it was compiled with a different engine version. If that happens, click **Yes** and let Unreal rebuild it.

This approach does **not work 100% of the time**, depending on engine version and toolchain, but it may be worth trying before building manually.



## Usage

> The plugin includes ready-to-use **example Blueprints** under the plugin `Content/` folder, which can be used to quickly test both Sender and Receiver setups without writing any code.

### Sender

- Call `USpoutBPFunctionLibrary::SpoutSender` every frame (Tick) or on a timer.
- Set a **Spout Name**, which uniquely identifies the stream and must match the receiver.
- Choose what to send using **Send Texture From**:
  - `GameViewport` – sends the active game viewport (backbuffer).
  - `TextureRenderTarget2D` – sends the contents of a specified render target.
- When using `TextureRenderTarget2D`, ensure the render target asset is valid and updated.
- The sender automatically creates and updates the Spout stream while it is being called.

#### SceneCapture2D and Render Target Notes

When using a `TextureRenderTarget2D` as the source for the Sender, a common setup is to drive it using a `SceneCapture2D`, as shown in the example Blueprints included with the plugin.

In this case, the `SceneCapture2D` **Capture Source** must be set to:

- **~~`FinalColor (LDR)`~~**
- **Final Color (HDR) in Linear Working Color Space**

and **not** to `SceneColor (HDR) in RGB`.

If `SceneColor (HDR)` is used, the texture sent through Spout may appear **black** on the receiving application.

At the moment, this limitation is still under investigation. A proper fix or improvement will be added in a future update if possible.  
If you have suggestions or insights on how to handle this correctly, contributions and advice are very welcome.

Once the Capture Source is set correctly, the `Texture Target` of the `SceneCapture2D` can be safely used as the input Render Target for the Spout Sender.

#### Colour space when sending

Spout carries a DXGI format and nothing else, so the format is what tells a receiver whether the
pixels are gamma-encoded or linear. What the sender publishes follows directly from the source:

| Source render target | Published format | What a receiver on **Auto** does |
|---|---|---|
| 8-bit, sRGB (`Force Linear Gamma` **off**) | `..._UNORM_SRGB` | decodes as sRGB — correct |
| 8-bit, linear (`Force Linear Gamma` **on**) | `..._UNORM` | decodes as sRGB — **set the receiver to Linear** |
| `A2B10G10R10` | `R10G10B10A2_UNORM` | linear — correct |
| `FloatRGBA` (HDR) | `R16G16B16A16_FLOAT` | linear — correct |

`Create Texture Render Target 2D` defaults to `Force Linear Gamma = true`, which is the second row.
That is the one combination where the two sides can disagree, because most Spout applications
publish plain 8-bit `_UNORM` for ordinary gamma-encoded output and receivers assume it. Either turn
`Force Linear Gamma` off so the format states the colour space itself, or set **Color Space =
Linear** on the receiving side.

The sender never converts or reinterprets: a format it cannot share is refused with a log line
naming it, rather than being published as something it is not.

**Notes**
- Only one sender can own a given Spout Name at a time.
- The sender must be called continuously to keep the stream alive.
- The sender takes Spout's cross-process texture lock around every publish and announces each frame
  through Spout's frame counter, so receivers see whole frames and can tell a new frame from a
  repeated one. If a receiver holds the texture longer than the sender's frame time, that frame is
  skipped rather than overwritten mid-read, and the skip is logged.
- This workflow is Windows-only and relies on DX11-on-DX12 GPU interop.


---

### Receiver

- Call `USpoutBPFunctionLibrary::SpoutReceiver` every frame (Tick) or on a timer.
- The **Spout Name** must exactly match the name used by the sender.
- The function outputs:
  - `OutTexture` – a transient `UTexture2D` updated from the Spout stream.
  - `OutMat` – a dynamic material instance bound to the received texture.
  - Optional `UTextureRenderTarget2D` – the receiver will copy the stream into it.
- Always validate `OutMat` before using it, as the sender may not be available or may reconnect.

#### Ownership and closing

There is **one receiver session per Spout sender name**.

- `OutTexture` belongs to the plugin, which keeps it alive while the receiver is open. Every graph
  receiving the same sender name shares that one texture, because the session is per name.
- **Each graph gets its own material instance**, created from its own `InputMaterial`. Two graphs
  receiving the same name with different input materials each keep their own; a graph is never
  handed another graph's material because the sender names matched.
- Call `CloseReceiver` when a receiver is no longer needed, passing back the `OutTexture` and
  `OutMat` you were given. It closes the shared session for **every** graph receiving that name,
  detaches the Spout texture from **every** material the plugin bound it into, releases the plugin's
  texture, and clears both variables.
- Graphs still receiving that name recover on their next `SpoutReceiver` call, with a **new** texture
  object. References you handed the old texture to (a UMG image, another material) keep the old one
  and stop updating, so re-read `OutTexture` after a close.
- `CloseReceiver` will not destroy a texture or material it did not create. If you pass something
  else, it is left untouched and the mismatch is written to the log.
- A full shutdown (PIE stop, `Close All Streams`, editor exit) releases every open receiver's
  texture and detaches every material binding automatically.

#### Colour space

`Color Space` (an advanced pin on the Spout Receiver node) decides how the incoming bytes are read:

| Setting | What it does |
|---|---|
| **Auto** (default) | 8-bit frames decode as sRGB; 10-bit and 16-bit float frames stay linear. This is what other Spout applications publish, so it is right for almost every sender. |
| **Linear** | No decoding. Use it when the sender publishes linear or data values in an 8-bit format — including this plugin's own sender when the source render target was created with `Force Linear Gamma`. |
| **sRGB** | Force decoding. Only valid for 8-bit frames; asking for it on a 10-bit or HDR float frame fails the call and logs, rather than being quietly ignored. |

If a received stream looks washed out or too dark, this is the setting to change. Switching it
rebuilds the received texture, so the change takes effect on the next frame and nothing is
reinterpreted in place.

A frame whose format this plugin cannot share — anything outside BGRA8, RGBA8, RGB10A2 and RGBA16F,
including compressed and planar formats — is refused with a log line naming it. It is never
reinterpreted as something else.

#### Render Target Notes

If you choose to output the received Spout stream into a `UTextureRenderTarget2D`, the recommended and most reliable format is:

- **RTF RGBA8 sRGB**

Other formats may either not work correctly or result in poor visual quality.  
Support for additional formats is still under investigation, and users are encouraged to test and choose what best fits their use case.

#### Material Output Notes

If you want to use a **Material** as the output for the Spout Receiver:

- You can provide **any material** as input to the Receiver.
- Inside that material, add a **Texture Parameter**.
- The name of this Texture Parameter **must exactly match** the parameter name specified in the Spout Receiver node.
- The Receiver will automatically bind the incoming Spout texture to that parameter at runtime.

If you do not wish to use a material, this step can be safely skipped and the raw texture or render target output can be used instead.

**Notes**
- The receiver automatically reconnects when the sender becomes available.
- The assigned material must be compatible with Spout input.
- Avoid heavy logic on Tick; use a timer if possible.
- The receiver only copies when the sender has actually published a new frame, so calling it every
  tick against a slower sender costs nothing on the frames in between. It also takes Spout's
  cross-process texture lock around the copy, so a frame is never read while the sender is midway
  through writing it. If the lock is busy the frame is not lost: it stays pending and is picked up
  on a following call.
- Frame counting can be turned off machine-wide in SpoutSettings, and some senders do not publish a
  count. When there is no count to compare, every call is treated as a new frame and copies, which
  is the only safe answer.




## Content
This plugin ships with sample content under `Content/` when `CanContainContent` is enabled in `SpoutPlugin.uplugin`.

## Notes and Limitations

- This plugin is **Windows-only** and requires Unreal Engine to run with the **D3D12 RHI** enabled.  
  Internally, a D3D11-on-DX12 device is created to support Spout interoperability.

- At the moment, only a **limited set of texture formats** is supported.  
  Support for additional formats is still a work in progress.

- For now, the **recommended and most reliable format** for Render Targets are (tested):  
  **RTF RGBA8 sRGB**
  **RTF RGB10A2**

- The Spout Receiver uses a **GPU-direct path by default** (`r.Spout.GPUReceiver=1`): the shared texture is copied straight into the Unreal texture on the GPU. Setting the CVar to `0` falls back to the legacy **CPU readback** path, which is expensive at large resolutions or high update rates.

- The Sender and Receiver must be called **continuously** (Tick or timer) to keep the Spout stream alive and updated.

- Only **one sender per Spout Name** can be active at a time. Multiple senders using the same name will conflict.

- This plugin is **not guaranteed to be production-ready** for shipping products without thorough testing.  
  Stability and performance may vary depending on hardware, drivers, texture sizes, and usage patterns.

- Error handling and recovery are intentionally minimal and designed to favor debuggability over silent failure.

- This plugin is intended primarily for **real-time workflows, prototyping, tools, and experimentation**, rather than long-running unattended deployments.

## Troubleshooting

### No texture received / Receiver outputs invalid material
- Ensure the **Spout Name** exactly matches between Sender and Receiver.
- Verify that the Sender is running and being called continuously (Tick or timer).
- Check that only **one sender** is using the same Spout Name.
- Make sure the Render Target format is supported.  
  The recommended format is **RTF RGBA8 sRGB**.

### Sender does not appear in external applications
- Confirm that the project is running with the **D3D12 RHI** enabled.
- Ensure the Spout runtime is available and that `Spout.dll` is correctly staged in `Binaries/Win64`.
- If sending from the viewport, make sure the game window is actively rendering and not minimized.

### Performance issues or frame drops
- Large textures and high update rates can be expensive, especially on the Receiver side due to CPU readback.
- Reduce resolution or update frequency if possible.
- Prefer timers over Tick when full frame-rate updates are not required.

### Crashes or instability
- Verify GPU drivers are up to date.
- Test with different texture resolutions and formats.
- Avoid changing Render Target resources at runtime while the Sender or Receiver is active.
- If issues persist, reproduce the problem in a minimal project before reporting it.

### Plugin does not compile or load
- Ensure the plugin is placed inside the project `Plugins` directory, not the engine directory.
- Confirm you are using a supported Unreal Engine 5 version.
- Regenerate project files and rebuild after enabling the plugin.

### `Could not find definition for module 'SpoutPlugin'` (compile or packaging fails instantly)

This is a **build-time** error (it aborts in well under a second, and packaging reports `dotnet.exe ExitCode=8`), different from the load-time error below. It means your copy of the plugin has **no `Source/` folder**, so Unreal Build Tool can't find `SpoutPlugin.Build.cs`. A source-less, binary-only copy only loads in the editor when the prebuilt binary matches your exact engine version; it cannot be compiled or packaged against.

1. Use a copy that **includes `Source/`**. Every current release ships it (0.0.8 and later). If you have an older, source-less download, re-download or clone this repository into `YourProject/Plugins/SpoutPlugin/`.
2. Confirm `Plugins/SpoutPlugin/Source/SpoutPlugin/SpoutPlugin.Build.cs` exists.
3. Delete `Plugins/SpoutPlugin/Binaries` and `Plugins/SpoutPlugin/Intermediate`, regenerate project files, and rebuild.

### `Plugin 'SpoutPlugin' failed to load because module 'SpoutPlugin' could not be loaded`

This almost always means the compiled module binary doesn't match your engine, or `Spout.dll` isn't beside the module binary.

1. **Verify the build matches your engine.** A precompiled download must be built for your **exact** Unreal Engine version. If it isn't, rebuild from source.
2. **Verify `Spout.dll` is present — in the right place for your layout.** This is where most reports go wrong, because the location differs depending on how you are using the plugin. In the editor it belongs at `Plugins/SpoutPlugin/Binaries/Win64/Spout.dll`, beside `UnrealEditor-SpoutPlugin.dll`. In a **packaged game** it belongs at `<YourProject>/Binaries/Win64/Spout.dll`, beside the game executable, and the plugin's own `Binaries` folder is not staged at all. See [Where `Spout.dll` ends up](#where-spoutdll-ends-up) for all five layouts.
3. **Read the log.** Open `Project/Saved/Logs/ProjectName.log` and search for `SpoutPlugin`, `Missing import`, `GetLastError`, `Spout.dll`. The plugin logs the path it resolved — `Loaded Spout.dll from '<path>'` — or an explicit error if it could not find it. If that line names a path you did not expect, you have found the problem.
4. **If rebuilding from source,** delete these folders, then regenerate project files and rebuild:
   - `Project/Binaries`
   - `Project/Intermediate`
   - `Plugins/SpoutPlugin/Binaries`
   - `Plugins/SpoutPlugin/Intermediate`


## Building a release, and validating one

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

:: The complete matrix: all six engines, Development and Shipping packaged hosts.
build.bat -TrackD -FullMatrix

:: Performance baselines (1080p60, 4K60, multi-stream, CPU fallback, idle, resize churn).
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

Evidence — build logs, staging manifests, smoke-test markers, performance JSON, hashes and
Unreal Insights traces — is retained under `Artifacts/TrackD/<run-id>/`, which is git-ignored and
never included in a release archive.

---

## Third-Party
- Spout: http://spout.zeal.co/ (BSD 2-Clause, © Lynn Jarvis)

The Spout SDK headers and the redistributed `Spout.dll` / `Spout.lib` are covered by their
own license. Full terms, bundled file paths, and binary hashes are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), which ships inside every release archive.

## Attribution and Redistribution

This project is released under the MIT License.

While commercial use is allowed, redistributing this plugin as-is or with minimal changes and selling it as a standalone product goes against the spirit of this project.

If you build something cool with this plugin and feel like sharing it, let me know. I would be happy to consider adding it to a future **Showcase** section.

### Note on AI-assisted development

Parts of this plugin were refined with the assistance of modern AI tools.  
AI was used primarily as a development aid to help debug certain edge cases, accelerate some refactoring steps, and improve code clarity and documentation.

The overall architecture, implementation, and the majority of the code were designed and written by the author, building on the original repository and hands-on development work. AI assistance was used in the same spirit as other developer tools: to help review, polish, and streamline parts of the process.


## License

This project is licensed under the MIT License.  
See the [LICENSE](LICENSE) file for the full terms, which preserve the original
copyright of the upstream [zuyi53/Spout-UE5](https://github.com/zuyi53/Spout-UE5) project
alongside the copyright for the modifications in this fork.

Bundled third-party components are listed with their own license terms in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Both files are included in every release
archive.
