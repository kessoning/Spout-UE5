# Sender Workflow

The sender publishes a UE texture to other Spout applications as a shared GPU texture. There
are two sources, selected by the `Send Texture From` pin on the Spout Sender node.

## Sources

### GameViewport

Sends the active game viewport backbuffer. Internally the plugin hooks the Slate renderer's
"back buffer ready to present" event and queues the capture there, so the copy happens at the
correct point in the frame. Because of this:

- The game window must be actively rendering. A minimized window does not present a backbuffer.
- The backbuffer is captured in its Present state and transitioned for the copy, then restored.

When using GameViewport, leave the Texture Render Target 2D pin empty.

### TextureRenderTarget2D

Sends the contents of a `UTextureRenderTarget2D` you provide. The render target's RHI texture
is resolved on the game thread and the GPU copy is enqueued on the render thread. A common way
to fill the render target is with a `SceneCapture2D` - see [SCENECAPTURE2D.md](SCENECAPTURE2D.md).

## Stream name rules

- The Spout Name uniquely identifies the stream and must match the receiver's name exactly,
  including case.
- Only one sender can own a given name at a time. Two senders sharing a name conflict.
- The registry is keyed by name and type, so a sender and a receiver may use the same name in
  the same process without corrupting each other.

## Lifetime and update behavior

- The sender must be called continuously (Tick or timer) to request continuous frame updates; registration persists until close.
  There is no background thread keeping it running between calls.
- The shared texture is created or resized automatically when the source dimensions or format
  change. On the first valid send the stream is registered in Spout's global registry; later
  sends update it.
- Call Close Sender to remove a sender and release its shared handle. Close is scoped to the
  sender entry only and leaves any receiver with the same name untouched.

## How the copy works

The plugin runs UE on the DirectX 12 RHI but Spout shares DirectX 11 textures. A process-wide
D3D11-on-D3D12 interop device wraps the UE D3D12 source texture as a D3D11 texture, then issues
a `CopyResource` into a Spout-owned shared DX11 texture. No CPU readback and no disk or network
transfer are involved on the send path.

## Performance notes

- Cost scales with resolution. Larger textures cost more GPU copy time and bandwidth per frame.
- Use a timer instead of Tick when you do not need full frame-rate updates.
- The shared texture and the D3D11 wrap are cached and reused while geometry and format are
  stable, so steady-state sending avoids reallocations.
- Keep per-frame Blueprint logic around the send call minimal.

## Known constraints

- Sender-side colour correction is not applied; the pixel bytes and alpha are shared unchanged.
  Every 8-bit source is published as the plain `_UNORM` format (`B8G8R8A8_UNORM` or
  `R8G8B8A8_UNORM`), never `_UNORM_SRGB`, because Spout 2.007's OpenGL receivers, including the
  Spout Demo Receiver 2.007.015, refuse `B8G8R8A8_UNORM_SRGB`.
- Keeping the bytes unchanged does not keep the sRGB format tag: an sRGB render target (Force
  Linear Gamma off) and a Force Linear Gamma render target both publish `_UNORM`, so a receiving
  application cannot tell the source's colour intent from the format. `_UNORM` does not mean the
  bytes are linear. By the Spout convention 8-bit `_UNORM` bytes are gamma-encoded (sRGB), which
  is what an sRGB render target produces and what receivers on Auto expect; a Force Linear Gamma
  target stores linear values, so set its receiver's Color Space to Linear. Manage colour on the
  source side (Capture Source, render target format and gamma) and pair it with the receiver's
  Color Space pin.
- Each publish takes Spout's cross-process access lock for the shared texture. If a receiver holds
  the texture, the publish waits at most `r.Spout.AccessWaitMs` (default 2 ms, clamped to 0 to 10)
  on the render thread for it to finish. If it is still busy after that, the frame is skipped
  rather than overwritten mid-read, and the next frame is published once the texture is free. The
  skip is logged as "shared-texture access was unavailable", at most once per stream every 5
  seconds, and counted in Get Spout Stream Status (`AccessMisses`). The wait applies to each
  publish, not to a whole frame; see [LIMITATIONS.md](LIMITATIONS.md#performance-and-diagnostics).
- Windows-only, DirectX 12 RHI required.

See the [Blueprint API Reference](BLUEPRINT_API.md) for the exact Spout Sender and Close Sender
signatures.
