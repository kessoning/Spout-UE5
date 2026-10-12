# Native regression tools

Requirements: 64-bit Windows, Visual Studio C++ tools, CMake, a hardware D3D11/D3D12
device, and Windows Graphics Tools for the debug-layer probe. These tests use the
repository's exact SDK headers, import library and DLL.

From the repository root:

```powershell
cmake -S Tests/Native -B Artifacts/NativeBuild -A x64
cmake --build Artifacts/NativeBuild --config Release
ctest --test-dir Artifacts/NativeBuild -C Release --output-on-failure
```

`SpoutSdkSafetyTests` checks exact-name collision refusal at 31/63/127/255 bytes,
preservation of foreign metadata, capacity refusal, balanced teardown and a
separate process that dies holding the access mutex. The capacity test changes
only its local SDK instance, not the user's persistent Spout settings. Crash peers
use unique names and terminate only themselves.
An additional private SDK memory map tests abandoned registry-map ownership;
the crash test does not abandon the machine-wide sender registry.

`SpoutNotificationTests` compiles the plugin's two engine-free receive headers
(`SpoutNotificationState.h`, `SpoutPublishPredictor.h`) as they are. It checks every transition of
the receive-side notification state against expectations written by hand, drives the publication
predictor with made-up times, and then drives the bundled SDK's public frame-count calls
(`GetNewFrame`, `GetSenderFrame`, `IsFrameCountEnabled`), also while a second receiver reads the
same sender as fast as it can. It exits 2, not 0, when frame counting is disabled on the machine,
so a skipped SDK part is never read as a pass. Its `INFO` lines report how often the other
receiver's read made the count look one low; those figures depend on the machine and are not
judged.

`SpoutSlotPoolTests` compiles `SpoutSlotPool.h`, the engine-free bookkeeping of the experimental
asynchronous receive stream, with no GPU and no Spout library. It checks every transition of the
slot pool against expectations written by hand, including a publication replaced before it was
read, no free slot, a refused access and a completion that timed out; then what starts a worker
transfer, with and without a usable frame count; then three long seeded sequences of worker and
reader steps against a read fence that completes late, checked after every step against the rules
and against an account of where every publication went. It says nothing about the graphics calls
the plugin makes around this bookkeeping.

`D3D11On12WrapProbe` isolates Windows' optional-metadata diagnostic (ID 916).
On the qualification machine it prints HRESULT 0 and a valid wrapper together
with that diagnostic, without Unreal or Spout loaded. Its exit code tests the
actual API result; it is not a claim that the debug queue is empty.

`SpoutPixelPeer` is launched by `SpoutPlugin.P0.ExternalProcessPixelsBothDirections`.
It uses a separate native D3D11 device and the stock SDK texture-access protocol.
Both sides compare every pixel of a sequence-dependent pattern. The native sender
also alternates sizes. Pass its executable to an isolated Unreal host:

```text
UnrealEditor-Cmd.exe <SpoutHostTest.uproject> "-ExecCmds=Automation RunTests SpoutPlugin.P0;Quit" -unattended -NoSound -rhivalidation -d3ddebug "-SpoutNativePeer=<absolute SpoutPixelPeer.exe path>" "-ReportExportPath=<new empty report directory>"
```

The external-process test reports a warning and skips when `-SpoutNativePeer` is
absent. Such a run does **not** qualify cross-process correctness. Qualification
requires that argument, a fresh nonempty report, every selected test passing,
and engine exit code zero. With `-d3ddebug`, only the exact reproduced ID 916
diagnostic is expected in the pixel tests; resource-state errors remain failures.
Production code does not filter the debug queue.

The automation, packaged-host and performance scripts build the peer in an isolated directory and
pass it automatically. Packaged smoke and performance qualification fail when it is absent.
The `receive` mode takes an optional final argument naming the format it requires: `bgra` (or
`linear`, the default) for `B8G8R8A8_UNORM`, `rgba` for `R8G8B8A8_UNORM`, and `srgb`/`bgra_srgb` or
`rgba_srgb` for the `_UNORM_SRGB` variants. The plugin shares every 8-bit source as `_UNORM`, so
qualification uses `bgra` and `rgba`. The peer requires both the advertised format and the shared
texture's real format to match (exit 18 otherwise), because SpoutGL receivers check the real
format, and it creates a shader-resource view in that format, so incompatible color metadata fails
qualification. An unknown format argument exits 17. Patterns include changing alpha and all channel values, while the
origin alpha remains 255 to distinguish a frame from a newly cleared receiver output.

`Scripts/Test-TrackDDelivery.ps1` takes a provenance-checked packaged executable and native peer.
It deliberately stops measured transfers, corrupts qualification pixels, and runs corrupted pixels
through packaged smoke. It requires the intended failures and error exits, and uses the matrix's
actual required-marker list and smoke verdict to verify rejection.

Use a dedicated host project. Tests close their own streams and restore the
receiver-path console setting, but are intended for automation rather than a
user's active scene. The checked-in host/matrix scripts provide isolated staging.
