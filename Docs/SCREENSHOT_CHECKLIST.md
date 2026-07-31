# Screenshot Checklist

Screenshots for the User Manual PDF. The manual embeds each image automatically when a file
with the exact name below exists in `Docs/manual_screenshots/`; until then it shows an
instruction placeholder. After adding images, rebuild the PDF.

How to add a screenshot:

1. Capture it (1920x1080 or higher, Windows, clean editor theme).
2. Save it into `Docs/manual_screenshots/` with the exact filename in the table.
3. Rebuild the PDF (re-run the manual build script). The placeholder is replaced by the image
   with a numbered caption.

Filenames and what each must show:

| ID | Filename | Must show |
|---|---|---|
| SP-001 | `SP-001_plugin-enabled.png` | Edit > Plugins with "Spout" searched; the Spout Plugin tile, its ticked enabled checkbox, and the Rendering category. Include the Beta label if shown. |
| SP-002 | `SP-002_rhi-directx12.png` | Project Settings > Platforms > Windows; the Default RHI dropdown set to DirectX 12. |
| SP-003 | `SP-003_plugin-folder.png` | File Explorer at MyProject/Plugins/SpoutPlugin/ showing Binaries, Content, Resources, Source, ThirdParty, and SpoutPlugin.uplugin. |
| SP-004 | `SP-004_spout-dll-binaries.png` | File Explorer at Plugins/SpoutPlugin/Binaries/Win64/ with both Spout.dll and UnrealEditor-SpoutPlugin.dll visible. |
| SP-017 | `SP-017_add-sender-node.png` | Blueprint context menu (right-click) with "Spout Sender" searched, showing the node under the Spout category before placing. |
| SP-005 | `SP-005_sender-viewport-graph.png` | Event Tick -> Spout Sender; Spout Name = UnrealViewport; Send Texture From = GameViewport; Texture Render Target 2D empty. Pin text legible. |
| SP-018 | `SP-018_timer-setup.png` | Set Timer by Event (or by Function Name) calling the function that runs Spout Sender; Time = 0.033, Looping = true; bound event/function name visible. |
| SP-006 | `SP-006_sender-rendertarget-graph.png` | Event Tick -> Spout Sender; Send Texture From = TextureRenderTarget2D; the render target reference and Spout Name visible. |
| SP-007 | `SP-007_scenecapture-source.png` | SceneCapture2D Details > Capture Source dropdown showing "Final Color (HDR) in Linear Working Color Space". Highest-priority figure. |
| SP-008 | `SP-008_rendertarget-settings.png` | TextureRenderTarget2D asset settings: Render Target Format (RGBA8 sRGB) and Size X / Size Y. |
| SP-009 | `SP-009_receiver-graph.png` | Event Tick -> Spout Receiver with advanced pins expanded; Out Mat, Out Texture, return bool wired (e.g. Branch on return, Out Mat into Set Material). Pin names legible. |
| SP-010 | `SP-010_material-texture-parameter.png` | M_SpoutMaterial (or your own) with the Texture Parameter selected; its Parameter Name (default SpoutTexture) readable and connected to Base Color / Emissive. |
| SP-011 | `SP-011_external-app-receiving.png` | TouchDesigner (or another receiver) Spout In TOP showing the live Unreal image, with the Unreal sender name visible in the dropdown. |
| SP-012 | `SP-012_spout-info-debug.png` | A graph calling Spout Info and printing the array (name, width, height), or the on-screen/log output listing senders. |
| SP-013 | `SP-013_log-loaded.png` | Output Log filtered to LogSpoutPlugin showing "Spout Plugin Loaded" and "SpoutDX11on12 Initialized.". |
| SP-014 | `SP-014_log-dll-missing.png` | Output Log showing the Spout.dll-not-found startup error naming the checked paths. Optional - include only if cleanly reproducible. |
| SP-015 | `SP-015_cvar-console.png` | Editor console showing r.Spout.GPUReceiver and its current value, plus an example of setting it to 0 or 1. |
| SP-016 | `SP-016_example-content.png` | Content Browser (Show Plugin Content enabled) with BP_SpoutSenderExample_, BP_SpoutReceiverExample, M_SpoutMaterial, RT_SpoutSenderTest_, RT_SpoutReceiverTest. |

Notes:

- SP-005, SP-006, SP-009, SP-017, SP-018 are the Blueprint figures; capture them zoomed so pin
  names and values are readable, since they are the core of the manual.
- SP-007 is the most important troubleshooting figure (the wrong Capture Source is the most
  common black-output cause).
- Use the real example asset names when photographing the Content folder; do not rename them.
- Files go in `Docs/manual_screenshots/`. PNG or JPG both work.
