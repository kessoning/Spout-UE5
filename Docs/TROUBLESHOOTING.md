# Troubleshooting

Work top to bottom within each section. The plugin logs to the `LogSpoutPlugin` category, so
`Project/Saved/Logs/ProjectName.log` is the first place to look for any of these.

## Plugin does not compile

- Confirm the plugin is in the project `Plugins/` folder, not the engine plugins folder.
- Confirm you are on a supported engine (5.3 to 5.8) and have Visual Studio 2022 with the C++
  toolchain and a matching Windows SDK.
- Regenerate Visual Studio project files, then rebuild.
- After an engine upgrade, clean first: delete `Project/Binaries`, `Project/Intermediate`,
  `Plugins/SpoutPlugin/Binaries`, and `Plugins/SpoutPlugin/Intermediate`, then regenerate and
  rebuild.

## Build aborts instantly - "Could not find definition for module 'SpoutPlugin'"

Symptom: compiling or packaging fails in well under a second with
`Could not find definition for module 'SpoutPlugin', (referenced via <project>.uproject -> SpoutPlugin.uplugin)`
(packaging also reports `dotnet.exe ExitCode=8`). This is a build-time error, distinct from the
runtime "module could not be loaded" error below.

It means the plugin folder has no `Source/`, so UnrealBuildTool cannot find
`SpoutPlugin.Build.cs`. UBT reads that file on every compile or package, even when it does not
recompile the module, so a binary-only copy cannot be compiled or packaged against - it only
loads in the editor when the prebuilt binary matches your exact engine version.

1. Use a copy that includes `Source/`. Every current release zip ships it (0.0.8 and later); if
   you have an older, source-less download, re-download or clone
   `https://github.com/kessoning/Spout-UE5` into `YourProject/Plugins/SpoutPlugin/`.
2. Confirm `Plugins/SpoutPlugin/Source/SpoutPlugin/SpoutPlugin.Build.cs` exists.
3. Delete `Plugins/SpoutPlugin/Binaries` and `Plugins/SpoutPlugin/Intermediate`, regenerate
   project files, and rebuild.

## Plugin fails to load - "module 'SpoutPlugin' could not be loaded"

This almost always means the compiled binary does not match your engine, or `Spout.dll` is not
beside the module binary.

1. A precompiled download must match your exact engine version. If it does not, rebuild from
   source.
2. Verify `Spout.dll` is at `Plugins/SpoutPlugin/Binaries/Win64/Spout.dll`, beside
   `UnrealEditor-SpoutPlugin.dll`.
3. Read the log. The plugin logs an explicit error or warning at startup if `Spout.dll` is not
   found, naming the paths it checked. Search the log for `SpoutPlugin` and `Spout.dll`.
4. If rebuilding from source, do the clean-rebuild steps from the previous section.

## Spout.dll missing

- For precompiled releases, `Spout.dll` must ship in `Binaries/Win64`. If the startup log warns
  that the DLL is present under `ThirdParty` but missing beside the module binary, rebuild from
  source or copy `Spout.dll` from `ThirdParty/Spout/lib/amd64/` into `Binaries/Win64/`.
- For source builds, the DLL is staged automatically. If it is missing, confirm the build
  completed and that `ThirdParty/Spout/lib/amd64/Spout.dll` exists.

## Works in the editor but not in a packaged build

The packaged game stages files differently from the editor, so check these in order:

1. **Plugin version.** Versions before 0.0.9 only looked for `Spout.dll` in the plugin's own
   folders, which do not exist inside a packaged game; the DLL is actually staged next to the
   game executable, and the plugin silently disabled itself. Update to 0.0.9 or later.
2. **Confirm `Spout.dll` is next to the game executable** in the packaged output
   (`<Package>/<Project>/Binaries/Win64/Spout.dll`). It is staged there automatically by the
   build; if it is missing, the build itself is broken.
3. **Confirm the packaged game runs on DX12.** If the project's default RHI is DX11, the GPU
   does not support DX12, or the game is launched with `-d3d11`, the plugin logs `SpoutPlugin
   requires DX12 RHI.` and disables itself. The editor and the packaged game can end up on
   different RHIs, which makes this the classic "works in editor only" cause.
4. **Use a Development package to read the log.** Shipping builds compile logging out by
   default, so a disabled plugin looks like silent nothing. Package in Development, run, then
   read `Saved/Logs/<Project>.log` and search for `SpoutPlugin`.

## Packaging the project fails

- **Package with Visual Studio:** the supported packaging workflow uses Visual Studio 2022 with
  the C++ workload, its MSVC toolchain and a Windows SDK supported by your engine version. The
  prebuilt editor binaries only remove the need to compile when opening a project in a matching
  editor.
- **Packaged game says module `SpoutPlugin` could not be found:** check whether the plugin is in
  the `Plugins/` folder of a Blueprint-only project. In that setup our UE 5.8 test packaged
  Unreal's prebuilt game executable without the plugin, also with the plugin enabled explicitly in
  the `.uproject`. The verified workaround is to add a C++ class to the project (Tools > New C++
  Class) and package again, so Unreal compiles the plugin from its `Source/` folder into the
  game.
- **Packaging for a non-Windows platform:** update to 0.0.9+, which marks the plugin Win64-only
  (`SupportedTargetPlatforms`) so other platforms skip it instead of failing the cook on its
  example content.

## Sender does not appear in the external application

- Confirm the project is running on the DirectX 12 RHI. The log shows `SpoutPlugin requires
  DX12 RHI.` if it is not, and the plugin will not initialize.
- Confirm the sender is being called continuously (Tick or timer), not once.
- Confirm only one sender is using that name.
- For the GameViewport source, make sure the game window is actively rendering and not
  minimized.
- Look for `SpoutDX11on12 Initialized.` in the log to confirm the interop device came up.

## Spout Demo Receiver shows "No sender detected" for an sRGB render target

Fixed in 1.0.0. Earlier 1.0.0 pre-release builds published 8-bit sRGB render targets (Force
Linear Gamma off), including the shipped sender example, as `B8G8R8A8_UNORM_SRGB`. Spout 2.007's
OpenGL receive path, used by the Spout Demo Receiver 2.007.015 and other SpoutGL applications,
checks the shared texture's real format against a list that does not include that format, so it
refused the sender. The sender now publishes every 8-bit source as plain `_UNORM` with the bytes
unchanged.

If you still see it: confirm you are running the final 1.0.0 archive, then work through
[Sender does not appear in the external application](#sender-does-not-appear-in-the-external-application).
Because the format no longer carries the sRGB tag, see [Wrong colors or gamma](#wrong-colors-or-gamma)
if the picture appears but its colours look off.

## Log says "shared-texture access was unavailable"

Sender: `Spout sender '<name>': shared-texture access was unavailable because a receiver still held
it after waiting up to 2 ms (r.Spout.AccessWaitMs), so this frame was skipped and not published.`
Receiver: `Spout receiver '<name>': shared-texture access was unavailable because a peer (the sender
or another receiver) still held it after waiting up to 2 ms (r.Spout.AccessWaitMs), so this frame
was not read yet.` (the CPU receive path says `... unavailable on the CPU path ...`). The number is
the current setting.

- This is not an error. Spout guards each shared texture with a cross-process access lock. The
  plugin waits at most `r.Spout.AccessWaitMs` (default 2 ms) for another application to release
  it. When the texture is still held after that, the sender skips that frame and publishes the next
  one once the texture is free; the receiver tries again on a later call and copies the sender's
  newest frame then, so a frame the sender replaced in the meantime is lost, not queued.
- The wait applies to each access attempt, not to a frame, and runs on the render thread. A peer
  that keeps the texture locked (a stalled or paused application, for example) costs up to that
  long on every frame for that stream, and several locked streams add up: in testing, a peer that
  locked eight 1080p streams added about 13 ms of waiting per frame. `r.Spout.AccessWaitMs 0` never
  waits and skips more frames when both applications run at the same frame rate. It restores
  zero-wait acquisition only, not earlier builds as a whole; the plugin's other fixes stay active.
- The line is throttled to at most one per stream every 5 seconds, so it does not show how often it
  happens. `Get Spout Stream Status` counts every skip in `AccessMisses`.
- Occasional misses are normal when two applications share a texture. If `AccessMisses` climbs
  steadily and frames are visibly dropped, the peer is holding the texture for a large part of each
  frame: check its load and frame rate, and reduce resolution or update rate on either side.
- A keyed-mutex sender that holds its texture for tens of milliseconds can cause runs of skipped
  frames with or without the wait. See
  [LIMITATIONS.md](LIMITATIONS.md#performance-and-diagnostics).

## Received frames occasionally skip or repeat, with no access misses

- The receiver samples the stream once per Unreal frame and gets the sender's newest frame; Spout
  does not queue frames. When the sender and Unreal run at the same frame rate, their timing
  drifts in and out of alignment. Near alignment, single frames are skipped and the previous frame
  is shown for one extra frame, while `AccessMisses` stays flat. In 60 fps testing this lowered
  delivery to 87 to 89 % of frames in individual 10-second stretches; whole runs stayed near 99 %.
- `r.Spout.AccessWaitMs` does not affect this. Running Unreal at a higher frame rate than the
  sender reduces it, but no frame rate guarantees every frame: scheduling jitter, frame hitches and
  contention can still lose frames.
- If `AccessMisses` rises with the skipped frames, or the picture freezes for several frames, the
  cause is access contention instead; see the section above.

## Receiver returns an invalid texture or material

- The Spout Name must match the sender exactly, including case.
- The Spout Receiver node returns false when the sender is not available - check the return
  value and validate `OutMat` and `OutTexture` before using them.
- For the material path, the Texture Parameter Name must match a Texture Parameter inside the
  material. If left empty, the plugin uses `SpoutTexture`.

## Black texture from SceneCapture2D

- Set the SceneCapture2D Capture Source to Final Color (HDR) in Linear Working Color Space, not
  SceneColor (HDR). See [SCENECAPTURE2D.md](SCENECAPTURE2D.md) for the full checklist.

## Wrong colors or gamma

- The plugin does not color-correct on send. Adjust the render target format and the
  SceneCapture2D Capture Source. The tested combination is Final Color (HDR) in Linear Working
  Color Space with an RGBA8 sRGB render target.
- Try RGBA8 sRGB first; if you see banding, try RGB10A2.
- The shared format does not say whether 8-bit bytes are sRGB or linear: the sender publishes
  plain `_UNORM` for both, so receivers follow the Spout convention that 8-bit bytes are
  gamma-encoded (sRGB). If the source render target uses Force Linear Gamma (the default of
  Create Texture Render Target 2D), its bytes are linear: set the receiving Spout Receiver node's
  Color Space to Linear, or the equivalent setting in the external application, or turn Force
  Linear Gamma off on the source.
- When Unreal receives from another application, Auto is right for ordinary senders; use Linear
  when that sender publishes linear 8-bit data.
- When Unreal receives another Unreal's viewport stream (GameViewport sender) and it looks too
  bright: the viewport is sent as display (sRGB-encoded) values in 10 bits, which Auto reads as
  linear. Set the Spout Receiver node's Color Space to sRGB. An Optional Output Render Target must
  then be RGBA16f.

## Stream name mismatch

- Names are case-sensitive and must match exactly between sender and receiver. Use Spout Info
  to list the exact names currently registered.

## Multiple senders conflict

- Only one sender may own a name at a time. Give each stream a unique, stable name.

## Performance drops

- Large textures and high update rates are expensive, especially on the legacy CPU readback
  receiver path. Leave `r.Spout.GPUReceiver` at its default of `1` (GPU-direct).
- Reduce resolution or update frequency. Prefer a timer over Tick when full frame rate is not
  needed.

## Crashes or instability

- Update GPU drivers.
- Avoid swapping render target resources at runtime while a sender or receiver is active.
- Test different resolutions and formats.
- Reproduce in a minimal project before reporting.

## External app cannot see the stream

- Confirm the external application supports the Spout version and DXGI format in use.
- Confirm both applications are on the same GPU. Cross-GPU sharing is not supported by Spout's
  shared-texture model.

## After an engine upgrade

- Do the clean-rebuild steps: delete the four Binaries/Intermediate folders listed above,
  regenerate project files, and rebuild from scratch. Binaries built for a different engine
  version will not load.
