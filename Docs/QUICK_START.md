# Quick Start

Three minimal workflows: send the game viewport, send a render target, and receive a stream.
All three assume the plugin is installed, the project runs on DirectX 12, and an external
Spout application (for example TouchDesigner) is available to view or produce a stream.

The plugin ships example Blueprints under its `Content/` folder that you can open instead of
building these by hand: `BP_SpoutSenderExample_`, `BP_SpoutReceiverExample`, `M_SpoutMaterial`,
`M_SpoutDisplayUnlit`, `RT_SpoutSenderTest_`, and `RT_SpoutReceiverTest`. Place both example
Blueprints in a level and press Play: they connect to each other through the name
`UESpout_Test_Example`. To receive another sender with the receiver example, change its Spout Name
in the Details panel.

## 0. No Blueprint: drag-and-drop actors

1. Open Place Actors and choose the **Spout** category (under More when the panel is short, or search "Spout").
2. Drag **Spout Sender** into the level to send the game (Game Viewport Only).
3. Drag **Spout Receiver** into the level to show a sender of another application on a screen in the level (or
   type a Sender Name in its Details panel, or pick one from Choose Running Sender).
4. Press Play.
5. Optional, to stream while you edit the level without Play: set Edit > Project Settings > Plugins > Spout >
   Startup Policy to **Editor and Play**, and turn on **Send In Editor** on the sender or **Receive In Editor** on the
   receiver. In the editor, the sender's Game Viewport Only sends the active level viewport. The level viewport must
   be Realtime (Ctrl+R); otherwise the actors' Status says they are paused.

## 1. Send the game viewport

1. Create a Blueprint Actor, or use the Level Blueprint.
2. On Event Tick (or a timer), add a Spout Sender node.
3. Set Spout Name to a unique stream name, for example `UnrealViewport`.
4. Leave Send Texture From at `GameViewportOnly` (Game Viewport Only, the game without the editor's panels), or
   choose `GameViewport` (Game Viewport (Whole Window)) for the whole window.
5. Leave Texture Render Target 2D empty for this mode.
6. Play. In your external Spout receiver, select the `UnrealViewport` sender.

The viewport path captures the window backbuffer when it is ready to present, so the sender
must be called while the game window is actively rendering and not minimized.

## 2. Send a render target

1. Create a `TextureRenderTarget2D` asset (RGBA8 sRGB is the recommended format), or call the
   Create Texture Render Target 2D node at runtime. That node defaults to Force Linear Gamma =
   true, which stores linear values; the stream's format cannot say so, so either set the
   receiver's Color Space to Linear or turn Force Linear Gamma off. See
   [colour space](SENDER.md#known-constraints).
2. Place a `SceneCapture2D` actor in the level.
3. Assign the render target to the SceneCapture2D Texture Target.
4. Set the SceneCapture2D Capture Source to **Final Color (HDR) in Linear Working Color Space**.
   Qualify other capture sources with known pixels and matching receiver color space.
5. On Tick or a timer, call Spout Sender with Send Texture From set to
   `TextureRenderTarget2D` and Texture Render Target 2D set to your render target.
6. View the stream in the external application.

If the output is black, the most common cause is the Capture Source setting in step 4. See
[SCENECAPTURE2D.md](SCENECAPTURE2D.md).

## 3. Receive a stream

1. On Tick or a timer, call Spout Receiver.
2. Set Spout Name to exactly match the sender's name.
3. Optionally pass a base material as Input Material and a Texture Parameter Name.
4. Read the outputs:
   - Out Texture - a transient `UTexture2D` updated from the stream.
   - Out Mat - a dynamic material instance with the received texture bound to the parameter.
   - Optional Output Render Target - if provided, the received frame is copied into it. Its
     pixel format must match the incoming frame (RGBA16f for a 10-bit sender received with Color
     Space sRGB) and it must be single-sampled; a mismatched target is left untouched with a
     logged warning (size is adjusted automatically).
   - Receiving another Unreal's viewport stream: set Color Space (advanced pin) to sRGB. The
     viewport is sent as 10-bit display values, which Auto reads as linear (too bright).
5. Validate Out Mat and Out Texture before using them. The sender may not be running yet.
6. Apply Out Mat to a mesh, or sample Out Texture directly.

For the material path, the texture parameter name you pass to the node must match a Texture
Parameter inside your material. See [MATERIALS](RECEIVER.md#materials) in the Receiver guide.
