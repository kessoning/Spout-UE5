# Receiver Workflow

The receiver consumes a Spout stream published by any Spout application and exposes it to
Unreal as a texture, an optional dynamic material instance, and an optional render target copy.

Call Spout Receiver continuously (Tick or timer) with a Spout Name that exactly matches the
sender.

## Outputs

| Output | Type | Notes |
|---|---|---|
| Out Texture | `UTexture2D*` (by ref) | Transient texture owned by the plugin, created and resized to match the stream, updated each call. |
| Out Mat | `UMaterialInstanceDynamic*` (by ref) | Created from Input Material on first call if Input Material is set, with Out Texture bound to the texture parameter. |
| Optional Output Render Target | `UTextureRenderTarget2D*` (input) | If provided, the received frame is also copied into it. It is resized to match the stream automatically, but its pixel format must match the incoming frame exactly and it must be single-sampled; a mismatched target is left untouched (see below). |

Always validate Out Mat and Out Texture before using them. The function returns false when the
named sender is not currently available, in which case the outputs are not updated.

## Finding and matching senders

- Use [Spout Info](BLUEPRINT_API.md#spout-info) to list active sender names, widths, and heights.
- The Spout Name must match the sender exactly. A mismatch returns no data.
- The receiver reconnects automatically. When the named sender appears, opens a new shared
  handle, or changes size, the plugin reopens the shared texture and resizes its outputs.

## Receive paths (GPU-direct vs CPU readback)

The receiver has two implementations, selected at runtime by the console variable
`r.Spout.GPUReceiver`:

| Value | Path | Behavior |
|---|---|---|
| `1` (default) | GPU-direct | Opens the sender's shared DX11 texture and copies it straight into the UE RHI destination through D3D11-on-12 wrapping. No staging texture, no Map/Unmap, no per-frame CPU copy. |
| `0` | Legacy CPU readback | Copies into a CPU-readable staging texture, maps it, and uploads via `FRHICommandList::UpdateTexture2D`. Kept for debugging and parity with older builds. More expensive at high resolution. |

Both paths produce visually identical output. The difference is performance only. Unless you
have a specific reason to use the legacy path, leave the default.

Where to set the CVar:

- In-editor console (per session): `r.Spout.GPUReceiver 0` or `1`.
- `Config/DefaultEngine.ini` under `[SystemSettings]`: `r.Spout.GPUReceiver=1`.
- Launch argument: `-ExecCmds="r.Spout.GPUReceiver 0"`.
- Blueprint: Execute Console Command with `r.Spout.GPUReceiver 0`.

The value is read once per receive call on the game thread, so changes take effect on the next
call with no restart needed.

## Async Receive (Beta)

Both receive nodes have an advanced **Async Receive (Beta)** input, and the shipped
`BP_SpoutReceiverExample` has a checkbox of the same name under **Spout > Experimental** in its
Details. It is off by default. When on, a worker thread takes the sender's texture and waits for
the copy, and the render thread only queues a copy of the newest completed image. That can improve
delivery under load, but it can also increase frame-time spikes or image latency: it is a beta
trade-off, not a speed-up.

The setting applies per sender name. All graphs and actors receiving one name share one receiver
session, the first accepted receive decides its mode, and a call asking for the other mode is
refused and logged until the receiver is closed. To change it, set it before Play, or close the
receiver and receive again. A sender format other than BGRA8 or RGBA8, or a failure of the worker,
makes the session run synchronously; `GetSpoutStreamStatus` reports the requested mode, the
effective mode and why. Details: [Async Receive (Beta)](BLUEPRINT_API.md#async-receive-beta) and
[limitations](LIMITATIONS.md#async-receive-beta-off-by-default).

## Colour space

The Color Space pin (advanced) decides how the received bytes are read. **Auto** decodes 8-bit
frames as sRGB, whether the sender publishes `_UNORM` or `_UNORM_SRGB`, and keeps 10-bit and
16-bit float frames linear. **Linear** applies no decoding. **sRGB** forces decoding and is only
valid for 8-bit frames. See [ESpoutColorSpace](BLUEPRINT_API.md#espoutcolorspace).

An 8-bit `_UNORM` format does not say whether the bytes are sRGB-encoded or linear. This plugin's
own sender publishes `_UNORM` for both sRGB and Force Linear Gamma render targets, so the format
cannot tell the receiver which one it is. Auto follows the Spout convention that 8-bit bytes are
gamma-encoded, which is correct for an sRGB source. For a sender that publishes linear 8-bit data,
such as a Force Linear Gamma render target, set Color Space to Linear.

## Shared-texture access

Each copy takes Spout's cross-process access lock for the shared texture, so a frame is never read
while the sender is midway through writing it. If a peer (the sender or another receiver) holds the
texture, the copy waits at most `r.Spout.AccessWaitMs` (default 2 ms, clamped to 0 to 10) for it,
on the render thread, and goes ahead as soon as the peer lets go. If the texture is still busy
after that, the frame is not read yet: the receiver tries again on a later call and copies the
sender's newest frame then, so a frame the sender replaced in the meantime is lost, not queued. The
log says "shared-texture access was unavailable", at most once per stream every 5 seconds, and Get
Spout Stream Status counts each miss in `AccessMisses`. The wait applies to each copy attempt, not
to a whole frame, so several busy streams add up; Windows scheduling can make it last longer than
requested. `r.Spout.AccessWaitMs 0` restores zero-wait acquisition only; the plugin's other fixes
stay active. See [LIMITATIONS.md](LIMITATIONS.md#performance-and-diagnostics).

## Frame sampling

The receiver copies at most once per Unreal frame and each copy gets the sender's newest frame;
Spout does not queue frames. A frame the sender replaces before the next copy is never received,
even when access is never contended. When the sender and Unreal run at the same frame rate, their
timing drifts in and out of alignment, and near alignment single frames are skipped and the previous
frame is shown for one extra Unreal frame (in 60 fps testing, 87 to 89 % of frames in individual
10-second windows, about 99 % over whole runs). Running Unreal faster than the sender reduces this
but does not guarantee every frame. Keyed-mutex senders that hold their texture for long periods
can also cause runs of skipped frames. See [LIMITATIONS.md](LIMITATIONS.md#performance-and-diagnostics).

## Materials

To drive a material from the received stream:

1. Create a material with a Texture Parameter (the example content is `M_SpoutMaterial`).
2. Pass that material as Input Material to the Spout Receiver node.
3. Set Texture Parameter Name to match the parameter name inside the material. If you leave it
   empty the plugin uses `SpoutTexture` as the default name, so name your parameter
   `SpoutTexture` to match the default.
4. On the first call with a valid Input Material, the plugin creates a dynamic material
   instance, binds Out Texture to the parameter, and returns it as Out Mat.
5. Apply Out Mat to a mesh.

If you do not need a material, leave Input Material empty and use Out Texture directly, or use
the Optional Output Render Target.

## Render target output

If you pass an Optional Output Render Target, the received frame is copied into it on the same
render path as Out Texture. It is resized to the stream dimensions automatically. Two things
are not adjusted automatically and must match:

- The render target's **pixel format** must equal the received frame's format exactly. The plugin
  never reformats your asset at runtime.
- The render target must be **single-sampled** (no MSAA).

A target that fails either check is excluded from the copy and left completely untouched, and a
warning naming both formats is written to the log; the call still succeeds and Out Texture keeps
updating. If the target stays black while Out Texture works, check the log for that warning and
recreate the render target to match the sender. The recommended format is RGBA8 sRGB, which
matches the most common senders. See
[RENDER_TARGETS_AND_FORMATS.md](RENDER_TARGETS_AND_FORMATS.md).

Note: the `OutputRenderTarget` field on `FSenderStruct` is a separate, unused field and is not
the same as this parameter. The functional render-target output is the Optional Output Render
Target pin on the Spout Receiver node.

## Performance notes

- Prefer the default GPU-direct path. The legacy CPU readback path adds a full-frame copy,
  map, and upload each frame and becomes expensive at large resolutions or high frame rates.
- Use a timer instead of Tick when you do not need full frame-rate updates.
- Out Texture is recreated only when the stream size changes, so steady-state receiving reuses
  the same texture.
