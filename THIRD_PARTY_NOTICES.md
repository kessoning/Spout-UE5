# Third-Party Notices

This file lists the third-party components distributed with the Spout Plugin for Unreal
Engine 5, in source form in this repository and in binary form in every release archive.

The plugin itself is licensed under the MIT License; see [LICENSE](LICENSE). The
components below are covered by their own licenses, reproduced in full.

---

## Spout / Spout2

**Component:** Spout2 SDK (headers and prebuilt `Spout.dll` / `Spout.lib`)
**Author:** Lynn Jarvis
**Project home:** <https://spout.zeal.co/>
**Source repository:** <https://github.com/leadedge/Spout2>
**License:** BSD 2-Clause "Simplified" License

Additional credit recorded in the bundled headers: thanks and credit to Malcolm Bechard
(<https://github.com/mbechard>) for modifications to the `spoutSenderNames` class.

### Bundled files

| Path | Description |
|---|---|
| `ThirdParty/Spout/include/SpoutDX/` | DirectX 11 SDK headers (used by this plugin) |
| `ThirdParty/Spout/include/SpoutDX12/` | DirectX 12 SDK headers |
| `ThirdParty/Spout/include/SpoutDX9/` | DirectX 9 SDK headers |
| `ThirdParty/Spout/include/SpoutGL/` | OpenGL SDK headers |
| `ThirdParty/Spout/include/SpoutLibrary/` | C-style library interface header |
| `ThirdParty/Spout/lib/amd64/Spout.lib` | x64 import library |
| `ThirdParty/Spout/lib/amd64/Spout.dll` | x64 runtime library, redistributed in release archives |

In a release archive the runtime library is also staged at
`SpoutPlugin/Binaries/Win64/Spout.dll`, which is the copy the plugin loads at startup.

### Binary provenance

| Property | Value |
|---|---|
| `Spout.dll` SHA-256 | `4c505aa59ce56d41135e3eb67fd9c4b30a2817ff1e14edca11abcb4b9148941e` |
| `Spout.dll` size | 284,160 bytes |
| `Spout.dll` PE machine | `0x8664` (x64) |
| `Spout.dll` PE link timestamp | 2025-10-23 00:44:33 UTC |
| `Spout.lib` SHA-256 | `23258fc2d31dbdb0fe2eae3f9abc638c561bb9d352e7179b6c8faaecc110ee6e` |
| `Spout.lib` size | 207,656 bytes |
| Authenticode signature | None (`NotSigned`) |
| Embedded file/product version | None; the PE version resource is absent |

### Provenance status: partially established

The exact upstream Spout2 release, tag, or commit that produced the bundled binaries
**cannot be proven from this repository**. The evidence available is:

- The binaries carry no embedded file or product version and are not code-signed, so they
  identify no upstream release themselves.
- Both binaries entered this repository in a single commit,
  `c927d90c7288e50f946c06f8b0b41ebd94077f6d` (2025-12-22), with no recorded upstream
  reference, so git history does not identify one either.
- The bundled headers carry copyright notices ranging from `2014-2024` to `2021-2025`. That
  bounds the headers to no earlier than 2025 but identifies no release.
- `SpoutUtils.h` mentions `2.007.000` and `2.007.009` in a comment, but only as examples
  illustrating the SDK's version-number *format*. They are not a version declaration for this
  header set and do not establish which release it came from. No version number is claimed here
  on that basis.
- The `Spout.dll` PE link timestamp is 2025-10-23, which bounds the build date but not the
  source revision.

Concluding a specific version number from that evidence would be a guess, so none is
claimed here. Resolving this requires either rebuilding `Spout.dll` from an identified
upstream commit and comparing hashes, or obtaining the provenance from whoever produced
the bundled binary. The hashes recorded above pin exactly what is being redistributed in
the meantime, and the release process verifies that the `Spout.dll` inside every archive
matches the `Spout.dll` in this repository.

### License text

```
Copyright (c) 2014-2025, Lynn Jarvis. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

The notice above is reproduced from the bundled headers. Individual headers carry the same
BSD 2-Clause terms with copyright year ranges between 2014 and 2025; the widest range is
used here. Some headers additionally credit Malcolm Bechard, as noted above.

---

## Upstream project

This plugin is a fork of <https://github.com/zuyi53/Spout-UE5>, copyright (c) 2023
RockmanZuyi, MIT licensed. That notice is preserved in [LICENSE](LICENSE). No files from
the upstream project are vendored as a separate third-party component; the fork
relationship is a licensing matter covered by the root license rather than by this file.

---

## Unreal Engine

Unreal Engine itself is not redistributed by this plugin. Use of the engine and of any
project built with it is governed by the Unreal Engine End User License Agreement between
you and Epic Games, Inc.
