# Release readiness

This page ships inside the 1.0.0 release archives. It describes what qualification of a release
consists of and what it does not cover. Do not treat historical releases or successful compilation
as approval of changed source.

## Automated qualification

For 1.0.0 the release pipeline:

- builds the plugin with BuildPlugin for every declared engine (UE 5.3 through 5.8) from the
  release source (a commit, or a recorded base commit plus exact patch) and validates each
  archive's layout, descriptor, `Spout.dll` hash, precompiled data, licensing files and shipped
  document links;
- runs the plugin automation suite, and Development and Shipping packaged hosts with repeated
  launches, on every declared engine from that source;
- installs the exact UE 5.3 and UE 5.8 archives into fresh host projects and repeats automation and
  Development/Shipping packaged checks from them. Those checks load `Spout.dll` from the packaged
  game, run the shipped Blueprint examples and their EndPlay cleanup, restart same-name streams,
  travel between levels and compare pixels in both directions with an independent native Spout
  process;
- measures a packaged UE 5.8 Development host built from the exact UE 5.8 archive at a 60 Hz work
  rate on one development machine (NVIDIA RTX 4090, Windows 11): 1080p and 4K send and receive,
  four simultaneous 1080p streams, CPU receive fallback, an idle receiver, resize churn and the
  paced close/reopen workload below. A scenario passes only when at least 90% of publication
  opportunities and of the 60 Hz target are observed with zero corrupt pixels;
- runs the 30-minute release test (at least 30 minutes) on a packaged UE 5.8 Development build made
  from the exact UE 5.8 archive: a 1080p 60 fps BGRA8 stream in each direction, consumed by external
  Spout processes, judged against acceptance criteria that are fixed before the run.

The measurements include a serialized pixel observer and queue drain. They are not a claim about
other hardware, about isolated plugin cost or about unobserved asynchronous throughput.

## Close and reopen

The qualified close/reopen workload is specific: one 1080p BGRA8 sender and receiver in one packaged
process, both closed and reopened every two seconds through the Blueprint nodes, with the caller
clearing its references at close, at the default `r.Spout.MaxRetainedMiB` and Unreal's default
garbage-collection interval, for 60 and 180 seconds. It is not a general safe restart rate.

Closed receiver outputs stay reserved until Unreal collects them, so budget pressure depends on
garbage-collection timing, resolution, format, concurrent streams and caller-held textures. On the
tested machine, closing and reopening one 1080p stream twice per second exhausted the default budget
after about 20 seconds, and new receiver outputs were then refused until the next collection. That
stress case stays in the pipeline, fails its delivery gate at the default budget and is not supported
at that budget. It passed with a 4 GiB diagnostic budget, which is a comparison, not a changed
default. A receiver that stays open while its sender restarts is unaffected. See
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
