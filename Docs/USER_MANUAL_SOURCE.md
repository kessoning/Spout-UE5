# SpoutPlugin User Manual - Source

This is the source content for the SpoutPlugin User Manual PDF. It is user-facing and focused on
using the plugin in Unreal Engine, mainly through Blueprints. Internal C++ detail appears only
where it helps explain a requirement, limitation, or troubleshooting step.

- Plugin: SpoutPlugin (Spout Plugin)
- Version: 1.0.0 (must match `SpoutPlugin.uplugin`; `Scripts/Validate-Documentation.ps1` fails the
  release when it does not)
- Engine target: Unreal Engine 5.3 - 5.8 (Win64)
- Module: SpoutPlugin (Runtime)
- Platform: Windows 64-bit, DirectX 12 RHI
- Documentation date: 2026-09-12

---

## 1. What SpoutPlugin Is

Spout is a Windows technology for sharing GPU textures between real-time applications with no
disk or network transfer. It lets one application publish a texture (a "sender") and another
read it live (a "receiver"). It is widely used to move video between tools such as TouchDesigner,
Resolume, Notch, and OBS.

SpoutPlugin brings Spout into Unreal Engine 5. It lets you send Unreal's rendered output, or any
render target, to other Spout applications, and receive Spout streams back into Unreal as
textures and materials. The default transfer path stays on the GPU; CPU receiver fallback uses system memory for readback and upload.

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
- For source builds and for packaging a game (the supported workflow): Visual Studio 2022 with
  the C++ workload and toolchain and a Windows SDK supported by your engine version. Opening a
  project with the prebuilt plugin in a matching editor does not need a compiler.
- Install into your project's `Plugins/` folder, the qualified consumer layout. Qualify engine-wide installations separately.
- Blueprint-only project with the plugin in its `Plugins/` folder: in the UE 5.8 test the packaged
  game did not contain the plugin and reported that module `SpoutPlugin` could not be found. The
  verified workaround is to add a C++ class to the project before packaging.

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

1. Use a C++ project, the supported source-build workflow (add a C++ class to a Blueprint-only
   project first), and copy `SpoutPlugin` into `MyProject/Plugins/`.
2. Set the project RHI to DirectX 12.
3. Generate Visual Studio project files from the `.uproject`.
4. Build from Visual Studio or let the editor compile on open.

`Spout.dll` is staged automatically to the plugin's `Binaries/Win64` via Unreal's runtime
dependency system. Do not copy it anywhere by hand.

### Set DirectX 12

Edit > Project Settings > Platforms > Windows > Default RHI > DirectX 12, then restart the editor.

### Enable the plugin

A plugin in the project's `Plugins/` folder is enabled by default. To confirm: Edit > Plugins, search "Spout Plugin",
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
4. Start with Final Color (HDR) in Linear Working Color Space. Qualify capture source, exposure
   and format together.
5. On Tick or a timer, call Spout Sender with Send Texture From = `TextureRenderTarget2D` and the
   render target assigned.
6. View the stream externally.

Most common black-output cause: the Capture Source in step 4.

(Figure placeholder SP-006 - Spout Sender from a render target.)
(Figure placeholder SP-007 - SceneCapture2D Capture Source setting.)

---

## 6. Quick Start: Receiving a Spout Stream

1. Create a dynamic material instance once, store it in a variable, and apply it to the mesh.
2. On Tick or a timer, call Spout Receiver With Material with that stored MID as Consumer Material.
3. Set Spout Name to match the sender and Texture Parameter Name to match the material parameter.
4. Optionally supply Optional Output Render Target for a second copy of the received image.
5. Check the return bool and validate Out Texture before use. True means accepted and queued;
   it does not prove the pixels have finished copying.
6. Keep using the stored MID, or sample the transient Out Texture directly.

(Figure placeholder SP-009 - Spout Receiver With Material, stored MID input and outputs.)

---

## 7. Sender Workflow

Two sources, chosen by Send Texture From.

GameViewport - sends the active window backbuffer, captured at present time. The window must be
rendering and not minimized.

TextureRenderTarget2D - sends a render target you provide. Commonly driven by a SceneCapture2D.

Stream name rules: the name must match the receiver exactly; only one sender owns a name at a
time; the registry is keyed by name and type so a sender and a receiver can share a name without
conflict.

Lifetime: call the sender continuously; registration and the last image persist between calls until close. The shared
texture is created and resized automatically. Call Close Sender to stop a stream; it affects only
the sender entry.

Under the hood: the UE D3D12 source texture is wrapped as a D3D11 texture through the interop
device and copied into a Spout shared DX11 texture. No CPU readback on send.

Performance: cost scales with resolution; use a timer when full frame rate is not needed; the
shared texture and wrap are cached while size and format are stable.

Constraint: no sender-side colour conversion. The pixel bytes and alpha are copied unchanged, and
every 8-bit source is published as plain `_UNORM`, never `_UNORM_SRGB`, because Spout 2.007's
OpenGL receivers (such as the Spout 2.007 Demo Receiver) refuse `B8G8R8A8_UNORM_SRGB`.
Keeping the bytes unchanged does not keep the sRGB format tag: an sRGB render target (Force Linear
Gamma off) and a Force Linear Gamma render target both publish `_UNORM`, so a receiving
application cannot tell the source's colour intent from the format. `_UNORM` does not mean the
bytes are linear: by the Spout convention 8-bit `_UNORM` bytes are gamma-encoded (sRGB), which is
what an sRGB render target produces and what receivers on Auto expect. A Force Linear Gamma target
stores linear values, so set its receiver's Color Space to Linear.

Access: each publish takes Spout's cross-process access lock. If a receiver holds the texture,
the publish waits at most r.Spout.AccessWaitMs (default 2 ms) for it. If it is still busy, that
frame is skipped, the next one is published once the texture is free, and a throttled
"shared-texture access was unavailable" warning is logged.

---

## 8. Receiver Workflow

Call Spout Receiver continuously with a matching name.

Outputs: Out Texture (transient texture, resized to the stream), Out Mat (dynamic material
instance created from Input Material on first call, with Out Texture bound to the parameter), and
Optional Output Render Target (the frame is also copied into it if provided; its pixel format
must match the incoming frame and it must be single-sampled, otherwise it is left untouched with
a logged warning while Out Texture keeps updating).

Color Space (advanced pin): how the received bytes are interpreted. Auto follows the format
convention and is right for almost every sender; Linear turns decoding off for senders that
publish linear 8-bit data; sRGB forces decoding and is valid for 8-bit frames only. An 8-bit
`_UNORM` format does not say which case applies (this plugin's sender publishes `_UNORM` for sRGB
and Force Linear Gamma render targets alike), so pick Linear yourself for a linear sender. See the
Blueprint API Reference for the full contract.

Access: each copy takes Spout's cross-process access lock. If a peer (the sender or another
receiver) holds the texture, the copy waits at most r.Spout.AccessWaitMs (default 2 ms) for it. If
it is still busy, the receiver tries again on a later call and a throttled "shared-texture access
was unavailable" warning is logged; that call copies the sender's newest frame, so a frame the
sender replaced in the meantime is lost, not queued.

Sampling: the receiver copies at most once per Unreal frame and gets the sender's newest frame;
Spout does not queue frames. When the sender and Unreal run at the same frame rate, their timing
drifts in and out of alignment, and near alignment single frames are skipped and the previous one
is shown for an extra frame, without any access contention. A higher Unreal frame rate reduces
this but does not guarantee every frame. See Limitations.

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

Other capture modes, including SceneColor HDR, require qualification of exposure, alpha, target
format and receiver interpretation. The plugin does not tone-map pixels.

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

For repeated Blueprint use, prefer **Spout Receiver With Material**. Create a Dynamic Material
Instance once, store it in a variable, and pass it to the Consumer Material input each tick.
This node creates no material and returns the shared Out Texture. Passing null supports texture-only
use. The original signature below remains available; its Out Mat pin is an output in Blueprint.

**Release Receiver Material Binding** removes just one MID/parameter binding while keeping the
shared receiver open. It restores the previous texture or engine default only if that parameter
still points to the receiver's image. A later manual change or another sender's binding survives.

`bool SpoutReceiver(const FName SpoutName, UMaterialInterface* InputMaterial, FName TextureParameterName, UMaterialInstanceDynamic*& OutMat, UTexture2D*& OutTexture, UTextureRenderTarget2D* OptionalOutputRenderTarget = nullptr, ESpoutColorSpace ColorSpace = ESpoutColorSpace::Auto)`

Receives a stream. `OptionalOutputRenderTarget` and `ColorSpace` are advanced-display pins.
Returns true when the sender exists and a receive was enqueued. Outputs Out Mat and Out Texture.
Texture Parameter Name defaults to `SpoutTexture` when empty. Validate outputs before use.

Color Space decides how the received bytes are interpreted. Auto (the default) follows the format
convention: 8-bit frames (`_UNORM` or `_UNORM_SRGB`) decode as sRGB, 10-bit and 16-bit float
frames stay linear, which matches what other Spout applications publish. Linear applies no
decoding; use it when the sender publishes linear or data values in an 8-bit format, such as a
render target created with Force Linear Gamma, which publishes the same `_UNORM` format as an sRGB
one. sRGB forces decoding and is only valid for 8-bit frames; requesting it for a 10-bit
or float frame fails the call with a logged diagnostic rather than being silently ignored.

All consumers of a sender name share one color interpretation. A conflicting request is refused;
close the shared receiver before changing it. A source pixel-format change can establish a new
interpretation. Optional targets catch up separately from the completed image, even if attached
or recreated while the sender is paused.

The Optional Output Render Target must match the incoming frame's pixel format exactly and be
single-sampled. A mismatched target is left untouched and the mismatch is logged; the call still
succeeds and Out Texture still updates. Size differences are resolved by resizing the target.

### Operational status

`FSpoutStreamStatus GetSpoutStreamStatus(FName SpoutName, ESpoutType Type)` returns a game-thread
snapshot of the current role's activity, queue admissions/rejections, submitted frames, access
misses and native failures. Activity can mean awaiting a first frame. Submission does not
acknowledge display by another application. Viewport coalescing is outside non-viewport queue
counters; closing a role retires its counters.

`FSpoutRuntimeStatus GetSpoutRuntimeStatus()` reports initialized interop availability, active
roles, pending/peak commands, role/queue-cap refusals and estimated image reservations. Allocation
refusals have a separate counter. The query does not initialize the device.

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
Returns 0 when the request could not be applied, either because the requested value was outside the accepted range
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
share (supported: B8G8R8A8, R8G8B8A8, A2B10G10R10, FloatRGBA). Force Linear Gamma decides what an
8-bit target's bytes contain; the sender publishes plain `_UNORM` either way. True (default)
stores linear values: set the receiver's Color Space to Linear. False stores sRGB-encoded bytes,
the Spout convention, which a receiver on Auto reads correctly.

### Enums

ESpoutType: Sender, Receiver. ESpoutSendTextureFrom: GameViewport, TextureRenderTarget2D.
ESpoutColorSpace: Auto (follow the format convention - 8-bit decodes as sRGB, 10-bit and float
stay linear), SRGB (force sRGB decoding; 8-bit formats only), Linear (no decoding).

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
the precompiled UnrealGame Development and Shipping objects, generated headers and the
`.precompiled` manifests that `BuildPlugin` produces. It is not a supported way to package a game
without Visual Studio. Every generated archive is then
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
- Supported formats are BGRA8, RGBA8, RGB10A2 and RGBA16F; color interpretation must match the source.
- No colour conversion on send. Every 8-bit source is published as plain `_UNORM` with its bytes
  unchanged; `_UNORM_SRGB` is never published, because Spout's OpenGL receivers refuse it.
- The sRGB format tag is not carried: an sRGB render target and a Force Linear Gamma render target
  both publish `_UNORM`, so receivers cannot tell the source's colour intent from the format.
  `_UNORM` does not mean linear; by the Spout convention the bytes are sRGB-encoded, and a Force
  Linear Gamma source needs Color Space = Linear on the receiver.
- Qualify Capture Source, exposure and output format together; the plugin does not tone-map pixels.
- One sender per name.
- Senders and receivers must be called continuously.
- Receiving samples a stream once per Unreal frame and does not queue frames. At equal frame rates
  the two clocks drift in and out of alignment; near alignment single frames are skipped and the
  previous one repeated, with no access contention involved. In 1080p 60 fps testing this lowered
  delivery to 87 to 89 % of distinct frames in individual 10-second windows, while whole runs
  delivered about 99 %. r.Spout.AccessWaitMs does not change it; a higher Unreal frame rate reduces
  it but does not guarantee every frame, because jitter, hitches, stalls and contention can still
  lose frames.
- The access wait (r.Spout.AccessWaitMs, default 2 ms) applies to each attempt, not to a frame, so
  several busy streams add up: in testing, a peer that locked eight 1080p streams added about 13 ms
  of render-thread waiting per frame. 0 restores zero-wait acquisition only; the plugin's other
  fixes stay active.
- A keyed-mutex sender that holds its texture for tens of milliseconds can cause runs of consecutive
  skipped frames, with or without the access wait. With one 1080p 60 fps keyed test sender holding
  its texture 20 ms on every tenth frame, delivery was 84 % with up to 14 frames skipped in a row.
  Qualify keyed senders with their actual workload.
- Spout Info and Get Sender Info populate only Name, Width, Height, bIsAlive.
- The descriptor sets IsBetaVersion to false; this does not certify production readiness. Error handling favors visible,
  logged failures over silent recovery.

---

## 16. Troubleshooting

The plugin logs to the `LogSpoutPlugin` category, so `Project/Saved/Logs/ProjectName.log` is the
first place to look for any of these.

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
RGB10A2. On the receiving side, set the Spout Receiver node's Color Space pin: Auto for ordinary
senders, Linear when the sender publishes linear 8-bit data (for example a Force Linear Gamma
render target sent from another Unreal instance). The format cannot tell a receiver this, because
8-bit sources are published as `_UNORM` whatever their gamma; set Linear in external applications
too when the Unreal source uses Force Linear Gamma.

Spout Demo Receiver shows "No sender detected" for an sRGB render target: fixed in 1.0.0. Earlier
pre-release builds published 8-bit sRGB render targets as `_UNORM_SRGB`, which Spout 2.007's
OpenGL receivers refuse; the sender now publishes plain `_UNORM` with unchanged bytes.

Log says "shared-texture access was unavailable": another application still held the shared
texture after the access wait (r.Spout.AccessWaitMs, default 2 ms). The sender skips that frame and
publishes the next, and the receiver copies the sender's newest frame on a later call. The line is
throttled to one per stream every 5 seconds; Get Spout Stream Status counts every miss. The wait
applies to each attempt on the render thread, not to a frame, so a peer that keeps a texture locked
costs up to that long on every frame for that stream and several locked streams add up;
r.Spout.AccessWaitMs 0 never waits (zero-wait acquisition only; other fixes stay active).

Frames occasionally skip or repeat with no access misses: sampling, not contention. The receiver
reads once per Unreal frame and a sender at the same frame rate drifts in and out of alignment
with it. r.Spout.AccessWaitMs does not change this; see Limitations.

Stream name mismatch: use the exact external spelling from Spout Info. Local FName keys do not distinguish case.

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
- For SceneCapture2D sends, start with Final Color (HDR) in Linear Working Color Space and qualify the chosen capture pipeline.
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

## Operational limits and status

Every public API requires the game thread. Failed discovery retains no stream record. Open streams
and their last images persist until close; Tick/timers request transfers rather than keeping a
registration alive. Editor world cleanup closes all plugin streams across PIE worlds. Packaged
callers must close at their own level/EndPlay boundary. Device removal requires an application restart.

GetSpoutStreamStatus reports scheduling, submitted-frame, contention and native-failure counters.
GetSpoutRuntimeStatus reports active roles, pending work, rejection counts and estimated retained
resource reservations. These snapshots are not acknowledgments from another application.

Default operational limits are 32 active roles, 128 pending commands, 4 pending commands per stream,
32 material bindings/optional targets per receiver, 8192 pixels per dimension, 128 MiB per frame and
1024 MiB aggregate image reservations. Console variables under r.Spout.Max control these limits.
Cancelled commands remain counted until released. Old received textures held by callers remain
charged until GC/resource release. Caller-owned resources and driver/engine allocations are additional.
Named and keyed texture access wait at most r.Spout.AccessWaitMs (default 2 ms, clamped to 0 to 10)
for a busy peer, per attempt and on the render thread; 0 never waits (zero-wait acquisition only;
the plugin's other fixes stay active). It is not a per-frame bound: several busy streams can each
add up to that bound in one frame (about 13 ms per frame with eight locked 1080p streams in
testing), and Windows scheduling can make a wait last longer than requested (about 16 ms at the
default Windows timer resolution; Unreal normally requests 1 ms). A texture still busy after the
wait skips that transfer until a later call. GPU completion can still stall rendering. Receiving
samples each stream once per Unreal frame and does not queue frames, so frames can be skipped or
repeated without contention when both sides run at the same frame rate.

Release scripts require PowerShell 7.2 or newer. Release testing installs the exact release ZIP
into fresh host projects and runs the shipped examples, in addition to native API tests.
