# SceneCapture2D and Render Target Capture

A `SceneCapture2D` actor is the usual way to produce a render target to send through Spout.
This page covers the capture settings that matter and the most common failure (a black image).

## Setup

1. Add a `SceneCapture2D` actor to the level and frame it on what you want to send.
2. Create a `TextureRenderTarget2D` (RGBA8 sRGB recommended).
3. Assign that render target to the SceneCapture2D Texture Target.
4. Set the Capture Source as described below.
5. Send the same render target with the Spout Sender node using Send Texture From set to
   `TextureRenderTarget2D`.

## Capture Source - the important setting

Set Capture Source to:

- **Final Color (HDR) in Linear Working Color Space**

Do not use:

- **SceneColor (HDR) in RGB** - this produces a black image on the receiving application.

This is the single most common cause of a black Spout output when sending from a SceneCapture2D.

## Why black happens

`SceneColor (HDR)` is not in a state the shared-texture copy path reproduces correctly, so the
external application receives black. Final Color (HDR) in Linear Working Color Space gives a
resolved image that copies through Spout correctly. This is a known limitation rather than a
configuration you can work around with other Capture Source values.

## Black-output checklist

If your sent render target is black, check in this order:

1. Capture Source is Final Color (HDR) in Linear Working Color Space, not SceneColor (HDR).
2. The render target is actually assigned to the SceneCapture2D Texture Target.
3. The SceneCapture2D is capturing (it is enabled and pointed at visible geometry).
4. You are sending the same render target asset you assigned to the capture.
5. The render target format is RGBA8 sRGB (or another tested format).

## Capture frequency

SceneCapture2D can capture every frame, which is expensive. If you do not need full frame-rate
output, reduce how often you capture and how often you call the sender (use a timer), and
consider a smaller render target. See [Performance](LIMITATIONS.md) for general guidance.
