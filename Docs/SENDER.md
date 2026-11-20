# Sender Workflow

The sender publishes a UE texture to other Spout applications as a shared GPU texture. There
are three sources, selected by the `Send Texture From` pin on the Spout Sender node. The pin defaults to
Game Viewport Only.

**Upgrading from a version where Game Viewport was the default:** when an existing node is reconstructed (for
example when its Blueprint is loaded in the editor), Unreal compares the pin's saved value with its default; it
does not track whether you selected that value yourself. An unconnected pin set to Game Viewport therefore
switches to Game Viewport Only, even if you selected Game Viewport explicitly. Texture Render Target 2D selections
and connected inputs remain intact. To retain whole-window capture, select Game Viewport (Whole Window) after
upgrading and save the Blueprint.

## Without a Blueprint: the Spout Sender actor

Drag **Spout Sender** from the Place Actors panel (category Spout) into the level and press Play. It sends Game
Viewport Only by default; its Details panel has the same settings as the Spout Sender node. See the README section
"Drag-and-drop actors" and [BLUEPRINT_API.md](BLUEPRINT_API.md#placeable-actors-spout-sender-and-spout-receiver).

**Sending while you edit the level (since 1.1.0).** Set Edit > Project Settings > Plugins > Spout > Startup Policy to
Editor and Play and turn on the actor's **Send In Editor**. In the editor, Game Viewport Only sends the level editor's
active viewport, Game Viewport (Whole Window) the level editor's window, and a Texture Render Target is sent as during
Play. When you press Play, Play's copy of the actor takes the name over; the editor's sender continues when Play
ends. Unreal updates the level only while a level viewport is Realtime: without one the stream pauses, and the
actor's Status and the Output Log say so (Ctrl+R in the viewport resumes it). The Spout Sender node itself still
sends only while the game runs.

## Sources

### GameViewport (shown as "Game Viewport (Whole Window)")

Sends the active game viewport backbuffer: the whole window that shows the game viewport. In a
packaged or standalone game that is the game; in Play In Editor it is the whole editor window, with its toolbars and
panels. Use Game Viewport Only to send just the game while playing in the editor. Internally the plugin hooks the Slate renderer's
"back buffer ready to present" event and queues the capture there, so the copy happens at the
correct point in the frame. Because of this:

- The game window must be actively rendering. A minimized window does not present a backbuffer.
- The backbuffer is captured in its Present state and transitioned for the copy, then restored.

When using GameViewport, leave the Texture Render Target 2D pin empty.

### GameViewportOnly (shown as "Game Viewport Only")

Sends only the rendered game viewport. It uses the same back-buffer event as GameViewport, then copies the game
viewport's rectangle out of the back buffer on the GPU into a texture the plugin owns and publishes that.

- **Included:** everything drawn inside the game viewport: the 3D scene, the HUD (Canvas drawing) and UMG widgets
  added to the viewport or to a player's screen.
- **Editor overlays inside the viewport:** in Play In Editor, the editor draws a few things over the viewport's own
  area, and those are part of the stream because they are inside the rectangle. Measured on UE 5.8: the 1-pixel
  blue focus line along the top edge, shown while the level viewport is the active one. From the engine source,
  not captured in validation: a green border that fades out about 1.5 s after Play starts, and a red-tinted border
  while paused at a Blueprint breakpoint. A packaged or standalone game has none of these.
- **Left out:** everything outside the game viewport's rectangle: in Play In Editor the editor's toolbars, panels,
  tabs and borders; in a packaged game, any part of the window the viewport does not cover.
- **Size:** the viewport's size on screen in physical pixels (display scaling included). Resizing the window or
  the editor layout changes the stream's size, as a resize does for GameViewport.
- **Colour:** the same format and values as GameViewport (display values; a receiving Unreal should use Color
  Space sRGB on a 10-bit back buffer).
- **Only while playing:** outside Play there is no game viewport, so nothing is sent and the Spout Sender node
  returns false. To send the editor's level viewport outside Play, use a Spout Sender actor with Send In Editor
  (above).
- **Resizing:** a new size is logged once it has held for half a second, so dragging a window edge gives one line.

Leave the Texture Render Target 2D pin empty, as for GameViewport.

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

The plugin runs UE on the DirectX 12 RHI but Spout shares DirectX 11 textures. By default (the
shared-slot path) Unreal copies the source texture into a plugin-owned "slot" texture with an
ordinary RHI copy and the render thread waits at most 1 s for that copy; the plugin's native
DirectX 11 device, on the same adapter, then copies the slot into the Spout-owned shared DX11
texture under the stream's access lock. No Unreal texture is wrapped for DirectX 11. With
`r.Spout.SharedSlotInterop 0` the earlier path is used instead: a process-wide D3D11-on-D3D12
interop device wraps the UE D3D12 source texture and copies it into the shared texture. The choice
is made once per process; see [Limitations](LIMITATIONS.md#interop-path). No CPU readback and no
disk or network transfer are involved on the send path.

## Performance notes

- Cost scales with resolution. Larger textures cost more GPU copy time and bandwidth per frame.
- Use a timer instead of Tick when you do not need full frame-rate updates.
- The shared texture and the slot (or, on the D3D11-on-12 path, the D3D11 wrap) are cached and reused while geometry and format are
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
- The GameViewport source sends the back buffer as it is: the values Unreal shows on screen,
  encoded for an sRGB display, in `R10G10B10A2_UNORM` by default, with the alpha Unreal wrote (0
  in a standard scene). Applications that show the values as they are (the Spout Demo Receiver,
  for example) show it as Unreal did. A receiving Unreal must set Color Space to sRGB: on Auto it
  reads 10-bit values as linear and the image looks too bright. A back buffer with HDR output
  enabled holds an encoding that no receiver setting decodes.
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
