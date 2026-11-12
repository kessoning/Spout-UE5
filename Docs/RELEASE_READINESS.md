# Release readiness

This page ships inside the 1.0.0 release archives. It describes what qualification of a release
consists of and what it does not cover. Do not treat historical releases or successful compilation
as approval of changed source.

## Final release additions

The final 1.0.0 release adds, to the qualified release-candidate code described under "Automated qualification of
the release candidate" below: Game Viewport Only (now the default of the Spout Sender node), the drag-and-drop
Spout Sender and Spout Receiver actors with an editor-only module for the Place Actors panel, and internal
receiver-session bookkeeping the actors use. These additions were checked in proportion to the change, not with
the full qualification below:

- on UE 5.8, in Play In Editor embedded in the level editor: Game Viewport Only (exact rectangle, colours, HUD and
  UMG, resize, Play stop and restart); the actors (placement registration, default settings, discovery and
  reconnection, aspect ratio, output render target, shared ownership with other actors and Blueprint callers,
  shared settings, Play stop and restart); the receiver's session membership and reopen after a Blueprint Close
  Receiver; and a manual review including placement from the Place Actors panel;
- on every declared engine (UE 5.3 through 5.8): BuildPlugin and archive checks; the actors' automation tests
  (placement registration, default settings including Async Receive on, session membership and identity, and the
  ownership rules); and a packaged Development game that loads `Spout.dll` from the package, sends Game Viewport
  Only through a Spout Sender actor and receives an external Spout sender with a Spout Receiver actor (exact
  pixels, disable and enable, a Blueprint Close Receiver and reopen, a refused consumer beside a shared session,
  and a clean exit and restart).

Those runtime checks ran on binaries built from the same code before the release's version and documentation
were finalised. The published archives were then rebuilt from the final source, whose code is the same apart from
comments; they were not run again.

**Engines used for the final archives** (one desktop, NVIDIA GeForce RTX 4090, Windows 11): UE 5.6, 5.7 and 5.8 are
standard installs. UE 5.4 and 5.5 are installs with local changes (two edited engine header lines each and a locally
rebuilt UnrealBuildTool), as for the candidate. UE 5.3 is UE 5.3.2 with one engine header line changed locally (the
AddressSanitizer `__has_feature` guard in `ConcurrentLinearAllocator.h`) and a locally rebuilt UnrealBuildTool,
compiling with MSVC 14.44. The final UE 5.3 archive was not run on an unmodified UE 5.3.2. Its prebuilt binaries
carry Epic's UE 5.3.2 module build ID (27405482), the one an unmodified UE 5.3.2 editor expects. The candidate's UE
5.3 archive, an earlier build, was also checked on an unmodified UE 5.3.2 with MSVC 14.36; that result is not
carried over to the final archive. An unmodified UE 5.3.2 does not compile its own headers with MSVC 14.44
(warning C4668 on `__has_feature`), with or without this plugin.

The candidate results below stand for the code the final release did not change. They are not a qualification of
the additions.

## Automated qualification of the release candidate

For the 1.0.0 release candidate the release pipeline:

- builds the plugin with BuildPlugin for every declared engine (UE 5.3 through 5.8) from the
  release source (a commit, or a recorded base commit plus exact patch) and validates each
  archive's layout, descriptor, `Spout.dll` hash, precompiled data, licensing files and shipped
  document links;
- runs the plugin automation suite, and Development and Shipping packaged hosts with repeated
  launches, on every declared engine from that source;
- installs the exact UE 5.3 and UE 5.8 archives into fresh host projects and repeats automation and
  Development/Shipping packaged checks from them. Those checks load `Spout.dll` from the packaged
  game, run the shipped Blueprint examples and their EndPlay cleanup, restart same-name streams,
  travel between levels, compare pixels in both directions with an independent native Spout
  process, and receive a 10-bit stream with Color Space sRGB, checking every decoded value against
  the exact sRGB function;
- measures a packaged UE 5.8 Development host built from the exact UE 5.8 archive at a 60 Hz work
  rate on one development laptop (NVIDIA GeForce RTX 2060 laptop GPU, Intel Core i7-10750H,
  Windows 11): 1080p and 4K send and receive, four simultaneous 1080p streams, CPU receive
  fallback, an idle receiver, resize churn and the paced close/reopen workload below. A scenario
  passes only when at least 90% of publication opportunities and of the 60 Hz target are observed
  with zero corrupt pixels;
- runs the 30-minute release test (at least 30 minutes) on the same laptop, on a packaged UE 5.8
  Development build made from the exact UE 5.8 archive: a 1080p 60 fps BGRA8 stream in each
  direction, consumed by external Spout processes, judged against acceptance criteria that are fixed
  before the run. The release test's scene does not reach 60 fps on that laptop at its default
  (Epic) quality, even with no Spout activity. The release test therefore runs at the highest
  standard Unreal scalability level at which the scene alone meets the release test's frame-pacing
  limits; that level is chosen by measurement and fixed before the run. The render-target send is
  asynchronous by default, so the release test's send-access criteria judge frames abandoned for
  access, not failed access attempts, and every frame must still be published or counted from the
  send's per-frame outcome log; the original attempt counts are reported beside them. The receive is
  Async Receive by default: for a receive shown by its log to have run on the worker from start to
  end, the receive-contention ratio of failed access attempts to publications is reported, not
  gated, because the worker counts every failed poll; delivery, freeze, recovery and every other
  criterion apply unchanged, and a synchronous receive is judged by the original criteria. The
  continuity limits are: no 10-second window below 90 %, at most 3 frames missed in a row, no
  freeze, and a new frame within 3 frames after a missed one. The release test's result belongs to
  one set of binaries. For 1.0.0 the release test ran once on this release's binaries and met every
  criterion but one: one 10-second window of 183 showed 89.8 % of the sender's frames. Whole-run
  delivery was 98.4 %, at most 2 frames were missed in a row, there was no freeze, and a new frame
  followed every missed one within 3 frames. 1.0.0 ships with that result as a known limitation: the
  run is not counted as a pass, and the result is not established for other workloads or hardware.
  An earlier 1.0.0 build also ran the release test once. Its result belongs to that build only; it
  does not carry over to changed binaries. Both runs' measured results and what they look like on
  screen are in [limitations](LIMITATIONS.md#async-receive-the-default);
- measures the same scene at its default quality with no Spout activity, with one stream sent,
  with one received, and with both. This shows the plugin's cost in a scene that keeps the GPU busy
  (see [limitations](LIMITATIONS.md)). It is a measurement, not a gate. The asynchronous
  render-target send's own extra GPU time was not established on the laptop; 1.0.0 ships with
  that result as a known limitation, not as a measurement that passed.

The measurements include a serialized pixel observer and queue drain. They are not a claim about
other hardware, other scenes, isolated plugin cost or unobserved asynchronous throughput.

## Close and reopen

The qualified close/reopen workload is specific: one 1080p BGRA8 sender and receiver in one packaged
process, both closed and reopened every two seconds through the Blueprint nodes, with the caller
clearing its references at close, at the default `r.Spout.MaxRetainedMiB` and Unreal's default
garbage-collection interval, for 60 and 180 seconds. It is not a general safe restart rate.

Closed receiver outputs stay reserved until Unreal collects them, so budget pressure depends on
garbage-collection timing, resolution, format, concurrent streams and caller-held textures. On an
RTX 4090 development desktop, closing and reopening one 1080p stream twice per second exhausted the
default budget after about 20 seconds, and new receiver outputs were then refused until the next
collection; the same case passed with a 4 GiB diagnostic budget, which is a comparison, not a
changed default. That stress case stays in the pipeline and fails its delivery gate at the default
budget on the laptop too; it is not supported at that budget. A receiver that stays open while its
sender restarts is unaffected. See
[limitations](LIMITATIONS.md) for the retention contract and the options available to an application.

## Not covered by automated qualification

External product interoperability (for example TouchDesigner, Resolume or OBS), GPU vendors other
than NVIDIA, multi-GPU machines, driver resets and device loss, engine-level
(`Engine/Plugins/Marketplace`) installation and manual editor use require target-system
qualification. Success against the native test peer does not establish those claims. A final
manual acceptance test is run on the exact release archives before publication.

The supported packaging workflow uses Visual Studio with the C++ workload and a supported Windows
SDK; packaging without a compiler is not a supported configuration. With the plugin in a
Blueprint-only project's `Plugins/` folder, a UE 5.8 package built during release testing did not
contain the plugin; adding a C++ class to the project is the verified workaround.

The descriptor sets `IsBetaVersion` to false; that metadata does not certify production readiness
for a particular deployment. Qualify the exact archive with your engine build, GPU, driver, external
application and expected long-running workload before deployment. Public behavior is documented in
[limitations](LIMITATIONS.md) and [Blueprint API](BLUEPRINT_API.md).
