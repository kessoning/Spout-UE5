# Render Targets and Formats

This page covers render target setup and the pixel formats the plugin works with, for both the
sender (render-target source) and receiver (texture and optional render-target output).

## Recommended render target formats

| Format | Status | Notes |
|---|---|---|
| RTF RGBA8 sRGB | Recommended, tested | Best general-purpose choice for both sender source and receiver output. |
| RTF RGB10A2 | Tested | Higher precision option, useful to reduce banding. |
| Other formats | Use with caution | May not work correctly or may reduce quality. Test before relying on them. |

For most workflows, start with RGBA8 sRGB. Move to RGB10A2 only if you see banding and your
external application supports it.

## What the receiver maps internally

When receiving, the plugin reads the sender's DXGI format and maps it to a UE pixel format for
the transient texture it creates:

| Sender DXGI format | UE pixel format |
|---|---|
| B8G8R8A8 (UNORM / TYPELESS / SRGB) | `PF_B8G8R8A8` |
| R8G8B8A8 (UNORM / TYPELESS / SRGB) | `PF_R8G8B8A8` |
| R10G10B10A2 (UNORM / TYPELESS) | `PF_A2B10G10R10` |
| R16G16B16A16_FLOAT (or TYPELESS) | `PF_FloatRGBA` |
| Anything else | Refused; a warning naming the format is logged |

This mapping defines what the receiver can interpret. A format outside this table is never
reinterpreted: the frame is refused and a warning naming the advertised format is written to
the log, so an unsupported sender shows up as a clear rejection rather than as corrupted or
misread output. The typeless rows are a deliberate mapping to the typed layout Spout peers
expect, not a fallback.

## Create Texture Render Target 2D defaults

The Create Texture Render Target 2D node defaults to:

- Width 1024, Height 768
- Format `PF_B8G8R8A8`
- Force Linear Gamma true
- Wrap addressing on both axes

Override Width, Height, and Format as needed. For a sender source intended for an sRGB
pipeline, an RGBA8 sRGB asset created in the editor is usually clearer than the linear-gamma
runtime default.

## sRGB and linear notes

- The plugin shares the texture as-is. It applies no colour conversion on the send path: the
  pixel bytes and alpha are copied unchanged. Every 8-bit source (BGRA8 or RGBA8, sRGB or linear,
  typed or typeless) is published as the plain `_UNORM` format, never `_UNORM_SRGB`, because
  Spout 2.007's OpenGL receivers, such as the Spout Demo Receiver 2.007.015, refuse
  `B8G8R8A8_UNORM_SRGB`. RGB10A2 and RGBA16F render targets store linear values and are
  published as they are.
- The viewport sender publishes the back buffer as it is: display (sRGB-encoded) values, in
  `R10G10B10A2_UNORM` by default. The format cannot tell a receiver that these 10-bit values are
  display values rather than linear ones, so a receiver on Auto reads them as linear and the image
  looks too bright. Set the receiving Unreal's Color Space to sRGB: it decodes each 10-bit frame
  into a linear 16-bit float texture. Applications that show the values as they are, such as the
  Spout Demo Receiver, show the viewport stream as Unreal showed it.
- Keeping the bytes unchanged does not keep the sRGB format tag: an sRGB render target (Force
  Linear Gamma off) and a Force Linear Gamma render target both publish `_UNORM`, so a receiving
  application cannot tell the source's colour intent from the format. `_UNORM` does not mean the
  bytes are linear. By the Spout convention 8-bit `_UNORM` bytes are gamma-encoded (sRGB), which
  is what an sRGB render target produces and what receivers on Auto expect. A Force Linear Gamma
  target stores linear values, so its receiver must be set to Color Space = Linear (or the
  receiving application's equivalent).
- On receive, `_UNORM_SRGB` from other peers is still accepted and decodes as sRGB on Auto, like
  8-bit `_UNORM`.
- Color correctness is therefore controlled by your Capture Source, render target format and
  gamma choices and by the receiver's configuration, not by the plugin. The tested combination is Final Color (HDR) in Linear Working
  Color Space as the SceneCapture2D Capture Source paired with an RGBA8 sRGB render target.
- If colors look washed out or too dark in the external application, revisit the render target
  format and the SceneCapture2D Capture Source before anything else.

See [SCENECAPTURE2D.md](SCENECAPTURE2D.md) for capture setup and the black-output cause.
