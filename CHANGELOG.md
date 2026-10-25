# Changelog

## 1.0.0 - 2026-09-21

First stable release. The plugin leaves beta: the sender and receiver lifecycles, the cross-process
synchronisation protocol and the packaging layout are now covered by automated validation that runs
on every supported engine version, and the documentation matches the implementation.

Supported scope is unchanged: Windows 64-bit, Direct3D 12, Unreal Engine 5.3 through 5.8.

1.0.0 was prepared in August 2026 but not distributed. A production-readiness review followed; its
fixes and the release testing are part of this release and are listed first, followed by the notes
from the August preparation.

### Production-readiness fixes and release testing (September 2026)

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
