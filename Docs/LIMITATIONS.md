# Limitations

Honest list of what the plugin does not do or does not guarantee, drawn from the current
source.

## Platform and RHI

- Windows 64-bit only. The module is restricted to Win64 by `PlatformAllowList`.
- DirectX 12 RHI required. The plugin creates a D3D11-on-D3D12 interop device and refuses to
  initialize on any other RHI, logging `SpoutPlugin requires DX12 RHI.`.
- Both applications sharing a stream must be on the same GPU. Spout's shared-texture model does
  not span GPUs.

## Formats

- Tested and recommended render target formats are limited to RGBA8 sRGB and RGB10A2.
- The receiver accepts a fixed set of DXGI formats (BGRA8, RGBA8, R10G10B10A2, RGBA16F, and
  their typeless variants). Anything else is refused with a logged warning naming the format;
  frames are never reinterpreted as a different layout.

## Color

- No colour conversion is applied on the send path. What the sender advertises follows the render
  target's own gamma setting: a Force Linear Gamma target publishes a plain `_UNORM` format, a
  non-linear one publishes `_UNORM_SRGB`. Colour correctness therefore depends on your Capture
  Source and render target format choices, and on pairing them with the receiver's Color Space pin.
- SceneCapture2D must use Final Color (HDR) in Linear Working Color Space. SceneColor (HDR)
  produces a black image. This is a known limitation, not a misconfiguration you can tune around.

## Behavior and lifetime

- One sender per name. Two senders sharing a name conflict.
- Senders and receivers must be called continuously (Tick or timer). Nothing keeps a stream
  alive between calls.
- The Spout Info and Get Sender Info nodes populate only `Name`, `Width`, `Height`, and
  `bIsAlive` on `FSenderStruct`. The other fields are not driven by the plugin.
- The `FSenderStruct::OutputRenderTarget` field is unused. The functional render-target output
  is the Optional Output Render Target pin on the Spout Receiver node.

## Performance

- The legacy CPU readback receiver path (`r.Spout.GPUReceiver 0`) performs a full-frame copy,
  map, and upload each frame and is expensive at high resolution or frame rate. The default
  GPU-direct path avoids this.
- Cost scales with resolution on both send and receive.

## Production readiness

- The plugin is marked beta (`IsBetaVersion` in the descriptor) and is not guaranteed
  production-ready without thorough testing on your target hardware and drivers.
- Error handling is intentionally minimal and favors visible, logged failures over silent
  recovery. It is built for real-time workflows, prototyping, tools, and experimentation rather
  than long-running unattended deployments.

## Reserved or partial functionality

- None currently. The reserved sender-side gamma correction that earlier versions of this document
  described was removed from the source; colour space is now expressed through the advertised DXGI
  format and the receiver's Color Space pin instead. See `CHANGELOG.md`.
