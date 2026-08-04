# Quick Start

Three minimal workflows: send the game viewport, send a render target, and receive a stream.
All three assume the plugin is installed, the project runs on DirectX 12, and an external
Spout application (for example TouchDesigner) is available to view or produce a stream.

The plugin ships example Blueprints under its `Content/` folder that you can open instead of
building these by hand: `BP_SpoutSenderExample_`, `BP_SpoutReceiverExample`, `M_SpoutMaterial`,
`RT_SpoutSenderTest_`, and `RT_SpoutReceiverTest`.

## 1. Send the game viewport

1. Create a Blueprint Actor, or use the Level Blueprint.
2. On Event Tick (or a timer), add a Spout Sender node.
3. Set Spout Name to a unique stream name, for example `UnrealViewport`.
4. Set Send Texture From to `GameViewport`.
5. Leave Texture Render Target 2D empty for this mode.
6. Play. In your external Spout receiver, select the `UnrealViewport` sender.

The viewport path captures the window backbuffer when it is ready to present, so the sender
must be called while the game window is actively rendering and not minimized.

## 2. Send a render target

1. Create a `TextureRenderTarget2D` asset (RGBA8 sRGB is the recommended format), or call the
   Create Texture Render Target 2D node at runtime.
2. Place a `SceneCapture2D` actor in the level.
3. Assign the render target to the SceneCapture2D Texture Target.
4. Set the SceneCapture2D Capture Source to **Final Color (HDR) in Linear Working Color Space**.
   Do not use `SceneColor (HDR)` - it produces a black image on the receiver.
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
     pixel format must match the incoming frame and it must be single-sampled; a mismatched
     target is left untouched with a logged warning (size is adjusted automatically).
5. Validate Out Mat and Out Texture before using them. The sender may not be running yet.
6. Apply Out Mat to a mesh, or sample Out Texture directly.

For the material path, the texture parameter name you pass to the node must match a Texture
Parameter inside your material. See [MATERIALS](RECEIVER.md#materials) in the Receiver guide.
