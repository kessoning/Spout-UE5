# C++ API Reference

Everything the plugin offers to Blueprint is also available to C++: the same static functions, the same two
placeable actors and the same types. This chapter shows how to call them from a C++ project. What each function does
(return values, sessions, colour space, Async Receive, output targets, limits) is described once, in the Blueprint API
Reference; the C++ signatures there are the source of truth and are not repeated in full here.

## Modules

| Module | Type | What it is for you |
|---|---|---|
| `SpoutPlugin` | Runtime | The public API: `USpoutBPFunctionLibrary`, `ASpoutSenderActor`, `ASpoutReceiverActor`, `USpoutSettings`, the enums and structs. Depend on this module. |
| `SpoutPluginShaders` | Runtime | Global shaders (the 10-bit sRGB decode). Internal; no public API. |
| `SpoutPluginEditor` | Editor | Registers the two actors in the Place Actors panel and runs their editor streams. Internal; no public API. |

All three modules are Windows 64-bit only (`PlatformAllowList` Win64 in `SpoutPlugin.uplugin`).

## Project setup

1. Use a C++ project, with the plugin in the project's `Plugins/SpoutPlugin` folder (a project plugin is enabled by
   default).
2. Add the module to your game module's dependencies in `<YourModule>.Build.cs`. The plugin builds for Win64 only, so
   guard the dependency if your project also targets other platforms:

```csharp
if (Target.Platform == UnrealTargetPlatform.Win64)
{
    PrivateDependencyModuleNames.Add("SpoutPlugin");
}
```

3. Include the headers you use, and guard the code with `PLATFORM_WINDOWS` if the project also builds for other
   platforms:

```cpp
#include "SpoutBPFunctionLibrary.h"   // functions, enums, FSenderStruct, status structs
#include "SpoutSenderActor.h"         // ASpoutSenderActor
#include "SpoutReceiverActor.h"       // ASpoutReceiverActor
```

The module's public dependencies are Core, CoreUObject, Engine, RHI and RenderCore. Its public headers do not include
any Direct3D or Spout SDK header.

## Rules that apply to every call

- **Game thread only.** Every public function, C++ callers included, must be called on the game thread. The GPU work
  runs later on the render thread (and, for Async Receive, a worker thread).
- **True means accepted, not delivered.** Spout Sender and the receive functions return whether the request was
  accepted and queued. Failures after that are written to the log category `LogSpoutPlugin`, and counted by
  `GetSpoutStreamStatus`.
- **Call continuously.** Send and receive once per frame (from `Tick`) or on a timer. A sender keeps advertising its
  last image while you pause; a receiver copies the sender's newest frame on each call.
- **Close what you open.** Close Sender and Close Receiver end a stream. In the editor, ending Play closes every
  stream automatically; in a packaged game, close your streams yourself, for example in `EndPlay`.
- **Keep your objects referenced.** A render target from `CreateTextureRenderTarget2D` is not rooted, and the
  material instances you create are yours: hold them in `UPROPERTY` members, as with any UObject.

## USpoutBPFunctionLibrary

`USpoutBPFunctionLibrary` (exported with `SPOUTPLUGIN_API`) holds every function as a `static` member. The
Blueprint node name, the C++ function and the parameters correspond one to one.

| Blueprint node | C++ function | Returns |
|---|---|---|
| Spout Sender | `SpoutSender(FName SpoutName, ESpoutSendTextureFrom SendTextureFrom = ESpoutSendTextureFrom::GameViewportOnly, UTextureRenderTarget2D* TextureRenderTarget2D = nullptr)` | `bool` |
| Close Sender | `CloseSender(FName SpoutName)` | `void` |
| Spout Receiver With Material | `SpoutReceiverWithMaterial(FName SpoutName, UMaterialInstanceDynamic* ConsumerMaterial, FName TextureParameterName, UTexture2D*& OutTexture, UTextureRenderTarget2D* OptionalOutputRenderTarget, ESpoutColorSpace ColorSpace, bool bAsyncReceive)` (the last three have defaults) | `bool` |
| Spout Receiver | `SpoutReceiver(const FName SpoutName, UMaterialInterface* InputMaterial, FName TextureParameterName, UMaterialInstanceDynamic*& OutMat, UTexture2D*& OutTexture, UTextureRenderTarget2D* OptionalOutputRenderTarget, ESpoutColorSpace ColorSpace, bool bAsyncReceive)` (the last three have defaults) | `bool` |
| Release Receiver Material Binding | `ReleaseReceiverMaterialBinding(FName SpoutName, UMaterialInstanceDynamic* ConsumerMaterial, FName TextureParameterName)` | `bool` |
| Close Receiver | `CloseReceiver(FName SpoutName, UTexture2D*& ReceivedTexture, UMaterialInstanceDynamic*& ReceivedMaterial)` | `void` |
| Get Spout Stream Status | `GetSpoutStreamStatus(FName SpoutName, ESpoutType Type)` | `FSpoutStreamStatus` |
| Get Spout Runtime Status | `GetSpoutRuntimeStatus()` | `FSpoutRuntimeStatus` |
| Spout Info | `SpoutInfo(TArray<FSenderStruct>& Senders)` | `bool` |
| Get Sender Info | `GetSenderInfo(FName SpoutName, FSenderStruct& OutSenderStruct)` | `bool` |
| Set Max Senders | `SetMaxSenders(int32 Max)` | `int32` |
| Get Max Senders | `GetMaxSenders(int32& Max)` | `void` |
| Get Max Spout Senders | `GetMaxSpoutSenders()` | `int32` |
| List Spout Senders | `ListSpoutSenders(TArray<FSpoutSenderInfo>& Senders)` | `bool` |
| Get Spout Receiver Session Info | `GetSpoutReceiverSessionInfo(FName SpoutName)` | `FSpoutReceiverSessionInfo` |
| Create Texture Render Target 2D | `CreateTextureRenderTarget2D(int32 Width = 1024, int32 Height = 768, EPixelFormat Format = PF_B8G8R8A8, bool bForceLinearGamma = true)` | `UTextureRenderTarget2D*` |

C++-specific notes:

- **Output parameters.** `OutTexture`, `OutMat`, `ReceivedTexture` and `ReceivedMaterial` are references to raw
  pointers, so pass a local `UTexture2D*` (or `UMaterialInstanceDynamic*`) variable, not a `TObjectPtr` member, and
  copy the result into your member afterwards. Outputs are assigned only when the call succeeds.
- **The received texture belongs to the plugin.** Keep the pointer to use it and to hand it back to Close Receiver;
  do not destroy it. It changes when the stream's size or format changes, so read it again after each receive.
- **Reusing a material.** Prefer `SpoutReceiverWithMaterial` with a material instance you create once. With
  `SpoutReceiver`, a C++ caller can also pass its stored `OutMat` back in by reference on every call.
- **Close Receiver closes the stream for everyone.** There is one receiver session per sender name. To stop only your
  own use, call `ReleaseReceiverMaterialBinding`; call `CloseReceiver` when no one in your game still needs the
  stream. Passing `nullptr` for an output you did not use is fine.
- **Not part of the API.** `GlobalShutdown()` and `CloseAllStreams()` are public for the plugin module's own use
  (module shutdown, end of Play in the editor) and close every stream in the process; do not call them.
  `FSpoutModule` is not exported for use from other modules. The `LogSpoutPlugin` log category is exported since
  1.1.0, for the plugin's editor module; log with your own category.

## Types

All types are declared in `SpoutBPFunctionLibrary.h` (the receiver actor's two enums in `SpoutReceiverActor.h`). The
Blueprint API Reference describes every value.

| Type | Values or fields |
|---|---|
| `ESpoutSendTextureFrom` | `GameViewport` (Game Viewport (Whole Window)), `TextureRenderTarget2D`, `GameViewportOnly` (the default) |
| `ESpoutColorSpace` | `Auto`, `SRGB`, `Linear` |
| `ESpoutReceiveMode` | `Synchronous`, `AsyncBeta` (shown as Async Receive) |
| `ESpoutType` | `Sender`, `Receiver` |
| `FSenderStruct` | `Name`, `bIsAlive`, `Width`, `Height` (filled by Spout Info and Get Sender Info); `SpoutType`, `TextureColor`, `MaterialInstanceColor`, `UserBaseMaterial`, `UserTextureParameter`, `OutputRenderTarget` (not filled) |
| `FSpoutStreamStatus` | `bActive`, `bHasFrame`, `PendingCommands`, `AcceptedRequests`, `QueueRejections`, `FramesSubmitted`, `AccessMisses`, `NativeFailures`, `LastNativeError`, `RequestedReceiveMode`, `EffectiveReceiveMode`, `ReceiveModeNote` |
| `FSpoutRuntimeStatus` | `bInteropAvailable`, `ActiveStreams`, `PendingCommands`, `PeakPendingCommands`, `RejectedRequests`, `EstimatedRetainedBytes`, `PeakEstimatedRetainedBytes`, `RejectedAllocations` |
| `ESpoutLostSenderDisplay` | `HoldLastFrame`, `ShowNoSignal` |
| `ESpoutReceiverActorState` | `Disabled`, `Waiting`, `RequestAccepted`, `Receiving`, `Refused` |
| `FSpoutSenderInfo` | `Name`, `Width`, `Height`, `Format`, `BitsPerChannel`, `SourceApplication`, `bFromThisGame`, `Gpu`, `OtherGpuName` (filled by List Spout Senders) |
| `ESpoutSenderGpu` | `Unknown`, `SameAsUnreal`, `Other` |
| `FSpoutReceiverSessionInfo` | `bSessionOpen`, `ReceiveMode`, `ColorSpace`, `ReceiverActors`, `bUsedByBlueprintOrCode`, `bHasLastResult`, `LastResult`, `LastResultText` |
| `ESpoutReceiveResult` | `Accepted`, `InvalidName`, `Unavailable`, `NoSender`, `BadMetadata`, `FrameTooLarge`, `ColorSpaceUnsupported`, `Busy`, `ModeConflict`, `ColorConflict`, `ConsumerLimit`, `TextureFailed`, `StreamQueueFull`, `OtherGpu` |
| `ESpoutStartupPolicy` (`SpoutSettings.h`) | `PlayOnly`, `EditorAndPlay` |

## Example: send the game viewport

An actor that sends the game view, without the editor around it in Play In Editor, while the game runs.

```cpp
// MySpoutViewportSender.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MySpoutViewportSender.generated.h"

UCLASS()
class AMySpoutViewportSender : public AActor
{
    GENERATED_BODY()

public:
    AMySpoutViewportSender();
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /** The name other Spout applications see. One name, one source. */
    UPROPERTY(EditAnywhere, Category = "Spout")
    FName SpoutName = TEXT("MyGame Viewport");
};
```

```cpp
// MySpoutViewportSender.cpp
#include "MySpoutViewportSender.h"
#include "SpoutBPFunctionLibrary.h"

AMySpoutViewportSender::AMySpoutViewportSender()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork; // after the frame's game work
}

void AMySpoutViewportSender::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    // True means the request was queued; publishing failures are logged under LogSpoutPlugin.
    USpoutBPFunctionLibrary::SpoutSender(SpoutName, ESpoutSendTextureFrom::GameViewportOnly, nullptr);
}

void AMySpoutViewportSender::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    USpoutBPFunctionLibrary::CloseSender(SpoutName);
    Super::EndPlay(EndPlayReason);
}
```

The viewport stream carries the back buffer's display (sRGB-encoded) values, 10-bit by default. A receiving Unreal
should set Color Space to sRGB for it.

## Example: send a Scene Capture through a render target

```cpp
// MySpoutCameraSender.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MySpoutCameraSender.generated.h"

class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

UCLASS()
class AMySpoutCameraSender : public AActor
{
    GENERATED_BODY()

public:
    AMySpoutCameraSender();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    UPROPERTY(EditAnywhere, Category = "Spout")
    FName SpoutName = TEXT("MyGame Camera");

    UPROPERTY(VisibleAnywhere, Category = "Spout")
    TObjectPtr<USceneCaptureComponent2D> Capture;

private:
    /** Created at runtime; held here because it is not rooted. */
    UPROPERTY(Transient)
    TObjectPtr<UTextureRenderTarget2D> Target;
};
```

```cpp
// MySpoutCameraSender.cpp
#include "MySpoutCameraSender.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "SpoutBPFunctionLibrary.h"

AMySpoutCameraSender::AMySpoutCameraSender()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
    Capture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("Capture"));
    RootComponent = Capture;
    // See the SceneCapture2D chapter for the capture source and the black-output checklist.
    Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
}

void AMySpoutCameraSender::BeginPlay()
{
    Super::BeginPlay();
    // 8-bit target holding sRGB-encoded bytes (Force Linear Gamma off): receivers on Auto read it correctly.
    Target = USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(1280, 720, PF_B8G8R8A8, false);
    Capture->TextureTarget = Target;
}

void AMySpoutCameraSender::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (Target)
    {
        USpoutBPFunctionLibrary::SpoutSender(SpoutName, ESpoutSendTextureFrom::TextureRenderTarget2D, Target);
    }
}

void AMySpoutCameraSender::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    USpoutBPFunctionLibrary::CloseSender(SpoutName);
    Super::EndPlay(EndPlayReason);
}
```

A render target you send must have one mip and Auto Generate Mips off, in a supported format (BGRA8, RGBA8,
RGB10A2 or RGBA16f).

## Example: receive into a material

The material instance is created once and reused; the receive call binds the shared received texture to its
`SpoutTexture` parameter without allocating a material.

```cpp
// MySpoutScreen.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SpoutBPFunctionLibrary.h"
#include "MySpoutScreen.generated.h"

class UStaticMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture2D;

UCLASS()
class AMySpoutScreen : public AActor
{
    GENERATED_BODY()

public:
    AMySpoutScreen();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /** Must match the sender's name exactly. */
    UPROPERTY(EditAnywhere, Category = "Spout")
    FName SenderName = TEXT("TouchDesigner");

    /** sRGB for a stream from an Unreal viewport; Auto for most other applications. */
    UPROPERTY(EditAnywhere, Category = "Spout")
    ESpoutColorSpace ColorSpace = ESpoutColorSpace::Auto;

    /** A material with a texture parameter named SpoutTexture. */
    UPROPERTY(EditAnywhere, Category = "Spout")
    TObjectPtr<UMaterialInterface> ScreenMaterial;

    UPROPERTY(VisibleAnywhere, Category = "Spout")
    TObjectPtr<UStaticMeshComponent> Screen;

private:
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> ScreenMID;

    /** Owned by the plugin; kept to hand back to Close Receiver. */
    UPROPERTY(Transient)
    TObjectPtr<UTexture2D> ReceivedTexture;
};
```

```cpp
// MySpoutScreen.cpp
#include "MySpoutScreen.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"

static const FName SpoutTextureParameter(TEXT("SpoutTexture"));

AMySpoutScreen::AMySpoutScreen()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
    Screen = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Screen"));
    RootComponent = Screen;
}

void AMySpoutScreen::BeginPlay()
{
    Super::BeginPlay();
    if (ScreenMaterial)
    {
        ScreenMID = UMaterialInstanceDynamic::Create(ScreenMaterial, this);
        Screen->SetMaterial(0, ScreenMID);
    }
}

void AMySpoutScreen::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    UTexture2D* Texture = nullptr;
    // No output render target; Async Receive on (the default).
    if (USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(SenderName, ScreenMID, SpoutTextureParameter, Texture,
            nullptr, ColorSpace, true))
    {
        ReceivedTexture = Texture; // can change when the stream's size or format changes
    }
}

void AMySpoutScreen::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // Stop using the stream ourselves.
    USpoutBPFunctionLibrary::ReleaseReceiverMaterialBinding(SenderName, ScreenMID, SpoutTextureParameter);
    // Close the session only if nothing else in the game receives this sender: it closes it for every consumer.
    UTexture2D* Texture = ReceivedTexture;
    UMaterialInstanceDynamic* NoMaterial = nullptr;
    USpoutBPFunctionLibrary::CloseReceiver(SenderName, Texture, NoMaterial);
    ReceivedTexture = nullptr;
    Super::EndPlay(EndPlayReason);
}
```

The receive returns false while the sender is not running; the call is retried on the next tick. Several consumers of
one sender name share one session and must agree on Color Space and Async Receive.

## Example: spawn the placeable actors

The Spout Sender and Spout Receiver actors can be spawned and configured from C++. Their settings are public members
(read-only in Blueprint); the actors read them on every tick and apply changes with the same rules as the Details
panel. Where a setter exists, it applies the change at once.

```cpp
#include "SpoutSenderActor.h"
#include "SpoutReceiverActor.h"

void AMyGameMode::StartSpout()
{
    UWorld* World = GetWorld();

    // A sender: set its name before it starts. Unique names are assigned automatically only to actors placed in the
    // editor, so give every spawned sender its own name.
    const FTransform Origin = FTransform::Identity;
    ASpoutSenderActor* Sender = World->SpawnActorDeferred<ASpoutSenderActor>(ASpoutSenderActor::StaticClass(), Origin);
    Sender->SpoutName = TEXT("MyGame Main");
    Sender->SendFrom = ESpoutSendTextureFrom::GameViewportOnly;
    Sender->FinishSpawning(Origin);

    // A receiver with its built-in screen, two meters in front of the origin.
    ASpoutReceiverActor* Receiver = World->SpawnActor<ASpoutReceiverActor>(FVector(200.f, 0.f, 100.f), FRotator::ZeroRotator);
    Receiver->SetSenderName(TEXT("TouchDesigner"));
    Receiver->ColorSpace = ESpoutColorSpace::Auto;
}
```

| Actor | Settings (members) | Functions |
|---|---|---|
| `ASpoutSenderActor` | `bEnabled`, `SpoutName`, `SendFrom`, `TextureRenderTarget`, `bSendInEditor`; read-only: `Status`, `bPublishing` | `SetEnabled(bool)`, `SetSpoutName(FName)`, `SetSource(ESpoutSendTextureFrom, UTextureRenderTarget2D*)` |
| `ASpoutReceiverActor` | `bEnabled`, `SenderName`, `bReceiveInEditor`, `ColorSpace`, `bAsyncReceive`, `OutputRenderTarget`, `bShowDisplay`, `DisplayHeight`, `bKeepAspectRatio`, `WhenSenderLost`, `Brightness`, `DisplayMaterial`, `TextureParameterName`; read-only: `State`, `Status`, `ConnectedSenderName`, `ReceivedSize`, `AppliedSettings` | `SetEnabled(bool)`, `SetSenderName(FName)`, `SetOutputRenderTarget(UTextureRenderTarget2D*)`, `GetReceivedTexture()`, `GetDisplayMaterialInstance()` |

The actors close their own streams when they are disabled, destroyed or the game ends. See the Placeable actors
section of the Blueprint API Reference for their behaviour (empty Sender Name, shared streams, refusals, output
target formats).

## Example: list senders and read a stream's status

```cpp
TArray<FSpoutSenderInfo> Senders;
if (USpoutBPFunctionLibrary::ListSpoutSenders(Senders))
{
    for (const FSpoutSenderInfo& Sender : Senders)
    {
        UE_LOG(LogTemp, Log, TEXT("Spout sender '%s' %dx%d %s from %s%s"), *Sender.Name.ToString(), Sender.Width, Sender.Height,
            *Sender.Format, *Sender.SourceApplication, Sender.Gpu == ESpoutSenderGpu::Other ? TEXT(" (another GPU)") : TEXT(""));
    }
}

const FSpoutReceiverSessionInfo Session = USpoutBPFunctionLibrary::GetSpoutReceiverSessionInfo(TEXT("TouchDesigner"));
if (Session.bHasLastResult && Session.LastResult != ESpoutReceiveResult::Accepted)
{
    UE_LOG(LogTemp, Warning, TEXT("%s"), *Session.LastResultText);
}

const FSpoutStreamStatus Status = USpoutBPFunctionLibrary::GetSpoutStreamStatus(TEXT("MyGame Viewport"), ESpoutType::Sender);
UE_LOG(LogTemp, Log, TEXT("active %d, frames %lld, access misses %lld"),
    Status.bActive ? 1 : 0, Status.FramesSubmitted, Status.AccessMisses);
```

`FramesSubmitted` counts frames this game published (or, for a receiver, copies it made); it does not confirm that
another application displayed them.

## Console variables

The plugin-wide settings are console variables (`r.Spout.*`), for example `r.Spout.GPUReceiver`,
`r.Spout.AccessWaitMs` and the `r.Spout.Max*` limits. Set them as any console variable from C++
(`IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.AccessWaitMs"))->Set(...)`), in `DefaultEngine.ini` under
`[SystemSettings]`, or from the console. Their meaning and defaults are in the Limitations chapter.

Since 1.1.0 the project's values can also be set in Project Settings > Plugins > Spout (`USpoutSettings`, in
`SpoutSettings.h`; read them with `USpoutSettings::Get()`). The page sets the console variables at project-setting
priority, so a value set from code, an ini file's `[ConsoleVariables]`, the command line or the console takes
precedence.
