# SpoutPlugin User Manual - Source

This is the source content for the SpoutPlugin User Manual PDF. It is user-facing and focused on
using the plugin in Unreal Engine, mainly through Blueprints. Internal C++ detail appears only
where it helps explain a requirement, limitation, or troubleshooting step.

- Plugin: SpoutPlugin (Spout Plugin)
- Version: 0.0.9 (must match `SpoutPlugin.uplugin`; `Scripts/Validate-Documentation.ps1` fails the
  release when it does not)
- Engine target: Unreal Engine 5.3 - 5.8 (Win64)
- Module: SpoutPlugin (Runtime)
- Platform: Windows 64-bit, DirectX 12 RHI
- Documentation date: 2026-06-05

---

## 1. What SpoutPlugin Is

Spout is a Windows technology for sharing GPU textures between real-time applications with no
disk or network transfer. It lets one application publish a texture (a "sender") and another
read it live (a "receiver"). It is widely used to move video between tools such as TouchDesigner,
Resolume, Notch, and OBS.

SpoutPlugin brings Spout into Unreal Engine 5. It lets you send Unreal's rendered output, or any
render target, to other Spout applications, and receive Spout streams back into Unreal as
textures and materials. Everything stays on the GPU.

The technical challenge it solves: Spout shares DirectX 11 textures, but modern UE5 runs on the
DirectX 12 RHI. The plugin stands up a D3D11-on-D3D12 interop device and bridges UE's D3D12
textures to Spout's D3D11 shared textures.

Typical use cases:

- Send an Unreal scene or camera into TouchDesigner for compositing, projection mapping, or LED
  output.
- Receive a live feed from another tool into Unreal and map it onto a mesh or use it in a
  material.
- Build a real-time pipeline between Unreal and the rest of a media-server or VJ setup.

What it does: GPU texture send and receive over Spout, from the game viewport or a render target,
into a texture, a dynamic material instance, or an optional render target.

What it does not do: it is not a video codec, not a network streaming solution, and not a
color-management system. It does not apply gamma or color correction on send. It is Windows-only
and requires DirectX 12.

This manual is written for Unreal users and technical artists working mainly in Blueprints. You
do not need to write C++ to use the plugin.

---

## 2. Requirements

- Windows 64-bit.
- Unreal Engine 5 (5.3 through 5.8).
- DirectX 12 RHI enabled in the project. The plugin creates a D3D11-on-D3D12 interop device and
  will not initialize on any other RHI. It logs `SpoutPlugin requires DX12 RHI.` and stops.
- `Spout.dll`, bundled with the plugin under `ThirdParty/Spout/lib/amd64/` and staged to the
  plugin's `Binaries/Win64/` at build time.
- For source builds: Visual Studio 2022 with the C++ toolchain and a matching Windows SDK.
- The plugin is project-local. Install it into your project's `Plugins/` folder, not the engine
  plugins folder.

Both applications sharing a stream must run on the same GPU. Spout's shared-texture model does
not span GPUs.

---

## 3. Installation and Project Setup

### Precompiled binary

1. Download the ZIP matching your exact engine version (prebuilt binaries are on Patreon).
2. Extract the `SpoutPlugin` folder into `MyProject/Plugins/`.
3. Confirm `Spout.dll` is at `Plugins/SpoutPlugin/Binaries/Win64/Spout.dll`.
4. Set the project RHI to DirectX 12.
5. Open the project. If Unreal offers to rebuild because of an engine-version mismatch, the
   binary does not match your engine - use the correct ZIP or build from source.

### Source build

1. Copy `SpoutPlugin` into `MyProject/Plugins/`.
2. Set the project RHI to DirectX 12.
3. Generate Visual Studio project files from the `.uproject`.
4. Build from Visual Studio or let the editor compile on open.

`Spout.dll` is staged automatically to the plugin's `Binaries/Win64` via Unreal's runtime
dependency system. Do not copy it anywhere by hand.

### Set DirectX 12

Edit > Project Settings > Platforms > Windows > Default RHI > DirectX 12, then restart the editor.

### Enable the plugin

The descriptor enables the plugin by default. To confirm: Edit > Plugins, search "Spout Plugin",
make sure it is enabled, and restart if you change it.

### Verify it loaded

In `Project/Saved/Logs/ProjectName.log`, look for `Spout Plugin Loaded` and, after first use,
`SpoutDX11on12 Initialized.`. If `Spout.dll` is missing, the plugin logs an explicit startup
error naming the paths it checked.

(Figure placeholder SP-001 - Plugin enabled in Edit > Plugins.)
(Figure placeholder SP-002 - Project RHI set to DirectX 12.)

---

## 4. Quick Start: Sending the Game Viewport

1. Create a Blueprint Actor or open the Level Blueprint.
2. On Event Tick or a timer, add a Spout Sender node.
3. Spout Name: a unique stream name, for example `UnrealViewport`.
4. Send Texture From: `GameViewport`.
5. Leave Texture Render Target 2D empty.
6. Play. In your external Spout receiver, select `UnrealViewport`.

The viewport is captured when the window is ready to present, so the game window must be
rendering and not minimized.

(Figure placeholder SP-005 - Spout Sender from GameViewport.)
(Figure placeholder SP-011 - external application receiving the stream.)

---

## 5. Quick Start: Sending a Render Target

1. Create a `TextureRenderTarget2D` (RGBA8 sRGB recommended), or call Create Texture Render
   Target 2D at runtime.
2. Add a `SceneCapture2D` to the level.
3. Assign the render target to the SceneCapture2D Texture Target.
4. Set Capture Source to Final Color (HDR) in Linear Working Color Space. Do not use SceneColor
   (HDR) - it produces black output.
5. On Tick or a timer, call Spout Sender with Send Texture From = `TextureRenderTarget2D` and the
   render target assigned.
6. View the stream externally.

Most common black-output cause: the Capture Source in step 4.

(Figure placeholder SP-006 - Spout Sender from a render target.)
(Figure placeholder SP-007 - SceneCapture2D Capture Source setting.)

---

## 6. Quick Start: Receiving a Spout Stream

1. On Tick or a timer, call Spout Receiver.
2. Spout Name: exactly match the sender.
3. Optionally set Input Material and Texture Parameter Name.
4. Use the outputs: Out Texture (transient `UTexture2D`), Out Mat (dynamic material instance), and
   optionally Optional Output Render Target (the frame is copied into it).
5. Validate Out Mat and Out Texture before applying them.
6. Apply Out Mat to a mesh, or sample Out Texture directly.

(Figure placeholder SP-009 - Spout Receiver node and outputs.)

---

## 7. Sender Workflow

Two sources, chosen by Send Texture From.

GameViewport - sends the active window backbuffer, captured at present time. The window must be
rendering and not minimized.

TextureRenderTarget2D - sends a render target you provide. Commonly driven by a SceneCapture2D.

Stream name rules: the name must match the receiver exactly; only one sender owns a name at a
time; the registry is keyed by name and type so a sender and a receiver can share a name without
conflict.

Lifetime: call the sender continuously; the stream is not kept alive between calls. The shared
texture is created and resized automatically. Call Close Sender to stop a stream; it affects only
the sender entry.

Under the hood: the UE D3D12 source texture is wrapped as a D3D11 texture through the interop
device and copied into a Spout shared DX11 texture. No CPU readback on send.

Performance: cost scales with resolution; use a timer when full frame rate is not needed; the
shared texture and wrap are cached while size and format are stable.

Constraint: no sender-side colour conversion. The colour space is carried by the DXGI format the
sender advertises, which follows the render target's Force Linear Gamma setting.

---

## 8. Receiver Workflow

Call Spout Receiver continuously with a matching name.

Outputs: Out Texture (transient texture, resized to the stream), Out Mat (dynamic material
instance created from Input Material on first call, with Out Texture bound to the parameter), and
Optional Output Render Target (the frame is also copied into it if provided).

Finding senders: use Spout Info to list active names, widths, and heights. Names must match
exactly. The receiver reconnects automatically when the sender appears or changes size.

Receive paths: selected by `r.Spout.GPUReceiver`. `1` (default) is GPU-direct - the shared
texture is copied straight into the UE destination via D3D11-on-12, with no staging texture and
no CPU copy. `0` is the legacy CPU readback path, kept for debugging and more expensive at high
resolution. Both look identical. Set the CVar in the console, in `DefaultEngine.ini` under
`[SystemSettings]`, with `-ExecCmds`, or from a Blueprint Execute Console Command. It is read
once per receive call, so changes apply on the next call.

Note: the `OutputRenderTarget` field on `FSenderStruct` is unused. The functional render-target
output is the Optional Output Render Target pin on the Spout Receiver node.

Performance: prefer the default GPU path; use a timer when full frame rate is not needed; Out
Texture is reused while the stream size is stable.

---

## 9. SceneCapture2D and Render Target Notes

Recommended Capture Source: Final Color (HDR) in Linear Working Color Space.

Do not use: SceneColor (HDR) in RGB - it produces a black image on the receiver. This is a known
limitation, not something tunable through other Capture Source values.

Recommended render target format: RGBA8 sRGB (start here). RGB10A2 is also tested and useful to
reduce banding.

The plugin shares the texture as-is and does not color-correct on send, so color correctness
depends on the Capture Source and render target format. The tested combination is Final Color
(HDR) in Linear Working Color Space with an RGBA8 sRGB render target.

Black-output checklist: confirm the Capture Source; confirm the render target is assigned to the
SceneCapture2D Texture Target; confirm the capture is active; confirm you are sending the same
render target asset; confirm a tested format.

---

## 10. Materials

To drive a material from a received stream:

1. Create a material with a Texture Parameter (example: `M_SpoutMaterial`).
2. Pass it as Input Material to the Spout Receiver node.
3. Set Texture Parameter Name to match the parameter in the material. If left empty, the plugin
   uses `SpoutTexture`, so naming your parameter `SpoutTexture` matches the default.
4. On the first call with a valid Input Material, the plugin creates a dynamic material instance,
   binds Out Texture to the parameter, and returns it as Out Mat.
5. Apply Out Mat to a mesh.

If you do not need a material, leave Input Material empty and use Out Texture directly, or use the
Optional Output Render Target.

(Figure placeholder SP-010 - material Texture Parameter setup.)

---

## 11. Blueprint API Reference

All nodes are static functions on `USpoutBPFunctionLibrary`, category `Spout`.

### Spout Sender

`bool SpoutSender(FName SpoutName, ESpoutSendTextureFrom SendTextureFrom, UTextureRenderTarget2D* TextureRenderTarget2D)`

Publishes a texture to Spout; the GPU copy runs on the render thread. Returns true when accepted
or queued. Provide the render target when using the TextureRenderTarget2D source. Call
continuously. Common mistakes: empty render target with the render-target source; calling once;
name mismatch.

### Close Sender

`void CloseSender(FName SpoutName)`

Removes the named sender and releases its handle. Scoped to senders only.

### Spout Receiver

`bool SpoutReceiver(const FName SpoutName, UMaterialInterface* InputMaterial, FName TextureParameterName, UMaterialInstanceDynamic*& OutMat, UTexture2D*& OutTexture, UTextureRenderTarget2D* OptionalOutputRenderTarget = nullptr)`

Receives a stream. `OptionalOutputRenderTarget` is an advanced-display pin. Returns true when the
sender exists and a receive was enqueued. Outputs Out Mat and Out Texture. Texture Parameter Name
defaults to `SpoutTexture` when empty. Validate outputs before use.

### Spout Info

`bool SpoutInfo(TArray<FSenderStruct>& Senders)`

Fills an array with one entry per active sender. Only Name, Width, Height, and bIsAlive are
populated. Returns true if the registry was queried.

### Get Sender Info

`bool GetSenderInfo(FName SpoutName, FSenderStruct& OutSenderStruct)`

Looks up one sender by name; fills Name, Width, Height, bIsAlive. Returns true if it exists.

### Set Max Senders

`int32 SetMaxSenders(int32 Max)`

Sets the maximum sender count in Spout's registry and returns the limit actually in effect.
Returns 0 when nothing changed, either because the requested value was outside the accepted range
(1-1024) or because Spout is unavailable. It does not echo the requested value back.

### Close Receiver

`void CloseReceiver(FName SpoutName, UTexture2D*& ReceivedTexture, UMaterialInstanceDynamic*& ReceivedMaterial)`

Closes the receiver session for that name. There is one session per sender name, so this closes it
for every graph receiving that name, and it detaches the Spout texture parameter from every
material the plugin bound it into. The **name** decides what is released, not the pointers: pass
back the Out Texture and Out Mat this plugin gave you and both are cleared; pass anything else and
it is left untouched with a warning. This node will not destroy a texture it did not create.

### Get Max Senders

`void GetMaxSenders(int32& Max)`

Reads the current maximum sender count.

### Create Texture Render Target 2D

`UTextureRenderTarget2D* CreateTextureRenderTarget2D(int32 Width = 1024, int32 Height = 768, EPixelFormat Format = PF_B8G8R8A8, bool bForceLinearGamma = true)`

Creates a render target at runtime, sets wrap addressing, and initializes its resource. The result
is **not** rooted: keep a reference to it exactly as you would with Create Render Target 2D, or it
will be collected. Returns null for invalid dimensions or for a pixel format the plugin cannot
share (supported: B8G8R8A8, R8G8B8A8, A2B10G10R10, FloatRGBA).

### Enums

ESpoutType: Sender, Receiver. ESpoutSendTextureFrom: GameViewport, TextureRenderTarget2D.

### FSenderStruct

Fields: Name, bIsAlive, SpoutType, Width, Height, TextureColor, MaterialInstanceColor,
UserBaseMaterial, UserTextureParameter (default `SpoutTexture`), OutputRenderTarget. The Info
nodes populate only Name, Width, Height, and bIsAlive. Treat the rest as your own Blueprint
fields, not plugin-driven. OutputRenderTarget is unused by the plugin.

---

## 12. Example Blueprint Setups

The plugin ships example content under its `Content/` folder. Use these to test quickly:

| Asset | Purpose |
|---|---|
| `BP_SpoutSenderExample_` | Example sender setup. |
| `BP_SpoutReceiverExample` | Example receiver setup. |
| `M_SpoutMaterial` | Material with a texture parameter for receiver output. |
| `RT_SpoutSenderTest_` | Example render target for sending. |
| `RT_SpoutReceiverTest` | Example render target for receiving. |

These are the actual asset names in the plugin Content folder. Open them to see working Sender
and Receiver graphs rather than building from scratch.

(Figure placeholder SP-016 - example content folder.)

---

## 13. Packaging and Distribution

`Spout.dll` ends up in a different place depending on how the plugin is being used. Conflating
those places is the most common cause of "module could not be loaded" reports, so all five are
listed separately:

| Layout | Where `Spout.dll` is |
|---|---|
| Source plugin in the repository | `ThirdParty/Spout/lib/amd64/Spout.dll` |
| Raw `RunUAT BuildPlugin -Package` output | `ThirdParty/Spout/lib/amd64/Spout.dll` only. RuntimeDependencies does **not** copy it into `Binaries/Win64` here |
| Release archive | `Binaries/Win64/Spout.dll`, put there explicitly by `build.ps1`, plus the `ThirdParty` copy |
| Plugin installed in a project, editor build | `Plugins/SpoutPlugin/Binaries/Win64/Spout.dll`, staged by RuntimeDependencies |
| Packaged game | `<Project>/Binaries/Win64/Spout.dll` beside the game executable. The plugin module is linked into the executable, so `$(BinaryOutputDir)` resolves to the project's binaries and the plugin's own `Binaries` folder is not staged at all |

The DLL is delay-loaded, and the module loads it by full path at startup so the delay-load thunks
bind by name rather than raising `0xC06D007E`.

A release archive contains `Binaries` (with `Spout.dll`), `Content`, `Resources`, `Source`,
`ThirdParty`, `Intermediate/Build`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, `README.md` and
`SpoutPlugin.uplugin`.

Releases are built per engine version with `RunUAT BuildPlugin ... -TargetPlatforms=Win64
-Rocket`. Debug symbols are removed, but `Intermediate/Build` is deliberately **kept**: it carries
the precompiled UnrealGame Development and Shipping objects and the `.precompiled` manifests that
let Blueprint-only users package a project without Visual Studio. Every generated archive is then
reopened and content-validated before it is published. The repository `build.ps1` does this for
UE 5.3 through 5.8.

A binary built for one engine version will not load in another. Ship one build per version.

Clean rebuild after an engine upgrade: delete `Project/Binaries`, `Project/Intermediate`,
`Plugins/SpoutPlugin/Binaries`, `Plugins/SpoutPlugin/Intermediate`, regenerate project files, and
rebuild.

(Figure placeholder SP-004 - Spout.dll in Binaries/Win64.)

---

## 14. Performance Notes

- Cost scales with texture resolution on both send and receive. Start at a lower resolution and
  raise it only as needed.
- Update rate matters. Use a timer instead of Tick when you do not need full frame-rate output.
- Leave `r.Spout.GPUReceiver` at its default of `1`. The legacy CPU readback path (`0`) adds a
  full-frame copy, map, and upload each frame.
- The send path is a GPU copy through the interop device; there is no CPU readback on send.
- Keep per-frame Blueprint logic around the send and receive calls minimal.
- Watch large textures specifically. A 4K stream at full frame rate is far more expensive than a
  1080p stream.

---

## 15. Limitations

- Windows 64-bit only.
- DirectX 12 RHI required; the plugin refuses to initialize otherwise.
- Both ends must be on the same GPU.
- Tested render target formats are limited to RGBA8 sRGB and RGB10A2.
- No colour conversion on send; the advertised colour space follows the render target's gamma setting.
- SceneCapture2D must use Final Color (HDR) in Linear Working Color Space; SceneColor (HDR) is
  black.
- One sender per name.
- Senders and receivers must be called continuously.
- Spout Info and Get Sender Info populate only Name, Width, Height, bIsAlive.
- Marked beta; not guaranteed production-ready without testing. Error handling favors visible,
  logged failures over silent recovery.

---

## 16. Troubleshooting

Plugin does not compile: confirm it is in the project Plugins folder, you are on UE 5.3-5.8 with
VS 2022, regenerate project files, and do a clean rebuild after engine upgrades.

Plugin fails to load ("module could not be loaded"): the binary must match your exact engine, and
`Spout.dll` must be in `Binaries/Win64`. Read the log; the plugin logs a startup error if the DLL
is missing.

Spout.dll missing: for precompiled releases, copy it from `ThirdParty/Spout/lib/amd64/` to
`Binaries/Win64/` or rebuild from source.

Sender does not appear externally: confirm DirectX 12; confirm continuous calls; one sender per
name; for GameViewport, the window must be rendering; look for `SpoutDX11on12 Initialized.`.

Receiver returns invalid texture or material: exact name match; check the return value; validate
outputs; texture parameter name must match the material (default `SpoutTexture`).

Black texture from SceneCapture2D: set Capture Source to Final Color (HDR) in Linear Working
Color Space.

Wrong colors or gamma: adjust render target format and Capture Source; try RGBA8 sRGB first, then
RGB10A2.

Stream name mismatch: names are case-sensitive; use Spout Info to read exact names.

Multiple senders conflict: unique, stable names per stream.

Performance drops: keep the GPU receiver default; reduce resolution or update rate; prefer timers.

Crashes or instability: update GPU drivers; avoid swapping render target resources at runtime;
test different resolutions; reproduce in a minimal project.

External app cannot see the stream: confirm Spout/format support and that both apps are on the
same GPU.

After an engine upgrade: clean rebuild (delete the four Binaries/Intermediate folders),
regenerate, rebuild.

---

## 17. Best Practices

- Use stable, unique Spout names and keep sender and receiver names identical.
- Start at a low resolution and raise it only when needed.
- Use a timer when full frame rate is not required.
- Always validate Out Mat and Out Texture before applying them.
- Keep the render target format simple: RGBA8 sRGB first.
- For SceneCapture2D sends, always use Final Color (HDR) in Linear Working Color Space.
- Test in a minimal project before integrating into production.
- Record the engine version and the plugin binary version together, since binaries are
  version-specific.

---

## 18. Appendix

### File structure

```
SpoutPlugin.uplugin
Source/SpoutPlugin/
  SpoutPlugin.Build.cs
  Public/   SpoutBPFunctionLibrary.h, SpoutModule.h
  Private/  SpoutBPFunctionLibrary.cpp, SpoutModule.cpp,
            SpoutSender.*, SpoutReceiver.*, SpoutD3DContext.*,
            SpoutD3DUtils.*, SpoutSenderRegistry.*, SpoutTextureUtils.*
ThirdParty/Spout/   include/, lib/amd64/ (Spout.dll, Spout.lib)
Content/            example Blueprints, material, render targets
Resources/          Icon128.png
build.ps1, build.bat
```

### Module and dependency summary

Single Runtime module `SpoutPlugin`, Win64 only. Public dependencies: Core, CoreUObject, Engine,
RHI, RenderCore. Private: ApplicationCore, Slate, SlateCore, Projects, D3D12RHI, D3D11RHI, plus
the engine DX12 and DX11 third-party libraries. `Spout.dll` is delay-loaded and staged as a
runtime dependency.

### Third-party attribution

- Spout: `http://spout.zeal.co/`.
- Based on the original repository by zuyi53: `https://github.com/zuyi53/Spout-UE5`.

### License

MIT License. Commercial use is allowed, but reselling the plugin as-is as a standalone product
goes against the spirit of the project.

### Glossary

- Spout: Windows GPU texture-sharing technology between real-time applications.
- Sender: an application publishing a shared texture.
- Receiver: an application reading a shared texture.
- Render Target: a `UTextureRenderTarget2D`, a texture you can render into and read back.
- SceneCapture2D: an Unreal actor that renders a view into a render target.
- RHI: Rendering Hardware Interface, Unreal's graphics API abstraction.
- D3D12: DirectX 12, the graphics API Unreal uses; required by this plugin.
- D3D11-on-D3D12: an interop layer that exposes D3D12 resources to D3D11 code (here, Spout).
- Shared texture: a GPU texture exposed via a shared handle so another process can read it.
