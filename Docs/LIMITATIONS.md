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

## Formats and color

- GPU and CPU receiver paths accept BGRA8, RGBA8, RGB10A2 and RGBA16F, including supported
  typeless aliases. Sources must be single-mip, single-sample 2D textures. Other layouts are refused.
- No tone mapping, exposure correction, gamut conversion or pixel gamma conversion is performed.
  Sender 8-bit sRGB metadata follows the actual source RHI resource; RGB10A2 and RGBA16F are linear.
  Receiver Auto follows the format policy, and explicit sRGB is refused for 10-bit/float formats.
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

- Named and keyed texture-access attempts are nonblocking. A busy peer causes a skipped transfer;
  the next call retries. A pending receiver frame is retained until a copy succeeds.
- Once access is acquired, GPU completion is required before handing the shared resource back.
  This can stall the render thread. Zero-wait peer acquisition does not imply nonblocking GPU work.
- When frame counting reports no new frame, the receiver avoids another upstream copy. Discovery,
  bookkeeping and render-command processing still have cost; a new/recreated destination may need
  a local copy of the cached frame. Without peer frame counting, each request copies conservatively.
- CPU fallback (`r.Spout.GPUReceiver 0`) requires staging, map and upload for a new image and has a
  substantial bandwidth cost. Cost also scales with resolution, active names, targets and churn.
- Native connect/registration failures retry with backoff from 10 ms to 1 second. A changed native
  identity resets that backoff. Repeated diagnostics are throttled; unsupported RHI is latched.
- Closing and reopening a stream reuses a pooled SDK frame-synchronisation object instead of
  constructing one, which measured about 15 ms per role on the tested machine. The pool is bounded
  (64 objects) and holds no kernel handles; native shared textures are never pooled.
- Get Spout Stream Status exposes accepted queued requests, queue rejections, submitted frames,
  access misses and native failures. Get Spout Runtime Status exposes active roles, queue peaks and
  reservation totals/rejections. These are snapshots, not cross-process delivery acknowledgments.

## Qualification

The descriptor currently sets `IsBetaVersion` to false. That metadata does not certify production
readiness. Review [release readiness](RELEASE_READINESS.md) and qualify the exact archive with your
engine build, GPU, driver, external application and expected long-running workload before deployment.
