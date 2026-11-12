# SpoutPlugin User Manual - Source

This file is the manual's own text and its chapter list. `Docs/build_user_manual.py` renders the PDF
`Docs/Manual/SpoutPlugin_User_Manual.pdf` from it: the sections below in order, with each `include` line replaced by
that document as a chapter. The included documents are the same pages shipped and published with the plugin, so the
manual and the documentation cannot say different things.

- Plugin: SpoutPlugin (Spout Plugin)
- Version: 1.0.0 (must match `SpoutPlugin.uplugin`; `Scripts/Validate-Documentation.ps1` fails the
  release when it does not)
- Engine target: Unreal Engine 5.3 - 5.8 (Win64)
- Modules: SpoutPlugin (Runtime), SpoutPluginShaders (Runtime), SpoutPluginEditor (Editor)
- Platform: Windows 64-bit, DirectX 12 RHI
- Documentation date: 2026-11-12

---

## About This Manual

This manual covers everything needed to use SpoutPlugin: installation, the drag-and-drop Spout Sender and Spout
Receiver actors, sending and receiving with Blueprints, render targets and colour, packaging, limitations and
troubleshooting, followed by the complete Blueprint API Reference and the C++ API Reference.

You do not need C++ to use the plugin: the actors work with no code at all, and every function is a Blueprint node.
The C++ API Reference is for projects that prefer to call the same functions from code.

The chapters after "Formats and Colour at a Glance" are the plugin's documentation pages, the same text that ships in
the `Docs` folder and is published with the source. Where a page refers to another page by its file name, the
corresponding chapter of this manual has the same title.

## What SpoutPlugin Is

Spout is a Windows technology for sharing GPU textures between real-time applications with no disk or network
transfer. One application publishes a texture (a sender) and another reads it live (a receiver). It is widely used to
move video between tools such as TouchDesigner, Resolume, Notch and OBS.

SpoutPlugin brings Spout into Unreal Engine 5. It sends Unreal's rendered output, or any render target, to other
Spout applications, and receives Spout streams back into Unreal as textures, materials and render targets. The
transfer stays on the GPU; the optional CPU receive path, for debugging, reads back through system memory.

Three ways to use it:

- **Drag-and-drop actors.** Place a Spout Sender or a Spout Receiver from the Place Actors panel (category Spout) and
  press Play. The receiver shows the stream on its own unlit screen, sized to the stream.
- **Blueprint nodes.** Spout Sender, Spout Receiver With Material, Close Sender, Close Receiver and the other nodes in
  the Spout category, for full control from any Blueprint.
- **C++.** The same functions and actors from your own C++ code.

The sender has three sources: Game Viewport Only (the default: the game view without the editor around it),
Game Viewport (Whole Window) and Texture Render Target 2D.

The technical problem it solves: Spout shares DirectX 11 textures, while Unreal Engine 5 runs on the DirectX 12 RHI.
The plugin bridges Unreal's D3D12 textures to Spout's D3D11 shared textures on the GPU. You do not interact with this
directly; it is why DirectX 12 is required and why the plugin is Windows-only.

What it does not do: it is not a video codec, not a network streaming solution and not a colour-management system.
It does not apply gamma or colour correction when sending.

## Requirements

- Windows 64-bit.
- Unreal Engine 5.3 to 5.8. Each prebuilt archive matches exactly one engine version. On Fab the plugin is listed
  for the engine versions Fab offers.
- The DirectX 12 RHI. The plugin does not initialize on any other RHI; it logs `SpoutPlugin requires DX12 RHI.`
  and stops.
- Both applications sharing a stream on the same GPU. Spout's shared textures do not span GPUs.
- For building from source and for packaging a game: Visual Studio 2022 with the C++ workload, its MSVC toolchain and
  a Windows SDK supported by your engine version. Opening a project with the prebuilt plugin in a matching editor
  needs no compiler.

The Installation chapter has the details, including the packaging requirement for Blueprint-only projects.

## Formats and Colour at a Glance

These rules apply everywhere in the plugin; the chapters that follow explain them in context.

- **Supported formats.** The plugin shares B8G8R8A8, R8G8B8A8, A2B10G10R10 and FloatRGBA (RGBA16f), including their
  typeless variants. Anything else is refused: a warning naming the advertised format is written to the log and
  nothing is published or received for that frame. Unsupported formats are never reinterpreted as B8G8R8A8, and no
  conversion is invented, because advertising a layout the source does not have produces silently garbled frames
  instead of an honest failure.
- **Sending changes no pixels.** The sender copies the bytes unchanged. Every 8-bit source is published as plain
  `_UNORM`, never `_UNORM_SRGB`, because Spout's OpenGL receivers refuse `_UNORM_SRGB`.
- **Receiving: the Color Space setting.** The receive functions take an `ESpoutColorSpace ColorSpace` parameter (the
  actor's Color Space setting), with the values ESpoutColorSpace: Auto, sRGB and Linear. Auto follows the format convention: 8-bit decodes as sRGB, while 10-bit and 16-bit
  float are read as linear. Linear applies no decoding, for an 8-bit sender of linear values (such as a Force Linear
  Gamma render target). sRGB decodes sRGB-encoded values, including 10-bit ones.
- **Unreal viewport streams need sRGB.** A viewport stream carries the back buffer's display (sRGB-encoded) values in
  10 bits, which Auto reads as linear: receive it with Color Space sRGB.
- **Output render targets.** An optional output render target must match the incoming frame's format exactly (except
  RGBA16f for a 10-bit sender received with Color Space sRGB) and be single-sampled, with Auto Generate Mips off. A
  mismatched target is left untouched and the mismatch is logged; a size difference is not a mismatch, the target is
  resized.

<!-- include: Docs/QUICK_START.md -->
<!-- include: Docs/INSTALLATION.md -->
<!-- include: Docs/SENDER.md -->
<!-- include: Docs/RECEIVER.md -->
<!-- include: Docs/RENDER_TARGETS_AND_FORMATS.md -->
<!-- include: Docs/SCENECAPTURE2D.md -->
<!-- include: Docs/PACKAGING_AND_DISTRIBUTION.md -->
<!-- include: Docs/TROUBLESHOOTING.md -->
<!-- include: Docs/LIMITATIONS.md -->
<!-- include: Docs/BLUEPRINT_API.md -->
<!-- include: Docs/CPP_API.md -->

## Appendix

### Example content

The plugin's `Content` folder holds working examples. Enable Show Plugin Content in the Content Browser settings to
see them.

| Asset | Purpose |
|---|---|
| `BP_SpoutSenderExample_` | Example Blueprint sender. |
| `BP_SpoutReceiverExample` | Example Blueprint receiver. |
| `M_SpoutMaterial` | Material with a `SpoutTexture` texture parameter for received streams. |
| `M_SpoutDisplayUnlit`, `T_SpoutDisplayDefault` | The Spout Receiver actor's unlit screen material and its default texture. |
| `RT_SpoutSenderTest_`, `RT_SpoutReceiverTest` | Example render targets for sending and receiving. |

### Plugin structure

```
SpoutPlugin.uplugin
Source/SpoutPlugin/          runtime module: public API (Public/), implementation (Private/)
Source/SpoutPluginShaders/   runtime module: global shaders (10-bit sRGB decode)
Source/SpoutPluginEditor/    editor module: Place Actors registration
Source/ThirdParty/Spout/     Spout SDK headers, Spout.lib, Spout.dll
Shaders/                     shader source
Content/                     examples and the receiver actor's screen material
Resources/                   plugin icon
Docs/                        documentation and this manual
```

### Third-party attribution

- Spout: http://spout.zeal.co/
- Based on the original repository by zuyi53: https://github.com/zuyi53/Spout-UE5

### License

MIT License (`LICENSE`). Use, modification and redistribution, including commercial redistribution, are permitted.
Every copy must keep the copyright and license notices in `LICENSE`, and redistributions of the Spout SDK must keep
its BSD notice in `THIRD_PARTY_NOTICES.md`. Crediting this project is appreciated but not required.

### Glossary

| Term | Meaning |
|---|---|
| Spout | Windows GPU texture-sharing technology between real-time applications. |
| Sender | An application, actor or Blueprint call publishing a shared texture under a name. |
| Receiver | An application, actor or Blueprint call reading a shared texture by name. |
| Render target | A `UTextureRenderTarget2D`: a texture Unreal can render into. |
| SceneCapture2D | An Unreal actor or component that renders a view into a render target. |
| Game Viewport Only | The sender source that sends only the rendered game viewport. |
| Receiver session | The one shared receive of a sender name inside a game, used by every consumer of that name. |
| RHI | Rendering Hardware Interface, Unreal's graphics API abstraction. |
| D3D12, D3D11 | DirectX 12 (Unreal's renderer, required) and DirectX 11 (Spout's shared textures). |
| Shared texture | A GPU texture exposed through a shared handle so another process can read it. |
