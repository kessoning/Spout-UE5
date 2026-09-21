# Blueprint API Reference

All Blueprint nodes are static functions on `USpoutBPFunctionLibrary` (category `Spout`). The
C++ signatures below are the source of truth. Pin names in Blueprint match the parameter names.

`GlobalShutdown()` and `EnsureSpoutInitialized()` exist in the class but are not Blueprint
functions and are not callable from Blueprints.

## Summary

| Node | Returns | Category |
|---|---|---|
| Spout Sender | `bool` | Spout |
| Close Sender | `void` | Spout |
| Spout Receiver | `bool` | Spout |
| Close Receiver | `void` | Spout |
| Spout Info | `bool` | Spout |
| Get Sender Info | `bool` | Spout |
| Set Max Senders | `int32` | Spout |
| Get Max Senders | `void` | Spout |
| Create Texture Render Target 2D | `UTextureRenderTarget2D*` | Spout |

---

## Spout Sender

```cpp
static bool SpoutSender(FName SpoutName,
                        ESpoutSendTextureFrom SendTextureFrom,
                        UTextureRenderTarget2D* TextureRenderTarget2D);
```

Publishes a texture to Spout. The GPU copy runs on the render thread and does not stall the
game thread.

- Inputs: `SpoutName` (stream name); `SendTextureFrom` (`GameViewport` or `TextureRenderTarget2D`);
  `TextureRenderTarget2D` (required for the render-target source, ignored for GameViewport).
- Returns: `true` if the send was accepted or queued, `false` on invalid input (for example a
  null render target, no game viewport, or Spout unavailable).
- Call it: every frame on Tick or on a timer, continuously, to request continuous frame updates.
- Common mistakes: leaving the render target empty while using the TextureRenderTarget2D source;
  calling it once instead of continuously; using a name that does not match the receiver.

## Close Sender

```cpp
static void CloseSender(FName SpoutName);
```

Removes the sender with this name and releases its shared handle. Scoped to the sender entry
only, so a receiver using the same name is unaffected.

- Call it: when you want to stop publishing a stream.

## Spout Receiver With Material (recommended for Blueprint)

```cpp
static bool SpoutReceiverWithMaterial(FName SpoutName, UMaterialInstanceDynamic* ConsumerMaterial,
    FName TextureParameterName, UTexture2D*& OutTexture,
    UTextureRenderTarget2D* OptionalOutputRenderTarget = nullptr,
    ESpoutColorSpace ColorSpace = ESpoutColorSpace::Auto);
static bool ReleaseReceiverMaterialBinding(FName SpoutName,
    UMaterialInstanceDynamic* ConsumerMaterial, FName TextureParameterName);
```

Create a MID once with Unreal's Create Dynamic Material Instance node, retain it in a variable,
and pass that variable to `ConsumerMaterial` on each receive. This is a real input pin and the
receiver allocates no material. Null supports texture-only use. Return true means the request was
accepted; native opening and copying still happen later on the render thread.

One texture is shared per sender name. Optional targets catch up independently from the last
completed image, including late attachment and resource recreation while the sender is paused.
Unchanged targets skip repeat copies. Same-name consumers must agree on resolved color space;
conflicting calls return false without changing the existing output. Close the receiver before
changing interpretation, or let a source pixel-format change establish a new interpretation.

`ReleaseReceiverMaterialBinding` removes one MID/parameter binding and keeps the receiver open.
It restores the previous texture (or the engine default) only while the parameter still points
to that receiver's image. Later manual edits and bindings to other sender names are preserved.
Keep the caller-created MID alive yourself. Close the shared receiver when no consumers need it.

## Spout Receiver (existing signature)

```cpp
static bool SpoutReceiver(const FName SpoutName,
                          UMaterialInterface* InputMaterial,
                          FName TextureParameterName,
                          UMaterialInstanceDynamic*& OutMat,
                          UTexture2D*& OutTexture,
                          UTextureRenderTarget2D* OptionalOutputRenderTarget = nullptr,
                          ESpoutColorSpace ColorSpace = ESpoutColorSpace::Auto);
```

Receives a stream and updates the outputs. `OptionalOutputRenderTarget` and `ColorSpace` are
advanced-display pins (hidden until expanded).

- Inputs: `SpoutName` (must match the sender); `InputMaterial` (optional base material to create
  a dynamic instance from); `TextureParameterName` (texture parameter to bind, defaults to
  `SpoutTexture` when empty); `OptionalOutputRenderTarget` (optional render target to also copy
  the frame into); `ColorSpace` (how the received bytes are interpreted — see
  [ESpoutColorSpace](#espoutcolorspace) below).
- `OptionalOutputRenderTarget` must match the incoming frame's pixel format exactly and be
  single-sampled. A mismatched target is excluded from the copy and left completely untouched,
  and the mismatch is logged; the call still succeeds and `OutTexture` still updates. A size
  mismatch is recoverable: the target is resized to the incoming frame automatically.
- Outputs: `OutMat` (dynamic material instance, created on first call when `InputMaterial` is
  set); `OutTexture` (transient texture updated from the stream).
- Ownership: `OutMat` and `OutTexture` are **outputs, not ownership**. The received texture
  belongs to the plugin, which keeps it alive for as long as the receiver is open. Keep the
  values this node returns and hand them to Close Receiver. C++ callers can reuse `OutMat` by
  reference; Blueprint has an output pin, so use the explicit material-input node above on Tick.
  They are only assigned on success and are deliberately left untouched on failure, so a
  temporary sender dropout does not orphan anything.
- Sessions: there is one receiver session per sender name. Every graph receiving the same name
  shares that one texture, but each gets its own material instance created from its own
  `InputMaterial`.
- Returns: `true` if the named sender was found and a receive was enqueued, `false` otherwise.
- Call it: on Tick or a timer. Upstream frames are consumed once; each optional target catches
  up independently. The same color-space and material-binding rules as the new node apply.
- Common mistakes: not validating `OutMat`/`OutTexture` before use; a texture parameter name
  that does not exist in the material; a name mismatch with the sender.

---

## Close Receiver

```cpp
static void CloseReceiver(FName SpoutName,
                          UTexture2D*& ReceivedTexture,
                          UMaterialInstanceDynamic*& ReceivedMaterial);
```

Tears down a receiver started with Spout Receiver: cancels any receive still queued for that
name, drops the registry entry, restores parameters still owned by this receiver,
and releases the plugin's ownership of the received texture so the engine
collects it normally.

- Inputs: `SpoutName` (the receiver to close); `ReceivedTexture` and `ReceivedMaterial`, passed
  by reference and cleared on success.
- The **name** decides what is released, not the pointers. Pass back the `OutTexture` and
  `OutMat` you got from Spout Receiver and both are cleared. Pass anything else and it is left
  exactly as it was, with a warning: this node will not destroy a texture it did not create, so
  a wrong pointer can no longer take out another receiver's texture.
- Sessions: there is one receiver session per sender name, so this closes it for **every** graph
  receiving that name. Those graphs get a fresh texture on their next Spout Receiver call.
- Memory: the released texture keeps its share of `r.Spout.MaxRetainedMiB` until Unreal
  garbage-collects it, even when nothing references it any more. Closing and reopening receivers
  rapidly can exhaust the budget between collections, after which new outputs are refused; see
  [limitations](LIMITATIONS.md).
- Call it: when a receiver is no longer needed, to avoid per-name resource growth.

## Spout Info

```cpp
static bool SpoutInfo(TArray<FSenderStruct>& Senders);
```

Fills an array with one entry per active sender in the Spout registry. For each entry only
`Name`, `Width`, `Height`, and `bIsAlive` (set to true) are populated. The other `FSenderStruct`
fields are left at their defaults.

- Returns: `true` if the registry was queried, `false` if Spout is unavailable.
- Call it: for discovery and debugging, for example to populate a dropdown of available senders.

## Get Sender Info

```cpp
static bool GetSenderInfo(FName SpoutName, FSenderStruct& OutSenderStruct);
```

Looks up one sender by name and fills `Name`, `Width`, `Height`, and `bIsAlive`.

- Returns: `true` if the sender exists, `false` otherwise.

## Set Max Senders

```cpp
static int32 SetMaxSenders(int32 Max);
```

Sets the maximum number of senders Spout's shared registry tracks and returns the limit that is
actually in effect afterwards. It does not echo the requested value back.

- Returns: the effective limit, read back from Spout after the change. Returns `0` when nothing
  was changed: either the requested value was outside the accepted range (1–1024) or Spout is
  unavailable.

## Get Max Senders

```cpp
static void GetMaxSenders(int32& Max);
```

Reads the current maximum sender count into `Max`.

## Create Texture Render Target 2D

```cpp
static UTextureRenderTarget2D* CreateTextureRenderTarget2D(int32 Width = 1024,
                                                           int32 Height = 768,
                                                           EPixelFormat Format = EPixelFormat::PF_B8G8R8A8,
                                                           bool bForceLinearGamma = true);
```

Convenience factory that creates a render target at runtime, sets wrap addressing on both axes,
and initializes its resource. Useful when you want to generate a sender or receiver target from
Blueprint instead of an asset.

- Returns: the new `UTextureRenderTarget2D`, or `null` for invalid dimensions or for a pixel
  format the plugin cannot share (supported: `PF_B8G8R8A8`, `PF_R8G8B8A8`, `PF_A2B10G10R10`,
  `PF_FloatRGBA`).
- Ownership: the result is **not** rooted. Keep a reference to it exactly as you would with the
  engine's own Create Render Target 2D node, or it will be garbage collected.
- `bForceLinearGamma` decides what colour space the sender advertises for 8-bit formats:
  `true` (default) publishes a plain `_UNORM` format, so set the receiver's Color Space to
  `Linear`; `false` publishes `_UNORM_SRGB`, which a receiver on `Auto` already reads correctly.

---

## Enums

### ESpoutType

| Value | Meaning |
|---|---|
| `Sender` | Publishes a shared texture to other Spout clients. |
| `Receiver` | Consumes a shared texture from another Spout client. |

### ESpoutSendTextureFrom

| Value | Meaning |
|---|---|
| `GameViewport` | Uses the active game viewport backbuffer. |
| `TextureRenderTarget2D` | Uses a user-provided `UTextureRenderTarget2D`. |

### ESpoutColorSpace

How the Spout Receiver node interprets the received bytes.

| Value | Meaning |
|---|---|
| `Auto` | Follows the format convention: 8-bit frames decode as sRGB, 10-bit and 16-bit float frames stay linear. Matches what other Spout applications publish; the right answer for almost every sender. |
| `SRGB` | Forces sRGB decoding. Only valid for 8-bit formats; requesting it for a 10-bit or float frame fails the call with a logged diagnostic rather than being silently ignored. |
| `Linear` | No decoding. Use when the sender publishes linear or data values in an 8-bit format, such as a render target created with Force Linear Gamma. |

## Struct: FSenderStruct

Lightweight Blueprint data type. Heavy D3D resources are never stored in it.

| Field | Type | Populated by plugin? | Notes |
|---|---|---|---|
| `Name` | `FName` | Yes (Info nodes) | Sender name. |
| `bIsAlive` | `bool` | Yes (Info nodes) | True when the sender was found at query time. |
| `SpoutType` | `ESpoutType` | No | User-settable; defaults to `Sender`. |
| `Width` | `int32` | Yes (Info nodes) | Stream width. |
| `Height` | `int32` | Yes (Info nodes) | Stream height. |
| `TextureColor` | `UTexture2D*` | No | User field; not filled by Info nodes. |
| `MaterialInstanceColor` | `UMaterialInstanceDynamic*` | No | User field. |
| `UserBaseMaterial` | `UMaterialInterface*` | No | User field. |
| `UserTextureParameter` | `FName` | No | Defaults to `SpoutTexture`. |
| `OutputRenderTarget` | `UTextureRenderTarget2D*` | No | Unused by the plugin. Not the same as the Spout Receiver Optional Output Render Target pin. |

Rely only on `Name`, `Width`, `Height`, and `bIsAlive` from the Info nodes. The remaining
fields are available for your own Blueprint use but are not driven by the plugin.

## Operational status

All public functions require the game thread. `GetSpoutStreamStatus(Name, Type)` returns the current
role's active flag, pending commands, accepted queued requests, queue rejections, submitted frames,
access misses, native-failure count and last native error. An active role can be awaiting its first
successful transfer; `bHasFrame` means a submitted frame has been recorded, not that a foreign peer
has displayed it. CPU receive uploads may still be queued when submission is recorded.

`GetSpoutRuntimeStatus()` returns interop availability, active roles, pending/peak commands,
rejected admissions and current/peak/rejected resource reservations. Queries do not initialize the
interop device. Viewport requests coalesce and are not included in non-viewport queue counters.
Closing a role retires its per-stream counters; aggregate counters last for the module lifetime.

Send/receive return false when the operational queue, stream, consumer or memory budget refuses
the request. See [limits and lifetime](LIMITATIONS.md) for the defaults, accounting boundaries,
editor multi-world cleanup behavior and device-loss contract.
