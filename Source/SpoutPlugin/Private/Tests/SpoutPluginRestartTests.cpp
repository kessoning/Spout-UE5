// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
//
// Stream restart cost. Most of the cost of a rapid close/reopen is the construction of the SDK's
// spoutFrameCount object, once for the receiver entry on the game thread and once for the sender
// entry on the render thread, so closed backends are recycled. These tests pin the pool's
// behaviour and prove that a recycled backend still detects new frames after a same-name restart.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
#include "SpoutBPFunctionLibrary.h"
#include "SpoutD3DContext.h"
#include "SpoutFrameSync.h"
#include "SpoutModule.h"
#include "SpoutSenderRegistry.h"
#include "Containers/StringConv.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#define SPOUT_RESTART_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

namespace
{
	TArray<FColor> RestartPattern(int32 W, int32 H, int32 Sequence)
	{
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(W * H);
		for (int32 Y = 0; Y < H; ++Y)
			for (int32 X = 0; X < W; ++X)
				Pixels[Y * W + X] = FColor((X * 11 + Sequence) & 255, (Y * 23 + Sequence * 3) & 255, (X ^ Y ^ Sequence) & 255, 255);
		return Pixels;
	}

	void QueueRestartPattern(UTextureRenderTarget2D* Target, int32 Sequence)
	{
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		const int32 W = Target->SizeX, H = Target->SizeY;
		TArray<FColor> Pixels = RestartPattern(W, H, Sequence);
		ENQUEUE_RENDER_COMMAND(SpoutRestartPattern)([Resource, W, H, Pixels = MoveTemp(Pixels)](FRHICommandListImmediate& Cmd)
		{
			Cmd.UpdateTexture2D(Resource->GetRenderTargetTexture(), 0, FUpdateTextureRegion2D(0, 0, 0, 0, W, H), W * sizeof(FColor), reinterpret_cast<const uint8*>(Pixels.GetData()));
		});
	}

	void QueueRestartRead(UTexture2D* Texture, int32 W, int32 H, TArray<FColor>& Pixels)
	{
		FTextureResource* Resource = Texture ? Texture->GetResource() : nullptr;
		ENQUEUE_RENDER_COMMAND(SpoutRestartRead)([Resource, W, H, &Pixels](FRHICommandListImmediate& Cmd)
		{
			if (!Resource || !Resource->GetTextureRHI()) return;
			FReadSurfaceDataFlags Flags(RCM_UNorm);
			Flags.SetLinearToGamma(false);
			Cmd.Transition(FRHITransitionInfo(Resource->GetTextureRHI(), ERHIAccess::Unknown, ERHIAccess::CopySrc));
			Cmd.ReadSurfaceData(Resource->GetTextureRHI(), FIntRect(0, 0, W, H), Pixels, Flags);
			Cmd.Transition(FRHITransitionInfo(Resource->GetTextureRHI(), ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		});
	}

	/** A backend that reports itself non-recyclable, exactly like the test fakes elsewhere. */
	class FDisposableBackend final : public ISpoutFrameSyncBackend
	{
	public:
		static int32 Destroyed;
		virtual ~FDisposableBackend() override { ++Destroyed; }
		virtual bool CreateAccessMutex(const ANSICHAR*) override { return true; }
		virtual void CloseAccessMutex() override {}
		virtual void EnableFrameCount(const ANSICHAR*) override {}
		virtual void CleanupFrameCount() override {}
		virtual bool CheckTextureAccess(void*, uint32) override { return true; }
		virtual bool AllowTextureAccess(void*) override { return true; }
		virtual void SetNewFrame() override {}
		virtual bool GetNewFrame() override { return true; }
	};
	int32 FDisposableBackend::Destroyed = 0;

	FString UniqueRestartName(const TCHAR* Suffix)
	{
		return FString::Printf(TEXT("SpoutRestart_%s_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits), Suffix);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutRestartPool, "SpoutPlugin.Restart.FrameSyncBackendPoolRecyclesClosedSdkObjects", SPOUT_RESTART_FLAGS)
bool FSpoutRestartPool::RunTest(const FString&)
{
	if (!FSpoutModule::IsSpoutDllLoaded())
	{
		AddInfo(TEXT("Spout.dll is not loaded in this process; there is no SDK backend to recycle."));
		return true;
	}

	DrainSpoutSdkFrameSyncBackendPool();
	TestEqual(TEXT("a drained pool is empty"), NumPooledSpoutSdkFrameSyncBackends(), 0);

	const FString NameA = UniqueRestartName(TEXT("A"));
	const FString NameB = UniqueRestartName(TEXT("B"));
	const auto Utf8A = StringCast<ANSICHAR>(*NameA);
	const auto Utf8B = StringCast<ANSICHAR>(*NameB);

	{
		FSpoutFrameSync First(MakeSpoutSdkFrameSyncBackend());
		TestEqual(TEXT("constructing with an empty pool pools nothing"), NumPooledSpoutSdkFrameSyncBackends(), 0);
		TestTrue(TEXT("a fresh SDK backend opens a name"), First.Open(Utf8A.Get()));
		First.PublishFrame();
	}
	TestEqual(TEXT("a closed SDK backend is pooled rather than destroyed"), NumPooledSpoutSdkFrameSyncBackends(), 1);

	{
		// The recycled backend must behave exactly like a fresh one for a DIFFERENT name: closed
		// on arrival, reopenable, and its frame count must be detectable by a peer.
		const double Start = FPlatformTime::Seconds();
		FSpoutFrameSync Sender(MakeSpoutSdkFrameSyncBackend());
		const double Elapsed = FPlatformTime::Seconds() - Start;
		TestEqual(TEXT("construction takes the pooled backend"), NumPooledSpoutSdkFrameSyncBackends(), 0);
		TestFalse(TEXT("a recycled backend arrives closed"), Sender.IsOpen());
		TestFalse(TEXT("a recycled backend has no pending frame"), Sender.HasPendingFrame());
		AddInfo(FString::Printf(TEXT("recycled backend construction took %.3f ms"), Elapsed * 1000.0));
		TestTrue(TEXT("a recycled backend opens a new name"), Sender.Open(Utf8B.Get()));

		FSpoutFrameSync Receiver(MakeSpoutSdkFrameSyncBackend());
		TestTrue(TEXT("a receiver opens the same name"), Receiver.Open(Utf8B.Get()));
		Receiver.RequestCurrentFrame();
		TestTrue(TEXT("the receiver's initial request is pending"), Receiver.ConsumeNewFrame());
		Receiver.MarkFrameConsumed();
		Sender.PublishFrame();
		TestTrue(TEXT("a frame published through the recycled backend is detected"), Receiver.ConsumeNewFrame());
		Receiver.MarkFrameConsumed();
		TestEqual(TEXT("the recycled backend counts its publications from zero"), Sender.GetPublishedFrameCount(), 1);
	}
	TestEqual(TEXT("both closed backends are pooled"), NumPooledSpoutSdkFrameSyncBackends(), 2);

	{
		FDisposableBackend::Destroyed = 0;
		FSpoutFrameSync Fake(MakeUnique<FDisposableBackend>());
		TestTrue(TEXT("a fake backend opens"), Fake.Open(Utf8A.Get()));
	}
	TestEqual(TEXT("a non-recyclable backend is destroyed"), FDisposableBackend::Destroyed, 1);
	TestEqual(TEXT("a non-recyclable backend is not pooled"), NumPooledSpoutSdkFrameSyncBackends(), 2);

	{
		FSpoutFrameSync Null(nullptr);
		TestFalse(TEXT("a null backend never opens"), Null.Open(Utf8A.Get()));
	}
	TestEqual(TEXT("a null backend is not pooled"), NumPooledSpoutSdkFrameSyncBackends(), 2);

	{
		const int32 Capacity = MaxPooledSpoutSdkFrameSyncBackends();
		TestTrue(TEXT("the pool has a positive bound"), Capacity > 0);
		TArray<TUniquePtr<FSpoutFrameSync>> Many;
		for (int32 Index = 0; Index < Capacity + 3; ++Index)
		{
			Many.Add(MakeUnique<FSpoutFrameSync>(MakeSpoutSdkFrameSyncBackend()));
		}
		TestEqual(TEXT("live streams drain the pool"), NumPooledSpoutSdkFrameSyncBackends(), 0);
		Many.Reset();
		TestEqual(TEXT("the pool never exceeds its bound"), NumPooledSpoutSdkFrameSyncBackends(), Capacity);
	}

	DrainSpoutSdkFrameSyncBackendPool();
	TestEqual(TEXT("draining destroys every pooled backend"), NumPooledSpoutSdkFrameSyncBackends(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutRestartPixels, "SpoutPlugin.Restart.SameNameRestartRecyclesBackendsAndDeliversPixels", SPOUT_RESTART_FLAGS)
bool FSpoutRestartPixels::RunTest(const FString&)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("d3ddebug")))
	{
		AddExpectedError(TEXT("ID3D12CompatibilityDevice::ReflectSharedProperties: Resource provided was not shared by D3D11, or with a D3D11 desc."), EAutomationExpectedErrorFlags::Contains, 0);
	}
	FSpoutD3DContext::Get().InitializeIfNeeded();
	if (!TestTrue(TEXT("real interop device available"), FSpoutD3DContext::Get().IsSpoutAvailable())) return false;

	USpoutBPFunctionLibrary::CloseAllStreams();
	FlushRenderingCommands();
	DrainSpoutSdkFrameSyncBackendPool();

	const FName Name(*UniqueRestartName(TEXT("Pixels")));
	const int32 W = 64, H = 32;
	TStrongObjectPtr<UTextureRenderTarget2D> Source(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
	Source->InitCustomFormat(W, H, PF_B8G8R8A8, true);
	UMaterialInstanceDynamic* Material = nullptr;
	UTexture2D* Texture = nullptr;

	// Every cycle is a full restart: both roles closed, both reopened under the same name. From the
	// second cycle on, the two registry entries must come from the pool (two fewer pooled objects
	// while open, two more once closed) and the recycled frame counting must still deliver the
	// new frame's pixels.
	for (int32 Cycle = 0; Cycle < 4; ++Cycle)
	{
		const int32 Sequence = 100 + Cycle * 7;
		const int32 PooledBefore = NumPooledSpoutSdkFrameSyncBackends();
		TestEqual(*FString::Printf(TEXT("cycle %d starts with the expected pool"), Cycle), PooledBefore, Cycle == 0 ? 0 : 2);

		QueueRestartPattern(Source.Get(), Sequence);
		TestTrue(*FString::Printf(TEXT("cycle %d send accepted"), Cycle), USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source.Get()));
		FlushRenderingCommands();
		TestTrue(*FString::Printf(TEXT("cycle %d receive accepted"), Cycle), USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, Material, Texture, nullptr, ESpoutColorSpace::Linear));
		FlushRenderingCommands();
		TestEqual(*FString::Printf(TEXT("cycle %d open streams took their backends from the pool"), Cycle), NumPooledSpoutSdkFrameSyncBackends(), 0);

		TArray<FColor> Actual;
		QueueRestartRead(Texture, W, H, Actual);
		FlushRenderingCommands();
		TestTrue(*FString::Printf(TEXT("cycle %d delivers the freshly published pixels"), Cycle), Actual == RestartPattern(W, H, Sequence));

		// A second frame in the same incarnation must also be counted as new by a recycled backend.
		QueueRestartPattern(Source.Get(), Sequence + 1);
		USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source.Get());
		FlushRenderingCommands();
		USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, Material, Texture, nullptr, ESpoutColorSpace::Linear);
		Actual.Reset();
		QueueRestartRead(Texture, W, H, Actual);
		FlushRenderingCommands();
		TestTrue(*FString::Printf(TEXT("cycle %d delivers the second frame"), Cycle), Actual == RestartPattern(W, H, Sequence + 1));

		USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material);
		USpoutBPFunctionLibrary::CloseSender(Name);
		FlushRenderingCommands();
		TestNull(TEXT("close clears the caller's texture handle"), Texture);
		TestFalse(TEXT("sender entry is gone after close"), FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Sender).IsValid());
		TestFalse(TEXT("receiver entry is gone after close"), FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Receiver).IsValid());
		TestEqual(*FString::Printf(TEXT("cycle %d close pooled both backends"), Cycle), NumPooledSpoutSdkFrameSyncBackends(), 2);
	}

	DrainSpoutSdkFrameSyncBackendPool();
	return true;
}
#endif
