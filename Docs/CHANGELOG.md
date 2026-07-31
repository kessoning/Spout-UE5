# Changelog (documentation copy)

The canonical changelog is the root [CHANGELOG.md](../CHANGELOG.md). This copy is kept in
`Docs/` for navigation alongside the rest of the documentation. Current release: 0.0.5.

## 0.0.5 - 2026-05-21

Fixed: release packaging for `Spout.dll` so precompiled builds load correctly. Added:
delay-loading for `Spout.dll`, startup diagnostics that log when the DLL is missing, and
troubleshooting plus a packaging checklist. Changed: replaced manual DLL copy logic with
Unreal's `RuntimeDependencies` staging to `$(BinaryOutputDir)`; restricted the module to Win64
via `PlatformAllowList`.

## 0.0.4 - 2026-04-30

Added: GPU-direct receiver path via D3D11-on-12 wrapping (single `CopyResource`, no Map/Unmap,
no per-frame CPU copy) and CVar `r.Spout.GPUReceiver` (default 1). Changed: registry keyed by
`(FName, ESpoutType)` so a sender and receiver sharing a name no longer corrupt each other;
optional render-target destination matches the primary destination format; Spout ThirdParty
headers moved to private includes; MID outer is now `GetTransientPackage()`; deprecated
`GetTexture2DRHI()` replaced with `GetTextureRHI()`. Fixed: strict-aliasing fix for
`D3D11On12CreateDevice`, a potential large-texture overflow, and a packaging build failure on
Game targets (added `#include "UObject/Package.h"`). No public Blueprint or exported-symbol
changes.

## 0.0.2

Minor bug fixes. Added HDR capture support for SceneCapture2D. Preferred setup: Final Color
(HDR in Linear Working Color Space) as Capture Source paired with an RGBA8 sRGB render target.

## 0.0.1

First release.
