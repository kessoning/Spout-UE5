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

- The plugin shares the texture as-is. It applies no colour conversion on the send path; the
  colour space is carried by the DXGI format the sender advertises, which follows the render
  target's own Force Linear Gamma setting.
- Color correctness is therefore controlled by your Capture Source and render target format
  choices, not by the plugin. The tested combination is Final Color (HDR) in Linear Working
  Color Space as the SceneCapture2D Capture Source paired with an RGBA8 sRGB render target.
- If colors look washed out or too dark in the external application, revisit the render target
  format and the SceneCapture2D Capture Source before anything else.

See [SCENECAPTURE2D.md](SCENECAPTURE2D.md) for capture setup and the black-output cause.
