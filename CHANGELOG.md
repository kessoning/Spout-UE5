# Changelog

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