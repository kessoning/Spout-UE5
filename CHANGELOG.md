# Changelog

## 1.1.0 - Unreleased

The editor-workflow release: a settings page, streams while the level is edited, a sender list, clearer reasons when
a receive is refused, examples that connect out of the box, and four measured performance changes. Nothing that
existing Blueprints, C++ callers or saved assets rely on was removed or renamed, and the new editor features are off
by default. Supported engines: UE 5.3 to 5.9, Windows 64-bit, Direct3D 12.

### Added
- **Project Settings > Plugins > Spout** (`USpoutSettings`, saved in `DefaultEngine.ini`): Startup Policy (Play only,
  the default, or Editor and Play), Default Sender Name for new Spout Sender actors, Access Wait (ms) and the stream
  and memory limits. The page sets the `r.Spout.*` console variables at project-setting priority; a value set with
  higher priority (an ini file's `[ConsoleVariables]`, the command line, the console, code) keeps it, and the editor
  logs which variable did.
- **Streams in the editor without Play:** Send In Editor (`ASpoutSenderActor::bSendInEditor`) and Receive In Editor
  (`ASpoutReceiverActor::bReceiveInEditor`), off by default, run while the Startup Policy is Editor and Play.
  - In the editor, Game Viewport Only sends the level editor's active viewport and Game Viewport (Whole Window) the
    level editor's window; a render target is sent as during Play.
  - During Play, Play's copy of a sender takes the name over and the editor's sender continues when Play ends (within
    0.1 s at each start and stop in the review); Play's copy of a receiver shares the editor receiver's stream.
    Closing a level closes the streams its actors used.
  - Without a Realtime level viewport Unreal does not update the level in the editor, so the editor streams pause.
    Their Status starts with "Paused: no level viewport is Realtime" and says how to resume (Ctrl+R in the viewport);
    the Output Log says it once.
- **List Spout Senders** (`FSpoutSenderInfo`): name, size, format and bits per channel, source application, whether
  this game publishes it, and whether its texture is on Unreal's graphics card (`ESpoutSenderGpu`, with the other
  GPU's name). **Get Max Spout Senders**, a pure node. The Spout Receiver actor's Choose Running Sender shows each
  sender's size and format and marks senders on another GPU.
- **Get Spout Receiver Session Info** (`FSpoutReceiverSessionInfo`): whether a name's receiver session is open, its
  receive mode and colour space, who uses it, and the outcome of its last receive call (`ESpoutReceiveResult`, now
  public) with a sentence naming the setting to change. Two new outcomes:
  - `StreamQueueFull`: the stream already has `r.Spout.MaxPendingPerStream` receive calls this frame (4 by default),
    so a fifth consumer was refused without a word in 1.0.x. Now also logged, throttled.
  - `OtherGpu`: the sender's texture is on another graphics card. Refused with one warning per sender texture that
    names both GPUs and the Windows setting to change; a sender that starts again is reported again. The receiver
    actor no longer adds a second warning.

### Changed
- **Ending Play In Editor closes only that Play session's streams** (`USpoutSubsystem` records the worlds, in the
  editor only), plus streams whose world cannot be told. Streams another world uses (the editor's, for example an
  editor stream or an editor utility) keep running. When nothing else uses a stream, the end of Play closes it as
  before. Packaged games are unchanged.
- **Example content:** `BP_SpoutReceiverExample` receives `UESpout_Test_Example`, the name `BP_SpoutSenderExample_`
  sends, so the two connect in Play. The name is an instance-editable Spout Name instead of a fixed `Spout`; a placed
  receiver example that received a sender called `Spout` needs its Spout Name set to `Spout`. It shows the stream
  through the unlit `M_SpoutDisplayUnlit`, once a receive is accepted, and keeps the last frame when the sender stops.
  Its output render target is an optional Output Render Target, empty by default: the fixed 8-bit
  `RT_SpoutReceiverTest` could not match a 10-bit stream and logged a warning every 5 seconds. No asset was renamed.
- **Viewport senders log a new size once,** after it has held for half a second (Game Viewport Only, and the level
  viewport in the editor), so dragging a window edge gives one line instead of one per frame.
- **The interop log line:** `Spout interop initialized: <shared-slot|wrap> path, D3D11-on-12 device <not
  needed|created>.` replaces `SpoutDX11on12 Initialized.`
- **`LogSpoutPlugin` is exported,** so the editor module logs under it.

### Performance
Each change was compared with the same build with that change switched off (one `r.Spout.Dev.*` switch per change,
development builds only), in alternating runs of a packaged UE 5.8 Development test game on one desktop (NVIDIA
GeForce RTX 4090), 60 seconds per run, two passes, the machine otherwise idle. Details:
`Docs/LIMITATIONS.md#performance-changes-in-110`.
- **The D3D11-on-12 device is created only for the D3D11-on-12 path** (`r.Spout.SharedSlotInterop 0`, or no native
  device). The default shared-slot path never used it. The first call of a 1080p send took 36.3 and 32.7 ms instead
  of 48.1 ms.
- **A receiver no longer reserves four frames of the retained-memory budget when it opens a sender:** the sender's
  texture is not its own, and the CPU readback path now reserves its staging memory when it creates it. A receiver on
  the default path (GPU copy, Async Receive) reserves six frames instead of ten; with a 4K stream sent and received in
  one game, the budget's peak fell from 464.6 MB to 331.9 MB, while GPU memory stayed the same.
- **Asynchronous senders resolve together:** at the end of a render-command flush, every asynchronous sender's owed
  frames get their attempt at once and the flush waits for all of them together, woken as each one resolves, instead
  of one sender after another checking every millisecond. With four 1080p streams the hook took 0.465 to 0.495 ms
  per frame instead of 1.266 to 1.292 ms.
- **A viewport source's own GPU textures are released when its last sender closes,** instead of being kept until
  Close All Streams: after a whole-window and a Game Viewport Only sender at 1920x1080 closed, the process held
  69.5 MB of dedicated GPU memory above its level before they opened, instead of 86.7 to 88.1 MB.
- Measured and left out of 1.1.0: a whole-window viewport send without the render-thread wait (it lost frames under
  GPU load) and a pause for Async Receive workers nobody reads (a receiver read once a second or less got no new
  image).

### Removed
- **Shipping builds** no longer register the development switches (`r.Spout.Dev.*`) and carry no send outcome log.
- **`r.Spout.AsyncReceive`**, retired since Async Receive became a per-node setting, is removed; an old ini value is
  ignored by Unreal.
- The stubs for non-Windows platforms.

### Spout SDK
- **Spout2 2.007.018:** `Spout.dll`, `Spout.lib` and the SpoutDX headers are the official release (hashes in
  `THIRD_PARTY_NOTICES.md`). The DirectX 9, DirectX 12, OpenGL and C-style header sets, which the plugin never
  included, are no longer bundled.

### Checked (before the release gate)
- **Automation suite** after each group of changes, built with the test marker, on UE 5.3 to 5.9 (one desktop,
  NVIDIA GeForce RTX 4090): 184, 197, 198, 199 and 203 tests, all passed on every engine. New tests: seven for the
  per-world closing (`SpoutPlugin.Worlds`), 13 for the settings page, editor streams, sender list, refusal reasons
  and the other GPU (`SpoutPlugin.Settings`, `.EditorStreams`, `.Senders`, `.Receivers`, `.Gpu`), one for the pause
  without a Realtime viewport, one for the examples connecting with their defaults
  (`SpoutPlugin.P2.ExamplesConnectWithTheirDefaults`), and the performance changes' tests.
- **BuildPlugin** on UE 5.3 to 5.9 with no plugin warnings; the documentation validator.
- **A sender on the desktop's Intel GPU** received by Unreal on the RTX 4090: refused, with one warning naming both
  GPUs, on every engine.
- **The packaged test game** (Development, UE 5.3 and 5.8) after the performance changes.
- **A review of the editor workflow** in the level editor on UE 5.8; its two defects (no word when the viewport was
  not Realtime, a repeated other-GPU warning) were fixed and tested.

### Documentation and repository
- README, release readiness, limitations, Blueprint and C++ references, guides and the user manual describe the
  editor workflow, the settings page, the new nodes, the examples and the measured performance changes.
- `Scripts/ExampleContent`: the editor-only tool that changed the receiver example through Unreal's Blueprint API.
  Not shipped.
- Performance harness: viewport scenarios (whole window under GPU load, viewport texture release) and a load
  recorder; the packaged test game starts its pixel peer on Unreal's graphics card.

## 1.0.1 - 2026-11-16

A maintenance release on top of 1.0.0: one sender fix, UE 5.9 support, quieter logs and lighter builds. Nothing that
existing Blueprints, C++ callers, console variables or saved assets rely on changes. Supported engines: UE 5.3 to 5.9,
Windows 64-bit, Direct3D 12.

### Fixed
- **Two Game Viewport Only senders started at different times.** When one Game Viewport Only name had been sending on
  its own and a second name started, the first name stopped publishing: every frame for it was refused with the
  warning "a viewport send to a name whose render-target sender uses the asynchronous send ... was refused".
  - **Cause:** a name sent alone goes through its own asynchronous sender. Once two names shared the viewport image,
    both went to the synchronous send, which refuses a name that already has an asynchronous sender (one name, one
    source).
  - **Fix:** in a send of several names, a name that already publishes through its asynchronous sender keeps doing
    so; the other names share the synchronous send as before (`SpoutSender.cpp`).
  - **Test:** `SpoutPlugin.AsyncSend.ANameSentAloneKeepsItsSenderWhenASecondNameJoins`. Without the fix it fails on
    UE 5.8 (the first name refused 3 times, 1 frame published instead of 4, its image not received); with the fix it
    passes on UE 5.3 to 5.9.

### Changed
- **Async Receive and 10-bit or float senders.** Async Receive takes 8-bit senders (BGRA8, RGBA8); other formats were
  always received synchronously. That fallback is expected, for example for another Unreal's 10-bit viewport stream,
  so it is now logged as information instead of a warning, with a clearer reason ("it supports 8-bit senders (BGRA8
  and RGBA8), and this sender is DXGI_FORMAT_R10G10B10A2_UNORM"). Real failures (device, resources, worker) are still
  warnings. `GetSpoutStreamStatus` reports the same reason in `ReceiveModeNote`.
- **Spout Receiver actor:** Applied Settings shows the receive path in use (`Async Receive on`, `synchronous receive
  (<reason>)`, `Async Receive off`), not only the one requested.
- **The automation tests no longer compile into users' editors.** The test suite, its test hooks and the
  development console commands (`Spout.T1.*`) are built only when the plugin's root folder holds the marker file
  `SpoutTests.enable`. The test pipeline writes the marker into every plugin it stages; release archives and Fab
  packages are built without it. The runtime module's editor build no longer depends on `UnrealEd`,
  `BlueprintGraph` or `KismetCompiler`.
- **Building the plugin no longer hashes every plugin file.** The build-input digest the test host checks is
  computed only in test builds; other builds record `unidentified`. A file that cannot be read gives `unidentified`
  instead of a failed build.
- **Example content:** the sender example's comments list the three Send Texture From sources and the supported
  render target formats, and no longer describe the old interop path or "just few" formats.

### Added
- **UE 5.9 support** and a precompiled UE 5.9 download. The one compile error on 5.9 was in test code (a printf
  argument type, `SpoutPluginColorDecodeTests.cpp`); it is fixed for every engine.

### Checked
- **Automation suite:** all 177 tests pass on every supported engine, UE 5.3 to 5.9, built from the release source
  with the test marker (one desktop, NVIDIA GeForce RTX 4090). The 17 tests that log a warning provoke it on
  purpose (rejected input, injected faults).
- **The published downloads, installed in the per-engine test projects** (build, launch, Play with a Spout Sender
  and two Spout Receivers, one receiving Unreal's own sender and one an external test sender, rebuild, relaunch, then
  packaged Development and Shipping games run with the same checks):
  - the precompiled ZIPs on UE 5.3 to 5.9 and the Fab source packages on UE 5.3 to 5.8: every check passes. In these
    C++ projects Unreal compiles the plugin from the download's source. UE 5.9 used the 5.8 test project with its
    build settings upgraded for 5.9.
  - the prebuilt editor binaries of every ZIP, in a new Blueprint-only project where nothing can compile: they load
    without compiling, send a render target that a separate application receives (exact size, red as sent), and
    receive a separate application's sender. The binaries are unchanged afterwards. This closes the gap left by 1.0.0,
    whose precompiled binaries were never run.
  - the Async Receive fallback for Unreal's 10-bit viewport stream now appears as an information line in these runs.
- **Not repeated for 1.0.1:** the packaged-host matrix, performance runs and soaks. Their 1.0.0 results stand for the
  code this release did not change (`Docs/RELEASE_READINESS.md`).

### Documentation and repository
- **Spout SDK provenance:** the bundled `Spout.dll` and `Spout.lib` are byte for byte the MD build in the official
  Spout2 2.007.017 release (`Spout-SDK-binaries_2-007-017_1.zip`), and the 36 headers match it apart from line
  endings. Recorded in `THIRD_PARTY_NOTICES.md`.
- **`LICENSE`** is the standard MIT text with both copyright lines, so GitHub recognises it. The fork note moved to
  `THIRD_PARTY_NOTICES.md`.
- `Docs/INSTALLATION.md` explains why the release archives' descriptor has no `EnabledByDefault` entry;
  `Docs/SCREENSHOT_CHECKLIST.md` matches the current layout and defaults; supported-engine ranges read 5.3 to 5.9.
- Build scripts: UE 5.9 joins the declared engines; stale entries for the old root `ThirdParty` folder are removed.

## 1.0.0 - 2026-11-11

First public stable release. It is the release candidate described below (qualified on UE 5.3 to 5.8 in September
and October 2026, not distributed) plus two additions made before publication: the drag-and-drop Spout Sender and
Spout Receiver actors, and the Game Viewport Only sender mode, now the default. Supported engines: UE 5.3 to 5.8,
Windows 64-bit, Direct3D 12. How the additions were checked: `Docs/RELEASE_READINESS.md` ("Final release
additions").

**Upgrading from earlier builds (0.0.x or test builds):** Spout Sender nodes left on Game Viewport switch to Game
Viewport Only when their Blueprint loads (details below); receive nodes whose Async Receive box was left unchecked
turn it on (see the release candidate notes).

### Fixed after publication: building with older Windows SDKs (2026-11-14)
- **Fab's UE 5.3 build failed** in the engine's `d3dx12_core.h` (`'D3D12_RESOURCE_DESC1': base class undefined`,
  `'Format': undeclared identifier`, `D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS` undeclared).
- **Cause:** `SpoutD3DContext.h` included the Windows SDK's `d3d11on12.h` before `d3d12.h`.
  - `d3d11on12.h` includes the `d3d12.h` from its own SDK folder. That copy's include guard then hides the engine's
    newer `d3d12.h` (`ThirdParty/Windows/DirectX/include`), which the engine's `d3dx12.h` requires.
  - With a Windows SDK older than 10.0.19041, the SDK's `d3d12.h` lacks types `d3dx12.h` uses, and the build fails.
  - It shows only in unity builds, where a file using `SpoutD3DContext.h` is followed by one that includes the engine's
    D3D12 headers. Newer SDKs compiled it anyway, so local builds passed.
- **Fix:** `SpoutD3DContext.h` includes `<d3d12.h>` before `<d3d11on12.h>`. No behaviour change.
- **Checked:**
  - reproduced with the UE 5.3 package's own compiler command lines and the Windows SDK 10.0.18362 headers: 98 errors,
    the same as Fab's report;
  - with the fix, every unit compiles with those headers, in the unity build and file by file;
  - the regenerated Fab packages pass `RunUAT BuildPlugin -Rocket` on UE 5.3 to 5.8, and the rebuilt precompiled ZIPs
    were built the same way, with no errors and no warnings from plugin code.
- **Scope:** every 1.0.0 download was rebuilt with this fix: the Fab source packages and the six precompiled ZIPs
  (GitHub release `v1.0.0` and Patreon), which replace the earlier files of the same names. Tag `v1.0.0` now points to
  the commit with this entry. The rebuilt packages also carry the README and notices updates made after the release.
  ZIPs downloaded earlier keep working: their binaries were not affected. Only compiling their sources with a Windows
  SDK older than 10.0.19041 can hit the error.

### Added: the user manual (PDF) in every archive
- **`Docs/Manual/SpoutPlugin_User_Manual.pdf`** now ships inside every Patreon and Fab archive. It covers:
  - installation, the drag-and-drop actors, sending and receiving, render targets and colour, packaging, limitations
    and troubleshooting;
  - the complete Blueprint API reference;
  - a new C++ API reference (`Docs/CPP_API.md`): module setup, every function and both actors from C++, and examples.
- **It is generated from the documentation pages themselves.** `Docs/build_user_manual.py` renders
  `Docs/USER_MANUAL_SOURCE.md` and every page it includes, so the PDF cannot contradict them.
- **It replaces the earlier `Docs/2026-06-05-SpoutPlugin-User-Manual.pdf`.** That manual predated the actors and Game
  Viewport Only and was not shipped.
- **Blueprint API reference fixes:**
  - the summary lists all 13 nodes; it missed four;
  - the access-miss description states the bounded access wait (`r.Spout.AccessWaitMs`) instead of "does not wait".

### Fixed in the additions before publication (UE 5.3 to 5.7)
- **Older engines could not compile the new code:** `FCoreDelegates::GetOnPostEngineInit` (UE 5.8 only),
  `FPlacementCategoryInfo::ShortDisplayName` (UE 5.5+) and the command-list `CreateTexture` (UE 5.4+) are now used
  behind engine-version checks, with the older API on older engines.
- **The receiver's display material and its default texture were saved by UE 5.8**, so UE 5.3 to 5.7 could not load
  them (no display material; UE 5.7's cook failed). Both are now saved by UE 5.3, the oldest supported engine, like the
  example assets.
- **Game Viewport Only crashed a UE 5.3 game** (D3D12 assertion `RequiresResourceStateTracking` on the RHI thread): its
  copy target was a shader-resource-only texture, which UE 5.3 does not state-track. It is now render-targetable, like
  the render-target sources the send path is built for.
- **Tests (shipped source, compiled in a C++ project's editor build):** four file-local test helpers were renamed so that
  no unity-build grouping can put two same-named helpers in one translation unit (a UE 5.3 project build failed with an
  ambiguous call). No behaviour change.

### Added: drag-and-drop Spout Sender and Spout Receiver actors
- **`ASpoutSenderActor` ("Spout Sender") and `ASpoutReceiverActor` ("Spout Receiver")**, in a new **Spout** category
  of the Place Actors panel (new editor-only module `SpoutPluginEditor`). They send and receive during Play with no
  Blueprint, through the existing `SpoutSender` / `CloseSender` and `SpoutReceiverWithMaterial` path; settings in
  the Details panel (category Spout) map to the Blueprint inputs. The Blueprint API and the example assets are
  unchanged.
- Sender: Game Viewport Only by default; a unique name per placement; render-target mode never falls back to another
  source; it does not start on a name another producer already publishes; Publishing reflects published frames.
- Receiver: built-in unlit screen (`M_SpoutDisplayUnlit`, new; default texture `T_SpoutDisplayDefault`) sized to the
  stream's aspect ratio; empty Sender Name connects to a sender of another application and keeps it through
  disconnections; States Waiting / Request Accepted / Receiving / Refused; output-target format checked before use.
- **Internal, no API change:** receiver sessions record actor consumers (leases), whether a Blueprint/C++ receive was
  accepted, and who uses each output target. A departing actor releases only its own material binding and
  output-target use, and closes the session only when nobody else uses it. `CloseReceiver` is unchanged. Each session
  has an identity, so a receiver actor recognises a session reopened after a Blueprint `CloseReceiver` (its frame count
  restarts) and decides shared-versus-own settings from its actual membership of the current session.
- **Docs:** README drag-and-drop section; the shipped link to the unshipped `Docs/SENDER.md` now points to the Blueprint
  reference; QUICK_START no longer tells users to pick `GameViewport` for the game alone.

### Added: Game Viewport Only sender mode
- **New `ESpoutSendTextureFrom::GameViewportOnly` ("Game Viewport Only").** Sends only the rendered game viewport.
  On each presentation the game viewport's rectangle (the `SViewport` geometry relative to its window, in physical
  pixels, resolved on the game thread when the send is queued) is copied on the GPU out of the window's back buffer
  into a plugin-owned texture of the same format, which is published through the viewport sender's shared slot. In
  Play In Editor the editor's toolbars and panels are therefore not sent; the scene, the HUD (Canvas) and UMG widgets
  on the viewport are. Editor overlays drawn inside the viewport's area are included (the 1-pixel focus line on the
  top edge, measured; the Play-start and breakpoint borders, from the engine source). Outside Play there is no game
  viewport and the call returns `false`. Resizes recreate the copy target; the size is logged once per change.
- **Changed default: Send Texture From now defaults to Game Viewport Only** (maintainer decision). New Spout Sender
  nodes start on Game Viewport Only. **Behaviour change for existing projects:** when an existing Spout Sender node
  is reconstructed (for example when its Blueprint is loaded in the editor), Unreal compares the pin's saved value
  with its autogenerated default; it does not track whether the user selected that value explicitly. Existing
  unconnected pins holding the old default, Game Viewport, therefore take the new default, Game Viewport Only,
  even if the user explicitly selected Game Viewport. Existing Texture Render Target 2D selections and connected
  inputs remain intact. In Play In Editor a switched node sends only the game viewport. In a packaged game whose
  viewport fills the window the picture is expected to be the same, with one extra GPU copy (packaged games were
  not validated for this mode). **To retain whole-window capture, select Game Viewport (Whole Window) after
  upgrading and save the Blueprint.** C++: `SpoutSender`'s `SendTextureFrom` defaults to `GameViewportOnly` and `TextureRenderTarget2D` to
  `nullptr`; existing calls pass both and are unaffected.
- **Unchanged:** `GameViewport` (0) and `TextureRenderTarget2D` (1) keep their values and behaviour; the new value is
  appended (2), so the numeric values are unchanged (Blueprint pins left on `GameViewport` migrate as described
  above). `GameViewport`'s display name is now
  "Game Viewport (Whole Window)", and every value has a tooltip.
- **Checked:** UE 5.8 in embedded Play In Editor, D3D12, 125 % display scaling (exact rectangle and size, HUD
  colours and position, UMG presence, resize during play, Play stop and restart, the two existing modes); UE 5.3 to 5.8
  in a packaged Development game where the game viewport fills the window (received externally at the logged
  rectangle). Not checked: New Editor Window, multi-player Play In Editor, Shipping, performance.

## 1.0.0 release candidate (prepared 2026-09-21; not distributed)

The release candidate of 1.0.0. The plugin leaves beta: the sender and receiver lifecycles, the cross-process
synchronisation protocol and the packaging layout are now covered by automated validation that runs
on every supported engine version, and the documentation matches the implementation.

Supported scope is unchanged: Windows 64-bit, Direct3D 12, Unreal Engine 5.3 through 5.8.

1.0.0 was prepared in August 2026 but not distributed. A production-readiness review followed; its
fixes and the release testing are part of this release and are listed first, followed by the notes
from the August preparation.

### Production-readiness fixes and release testing (September 2026)

#### Viewport colour: Color Space sRGB decodes 10-bit streams (end of October 2026)
- **Fixed: an Unreal viewport stream received by this plugin looked too bright, whatever the Color
  Space setting.** The viewport sender publishes the back buffer as it is: in Unreal Engine 5's default
  configuration `R10G10B10A2_UNORM` holding display (sRGB-encoded) values, with the alpha Unreal wrote.
  The receiver read every 10-bit frame as linear and refused `sRGB` on 10-bit, because Direct3D has no
  sRGB view of a 10-bit format. Found by the Spout Demo Receiver check; applications that show the
  values as they are, as that one does, were already correct.
- **`ESpoutColorSpace::SRGB` is now honoured on 10-bit frames.** Each frame is decoded by one
  pixel-shader pass into a linear 16-bit float received texture (`PF_FloatRGBA`, sRGB flag off): the
  exact IEC 61966-2-1 function per colour channel, computed in 32-bit float; alpha copied unchanged
  (the 2-bit alpha, 0 included, is never forced opaque); one texel per pixel (`Load`: no filtering,
  scaling or flip). 16-bit float keeps the 10-bit code's precision.
- **Unchanged:** Auto and Linear (10-bit and float frames are read as linear, which is right for
  10-bit and float render targets), the 8-bit behaviour, `sRGB` refused on 16-bit float frames, and
  everything the sender publishes. Spout 2.007 carries no colour information, so Auto cannot tell a
  10-bit viewport stream from a 10-bit render target and does not guess from values or names. The
  documentation no longer says that 10-bit frames are linear by nature, and states the colour
  contract (`Docs/LIMITATIONS.md`, Formats and color). HDR back buffers (PQ, scRGB) have no decode.
- **Receive paths.** Shared-slot (the default synchronous path): the decode pass replaces the
  slot-to-texture copy; the slot is created shader-readable for decode sessions only. Legacy wrap and
  CPU readback: the frame is written into a per-receiver 10-bit intermediate texture and decoded in
  the same render command. Async Receive handles 8-bit senders only, so a decode session runs
  synchronously, as 10-bit sessions already did, and the status says why.
- **Outputs.** The texture and the material carry the decoded values. An optional output render
  target must have the received texture's format, so RGBA16f for a decoded session; a target of
  another format is left untouched and logged, as before.
- **One interpretation per session.** Auto and Linear on a 10-bit sender are the same
  interpretation; `sRGB` is a different one, and a conflicting call is refused (false, outputs
  untouched, logged) until Close Receiver.
- **New runtime module `SpoutPluginShaders`** (Win64, loaded at `PostConfigInit`): maps
  `/Plugin/SpoutPlugin` to the plugin's new `Shaders` folder and holds the decode. The release
  archives ship `Shaders/`; `build.ps1` requires it.
- **Tests.** `SpoutPlugin.ColorSpace.ExplicitOverridesAreHonouredOrRejected` asserted the defect
  ("RGB10A2 rejects an sRGB override") and now asserts the decode. Six new tests
  (`SpoutPlugin.ColorDecode.*`) cover every 10-bit code and all four alpha codes against the exact
  decode, the texture, material and output render target of both receive functions, an unlit
  material drawn into an 8-bit sRGB render target (the source's display values within 1), sessions
  and mode changes, resize, sender restart and receiver reopen, the GPU, CPU readback and legacy wrap
  paths, Async fallback, and the 8-bit and float controls. The packaged host test adds the
  `SPOUT_COLOR_DECODE_PASS` step; the development command `Spout.T1.Viewport sRGB` checks a real
  10-bit back buffer; `Spout.Dev.MeasureColorDecode` measures the pass.
- **Measured cost** (RTX 2060 laptop, D3D12, GPU timestamp queries, medians of 7 rounds): the decode
  pass took 40.5/50.5/201.9 µs (UE 5.8) and 40.8/50.4/203.6 µs (UE 5.3) at 1248x702/1920x1080/3840x2160,
  against 40.3/55.7/212.7 µs and 42.2/61.4/219.0 µs for the 10-bit copy it replaces on the shared-slot
  path. On the legacy wrap and CPU paths it follows the copy, so it adds its own time. No frame of
  delay is added. Memory: the decoded texture is 8 bytes per pixel (4 for the 10-bit texture), plus a
  4-byte-per-pixel intermediate on the legacy wrap and CPU paths. No display latency was measured.
- **Release testing:** an earlier 1.0.0 build's 30-minute release test result belongs to that
  build only; it does not carry over to changed binaries (`Docs/RELEASE_READINESS.md`). The
  packaged host tests include the decode. These binaries were tested again from their exact
  archives:
  - the whole automation suite passed, 167 of 167 on each engine;
  - the packaged hosts passed in Development and Shipping on both engines;
  - receive modes, negative delivery, the 60 Hz matrix and the close/reopen workload all passed;
  - in a cross-process viewport check between two instances of the packaged UE 5.8 host, the 10-bit
    viewport received with `sRGB` was within 0.5 of an 8-bit step of the sending window's on-screen
    values. That result is from the second attempt, with a sample-alignment correction made before
    that attempt ran; the first attempt failed one 8-bit control by 2 steps.

  The 30-minute release test ran once on these binaries and met every criterion but one: one
  10-second window of 183 showed 89.8 %. Whole-run delivery was 98.4 %, at most 2 frames were
  missed in a row, the picture never froze, and every missed frame recovered within 3 frames. The
  61 missed frames in that window were single skips attributed to frame-timing alignment, none to
  the sender's lock. 1.0.0 ships with that result as a known limitation of these binaries and the
  tested configuration; the test stays recorded as failed, and no threshold changed
  (`Docs/RELEASE_READINESS.md`; `Docs/LIMITATIONS.md`, Async Receive).
- The shipped `Docs/LIMITATIONS.md` and `Docs/RELEASE_READINESS.md` were then updated to state that
  result; no binary, source or build input changed.

#### Release testing on an RTX 2060 laptop (October 2026)
- `Docs/RELEASE_READINESS.md` names an RTX 2060 laptop as the performance machine. The 60 Hz
  scenario matrix runs there with its 90 % delivery and zero-corruption gates.
- The 30-minute release test's scene does not reach 60 fps on that laptop at its default (Epic)
  quality, even with no Spout activity. The test runs at the highest standard scalability level at
  which the scene alone meets the test's frame-pacing limits; that level is measured and fixed
  before the run. The test's numeric limits are unchanged.
- The same scene is also measured at its default quality with no Spout activity, sending, receiving
  and both, as a measurement, not a gate.
- `Docs/LIMITATIONS.md` adds two measured limits from that laptop:
  - in a GPU-bound scene, a 1080p 60 fps send lowered about 40 fps to about 35, and a receive to about
    39.5 (the send's wait for its copy is inferred, not timed);
  - a sender that keeps its texture locked while its own GPU work completes made the receiver skip
    4.7 to 6.7 % of its frames in that scene.
- Measurements in the documentation that were taken on an RTX 4090 desktop are labelled as such.
  No code changed.

#### Asynchronous render-target send (late October 2026, the default)
- Render-target senders on the default shared-slot path publish from a worker thread:
  - the render thread no longer waits for the copy or for Spout's access;
  - frames are announced in order; frames superseded or abandoned for access are skipped and counted,
    never queued;
  - flush, close and stop give owed frames one access attempt, and a frame a receiver still blocks
    then is abandoned (every accepted frame is published or explicitly skipped);
  - one more frame of memory per render-target sender;
  - a viewport send to a name owned by a render-target sender is refused with a throttled warning.
- The viewport sender is unchanged.
- `r.Spout.Dev.AsyncSend` (default now 1) is a development switch, not a supported setting: `0` brings
  back the synchronous render-target send for diagnosis.
- Its extra GPU time per frame was not established on the test laptop (one comparison measured
  1.09 ms, two were inconclusive); 1.0.0 ships with that result as a known limitation. See
  `Docs/LIMITATIONS.md` (Performance and diagnostics).

#### Shared-slot interop (October 2026, the default)
- The sender and the synchronous receiver no longer wrap Unreal's textures for Direct3D 11. The plugin
  creates its own "slot" textures on Unreal's Direct3D 12 device in a shared heap, opens them on a
  separate native Direct3D 11 device on the same adapter, and orders the two devices with fences:
  a receive copies the Spout texture into a slot on the native device and Unreal copies the slot into
  its destination; a send copies the source into a slot on Unreal's queue and the native device copies
  the slot into the Spout texture under the stream's access lock. Pixel bytes, formats, alpha and
  the colour policy are unchanged.
- Every wait is bounded on the CPU: the render thread waits at most 100 ms for a receive's native
  copy and at most 1 s for Unreal's copy into a send slot; a copy that does not finish in time is not
  published. No queue waits on a fence value that was not already observed complete. Objects whose
  GPU work may still run are released only once fences prove it finished.
- Where a fence cannot prove it (a failed native signal, or Unreal's device removed), an event query
  on the native device does, and it is polled with a `GetData` that may flush. A query issued with
  no other work in its batch was measured to report only after about 3 s on an NVIDIA RTX 2060 when
  polled without flushing, which delayed those rare recovery paths and failed an automation test's
  2 s bound twice on UE 5.8.
- `r.Spout.SharedSlotInterop` (default 1) selects the path once per process, at the first Spout use.
  `0` restores the D3D11-on-12 path of earlier 1.0.0 builds as a fallback; there is no live
  switching and no automatic fallback. The log names the path the process latched. The earlier
  entries below that describe D3D11-on-12 wrapping describe that fallback path.
- With the default path, keyed-mutex textures are acquired by the plugin's native Direct3D 11 device
  rather than through the D3D11-on-12 device that shares Unreal's graphics queue.
- See `Docs/LIMITATIONS.md` (Interop path).

#### Async Receive becomes the default (end of October 2026)
- **Async Receive is the default receive mode.** The `bAsyncReceive` input of `SpoutReceiver` and
  `SpoutReceiverWithMaterial` now defaults to true, and so does `FSpoutReceiver::Receive` for C++
  callers that leave it out. Its display name is "Async Receive" (it was "Async Receive (Beta)").
  The synchronous receive stays available per receiver: set the input to false.
- **The receiver example:** its checkbox is "Async Receive" under Spout (it was "Async Receive (Beta)"
  under Spout > Experimental), on by default, with a new tooltip. The variable keeps its name,
  `AsyncReceiveBeta`, so values saved on placed examples stay attached. The asset was changed with
  Unreal's editor API and saved by UE 5.3.
- **Existing Blueprints:** when a graph loads, Unreal rebuilds its nodes. A receive node's input left
  at its earlier default (false) takes the new default (true); a checked input and a connected input
  are kept; a node saved before the input existed gets it at true. Unreal records no difference
  between a box left unchecked and one unchecked on purpose, so a box unchecked in a graph saved with
  a pre-release build of this setting turns on. Unchecking it again saves it as a choice. Placed
  examples that never changed the setting follow the new class default. The plugin does not modify
  assets.
- **`r.Spout.GPUReceiver 0` still selects the CPU readback path:** a session that asked for Async
  Receive steps aside while it is 0 (no worker; `ReceiveModeNote` says why) and at 1 starts its
  worker again, or reports again why it cannot (an earlier refusal is looked at again). Before, a session that asked for Async Receive used the worker whatever it held.
- `ESpoutReceiveMode::AsyncBeta` keeps its name, so saved Blueprint values and comparisons stay
  valid; it is displayed as "Async Receive". Log lines and status notes say "Async Receive".
- Retention, run limits and synchronous retry variants evaluated during development were not
  adopted; their development switches (`r.Spout.Dev.Recv.*`) were removed before this release and
  were never in a published build. The synchronous receive is unchanged.
- Measured against the synchronous receive, and the limits of those measurements: Docs/LIMITATIONS.md
  (Async Receive). Every time there is a software observation, not a display measurement.

#### The 30-minute release test with Async Receive (end of October 2026)
- The 30-minute release test ran once with the defaults (Async Receive and the asynchronous
  render-target send) on the RTX 2060 laptop, against the plugin's native test sender,
  which holds Spout's access lock through its own GPU work. Whole-run delivery (97.1 %, limit 97 %)
  and frame pacing passed. Five continuity checks failed:
  - three 10-second windows at 89.5 to 89.8 % (limit 90 % unless exempt);
  - 5 frames missed in a row in the first second (limit 3);
  - the picture froze once for 5 Unreal frames, about 83 ms (limit: never);
  - up to 4 Unreal frames without a copy after an access miss, and 4 to recover (limit 3 each).
- From the run's own records: a timing and contention limitation (the sender's back-to-back holds,
  and Unreal's frame timing staying aligned with the sender's); no plugin defect was established.
  That build was accepted with this result as a known limitation, to keep the low-latency receive.
  Its result belongs to that build only; it does not carry over to changed binaries. The test
  stays recorded as failed, no threshold changed, and it is separate from the known limitation on
  the send's GPU cost.
- `Docs/LIMITATIONS.md` (Async Receive) gives the measured result and what it looks like on screen,
  `Docs/RELEASE_READINESS.md` records it, and `README.md` points to both. No code, asset or binary
  changed.

#### Async Receive (Beta) (October 2026, off by default, not qualified)
- Added an opt-in **Async Receive (Beta)** setting per receiver: a `bAsyncReceive` input (default
  false, advanced pin, display name "Async Receive (Beta)") on `SpoutReceiver` and
  `SpoutReceiverWithMaterial`, and an instance-editable checkbox of the same name under
  Spout > Experimental on `BP_SpoutReceiverExample`, wired to that input and saved with the actor.
  Existing graphs, assets and C++ callers stay synchronous; an older graph gets the new pin set to
  false when it is opened.
- The receive mode belongs to the receiver session of a sender name. The first accepted receive
  for a name decides it until Close Receiver; a later call for that name asking for the other mode
  is refused (false, outputs untouched, a throttled warning) and leaves the session and its other
  consumers unchanged. Different names are independent. There is no live switching: changing the
  mode means closing the receiver and receiving again.
- `GetSpoutStreamStatus` reports `RequestedReceiveMode`, `EffectiveReceiveMode` and
  `ReceiveModeNote` (new `ESpoutReceiveMode` enum), so a session that asked for the asynchronous
  path but runs synchronously (a sender format other than BGRA8 or RGBA8, a device or resource that
  could not be created, a worker failure) says so. The fallback keeps delivering frames.
  `FramesSubmitted` and `bHasFrame` now also count the frames the asynchronous path delivers (they
  stayed 0 on that path before).
- `r.Spout.AsyncReceive`, the internal process-wide switch of the prototype, is retired: it is
  still registered, logs a warning when set to anything but 0, and no longer selects the path of any
  receiver. Measurement harnesses that set it must pass the per-receiver input instead.
- Performance is a trade-off, not an improvement: one measured pair under GPU load on one laptop
  showed a much shorter render-thread receive call and complete delivery, but a worse frame-time
  p99 and an older image. See Docs/LIMITATIONS.md.
- Correction (late October 2026): the worker no longer gives the sender's access lock back before
  its copy is proven finished. Before, it released the lock after 100 ms without proof, and at once
  after a failed fence signal, while the native copy might still read the sender's texture. Now the
  lock is held until the copy's fence is reached, the event query issued after a failed signal
  reports done, or the worker's device is removed; the 100 ms bound only classifies a copy as late
  (discarded, never published, and counted toward the stream's stop after repeated late copies).
  A failed signal still ends the stream and turns the asynchronous path off for the process, once
  the query has proven the copy. Destroying a stream joins its worker, which may wait for a copy in
  progress (a warning every 5 s), and hands its objects to the retirement list with the fence values
  and query that prove their work, instead of releasing them directly. Close Receiver, Close All
  Streams and shutdown can therefore wait as long as a stalled GPU copy, as on the synchronous path.
  New automation tests cover a late copy, a failed signal, and a close and a session stop with a
  copy outstanding.
- Delivery fix (end of October 2026): at equal frame rates the worker's copy of a frame could complete
  just after Unreal's receive call and the next copy just before the following call, so the earlier
  frame was replaced unread and never shown (94 % of the missing frames in a per-frame diagnostic). A
  receive call that finds no new image while the worker's copy of one is in progress now waits for
  that copy and takes it: at most 1 ms per render frame, shared by every Async Receive stream and
  consumer, on a high-resolution timer. The stream's end-of-stream log line counts the waits, those
  that delivered an image, and their total time. On the RTX 2060 laptop against the plugin's native
  test sender (300 s runs), distinct-frame delivery was 97.3 to 99.8 % at 59.94, 60 and 60.06 fps,
  against 96.6 % before at 59.94 fps, with 8 to 289 ms of waiting per 300 s; frames near that
  alignment can still be lost. Async Receive stays opt-in and off by default.
- Render-thread stall fix (end of October 2026): the receive call's check that a stream's frame
  sync is open no longer takes the frame sync's lock. The Async Receive worker holds that lock while
  it waits for the sender's access (up to `r.Spout.AccessWaitMs`), so the render thread could wait
  there before the receive call proper, unbounded by the 1 ms in-flight limit. The check's answer
  was advisory at every caller before as well (everything that needs an open stream checks again
  under the lock), so it now reads the flag directly; senders and synchronous receivers use the same
  check. On the RTX 2060 laptop (180 s runs at 59.94 and 60.06 fps), render-thread waits above 1 ms
  before the call fell from 1 to 15 per run (one of 17 ms) to 1, and with four receive streams from
  79 to 1; delivery and latency did not change measurably. Internal; no setting changed.

#### Experimental receive settings (October 2026, off by default, not yet qualified)
- Added four experimental console variables for the receive path: `r.Spout.FrameCountRecheck`,
  `r.Spout.FrameWaitMs`, `r.Spout.FrameWaitBudgetMs` and `r.Spout.FrameWaitPollUs`. All are off by
  default. With `r.Spout.FrameWaitMs` and `r.Spout.FrameCountRecheck` at 0 none of the added
  operations runs: a receive call reads the sender's frame notification once, copies when a frame is
  pending, and clears the pending frame only when the copy completed. A baseline run with the
  settings at their defaults recorded no read under access, no wait and no copy without a
  notification. No matched comparison with the previous build was made, so this is not a claim of
  identical performance.
- With the recheck on, the notification is read again while texture access is held, so a frame
  published between the read and the access is not reported new again after its image was already
  copied; and a receive call that found nothing new still copies when the sender published since
  the previous call. With the wait on, a receive call that found nothing new may wait up to the
  configured bound, on the render thread and before any access is taken, for a publication its
  history predicts.
- The wait of one receive call and the wait budget of one render frame are each limited to 1 ms
  (`SpoutLimits::MaxFrameWaitMs`), in every build configuration. A larger value of
  `r.Spout.FrameWaitMs` or `r.Spout.FrameWaitBudgetMs` is read as 1 ms. This limit was added after
  the baseline run, which used 1 ms settings on the commit before it; the limit is
  checked statically and its build and tests in Unreal are still to be run.
- Only Spout's public frame-count calls are used (`GetNewFrame`, `GetSenderFrame`,
  `IsFrameCountEnabled`). The count is a notification and never an image identity. Every completed
  copy remains a delivery; a pending frame survives a refused access, a failed copy, a failed
  completion wait and a retry, and is dropped only by closing the stream.
- The GPU and CPU receive paths now share one check stage. The pending frame is cleared at one
  completion point per path: on the GPU path after the copy's GPU completion while access is still
  held, on the CPU path after access was released and the map and the upload succeeded.
- Measurement instrumentation is compiled only into measurement builds (a marker file that no
  release contains); a release build contains none of it.
- New engine-free native tests (`Tests/Native/SpoutNotificationTests.cpp`) drive the notification
  state, the publication predictor and the bundled SDK's public calls without Unreal, including
  beside another receiver reading the same sender.

#### Documentation (UE 5.8 testing, September 2026)
- Corrected the 0.0.9 claim that the kept `Intermediate/Build` payload lets Blueprint-only users
  package a project without Visual Studio. The supported packaging workflow uses Visual Studio
  with the C++ workload and a supported Windows SDK; packaging without a compiler is not
  supported. Installing the prebuilt plugin and opening a project in a matching editor needs no
  compiler.
- Documented the tested UE 5.8 packaging behaviour instead of general rules. With the plugin in a
  Blueprint-only project's `Plugins/` folder (dropped in, or enabled explicitly in the
  `.uproject`), Unreal Build Tool builds no game target, so the package uses the engine's prebuilt
  game executable without the plugin and the game reports that module `SpoutPlugin` could not be
  found. Adding a C++ class is the verified workaround. The tested project-folder installs
  compiled the plugin from `Source/` and did not reuse the archive's precompiled objects. The
  earlier wording that project plugins are "always recompiled" and that Unreal "only builds"
  project plugins into C++ projects was replaced by these observations.
- The installation guide no longer says the release descriptor sets `EnabledByDefault`; the
  descriptor in the release archives does not, so an engine-level install must be enabled per
  project.
- Documented frame sampling (from long-run test diagnostics). A receiver copies at most
  once per Unreal frame and gets the sender's newest frame; Spout does not queue frames. With the
  sender and Unreal both at 60 fps on independent clocks, the two drift in and out of alignment, and
  near alignment single frames are skipped and the previous one is shown again, with no access
  contention involved. In 1080p 60 fps testing, individual 10-second windows fell to 87 to 89 %
  distinct-frame delivery while whole runs delivered 98 to 99 %. The limitations distinguish this
  from access starvation, state that `r.Spout.AccessWaitMs` does not change it, and state that a
  higher Unreal frame rate reduces it without guaranteeing every frame.
- Documented that a keyed-mutex sender holding its texture for long periods can cause runs of
  skipped frames with or without the access wait (tested with one 1080p 60 fps keyed sender holding
  its texture 20 ms on every tenth frame: 84 % delivery, up to 14 consecutive frames skipped), that
  a receiver retrying after an access miss copies the sender's newest frame (a replaced frame is
  lost, not queued), and the measured cumulative cost of the access wait: a peer that locked eight
  of ten 1080p streams added about 13 ms of render-thread waiting per 16.7 ms frame period.

#### Fixed
- Interoperability (found in UE 5.8 testing): the sender no longer publishes
  `_UNORM_SRGB`. Spout 2.007's OpenGL receive path, used by the Spout Demo Receiver 2.007.015 and
  other SpoutGL applications, checks the shared texture's real format against an allow-list that
  has no `B8G8R8A8_UNORM_SRGB` (91), so those receivers refused the shipped sender example and every
  8-bit sRGB render target ("No sender detected"). Every supported 8-bit source (BGRA8 and RGBA8,
  including sRGB render targets with Force Linear Gamma off, typeless sources and `_UNORM_SRGB`
  sources) is now shared as `B8G8R8A8_UNORM` (87) or `R8G8B8A8_UNORM` (28), with the pixel bytes
  and alpha copied unchanged and no colour conversion. This supersedes the fix below that
  advertised 8-bit sender colour intent from the effective RHI flags. `_UNORM_SRGB` (91/29) is
  still accepted on receive from peers that publish it; RGB10A2 and RGBA16F are unchanged.
  Consequence: preserving the encoded bytes does not preserve the sRGB format tag. An sRGB render
  target and a Force Linear Gamma render target both publish `_UNORM`, so external consumers can no
  longer infer the source's colour intent from the format. By the Spout convention 8-bit `_UNORM`
  bytes are gamma-encoded (sRGB), which is what an sRGB render target produces and what receivers on
  `Auto` expect; a Force Linear Gamma render target stores linear values and needs
  `Color Space = Linear` on its receiver. Unreal-to-Unreal results are unchanged, because `Auto`
  already decoded 8-bit `_UNORM` as sRGB. Automation now checks BGRA8 and RGBA8 sources, linear and
  sRGB, for the `_UNORM` share and unchanged bytes under every receiver colour-space mode; the
  native pixel peer requires the shared texture's real format as well as the advertised one; the
  packaged example check keeps the shipped sender target's authored gamma. The documentation states
  the lost format tag as a limitation.
- Shared-texture access (found in UE 5.8 long-run testing): each named or keyed
  access attempt now waits up to `r.Spout.AccessWaitMs` for a peer that holds the texture, instead
  of only trying once. Default 2 ms, clamped to 0 to 10, read per attempt on the render thread;
  `0` restores zero-wait acquisition only, not earlier builds as a whole: the keyed release below and
  the other fixes in this release stay active. The wait ends as soon as the peer releases the
  texture. With a zero wait, a receiver whose once-per-frame read kept landing inside the sender's
  short lock window skipped frame after frame whenever the two frame clocks lined up: in 60 fps
  long-run test diagnostics against an external sender that froze the received image for up to 32
  consecutive frames (about half a second) and delivered 5 % of frames in the worst second. With
  the 2 ms wait the same conditions produced no run longer than 2 frames. This changes the
  documented contract: access attempts are no longer nonblocking. The bound applies to each
  attempt, not to a frame, so several busy streams can each add up to it in one frame, and Windows
  scheduling can stretch a timed-out wait (2 to 3 ms at Unreal's 1 ms timer resolution, up to about
  16 ms at the default Windows resolution). For keyed-mutex textures the wait polls zero-timeout
  acquires about every 50 microseconds until the bound, instead of passing the timeout to
  `AcquireSync` (a timed keyed acquire on the plugin's D3D11-on-12 device delivered fewer frames in
  testing). The receive paths now also submit Unreal's keyed-mutex release as soon as access is
  given back; it used to wait for the device's next flush, which kept a keyed sender waiting on the
  GPU. With a keyed sender and no deliberate holds that took skipped transfers from 82 to 0 per
  minute; in paired runs that alternated the setting, the 2 ms wait delivered 93 % of distinct keyed
  frames against 85 % with no wait. Ownership, release, timeout, abandoned-mutex handling,
  cancellation and shutdown are unchanged; `Get Spout Stream Status` no longer takes the stream's
  frame-sync lock, so the game thread never waits behind a render-thread access wait. New
  automation covers the setting's default, clamping and invalid values, per-attempt use, the
  status query during a wait, and a real named mutex (timeout, release during the wait,
  abandonment); the native tests cover named and keyed contention across processes, a clamped
  `INFINITE` request and a peer crash during a wait.
- Diagnostics: the shared-texture access warnings no longer say the plugin "timed out waiting" on
  an unspecified wait. The sender reports that access was unavailable because a receiver still held
  the texture after waiting up to the configured `r.Spout.AccessWaitMs`, so the frame was skipped
  and the next one is published once the texture is free; the receiver reports that a peer (the
  sender or another receiver) still held it after that wait, so the frame was not read yet and the
  receiver tries again on its next call, which copies the sender's newest frame (the CPU path says
  the same). Throttling is unchanged: at most
  one line per stream every 5 seconds.
- Render targets: `Create Texture Render Target 2D` no longer loses a draw or clear issued in the
  frame it creates the target. It ended with a deferred `UpdateResource()` that re-created the
  resource with a pending clear, which ran at the next scene render and wiped the caller's first
  draw (found by the UE 5.8 interop matrix: targets cleared on creation published black).
  The factory now sets its addressing before creating the resource and finishes with an immediate
  update, as Unreal's own `Create Render Target 2D` does. A new target still starts opaque black.
  The packaged host smoke run checks that a same-frame clear survives.
- Restart performance: recycle closed Spout SDK frame-count backends instead of constructing
  one per stream incarnation. Profiling showed that construction cost about 14-15 ms twice per
  close/reopen (receiver entry on the game thread, sender entry on the render thread) and was nearly
  the whole restart stall; shared-texture creation, name registration, wrapping and the received
  texture each cost well under 1 ms. The pool is bounded, drained before the DLL unloads, and covered
  by restart automation tests. Trace scopes now mark every native send/receive/create/close step.
  Ownership, GPU completion, peer access and retention accounting are unchanged; closed outputs still
  stay reserved until Unreal garbage collection, which the retention budget documents.
- Release qualification: the packaged check of the shipped receiver example treated an optional
  render target that had just been resized and was still at its clear colour as corrupted pixels.
  That happens when the resizing call's transfer is skipped because the sender holds the access
  mutex. It failed one launch in about 180 and was reproduced every time under forced contention.
  Such a target now counts as not yet delivered; every other mismatch still fails and records the
  pixels read, and sustained delivery is still required. The receiver documentation now states
  that a resized target shows its clear colour until the next successful transfer.
- Technical debt: record Unreal editor module build IDs from `UnrealEditor.modules`, preserve full
  engine version/changelist metadata and hash the source files; report metadata parse failures.
- Share engine metadata resolution across release tools and reject engine-path overrides whose
  installed major/minor version differs from the requested build before launching Unreal.
- Independently validate performance duration, target delivery, counter consistency, end-of-run
  progress and exact invocation IDs instead of relying on a host-reported pass. Add offline regression
  coverage for malformed and misleading results; preserve validation of archived measurements.
- Retire closed stream generations, defer failed receiver discovery allocation, bound active roles
  and queued work, and account for caller-retained receiver images and resize overlap.
- Use exact ownership results for keyed texture access; preserve the key-zero Spout protocol. (This
  entry originally made keyed access nonblocking; access now waits up to `r.Spout.AccessWaitMs`,
  see above.)
- Enforce public game-thread affinity, keep final shutdown stopped, latch unsupported RHI and back off
  native connect/registration failures. Add public stream/runtime status and reservation diagnostics.
- Forward engine-path lists once, require PowerShell 7.2+, bound process termination/log draining and
  preserve native log bytes. Ship an explicit public documentation subset and validate archive-local links.
- Qualify exact archive installations and shipped example assets in consumer hosts; add restart-phase
  timings and regression checks for discovery churn, cancelled backlog, retained outputs and live contention.
- Repair the shipped sender's missing render-target input and the receiver's missing material input.
  Both examples now close their stream on EndPlay; the receiver creates and reuses its own MID.
- Cache completed receiver frames and deliver them independently to optional targets, including
  paused senders, late attachment and resource recreation.
- Restore only material bindings still owned by a receiver; preserve cross-name and manual rebinding.
- Advertise 8-bit sender color intent from effective RHI flags and reject conflicting color requests
  for an existing same-format receiver session. Log CPU map failures while retaining a pending retry.
- Require actual measured pixel progress in performance gates; isolate evidence per invocation and
  bind launches and artifacts to original source/build identities. Reject stale, duplicate or mixed evidence.
- Persist failed smoke/performance reports and propagate nonzero process exits on older Unreal launch loops.
- Register exact names under the SDK's shared registry lock, with explicit collision/capacity
  refusal; the bundled SDK binary is unchanged.
  Registry inspection uses bounded name slots, with no STL container ownership crossing the DLL.
- Release Windows mutex ownership on abandoned access, discard that frame, and allow recovery.
  Protect the exact-name registry transaction against the SDK's analogous memory-map lock bug.
  Failed synchronization setup now prevents copying and retries on later calls.
- Resolve sender/receiver RHI resources on the render thread and keep native source replacement
  paired with its destination snapshot. Serialize sender installation with cancellation.
- Validate opened native texture dimensions, format, mip count, array size and sample layout
  before copying. Reject incompatible optional targets before resizing them.
- Use writable, state-tracked backing resources for received `UTexture2D` objects, including
  after caller resource recreation. Initialize new outputs transparently.
- Order Unreal's native submission before D3D11On12 access and wait for GPU copy completion
  before shared-texture handoff. Force native copy-state barriers on UE 5.3-5.5, where ordinary
  RHI transitions defer them until an engine draw/copy. UE 5.3 uses an additional GPU-idle wait because it lacks the
  newer public submission barrier. These waits can increase render-thread frame time.
- Log interop/GPU completion failures with throttling; retry transient shared-handle failures.

#### Added
- `SpoutReceiverWithMaterial` takes an explicit reusable MID input; `ReleaseReceiverMaterialBinding`
  releases one parameter without closing the shared receiver. Existing signatures remain available.
- Compiled Blueprint reuse/GC, paused fan-out, CPU retry and color metadata regression tests.
- Packaged external-process pixel qualification and paced delivery/latency measurements with idle
  and independent no-plugin baselines. The twice-per-second restart-churn scenario now runs at the
  60 Hz work rate but still fails its delivery gate at the default retention budget, by contract;
  the failed result is retained rather than bypassed.
- A paced close/reopen performance scenario (`restartpaced`): one 1080p sender and receiver closed
  and reopened every two seconds through the Blueprint nodes. It is the qualified close/reopen
  workload at the default retention budget under those stated conditions, not a general safe
  restart rate. The retention contract, the default budget and the `startupshutdown` stress
  scenario are unchanged.
- Immutable artifact identity sidecars and evidence-isolation regression checks.
- Native SDK regression tools for exact-name ownership and abandoned mutex recovery.
- Real-pixel Unreal tests on GPU and CPU receive paths, with an independent native process,
  delayed initialization, resizing, resource replacement, cancellation and rejection cases.
- A standalone Windows runtime probe for the successful-wrap ID 916 debug diagnostic.
  The focused pixel tests recognize only that exact diagnostic; production logging is unfiltered.

### Highlights
- **Game viewport senders are validated end to end.** A packaged game now proves, on every release
  build, that a viewport sender is published through the real present loop: it appears in Spout's
  shared sender registry, its shared texture can be opened by an independent Direct3D 11 device, and
  its cross-process frame count advances across successive presented frames.
- **Resizing a window while sending the viewport no longer crashes the game.** Cached interop
  wrappers of the swap chain's back buffers kept those buffers alive, which made the engine's resize
  fail fatally. The wrappers are now released before the swap chain resizes (and, on Unreal 5.8,
  never held across frames), so resolution changes, window resizes and fullscreen transitions are
  safe while a viewport sender is running.
- **Unreal 5.3 back-buffer callback synchronisation corrected.** On 5.3 the engine's back-buffer
  callback list is not internally synchronised, so subscribing and unsubscribing is now performed on
  the thread that invokes it. Unreal 5.4 and newer are unaffected and keep their existing path.

### Added
- Frame-count protocol support, so receivers can tell a newly published frame from a repeated one
  and skip redundant copies.
- Cross-process texture-access synchronisation around every shared-texture read and write, with
  balanced acquire and release on all paths including failures and timeouts.
- Colour-space control on the receiver: `Auto` follows the format convention (8-bit decodes as sRGB,
  10-bit and float stay linear), with explicit `Linear` and `sRGB` options. An unsupported request,
  such as forcing sRGB on a float frame, is refused and logged rather than silently applied.
- A shipped `README.md` inside every release archive, so an unzipped plugin is self-describing.

### Changed
- Sender registration and same-name restart are now deterministic: closing a stream cancels work
  already queued for it, so a close followed immediately by a reopen of the same name cannot be
  overtaken by the frame in flight.
- Receiver-owned textures and materials are tracked explicitly and released in a defined order
  during teardown, so they cannot be collected while the render thread still holds their resources.
- Shutdown follows one fixed ordered sequence, and a stream left publishing when the process exits
  is torn down without taking the process with it.
- Streams started in Play-In-Editor are closed when that session ends, instead of lingering on their
  last frame until the editor exits.
- Unsupported pixel formats are refused with a warning naming the format. Frames are never
  reinterpreted as a different layout.
- A user-supplied output render target must match the incoming frame's pixel format exactly and be
  single-sampled; a mismatched target is left untouched and the mismatch is logged, rather than
  producing a silently black target.
- `Set Max Senders` returns the limit actually in effect afterwards, and returns 0 when nothing was
  changed, rather than echoing the requested value.

### Fixed
- Publishing the game viewport to several Spout names now costs a single GPU round trip in total
  rather than one per name.
- Unreal 5.8 compatibility for the changed back-buffer callback signature.
- Documentation corrected against the implementation: the format-refusal behaviour, the receiver's
  full parameter list including colour space, the `Set Max Senders` contract, the output render
  target requirements, where `Spout.dll` is staged for each install layout, and the removal of a
  dependency the plugin no longer uses.

### Release validation
- Every release archive is reopened and checked before it ships: plugin descriptor version, source
  tree, module binary, `Spout.dll` verified byte for byte against the repository copy, example
  content, plugin icon, licence and third-party notices, with internal material and debug symbols
  rejected.
- Validation stages now fail on a non-zero editor exit code and refuse to report success for a run
  that executed no test rows, so a report cannot pass on absent evidence.
- The documentation drift checker covers version 1.0.0 and later, scans the published website
  sources, and enforces each corrected behaviour contract.

## 0.0.9 - 2026-07-27

### Fixed
- **Spout was silently disabled in packaged games.** In a packaged (monolithic) build the plugin module links into the game executable, so the `RuntimeDependencies` target `$(BinaryOutputDir)` stages `Spout.dll` next to the game executable (`<Project>/Binaries/Win64/`) — the plugin's own `Binaries/Win64` and `ThirdParty` folders are not staged at all. The startup loader only checked those two plugin-relative paths, concluded the DLL was missing, and the `IsSpoutDllLoaded()` guard added in 0.0.7 then disabled Spout entirely (every node returned `false`) even though the DLL had shipped correctly beside the executable. `LoadSpoutRuntime` now also checks `<Project>/Binaries/Win64/Spout.dll` and the executable directory, and falls back to resolving `Spout.dll` through the OS search order as a last resort. Editor and precompiled-zip layouts are unchanged (plugin-relative paths are still checked first).
- **Sender now recreates its shared texture when the source format changes at the same resolution.** The shared-texture "unchanged" check compared only width/height, so switching a render target's pixel format (for example RGBA8 to FloatRGBA) at the same size kept the old-format shared texture and fed `CopyResource` mismatched formats, which D3D11 silently drops — receivers kept getting the last old-format frame. Format is now part of the recreate identity (`FSpoutSharedSender::SharedFormat`), matching the equivalent receiver-side fix shipped in 0.0.6.
- Sender restores non-viewport sources to `SRVMask` after the copy instead of `SRVGraphics`, mirroring the state it transitioned from and keeping UE's RHI state tracking consistent if the texture is later read by a compute shader.
- Legacy CPU receiver path migrated from the global `RHIUpdateTexture2D` to `FRHICommandList::UpdateTexture2D`. The global form is deprecated in UE 5.8 with an explicit "will no longer compile in the next release" warning, so this heads off a guaranteed compile/packaging break on the next engine version. The command-list form exists since UE 5.0, so all supported versions (5.3-5.8) still build.
- Receiver now reports a pixel-format mismatch on the optional output render target instead of failing silently. A user-supplied render target whose format differs from the sender's could never update (the GPU path's `CopyResource` between mismatched formats is dropped by D3D; the CPU path writes bytes the destination reinterprets), with nothing in the log to explain the black or frozen target. The warning names both formats and is logged when the pairing changes, not every frame. The render target is left untouched — silently reformatting a user's asset at runtime would be worse than the symptom.
- Viewport sender no longer reads `GEngine->GameViewport` from the render thread. The back-buffer delegate compared the presenting window against the game viewport's current window, dereferencing game-thread-owned state off-thread; the window identity is now resolved on the game thread in `SpoutSender` and compared as an opaque pointer (never dereferenced) under the existing lock.
- Senders and receivers started in PIE are now closed when the PIE world tears down, so a sender stops publishing when the user presses Stop instead of lingering (frozen on its last frame) until the editor exits. Editor-only and scoped to `EWorldType::PIE`, so a packaged game's level transitions do not disturb active streams. The D3D interop device is deliberately kept alive across PIE runs.

### Performance
- Viewport senders now share one GPU round trip. Publishing the game viewport to N Spout names previously ran N full sequences of transition, RHI flush, wrap, acquire, copy, release and flush; all N now share a single transition, a single flush and a single acquire scope, so N senders cost roughly what one used to.
- The viewport back buffer is no longer re-wrapped every frame. The wrap cache was a single entry keyed on the native D3D12 resource, but a swap chain rotates through several back buffers, so the cache missed on nearly every frame and paid a `CreateWrappedResource` device call each time. A small bounded cache (`FSpoutWrapCache`) now covers the whole rotation. The render-target path keeps its existing per-sender cache, which was already stable.

### Removed
- Dead sender-side gamma pipeline (about 430 lines): `FSpoutGammaPipeline`, its runtime-compiled HLSL, `EnsureGammaResources`, `SanitizeGamma`, and the `GammaTexture`/`GammaRTV` registry fields. Nothing ever called any of it. Also drops the `d3dcompiler.lib` system-library dependency and the `<d3dcompiler.h>` include, since runtime shader compilation was its only user. The code remains in git history if a future gamma feature wants it.

### Changed
- Added `"SupportedTargetPlatforms": [ "Win64" ]` to the plugin descriptor. Packaging a project for a non-Windows platform no longer cooks the plugin's example content, whose Blueprint nodes reference the Win64-only `SpoutPlugin` module and could fail the cook.
- `build.ps1` no longer deletes `Intermediate/` from release zips. `RunUAT BuildPlugin` deliberately emits precompiled `UnrealGame` Development/Shipping objects, `.precompiled` manifests, and generated headers there; installed under `Engine/Plugins/Marketplace/`, these let Blueprint-only users package a project without Visual Studio. Stripping them made no-compiler packaging impossible. (Plugins installed in a project's `Plugins/` folder always compile from source when packaging — that path still requires Visual Studio, which is an Unreal Build Tool rule, not a plugin choice.)

No Blueprint API changes; no exported-symbol removals or breaking changes.

## 0.0.8 - 2026-07-08

### Fixed
- Precompiled/free release zips could not be compiled or packaged against: `RunUAT` aborted almost immediately (~0.4s) with `Could not find definition for module 'SpoutPlugin', (referenced via <project>.uproject -> SpoutPlugin.uplugin)`, and packaging failed the same way (`dotnet.exe ExitCode=8`). The release build script (`build.ps1`) stripped the entire `Source/` folder from the Patreon and Root zips, removing `SpoutPlugin.Build.cs`. Such a binary-only plugin loads in the editor only when the prebuilt binary matches the user's exact engine version, but UnrealBuildTool reads every enabled module's `.Build.cs` during the module-discovery pass of any compile or package, so with `Source/` absent it could not resolve the `SpoutPlugin` module. `build.ps1` no longer removes `Source/`; all release variants now ship the source tree alongside the prebuilt `Binaries/`, so Blueprint-only users with a matching engine still skip compilation while C++/packaging (and engine-mismatch rebuild) users can build from the included source. This matches the layout already documented in `Docs/PACKAGING_AND_DISTRIBUTION.md`.

No Blueprint API changes; no code changes; no exported-symbol removals or breaking changes.

## 0.0.7 - 2026-07-04

### Fixed
- Editor/game crash (`Unhandled Exception: 0xC06D007E`) the first time a Spout node runs — most visible on `Spout Sender` in Event Tick — in packaged/precompiled releases. Two independent defects both contributed:
  - **`Spout.dll` was never shipped.** `RunUAT BuildPlugin` does not carry the `RuntimeDependencies`-declared DLL into its `-Package` output, so release zips contained only `UnrealEditor-SpoutPlugin.dll` and no `Spout.dll` at all. The release build script now stages `Spout.dll` into the packaged `Binaries/Win64` explicitly and aborts if it is ever absent, so releases can no longer ship without it.
  - **Even when present, the DLL was resolved too late.** `Spout.dll` is delay-loaded, so its imports were bound lazily on the first Spout call using the host executable's DLL search order, which never includes the plugin's own `Binaries/Win64`; the delay-load helper then raised `ERROR_MOD_NOT_FOUND` (`0xC06D007E`). The module now explicitly loads `Spout.dll` by full path at startup (`FPlatformProcess::GetDllHandle`, preferring `Binaries/Win64` and falling back to `ThirdParty/Spout/lib/amd64`), so the delay-loaded imports bind by name on first use. The handle is released in `ShutdownModule`.
- A genuinely missing `Spout.dll` no longer crashes: `FSpoutD3DContext::Initialize` now bails before constructing the Spout helpers when the DLL is not loaded, leaving Spout unavailable (nodes return `false`) with a clear log message instead of taking down the editor.

No Blueprint API changes; no exported-symbol removals or breaking changes.

## 0.0.6 - 2026-06-09

### Added
- `CloseReceiver` Blueprint node — tears down a receiver started with `SpoutReceiver`: drops its registry entry (releasing the opened shared texture and cached D3D11-on-12 wraps), destroys the transient texture, and clears the dynamic material instance. Pairs with `CloseSender` to bound long-running source-switching workflows that would otherwise grow one receiver entry per sender name.

### Fixed
- Receiver `UTexture2D` is now created with an explicit, format-driven `SRGB` flag: 8-bit color formats are sampled as sRGB (unchanged, matching UE's texture default), while 10-bit and float formats are sampled linearly. Removes reliance on the implicit engine default for color-space handling.
- Transient receiver texture now also recreates on a pixel-format or sRGB change at the same resolution (previously only a resolution change triggered a recreate), preventing silent garbled/dropped output when a sender reformats mid-stream.
- `CreateTextureRenderTarget2D` no longer permanently roots the render target it returns — every call previously leaked one render target for the process lifetime. The returned object is kept alive by the Blueprint reference, matching `UKismetRenderingLibrary` convention.
- Game-thread/render-thread data race in the receiver: the shared-texture "unchanged" check and the texture/handle swap are now performed atomically under `ResourceMutex`.
- `FSpoutD3DContext::Shutdown` now takes `InitMutex`, honoring its documented contract and preventing an initialize from racing teardown.
- Sender no longer leaves the source texture stuck in `CopySrc` when the sender lookup or resource wrap fails — the access-state restore now runs on every path.

### Changed
- Sender's heavyweight RHI flush (`FlushRHIThreadFlushResources`) moved outside the D3D11 immediate-context lock, matching the receiver path and keeping the per-frame stall out of the lock.
- Receiver GPU-direct path with both a transient texture and an optional output render target now issues a single GPU flush per frame instead of one per destination — `FScopedD3D11On12Acquire` acquires, releases, and flushes multiple wrapped resources in one scope.

Adds one Blueprint node (`CloseReceiver`); no exported-symbol removals or breaking changes.

## 0.0.5 - 2026-05-21

### Fixed
- Release packaging for `Spout.dll` so precompiled plugin builds can load correctly (addresses `Plugin 'SpoutPlugin' failed to load because module 'SpoutPlugin' could not be loaded`).

### Added
- Delay-loading for `Spout.dll` through `PublicDelayLoadDLLs`.
- Startup diagnostics that log a clear error/warning when `Spout.dll` is missing from `Binaries/Win64` or `ThirdParty`.
- Troubleshooting steps for "module could not be loaded" errors in the README, plus a precompiled-release packaging checklist.

### Changed
- Replaced fragile manual DLL copy logic (`GetUProjectPath`/`CopyToProjectBinaries`) with Unreal's `RuntimeDependencies` staging. The DLL is staged to `$(BinaryOutputDir)` (the plugin's own `Binaries/Win64`) so it lands in the same place for normal project builds, `RunUAT BuildPlugin` packaging, and precompiled release zips, and matches where the startup diagnostics look.
- Restricted the plugin module to `Win64` via `PlatformAllowList` in the plugin descriptor.

## 0.0.4 — 2026-04-30

### Added
- GPU-direct receiver path via D3D11-on-12 wrapping. Replaces the staging-texture readback with a single `CopyResource` straight into the UE RHI destination — no `Map`/`Unmap`, no per-frame CPU memcpy.
- CVar `r.Spout.GPUReceiver` (default `1`). `1` = GPU-direct, `0` = legacy CPU readback.

### Changed
- Sender/receiver registry keyed by `(FName, ESpoutType)`. A sender and a receiver sharing a name no longer corrupt each other; `FSpoutSender::Close` is scoped to senders only.
- Optional render-target destination on the receiver now matches the primary destination bit-for-bit (was producing wrong-colored output on format mismatch).
- Legacy receiver path no longer allocates a per-frame `TArray<uint8>` — mapped pointer goes straight to `RHIUpdateTexture2D`.
- Spout ThirdParty headers moved to `PrivateIncludePaths` so they no longer leak into consumer modules.
- MID outer now `GetTransientPackage()` instead of `nullptr`.
- Deprecated `GetTexture2DRHI()` replaced with `GetTextureRHI()`.
- Sender RHI `FromState` now driven by `bIsViewport` (`Present` for viewport backbuffers, `SRVMask` for render targets).
- `InitMutex` on `FSpoutD3DContext` marked `mutable` so `IsInitialized() const` can take the lock.

### Fixed
- Compile error from mistyped field name (`bUseRenderTarget` → `bIsViewport`).
- Strict-aliasing UB in the `IUnknown**` argument to `D3D11On12CreateDevice`.
- Theoretical `uint32` overflow in `RowPitch * Height` for very large textures.
- Packaging build failure on Game targets — added missing `#include "UObject/Package.h"` for `GetTransientPackage()`.

No public BP / exported-symbol changes.

## 0.0.2
- Minor bug fixes.
- Added support for HDR capture when using SceneCapture2D. The preferred setup for best final quality is now Final Color (HDR in Linear Working Color Space) as the Capture Source, paired with an RGB8 sRGB Render Target.

## 0.0.1
- First release
