# Limitations

This document describes the implemented deployment contract. A successful API request means
accepted work, not proof that a foreign application displayed a frame.

## Platform and RHI

- Windows x64 with Unreal's D3D12 RHI. Other RHIs disable interop and produce one capability warning.
- Both applications must use the same GPU adapter. Cross-adapter, remote-machine and network
  sharing are outside this plugin's contract. Multi-GPU selection is controlled by the applications.
- Device removal requires an application restart. The plugin does not rebuild Unreal's RHI or
  promise automatic recovery from driver resets. Native errors and device reasons appear in logs.
- Supported source/API range is UE 5.3 through 5.8. Match the plugin binary to the exact engine
  build; a declared range is not evidence that every release archive has passed qualification.

## Interop path

Spout shares Direct3D 11 textures; Unreal renders with Direct3D 12. The plugin has two ways to move
pixels between them, chosen once per process:

| Setting | Default | Meaning |
|---|---|---|
| r.Spout.SharedSlotInterop | 1 | 1: the shared-slot path. 0: the D3D11-on-12 path of earlier releases. Read once, when the process first uses Spout; a later change applies at the next process start |

- **Shared-slot path (default).** The plugin does not wrap any of Unreal's textures for Direct3D 11.
  It creates its own "slot" textures on Unreal's Direct3D 12 device, opens them on a separate native
  Direct3D 11 device on the same adapter, and orders the two devices with fences. A receive copies
  the sender's texture into a slot on the native device and Unreal copies the slot into its
  destination; the render thread waits at most 100 ms for the native copy. A send copies the source
  into a slot on Unreal's queue, waits at most 1 s for that copy, and the native device copies the
  slot into the Spout texture under the stream's access lock. A copy that does not finish in time is
  not published, and a stream whose slot objects fail stops transferring and logs why. There is no
  live switching and no automatic fallback to the other path.
- **D3D11-on-12 path (`r.Spout.SharedSlotInterop 0`).** Unreal's textures are wrapped for Direct3D 11
  through a D3D11-on-12 device that shares the engine's graphics queue, as in earlier releases. It is
  kept as a fallback. The release qualification of this version runs the default path; qualify the
  fallback in your own workload before relying on it.
- The log names the path the process latched: `Spout interop path latched for this process: ...`.
  The D3D11-on-12 device is still created at startup on both paths.

## Formats and color

- GPU and CPU receiver paths accept BGRA8, RGBA8, RGB10A2 and RGBA16F, including supported
  typeless aliases. Sources must be single-mip, single-sample 2D textures. Other layouts are refused.
- No tone mapping, exposure correction, gamut conversion or pixel gamma conversion is performed.
  The sender publishes every 8-bit source as plain `_UNORM` (`B8G8R8A8_UNORM` or `R8G8B8A8_UNORM`)
  with its bytes and alpha unchanged, and never publishes `_UNORM_SRGB`, which Spout 2.007's
  OpenGL receivers (the Spout Demo Receiver 2.007.015 among them) refuse. RGB10A2 and RGBA16F are
  linear. Receiver Auto follows the format policy, `_UNORM_SRGB` from other peers is still
  accepted on receive, and explicit sRGB is refused for 10-bit/float formats.
- The sRGB format tag is not published. Keeping the bytes unchanged does not keep the tag: an sRGB
  render target (Force Linear Gamma off) and a Force Linear Gamma render target both publish
  `_UNORM`, so external consumers cannot infer the source's colour intent from the format.
  `_UNORM` does not mean the bytes are linear. The bytes plus the configuration on both ends decide
  it: by the Spout convention 8-bit `_UNORM` bytes are gamma-encoded (sRGB), which is what an sRGB
  render target produces and what receivers on Auto expect; a Force Linear Gamma target stores
  linear values, and its receiver must be set to Color Space = Linear (or the receiving
  application's equivalent).
- Consumers of one open receiver name share one color interpretation. Close that receiver before
  changing it. Consumers requiring different interpretation must convert downstream themselves.
- SceneCapture output depends on Capture Source, exposure, post-processing, alpha and render-target
  format. A blanket assertion that SceneColor HDR always produces black is not a code guarantee.
  Qualify the chosen capture pipeline with known pixels and the receiving application.

## Calls, identity and ownership

- All public functions require the game thread, including queries, factories and cleanup.
- Names are process-global and keyed by sender/receiver role. One process cannot publish two
  independent streams under the same sender name. Failed discovery does not open a receiver.
- Calls enqueue one transfer request; call on Tick or a timer for continuous transfer. Registration,
  the last frame, receiver ownership and native resources persist between calls until explicit close
  or global cleanup. A paused sender can still be discovered and its last frame read.
- Editor world cleanup closes all plugin streams, including other PIE worlds. There is no per-world
  ownership isolation. Packaged applications must close their streams at the appropriate EndPlay/
  level-transition boundary; module shutdown performs final process-wide cleanup.
- CloseReceiver restores only material parameters still holding the texture it bound. Caller-created
  materials and caller-retained old output textures remain valid. Store returned textures/materials
  in reflected references so Unreal GC can see them.
- `FSenderStruct` queries populate Name, Width, Height and bIsAlive. Its legacy object fields are not
  populated. Use Optional Output Render Target for a functional render-target output.

## Operational limits

The following console variables apply to subsequent admissions; lowering a limit does not destroy
existing objects or cancel accepted work. Requests exceeding a limit are refused. Close unused names
and release unused outputs before increasing limits.

| Variable | Default | Purpose |
| --- | ---: | --- |
| r.Spout.MaxActiveStreams | 32 | Sender and receiver roles combined |
| r.Spout.MaxPendingCommands | 128 | Non-viewport commands, including cancelled work awaiting drain |
| r.Spout.MaxPendingPerStream | 4 | Queued commands per active generation |
| r.Spout.MaxConsumersPerReceiver | 32 | Material bindings and optional targets, each bounded separately |
| r.Spout.MaxTextureDimension | 8192 | Width/height; hard maximum 16384 |
| r.Spout.MaxFrameMiB | 128 | Uncompressed bytes per frame |
| r.Spout.MaxRetainedMiB | 1024 | Aggregate estimated retained image reservations |

`r.Spout.AccessWaitMs` (default 2, clamped to 0 to 10) is not an admission limit: it is the longest
time, in milliseconds, one shared-texture access attempt waits for a peer. It is not a per-frame
bound. See [Performance and diagnostics](#performance-and-diagnostics). A change applies from the
next attempt.

### Experimental receive settings (off by default)

These four settings are experimental. They are off by default and they are not part of the
qualified behaviour of this release. With both `r.Spout.FrameWaitMs` and `r.Spout.FrameCountRecheck`
at 0 none of the operations below runs, and a receiver behaves as described elsewhere in this
document. That was checked by counting those operations in a run with both at 0, not by a
side-by-side performance comparison with the previous build.

| Variable | Default | Purpose |
| --- | ---: | --- |
| r.Spout.FrameCountRecheck | 0 | 1: read the sender's frame notification again while texture access is held, and copy on a receive call that found nothing new when the sender published since the previous call |
| r.Spout.FrameWaitMs | 0 | Longest time, in milliseconds (0 to 1), one receive call waits for the sender's next frame when it found none and the sender's publication history says one is due. Implies the recheck |
| r.Spout.FrameWaitBudgetMs | 1 | Total new-frame wait per render frame across all receivers, in milliseconds (0 to 1) |
| r.Spout.FrameWaitPollUs | 150 | Interval between polls inside a wait, in microseconds (50 to 1000) |

What they do and do not do:

- One millisecond is the limit of both the wait of one receive call and the wait of all receivers
  together in one render frame, in every build configuration. A larger value of either setting is
  read as one millisecond.
- The wait runs on the render thread, before any texture access is taken, and keeps that thread
  busy while it waits. It is only attempted for a sender that publishes a Spout frame count and
  whose recent publications were regular enough to predict. A source slower than Unreal's own frame
  rate is not waited for.
- They use Spout's public frame-count calls only. The frame count is treated as a notification that
  something was published, never as the identity of an image: another receiver reading the same
  sender lowers the count for an instant, so two equal counts do not prove two equal images.
- Three quantities are kept apart. A *notification* is the sender's count having moved. A *copy* is
  a transfer that completed. A *delivery* is a copy counted in `Frames` and handed to materials and
  output targets. Every completed copy is a delivery, with these settings on or off. With the
  recheck on, a copy can be made without a notification, and such a copy can repeat the image that
  was already delivered; it is still counted as a delivery, because telling a repeat from a new
  image would mean trusting a count.
- A receiver still samples a single shared texture that the sender overwrites. These settings do
  not make receiving lossless between independent clocks, and they do not remove latency.

### Async Receive (Beta) (off by default)

Async Receive (Beta) is chosen per receiver: the `Async Receive (Beta)` input of Spout Receiver and
Spout Receiver With Material, or the checkbox of the same name on the shipped receiver example
(Spout > Experimental). It moves receive work off the render thread: a worker thread with its own
device takes the sender's texture and waits for the copy, and the render thread only queues a copy
of the newest completed image. It is off by default and it is not part of the qualified behaviour
of this release.

- **Per sender name, fixed while open.** The mode belongs to the receiver session of a name, which
  every graph receiving that name shares. The first accepted receive decides it; a later call for
  the same name asking for the other mode is refused (false, outputs untouched, a throttled
  warning) and leaves the session and its other consumers as they were. Different names are
  independent. To change the mode, close the receiver and receive again; nothing is switched or
  reopened behind the other consumers, and there is no live switching of an open session.
- **Fallback is visible.** A session that asked for it runs synchronously, and says why in the log
  and in `GetSpoutStreamStatus` (`EffectiveReceiveMode`, `ReceiveModeNote`), when the sender's
  format is not BGRA8 or RGBA8, when the worker's device or resources cannot be created, or after
  the worker failed. A later sender size, format or handle is tried again.
- **Console variables.** `r.Spout.AsyncReceive` is retired: still registered, ignored, and it logs a
  warning when set to anything but 0. It cannot turn the asynchronous path on for any receiver.
  `r.Spout.AsyncReceive.GpuPriority` (default 0) is a diagnostic for receivers that opted in.
  `r.Spout.FrameWaitMs` and the frame-count recheck apply to the synchronous path only.
- **Performance is a trade-off, not a gain.** In one measured pair on one laptop (RTX 2060, UE 5.8,
  a counter-strip receive under GPU load), the asynchronous receive cut the render thread's receive
  call p99 from 1.79 ms to 0.002 ms and delivered every publication (100% against 97.1%), but the
  frame-interval p99 was 3.24 ms worse and the age of the final copy was 3.87 ms worse on average
  and 15.19 ms worse at p99. Under light load the frame p99 was effectively unchanged. One pair is
  not a general, causal or cross-machine result, and the original targets for this mode (3 ms
  better frame p99, at most 1 ms / 1.5 ms worse image age) were not met. Measure it in your own
  project before relying on it.
- Same safety rules as the synchronous receive: the sender's texture is only read under its access
  lock, and the lock is given back only once the worker's copy is proven finished on the GPU (or
  the worker's device was removed), however long that takes. A copy that finished later than
  100 ms is discarded, never delivered, and repeated late copies stop the asynchronous path for that
  stream. A copy whose completion signal fails is proven by a GPU query instead, and then turns the
  asynchronous path off for the process (the stream continues synchronously, as before).
- What that means for stalls and shutdown (changed in late October 2026; before, the lock was given
  back after 100 ms, or at once after a failed signal, while the copy might still be reading the
  sender's texture): a GPU that stalls without being removed keeps the sender waiting for the lock,
  as the synchronous receive does. Close Receiver, Close All Streams and engine shutdown wait for a
  copy in progress before they return, and the log warns every 5 seconds while a wait goes on. The
  worker's GPU objects are released only once their work is proven finished. A closed receiver
  leaves no worker thread behind.
- It runs on either [interop path](#interop-path). Its tests and the measurement above used the
  default shared-slot path; the D3D11-on-12 path was exercised only by targeted checks.

Viewport requests coalesce by name until presentation. Estimates reserve three image equivalents per
received output (CPU mip plus GPU recreation overlap), two per sender resource, four per receiver native state (shared/staging/upload/cache
allowance), and two per attached optional target. Replaced output textures retain their reservation
through UObject/resource lifetime, including when held by callers. Resize admission includes old
and new reservations. The CPU mip is retained for UTexture2D resource recreation compatibility with installed engines.

Unreferenced closed outputs keep their reservation until Unreal garbage collection and render
resource release finish. Rapid close/recreate workloads can reach the default budget before the
next GC, even after the caller clears its references. The plugin refuses new allocations during
that interval; it does not force global GC. Reuse open sessions where possible, and qualify any
budget increase against the application's memory capacity. A receiver left open while its sender
restarts keeps and reuses its output; only closing the receiver releases the output by contract.
How quickly repeated close/reopen reaches the budget depends on when Unreal collects garbage (its
purge interval, memory pressure, editor or packaged play), on resolution and format, on the number
of open streams and on how long callers keep old textures. There is no general safe restart rate.
The release pipeline qualifies one specific close/reopen workload, described in
[release readiness](RELEASE_READINESS.md); closing and reopening a 1080p stream twice per second is
a stress case that exhausts the default budget and is not supported at that budget.
Unreal's global FName pool also retains interned names; retiring plugin records cannot reclaim
arbitrary names created by the application.

This budget is admission accounting, not a driver VRAM measurement. Caller-created render targets,
source images, arbitrary downstream RHI references, driver allocations and Unreal's allocator/cache
retention are additional. The render-target factory enforces frame dimensions/bytes, but its returned
caller-owned object is not charged to the retained receiver budget. Native GPU completion remains a
correctness requirement and cannot be abandoned merely to satisfy a time budget.

## Performance and diagnostics

- Named and keyed texture-access attempts wait for a bounded time. When a peer holds the shared
  texture, each attempt waits at most `r.Spout.AccessWaitMs` (default 2 ms, clamped to 0 to 10) and
  ends as soon as the peer releases it. The wait runs on the render thread. The bound applies to
  each attempt, not to a frame: every sender publish and every receiver copy that finds its texture
  busy is a separate attempt, so several busy streams can each add up to the bound in the same
  frame, and a peer that keeps a texture locked costs up to the bound on every frame for that
  stream.
- `r.Spout.AccessWaitMs 0` restores zero-wait acquisition only: each attempt tries once and a busy
  texture skips that transfer at once. It does not restore earlier builds as a whole; other changes,
  such as the immediate release of keyed-mutex textures and publishing 8-bit sources as `_UNORM`,
  stay active.
- The cumulative cost of the wait is material. In testing (UE 5.8 packaged, 60 fps, default 2 ms),
  a peer that kept eight of ten open 1080p streams (four received, four sent) locked for 30 seconds
  added up to about 13 ms of render-thread waiting per 16.7 ms frame period (99th percentile; at
  most 16 ms), which lowered the frame rate to about 59 fps on average, with 21 ms frames at the
  99th percentile and 24 ms at most, until the peer let go. With brief holds of 1.5 ms per frame on
  the same eight streams, no transfer was skipped and the frame rate stayed at 60 fps, but
  render-thread time at the 99th percentile rose from 6.4 to 7.9 ms. Allow for the number of streams
  a stalled peer could hold, or lower the setting.
- Windows scheduling can make an attempt last longer than requested. In testing, a timed-out 2 ms
  named-mutex attempt took 1.8 to 3.2 ms at the 1 ms timer resolution Unreal requests at startup,
  and about 10 to 16 ms in a process running at the default Windows timer resolution (about
  15.6 ms). A minimised packaged game kept the short waits in testing (at most 2.9 ms), but Windows
  11 does not guarantee a raised timer resolution to a hidden or minimised window. Keyed-mutex
  attempts poll against a high-resolution clock and ended at 2.0 ms in testing, but can still be
  delayed by thread scheduling. A wait that the peer ends by releasing the texture is not affected.
- If the peer still holds the texture when the wait ends, the transfer is skipped and the next call
  retries. A sender skips that frame and publishes the next one once the texture is free. A
  receiver keeps the new frame pending and copies on a later call, but it copies whatever the
  sender has published by then: Spout keeps one texture per sender, so a frame the sender replaces
  in the meantime is lost, not queued. Each skip is logged as "shared-texture access was
  unavailable", throttled to one line per stream every 5 seconds, and counted in Get Spout Stream
  Status. If the skipped transfer is on the call that resizes an optional render target, that
  target shows its Clear Color until the next successful transfer; a replaced received texture
  starts transparent.
- Receiving samples a stream; it does not queue frames. A receiver copies at most once per Unreal
  frame and each copy gets the sender's newest frame, so a frame that is replaced before the next
  copy is never received. This sampling loss is different from access starvation: no transfer is
  skipped, `AccessMisses` does not count it, and `r.Spout.AccessWaitMs` does not change it. When
  the sender and Unreal run at the same frame rate on independent clocks, their timing drifts in
  and out of alignment. While Unreal's copy lands within about a millisecond of the sender's
  publish, the two can swap order from one frame to the next: single frames are skipped and the
  previous frame is shown for one extra Unreal frame (a repeated frame, not a freeze). In testing
  (UE 5.8 packaged, 1080p, sender and Unreal both at 60 fps), alignment lowered distinct-frame
  delivery to 87 to 89 % in individual 10-second windows, with at most one access miss and at most
  two consecutive skipped frames in each of those windows, while whole runs of one to five minutes
  delivered 98 to 99 %. Access starvation looks different: `AccessMisses` rises with the losses, and
  without a wait a stream can stall for many frames (with `r.Spout.AccessWaitMs 0` and a sender
  holding its texture an extra 1.5 ms per frame, one test showed the same frame for 7.4 seconds).
  Running Unreal at a higher frame rate than the sender reduces sampling losses but does not
  guarantee that every frame is received: scheduling jitter, frame hitches, render-thread stalls and
  access contention can still lose frames, and Spout has no per-frame acknowledgement.
- A keyed-mutex sender (one that shares its texture with a DXGI keyed mutex instead of Spout's
  named access mutex) that holds its texture for long periods can cause runs of consecutive skipped
  frames, with or without the access wait. On the D3D11-on-12 path (`r.Spout.SharedSlotInterop 0`)
  Unreal's receive path acquires keyed textures through its D3D11-on-12 device, which shares the
  engine's graphics queue, so a long keyed hold also delays Unreal's own GPU work on that texture. On
  the default shared-slot path the plugin's native Direct3D 11 device acquires them, and a long hold
  delays the native copy the render thread waits for (at most 100 ms). The figures below were
  measured on the D3D11-on-12 path of an earlier 1.0.0 build and were not repeated on the
  shared-slot path. Tested scope: one 1080p 60 fps keyed sender (a native test
  application) received by a UE 5.8 packaged game at 60 fps. Holding the texture 20 ms on every
  tenth frame delivered 84 % of distinct frames at the default wait, with up to 14 consecutive
  frames skipped, in a 60-second run; a run alternating the setting every 5 seconds delivered 89 %
  at 2 ms and 83 % at 0, with up to 22 consecutive frames skipped at 0. The same holds from a
  named-mutex sender lost one frame per hold (90 %). Without deliberate holds, or with 1.5 ms holds,
  keyed delivery was 99.8 to 100 % in 60-second runs, while one 180-second run without holds that
  alternated the setting delivered 93 % at 2 ms and 85 % at 0. Qualify a keyed sender with its
  actual workload.
- Once access is acquired, GPU completion is required before handing the shared resource back.
  This can stall the render thread, independently of the access wait.
- Sending does not wait on the render thread. A render-target send copies the source into one of two
  slots on Unreal's queue and returns. A worker thread per sender takes Spout's access, copies the
  slot into the Spout texture, waits until that copy is proven finished, announces the frame and
  releases the access.
  - Frames are announced in order, normally within a few milliseconds of the send. Measured on the
    RTX 2060 laptop: acceptance to announcement p50 0.9 ms and p99 6 ms when only sending, and p50 4
    to 7 ms and p99 12 to 20 ms while Unreal and a peer also receive.
  - A receiver in the same process, in the same frame, may therefore still see the previous frame.
  - If a peer holds the texture, the worker keeps trying in attempts of `r.Spout.AccessWaitMs`. A
    frame that a newer one replaces before it got access is skipped (superseded or abandoned), and
    the worker gives up on a frame 100 ms after it was ready. Each skipped frame is counted, never
    queued.
  - Each render-target sender keeps one more frame of memory than before: four frame sizes.
  - Measured in a laptop scene at the highest scalability level at which the scene alone holds
    60 fps: sending held 60 fps (59.99 to 60.0), where the earlier synchronous send reached 57.7.
  - Its extra GPU time per frame is not established. On the laptop, one comparison with no Spout
    activity measured 1.09 ms, and two further comparisons were inconclusive because their reference
    runs disagreed. Measure your own scene with and without the streams.
  - The viewport sender is unchanged: it still waits for its copy on the render thread. In a scene
    that keeps the GPU busy, that copy runs behind the frame's own rendering, so the wait lengthens
    the frame. Measured on the laptop with the earlier synchronous render-target send, in a scene
    that ran at 40 to 43 fps with no Spout activity, sending one 1080p stream at 60 fps lowered the
    scene to about 35 fps (the waiting is inferred, not timed directly).
- Receiving one 1080p 60 fps stream lowered the 40 to 43 fps scene above to about 39.5 fps. In a light
  scene at 60 fps on the same laptop, receiving had no measurable cost. Measure your own scene with
  and without the streams.
- Closing a render-target sender, stopping the session, or flushing the rendering commands gives
  each frame still owed one access attempt. If a receiver holds the texture then, that frame is
  abandoned and never announced: a receiver keeps the last frame it saw. The plugin promises that
  every accepted frame is published or explicitly skipped, not that an external receiver sees the
  final frame.
- Receiving Unreal's own output in the same process makes the receive and the send's worker contend
  for the same texture. Measured in a paced close/reopen workload: 72 access warnings (throttled),
  where the synchronous send had none, and no failed cycle.
- A viewport send to a name that a render-target sender already publishes is refused, with a
  throttled warning: one name has one source.
- A sender that keeps its texture locked while it waits for its own GPU work makes receive attempts
  wait or skip whenever that GPU work is queued behind other work. On a busy GPU this can be on
  most frames.
  - Measured in the laptop scene above with the plugin's native test sender, which holds Spout's
    access lock until its GPU work completes, at the default 2 ms wait and 1080p 60 fps.
  - The sender held the texture longer than 2 ms on 29 to 45 % of its frames, and the receiver
    skipped 4.7 to 6.7 % of the sender's frames (counted as `AccessMisses`).
  - Up to 22 consecutive Unreal frames, about half a second, passed before a skip was recovered.
    Every skip was recovered, and no corrupt frame was observed: the previous frame stays on screen.
  Raising `r.Spout.AccessWaitMs` trades skips for render-thread waiting (above). Qualify such a
  sender with your actual workload.
- When frame counting reports no new frame, the receiver avoids another upstream copy. Discovery,
  bookkeeping and render-command processing still have cost; a new/recreated destination may need
  a local copy of the cached frame. Without peer frame counting, each request copies conservatively.
- CPU fallback (`r.Spout.GPUReceiver 0`) requires staging, map and upload for a new image and has a
  substantial bandwidth cost. Cost also scales with resolution, active names, targets and churn.
- Native connect/registration failures retry with backoff from 10 ms to 1 second. A changed native
  identity resets that backoff. Repeated diagnostics are throttled; unsupported RHI is latched.
- Closing and reopening a stream reuses a pooled SDK frame-synchronisation object instead of
  constructing one, which measured about 15 ms per role on the RTX 4090 development desktop. The
  pool is bounded (64 objects) and holds no kernel handles; native shared textures are never pooled.
- Get Spout Stream Status exposes accepted queued requests, queue rejections, submitted frames,
  access misses and native failures. Get Spout Runtime Status exposes active roles, queue peaks and
  reservation totals/rejections. These are snapshots, not cross-process delivery acknowledgments.

## Qualification

The descriptor currently sets `IsBetaVersion` to false. That metadata does not certify production
readiness. Review [release readiness](RELEASE_READINESS.md) and qualify the exact archive with your
engine build, GPU, driver, external application and expected long-running workload before deployment.
