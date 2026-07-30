# Changelog

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