// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5
//
// Async Receive (Beta): the per-session receive mode, tested with real transfers through the plugin's own sender
// and receiver in this process, on the shared-slot path (r.Spout.SharedSlotInterop 1, the default). What is observed
// is what actually ran, not the requested values: the effective mode in GetSpoutStreamStatus, the stream's worker
// object (its generation) and the process's count of live worker objects, and every texel of the received image.
//
//   default     both receive nodes and the C++ call stay synchronous unless asked, also with the retired
//               r.Spout.AsyncReceive set to 1
//   names       two names at once, one asynchronous and one synchronous; closing one leaves the other, its entry
//               and its slots exactly as they were; the closed name reopens in the other mode
//   same name   matching consumers share one session; a conflicting consumer is refused in either order and leaves
//               the session, the worker and the other consumers untouched; alternating calls never switch it; late
//               attachment, consumer removal, a receive cancelled by close, and close and reopen in the other mode
//   fallback    a request the asynchronous path cannot serve (an RGBA16F sender) runs synchronously, says why, and
//               starts no worker; the same name then served in BGRA8 starts one
//   identity    a sender that closes and restarts at another size keeps the session's mode, replaces the worker
//               with a new generation, and the image arrives exactly
//
// Nothing here is a performance statement.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
#include "SpoutAsyncReceive.h"
#include "SpoutBPFunctionLibrary.h"
#include "SpoutD3DContext.h"
#include "SpoutLimits.h"
#include "SpoutReceiver.h"
#include "SpoutSenderRegistry.h"
#include "SpoutSharedSlot.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Guid.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#define SPOUT_ASYNC_BETA_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

namespace SpoutAsyncBetaTests
{
	/** Every texel of a texture, read back the same way for a source and its received copy. */
	struct FPixels
	{
		TArray<FLinearColor> Linear;
		TArray<FFloat16Color> Half;
		bool operator==(const FPixels& O) const
		{
			if (Linear.Num() != O.Linear.Num() || Half.Num() != O.Half.Num()) return false;
			for (int32 I = 0; I < Linear.Num(); ++I) if (!(Linear[I] == O.Linear[I])) return false;
			for (int32 I = 0; I < Half.Num(); ++I)
			{
				if (Half[I].R.Encoded != O.Half[I].R.Encoded || Half[I].G.Encoded != O.Half[I].G.Encoded
					|| Half[I].B.Encoded != O.Half[I].B.Encoded || Half[I].A.Encoded != O.Half[I].A.Encoded) return false;
			}
			return true;
		}
		bool IsVaried() const
		{
			for (int32 I = 1; I < Linear.Num(); ++I) if (!(Linear[I] == Linear[0])) return true;
			for (int32 I = 1; I < Half.Num(); ++I) if (Half[I].R.Encoded != Half[0].R.Encoded || Half[I].G.Encoded != Half[0].G.Encoded) return true;
			return false;
		}
	};

	void ReadPixels(FRHITexture* Texture, FPixels& Out)
	{
		Out = FPixels();
		if (!Texture) return;
		const int32 W = Texture->GetDesc().Extent.X, H = Texture->GetDesc().Extent.Y;
		const EPixelFormat Format = Texture->GetFormat();
		FPixels* Target = &Out;
		ENQUEUE_RENDER_COMMAND(SpoutAsyncBetaRead)([Texture, W, H, Format, Target](FRHICommandListImmediate& Cmd)
		{
			Cmd.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
			if (Format == PF_FloatRGBA)
			{
				Cmd.ReadSurfaceFloatData(Texture, FIntRect(0, 0, W, H), Target->Half, CubeFace_PosX, 0, 0);
			}
			else
			{
				FReadSurfaceDataFlags Flags(RCM_MinMax);
				Flags.SetLinearToGamma(false);
				Cmd.ReadSurfaceData(Texture, FIntRect(0, 0, W, H), Target->Linear, Flags);
			}
			Cmd.Transition(FRHITransitionInfo(Texture, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		});
		FlushRenderingCommands();
	}

	/** Raw texels of a pattern that changes with Sequence, in Format's own layout (BGRA8, RGBA8 or RGBA16F). */
	TArray<uint8> MakePattern(EPixelFormat Format, int32 W, int32 H, int32 Sequence)
	{
		const int32 Bytes = Format == PF_FloatRGBA ? 8 : 4;
		TArray<uint8> Out;
		Out.SetNumZeroed(W * H * Bytes);
		for (int32 Y = 0; Y < H; ++Y)
		for (int32 X = 0; X < W; ++X)
		{
			const int32 I = Y * W + X;
			if (Format == PF_FloatRGBA)
			{
				FFloat16Color C(FLinearColor(float(X) / float(W), float(Y) / float(H), float((Sequence * 7) % 16) / 16.0f, float((X + Y) % 4) / 4.0f));
				FMemory::Memcpy(&Out[I * 8], &C, 8);
			}
			else
			{
				Out[I * 4 + 0] = uint8((X * 17 + Sequence) & 255);
				Out[I * 4 + 1] = uint8((Y * 29 + Sequence * 5) & 255);
				Out[I * 4 + 2] = uint8((X + Y + Sequence * 13) & 255);
				Out[I * 4 + 3] = uint8((X * 3 + Y * 7 + Sequence) & 255);
			}
		}
		return Out;
	}

	UTextureRenderTarget2D* MakeSource(int32 W, int32 H, EPixelFormat Format)
	{
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		Target->InitCustomFormat(W, H, Format, true);
		Target->UpdateResourceImmediate(true);
		return Target;
	}

	void Upload(UTextureRenderTarget2D* Target, int32 Sequence)
	{
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		const int32 W = Target->SizeX, H = Target->SizeY;
		const EPixelFormat Format = Target->GetFormat();
		TArray<uint8> Bytes = MakePattern(Format, W, H, Sequence);
		const int32 Pitch = W * (Format == PF_FloatRGBA ? 8 : 4);
		ENQUEUE_RENDER_COMMAND(SpoutAsyncBetaUpload)([Resource, W, H, Pitch, Bytes = MoveTemp(Bytes)](FRHICommandListImmediate& Cmd)
		{
			Cmd.UpdateTexture2D(Resource->GetRenderTargetTexture(), 0, FUpdateTextureRegion2D(0, 0, 0, 0, W, H), Pitch, Bytes.GetData());
		});
		FlushRenderingCommands();
	}

	FRHITexture* SourceTexture(UTextureRenderTarget2D* Target)
	{
		FTextureRenderTargetResource* Resource = Target ? Target->GameThread_GetRenderTargetResource() : nullptr;
		return Resource ? Resource->GetRenderTargetTexture() : nullptr;
	}

	FRHITexture* ReceivedTexture(UTexture2D* Texture)
	{
		FTextureResource* Resource = Texture ? Texture->GetResource() : nullptr;
		return Resource ? Resource->GetTextureRHI() : nullptr;
	}

	FName UniqueName(const TCHAR* What)
	{
		return FName(*FString::Printf(TEXT("SpoutAsyncBeta_%s_%s"), What, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	/** Publishes the source's current content under Name, completed on the render thread. */
	bool Send(const FName& Name, UTextureRenderTarget2D* Source)
	{
		const bool bAccepted = USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source);
		FlushRenderingCommands();
		return bAccepted;
	}

	/** One consumer of a name: which node it calls, what it asks for, and the outputs it keeps. */
	struct FConsumer
	{
		enum class ENode { Receiver, WithMaterial, Cpp };
		ENode Node = ENode::Receiver;
		bool bAsync = false;
		UTexture2D* Texture = nullptr;
		UMaterialInstanceDynamic* Material = nullptr;
	};

	/** One call of the consumer's node, completed on the render thread. */
	bool Call(const FName& Name, FConsumer& C)
	{
		bool bAccepted = false;
		switch (C.Node)
		{
		case FConsumer::ENode::Receiver:
			bAccepted = USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, C.Material, C.Texture, nullptr, ESpoutColorSpace::Linear, C.bAsync);
			break;
		case FConsumer::ENode::WithMaterial:
			bAccepted = USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(Name, C.Material, NAME_None, C.Texture, nullptr, ESpoutColorSpace::Linear, C.bAsync);
			break;
		case FConsumer::ENode::Cpp:
			bAccepted = FSpoutReceiver::Receive(Name, nullptr, NAME_None, C.Material, C.Texture, nullptr, ESpoutColorSpace::Linear, C.bAsync);
			break;
		}
		FlushRenderingCommands();
		return bAccepted;
	}

	TSharedPtr<FSpoutSharedSender> Entry(const FName& Name)
	{
		return FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Receiver);
	}

	/** The generation of the name's worker object, or 0 while it has none. Read after a flush: nothing changes it then. */
	uint64 WorkerGeneration(const FName& Name)
	{
		const TSharedPtr<FSpoutSharedSender> E = Entry(Name);
		return E && E->Async ? E->Async->GetGeneration() : 0;
	}

	/**
	 * Calls the consumer until the plugin-owned texture holds exactly what the source holds, or the timeout. The
	 * asynchronous worker copies on its own thread, so a delivery can take a few calls. False when a call was refused
	 * or the image never arrived.
	 */
	bool ReceiveExact(const FName& Name, FConsumer& C, UTextureRenderTarget2D* Source, double TimeoutSeconds = 5.0)
	{
		FPixels Sent;
		ReadPixels(SourceTexture(Source), Sent);
		if (!Sent.IsVaried()) return false;
		const double End = FPlatformTime::Seconds() + TimeoutSeconds;
		for (;;)
		{
			if (!Call(Name, C) || !C.Texture) return false;
			FPixels Got;
			ReadPixels(ReceivedTexture(C.Texture), Got);
			if (Got == Sent) return true;
			if (FPlatformTime::Seconds() > End) return false;
			FPlatformProcess::Sleep(0.002f);
		}
	}

	FSpoutStreamStatus Status(const FName& Name)
	{
		return USpoutBPFunctionLibrary::GetSpoutStreamStatus(Name, ESpoutType::Receiver);
	}

	void Close(const FName& Name, FConsumer& C)
	{
		USpoutBPFunctionLibrary::CloseReceiver(Name, C.Texture, C.Material);
		FlushRenderingCommands();
	}

	bool RequireSharedSlot(FAutomationTestBase& Test)
	{
		FSpoutD3DContext::Get().InitializeIfNeeded();
		return Test.TestTrue(TEXT("this process latched the shared-slot path (r.Spout.SharedSlotInterop 1), on which Async Receive (Beta) is tested"),
			SpoutInterop::IsSharedSlot());
	}

	/**
	 * Lets Unreal collect unreferenced outputs and release their render resources, then reads the retained budget once
	 * it stopped changing (or the timeout). Used before a test too, so that what earlier tests left for the garbage
	 * collector is not counted as this test's.
	 */
	int64 RetainedAfterCollection(double TimeoutSeconds = 3.0)
	{
		const double End = FPlatformTime::Seconds() + TimeoutSeconds;
		int64 Previous = -1;
		int32 Stable = 0;
		for (;;)
		{
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			FlushRenderingCommands();
			FSpoutGpuRetirement::Get().Poll();
			const int64 Now = SpoutLimits::RetainedBytes();
			Stable = Now == Previous ? Stable + 1 : 0;
			Previous = Now;
			if (Stable >= 3 || FPlatformTime::Seconds() > End) return Now;
			FPlatformProcess::Sleep(0.01f);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncBetaDefault, "SpoutPlugin.AsyncBeta.DefaultIsSynchronousAndTheRetiredSettingIsIgnored", SPOUT_ASYNC_BETA_FLAGS)
bool FSpoutAsyncBetaDefault::RunTest(const FString&)
{
	namespace AB = SpoutAsyncBetaTests;
	if (!AB::RequireSharedSlot(*this)) return false;

	// The retired process-wide setting is set to 1 for the whole test: it must not make anything asynchronous.
	IConsoleVariable* Retired = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.AsyncReceive"));
	if (!TestNotNull(TEXT("the retired setting is still registered"), Retired)) return false;
	const FString RetiredBefore = Retired->GetString();
	const EConsoleVariableFlags RetiredPriority = static_cast<EConsoleVariableFlags>(Retired->GetFlags() & ECVF_SetByMask);
	AddExpectedMessage(TEXT("r.Spout.AsyncReceive is retired and ignored"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	Retired->Set(TEXT("1"), ECVF_SetByCode);

	const int32 WorkersBefore = FSpoutAsyncStream::GetLiveStreams();
	struct FCase { const TCHAR* What; AB::FConsumer::ENode Node; };
	const FCase Cases[] = { { TEXT("Spout Receiver"), AB::FConsumer::ENode::Receiver }, { TEXT("Spout Receiver With Material"), AB::FConsumer::ENode::WithMaterial },
		{ TEXT("the C++ receive"), AB::FConsumer::ENode::Cpp } };
	for (const FCase& Case : Cases)
	{
		const FName Name = AB::UniqueName(TEXT("Default"));
		TStrongObjectPtr<UTextureRenderTarget2D> Source(AB::MakeSource(64, 32, PF_B8G8R8A8));
		AB::FConsumer C;
		C.Node = Case.Node; // bAsync left at its default
		for (int32 Sequence = 1; Sequence <= 3; ++Sequence)
		{
			AB::Upload(Source.Get(), Sequence);
			TestTrue(FString::Printf(TEXT("%s: send accepted"), Case.What), AB::Send(Name, Source.Get()));
			TestTrue(FString::Printf(TEXT("%s: sequence %d arrives exactly"), Case.What, Sequence), AB::ReceiveExact(Name, C, Source.Get()));
		}
		const FSpoutStreamStatus S = AB::Status(Name);
		TestTrue(FString::Printf(TEXT("%s: requested synchronous by default"), Case.What), S.RequestedReceiveMode == ESpoutReceiveMode::Synchronous);
		TestTrue(FString::Printf(TEXT("%s: and effective synchronous, with the retired setting at 1"), Case.What), S.EffectiveReceiveMode == ESpoutReceiveMode::Synchronous);
		TestTrue(FString::Printf(TEXT("%s: no note"), Case.What), S.ReceiveModeNote.IsEmpty());
		TestEqual(FString::Printf(TEXT("%s: no worker object for this name"), Case.What), AB::WorkerGeneration(Name), uint64(0));
		TestEqual(FString::Printf(TEXT("%s: and none in the process"), Case.What), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore);
		const TSharedPtr<FSpoutSharedSender> E = AB::Entry(Name);
		TestTrue(FString::Printf(TEXT("%s: delivered through the synchronous shared slots"), Case.What), E && E->SlotReceive.IsValid() && E->SlotTransfers.load() >= 3);
		AB::Close(Name, C);
		USpoutBPFunctionLibrary::CloseSender(Name);
		FlushRenderingCommands();
	}

	Retired->Set(*RetiredBefore, ECVF_SetByCode);
	Retired->ClearFlags(ECVF_SetByMask);
	Retired->SetFlags(RetiredPriority);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncBetaNames, "SpoutPlugin.AsyncBeta.TwoNamesKeepTheirOwnModes", SPOUT_ASYNC_BETA_FLAGS)
bool FSpoutAsyncBetaNames::RunTest(const FString&)
{
	namespace AB = SpoutAsyncBetaTests;
	if (!AB::RequireSharedSlot(*this)) return false;
	const int32 WorkersBefore = FSpoutAsyncStream::GetLiveStreams();
	const int64 RetainedBefore = AB::RetainedAfterCollection();
	const FName A = AB::UniqueName(TEXT("NameAsync")), B = AB::UniqueName(TEXT("NameSync"));
	TStrongObjectPtr<UTextureRenderTarget2D> SourceA(AB::MakeSource(64, 32, PF_B8G8R8A8));
	TStrongObjectPtr<UTextureRenderTarget2D> SourceB(AB::MakeSource(48, 24, PF_R8G8B8A8));
	AB::FConsumer ConsumerA; ConsumerA.Node = AB::FConsumer::ENode::Receiver; ConsumerA.bAsync = true;
	AB::FConsumer ConsumerB; ConsumerB.Node = AB::FConsumer::ENode::WithMaterial; ConsumerB.bAsync = false;

	// Both names at once, interleaved.
	for (int32 Sequence = 1; Sequence <= 3; ++Sequence)
	{
		AB::Upload(SourceA.Get(), Sequence); AB::Upload(SourceB.Get(), Sequence + 40);
		AB::Send(A, SourceA.Get()); AB::Send(B, SourceB.Get());
		TestTrue(FString::Printf(TEXT("A (asynchronous): sequence %d arrives exactly"), Sequence), AB::ReceiveExact(A, ConsumerA, SourceA.Get()));
		TestTrue(FString::Printf(TEXT("B (synchronous): sequence %d arrives exactly"), Sequence), AB::ReceiveExact(B, ConsumerB, SourceB.Get()));
	}
	const FSpoutStreamStatus StatusA = AB::Status(A), StatusB = AB::Status(B);
	TestTrue(TEXT("A requested Async Receive (Beta)"), StatusA.RequestedReceiveMode == ESpoutReceiveMode::AsyncBeta);
	TestTrue(TEXT("and A actually uses it"), StatusA.EffectiveReceiveMode == ESpoutReceiveMode::AsyncBeta);
	TestTrue(TEXT("A has no fallback note"), StatusA.ReceiveModeNote.IsEmpty());
	TestTrue(TEXT("B requested the synchronous receive"), StatusB.RequestedReceiveMode == ESpoutReceiveMode::Synchronous);
	TestTrue(TEXT("and B uses it"), StatusB.EffectiveReceiveMode == ESpoutReceiveMode::Synchronous);
	// The status counts what each path delivered: three exact sequences each.
	TestTrue(TEXT("A's status counts its asynchronous deliveries"), StatusA.bHasFrame && StatusA.FramesSubmitted >= 3);
	TestTrue(TEXT("B's status counts its synchronous deliveries"), StatusB.bHasFrame && StatusB.FramesSubmitted >= 3);
	const uint64 GenerationA = AB::WorkerGeneration(A);
	TestTrue(TEXT("A has a worker object"), GenerationA != 0);
	TestEqual(TEXT("B has none"), AB::WorkerGeneration(B), uint64(0));
	TestEqual(TEXT("exactly one worker in the process"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore + 1);
	TSharedPtr<FSpoutSharedSender> EntryB = AB::Entry(B);
	const uint64 SlotGenerationB = EntryB && EntryB->SlotReceive ? EntryB->SlotReceive->GetGeneration() : 0;
	TestTrue(TEXT("B delivers through its synchronous slots"), SlotGenerationB != 0);

	// Closing A changes nothing of B.
	AB::Close(A, ConsumerA);
	TestFalse(TEXT("A's session is gone"), AB::Entry(A).IsValid());
	TestEqual(TEXT("and with it A's worker: none left in the process"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore);
	AB::Upload(SourceB.Get(), 77); AB::Send(B, SourceB.Get());
	TestTrue(TEXT("B still arrives exactly after A closed"), AB::ReceiveExact(B, ConsumerB, SourceB.Get()));
	TestTrue(TEXT("B's session is the same object"), AB::Entry(B) == EntryB);
	TestEqual(TEXT("B's slots were not replaced"), EntryB && EntryB->SlotReceive ? EntryB->SlotReceive->GetGeneration() : 0, SlotGenerationB);
	TestTrue(TEXT("B is still synchronous"), AB::Status(B).EffectiveReceiveMode == ESpoutReceiveMode::Synchronous);

	// A reopens in the other mode: a new session, synchronous, no worker.
	ConsumerA.bAsync = false;
	AB::Upload(SourceA.Get(), 9); AB::Send(A, SourceA.Get());
	TestTrue(TEXT("A reopened synchronously arrives exactly"), AB::ReceiveExact(A, ConsumerA, SourceA.Get()));
	TestTrue(TEXT("A now requests the synchronous receive"), AB::Status(A).RequestedReceiveMode == ESpoutReceiveMode::Synchronous);
	TestTrue(TEXT("and uses it"), AB::Status(A).EffectiveReceiveMode == ESpoutReceiveMode::Synchronous);
	TestEqual(TEXT("no worker was started for it"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore);

	AB::Close(A, ConsumerA); AB::Close(B, ConsumerB);
	EntryB.Reset(); // this test's own reference to B's session, which would keep it (and its reservations) alive
	USpoutBPFunctionLibrary::CloseSender(A); USpoutBPFunctionLibrary::CloseSender(B);
	FlushRenderingCommands();
	SourceA.Reset(); SourceB.Reset();
	TestEqual(TEXT("everything closed: the retained-memory budget is back where it was"), AB::RetainedAfterCollection(), RetainedBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncBetaSameName, "SpoutPlugin.AsyncBeta.SameNameConsumersShareOneMode", SPOUT_ASYNC_BETA_FLAGS)
bool FSpoutAsyncBetaSameName::RunTest(const FString&)
{
	namespace AB = SpoutAsyncBetaTests;
	if (!AB::RequireSharedSlot(*this)) return false;
	AddExpectedMessage(TEXT("refused a receive asking for"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	const int32 WorkersBefore = FSpoutAsyncStream::GetLiveStreams();
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/SpoutPlugin/Content/M_SpoutMaterial.M_SpoutMaterial"));
	if (!Base) Base = UMaterial::GetDefaultMaterial(MD_Surface);
	TStrongObjectPtr<UMaterialInstanceDynamic> SharedMid(UMaterialInstanceDynamic::Create(Base, GetTransientPackage()));
	TStrongObjectPtr<UMaterialInstanceDynamic> LateMid(UMaterialInstanceDynamic::Create(Base, GetTransientPackage()));
	// A texture of the caller's own that a refused call must leave in the caller's variable.
	TStrongObjectPtr<UTexture2D> CallerTexture(NewObject<UTexture2D>(GetTransientPackage()));
	UTexture2D* const Untouched = CallerTexture.Get();

	// --- Asynchronous first. ---
	const FName N = AB::UniqueName(TEXT("SameAsync"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AB::MakeSource(64, 32, PF_B8G8R8A8));
	AB::FConsumer First; First.Node = AB::FConsumer::ENode::Receiver; First.bAsync = true;
	AB::FConsumer Matching; Matching.Node = AB::FConsumer::ENode::WithMaterial; Matching.bAsync = true; Matching.Material = SharedMid.Get();
	AB::FConsumer Conflicting; Conflicting.Node = AB::FConsumer::ENode::WithMaterial; Conflicting.bAsync = false;
	AB::Upload(Source.Get(), 1); AB::Send(N, Source.Get());
	TestTrue(TEXT("the first consumer (asynchronous) arrives exactly"), AB::ReceiveExact(N, First, Source.Get()));
	const uint64 Generation = AB::WorkerGeneration(N);
	TestTrue(TEXT("the session runs a worker"), Generation != 0);
	TestTrue(TEXT("a matching consumer through the other node is accepted"), AB::Call(N, Matching));
	TestTrue(TEXT("and shares the one received texture"), Matching.Texture == First.Texture);
	Conflicting.Texture = Untouched;
	TestFalse(TEXT("a consumer asking for the synchronous receive is refused"), AB::Call(N, Conflicting));
	TestTrue(TEXT("and its outputs are untouched"), Conflicting.Texture == Untouched && Conflicting.Material == nullptr);

	// Alternating contradictory calls: the mode never moves, the worker is never replaced, the image keeps arriving.
	for (int32 Round = 2; Round <= 11; ++Round)
	{
		AB::Upload(Source.Get(), Round); AB::Send(N, Source.Get());
		Conflicting.Texture = Untouched;
		TestFalse(FString::Printf(TEXT("round %d: the conflicting consumer is refused before"), Round), AB::Call(N, Conflicting));
		TestTrue(FString::Printf(TEXT("round %d: the first consumer still arrives exactly"), Round), AB::ReceiveExact(N, First, Source.Get()));
		TestFalse(FString::Printf(TEXT("round %d: and refused after"), Round), AB::Call(N, Conflicting));
		TestTrue(FString::Printf(TEXT("round %d: the session is still asynchronous"), Round), AB::Status(N).EffectiveReceiveMode == ESpoutReceiveMode::AsyncBeta);
		TestEqual(FString::Printf(TEXT("round %d: with the same worker object"), Round), AB::WorkerGeneration(N), Generation);
	}
	TestEqual(TEXT("one worker in the process throughout"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore + 1);

	// Late attachment with the matching mode, and removal of one consumer's material binding.
	AB::FConsumer Late; Late.Node = AB::FConsumer::ENode::WithMaterial; Late.bAsync = true; Late.Material = LateMid.Get();
	TestTrue(TEXT("a late matching consumer is accepted"), AB::Call(N, Late));
	TestTrue(TEXT("and gets the shared texture at once"), Late.Texture == First.Texture);
	TestTrue(TEXT("one consumer's material binding is released"), USpoutBPFunctionLibrary::ReleaseReceiverMaterialBinding(N, SharedMid.Get(), NAME_None));
	AB::Upload(Source.Get(), 50); AB::Send(N, Source.Get());
	TestTrue(TEXT("the session continues for the others"), AB::ReceiveExact(N, First, Source.Get()));
	TestTrue(TEXT("still asynchronous, same worker"), AB::Status(N).EffectiveReceiveMode == ESpoutReceiveMode::AsyncBeta && AB::WorkerGeneration(N) == Generation);

	// A receive cancelled by close: queued, then closed before the render thread runs it. No worker survives.
	First.bAsync = true;
	USpoutBPFunctionLibrary::SpoutReceiver(N, nullptr, NAME_None, First.Material, First.Texture, nullptr, ESpoutColorSpace::Linear, true);
	USpoutBPFunctionLibrary::CloseReceiver(N, First.Texture, First.Material);
	FlushRenderingCommands();
	TestFalse(TEXT("closed: the session is gone"), AB::Entry(N).IsValid());
	TestEqual(TEXT("closed with a receive still queued: no worker left"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore);

	// Close and reopen in the other mode: the earlier asynchronous consumers are now the conflicting ones.
	AB::FConsumer Reopened; Reopened.Node = AB::FConsumer::ENode::WithMaterial; Reopened.bAsync = false;
	AB::Upload(Source.Get(), 60); AB::Send(N, Source.Get());
	TestTrue(TEXT("reopened synchronously, the image arrives exactly"), AB::ReceiveExact(N, Reopened, Source.Get()));
	TestTrue(TEXT("reopened: requested synchronous"), AB::Status(N).RequestedReceiveMode == ESpoutReceiveMode::Synchronous);
	First.Texture = Untouched;
	TestFalse(TEXT("an asynchronous consumer is now refused"), AB::Call(N, First));
	TestTrue(TEXT("with its outputs untouched"), First.Texture == Untouched);
	TestEqual(TEXT("and no worker was started by it"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore);
	First.Texture = nullptr;
	AB::Close(N, Reopened);
	USpoutBPFunctionLibrary::CloseSender(N);
	FlushRenderingCommands();

	// --- Synchronous first, then asynchronous: the reversed order. ---
	const FName M = AB::UniqueName(TEXT("SameSync"));
	AB::FConsumer SyncFirst; SyncFirst.Node = AB::FConsumer::ENode::WithMaterial; SyncFirst.bAsync = false;
	AB::FConsumer AsyncLater; AsyncLater.Node = AB::FConsumer::ENode::Receiver; AsyncLater.bAsync = true;
	AB::Upload(Source.Get(), 70); AB::Send(M, Source.Get());
	TestTrue(TEXT("reversed: the synchronous consumer arrives exactly"), AB::ReceiveExact(M, SyncFirst, Source.Get()));
	AsyncLater.Texture = Untouched;
	TestFalse(TEXT("reversed: the asynchronous consumer is refused"), AB::Call(M, AsyncLater));
	TestTrue(TEXT("reversed: with its outputs untouched"), AsyncLater.Texture == Untouched);
	TestTrue(TEXT("reversed: the session stays synchronous"), AB::Status(M).RequestedReceiveMode == ESpoutReceiveMode::Synchronous
		&& AB::Status(M).EffectiveReceiveMode == ESpoutReceiveMode::Synchronous);
	TestEqual(TEXT("reversed: no worker"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore);
	AsyncLater.Texture = nullptr;
	AB::Close(M, SyncFirst);
	USpoutBPFunctionLibrary::CloseSender(M);
	FlushRenderingCommands();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncBetaFallback, "SpoutPlugin.AsyncBeta.AnUnservableRequestRunsSynchronouslyAndSaysWhy", SPOUT_ASYNC_BETA_FLAGS)
bool FSpoutAsyncBetaFallback::RunTest(const FString&)
{
	namespace AB = SpoutAsyncBetaTests;
	if (!AB::RequireSharedSlot(*this)) return false;
	AddExpectedMessage(TEXT("was requested but is not used"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	const int32 WorkersBefore = FSpoutAsyncStream::GetLiveStreams();
	const FName Name = AB::UniqueName(TEXT("Fallback"));
	AB::FConsumer C; C.Node = AB::FConsumer::ENode::Receiver; C.bAsync = true;

	// An RGBA16F sender: the asynchronous path handles BGRA8 and RGBA8 only.
	TStrongObjectPtr<UTextureRenderTarget2D> Half(AB::MakeSource(32, 16, PF_FloatRGBA));
	for (int32 Sequence = 1; Sequence <= 2; ++Sequence)
	{
		AB::Upload(Half.Get(), Sequence); AB::Send(Name, Half.Get());
		TestTrue(FString::Printf(TEXT("RGBA16F: sequence %d still arrives exactly, synchronously"), Sequence), AB::ReceiveExact(Name, C, Half.Get()));
	}
	FSpoutStreamStatus S = AB::Status(Name);
	TestTrue(TEXT("RGBA16F: requested Async Receive (Beta)"), S.RequestedReceiveMode == ESpoutReceiveMode::AsyncBeta);
	TestTrue(TEXT("RGBA16F: but effective synchronous"), S.EffectiveReceiveMode == ESpoutReceiveMode::Synchronous);
	TestTrue(TEXT("RGBA16F: and the status says why"), S.ReceiveModeNote.Contains(TEXT("BGRA8 and RGBA8")));
	TestEqual(TEXT("RGBA16F: no worker"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore);
	AddInfo(FString::Printf(TEXT("fallback note: %s"), *S.ReceiveModeNote));

	// The same name, the sender restarted in BGRA8: a new sender identity the asynchronous path serves.
	USpoutBPFunctionLibrary::CloseSender(Name);
	FlushRenderingCommands();
	TStrongObjectPtr<UTextureRenderTarget2D> Bgra(AB::MakeSource(32, 16, PF_B8G8R8A8));
	AB::Upload(Bgra.Get(), 3); AB::Send(Name, Bgra.Get());
	C.Texture = nullptr; // the format changed: the session's texture is replaced, the old one is no longer this caller's
	TestTrue(TEXT("BGRA8: arrives exactly, asynchronously"), AB::ReceiveExact(Name, C, Bgra.Get()));
	S = AB::Status(Name);
	TestTrue(TEXT("BGRA8: effective Async Receive (Beta)"), S.EffectiveReceiveMode == ESpoutReceiveMode::AsyncBeta);
	TestTrue(TEXT("BGRA8: the note is cleared"), S.ReceiveModeNote.IsEmpty());
	TestEqual(TEXT("BGRA8: one worker"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore + 1);
	AB::Close(Name, C);
	USpoutBPFunctionLibrary::CloseSender(Name);
	FlushRenderingCommands();
	TestEqual(TEXT("closed: no worker left"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncBetaIdentity, "SpoutPlugin.AsyncBeta.SenderRestartAndResizeKeepTheMode", SPOUT_ASYNC_BETA_FLAGS)
bool FSpoutAsyncBetaIdentity::RunTest(const FString&)
{
	namespace AB = SpoutAsyncBetaTests;
	if (!AB::RequireSharedSlot(*this)) return false;
	const int32 WorkersBefore = FSpoutAsyncStream::GetLiveStreams();
	const FName Name = AB::UniqueName(TEXT("Identity"));
	AB::FConsumer C; C.Node = AB::FConsumer::ENode::WithMaterial; C.bAsync = true;
	TStrongObjectPtr<UTextureRenderTarget2D> Small(AB::MakeSource(64, 32, PF_B8G8R8A8));
	AB::Upload(Small.Get(), 1); AB::Send(Name, Small.Get());
	TestTrue(TEXT("64x32: arrives exactly"), AB::ReceiveExact(Name, C, Small.Get()));
	const uint64 First = AB::WorkerGeneration(Name);
	TestTrue(TEXT("64x32: a worker runs"), First != 0);
	const uint64 DeliveredFirst = AB::Entry(Name) ? AB::Entry(Name)->ReceivedFrameNumber : 0;

	// The sender drops out: receives are refused while it is gone, and the session keeps its mode.
	USpoutBPFunctionLibrary::CloseSender(Name);
	FlushRenderingCommands();
	UTexture2D* const Held = C.Texture;
	TestFalse(TEXT("sender gone: the receive is refused"), AB::Call(Name, C));
	TestTrue(TEXT("sender gone: the caller keeps its texture"), C.Texture == Held);
	TestTrue(TEXT("sender gone: the session still requests Async Receive (Beta)"), AB::Status(Name).RequestedReceiveMode == ESpoutReceiveMode::AsyncBeta);

	// It restarts at another size: a new identity, a new worker generation, the same mode.
	TStrongObjectPtr<UTextureRenderTarget2D> Large(AB::MakeSource(96, 48, PF_B8G8R8A8));
	AB::Upload(Large.Get(), 2); AB::Send(Name, Large.Get());
	TestTrue(TEXT("96x48: arrives exactly"), AB::ReceiveExact(Name, C, Large.Get()));
	const uint64 Second = AB::WorkerGeneration(Name);
	TestTrue(TEXT("96x48: a new worker generation"), Second != 0 && Second != First);
	TestTrue(TEXT("96x48: still asynchronous"), AB::Status(Name).EffectiveReceiveMode == ESpoutReceiveMode::AsyncBeta);
	TestEqual(TEXT("96x48: one worker in the process, the old one is gone"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore + 1);
	TestTrue(TEXT("the delivery count only grew"), AB::Entry(Name) && AB::Entry(Name)->ReceivedFrameNumber > DeliveredFirst);
	for (int32 Sequence = 3; Sequence <= 5; ++Sequence)
	{
		AB::Upload(Large.Get(), Sequence); AB::Send(Name, Large.Get());
		TestTrue(FString::Printf(TEXT("96x48: sequence %d arrives exactly"), Sequence), AB::ReceiveExact(Name, C, Large.Get()));
	}
	TestEqual(TEXT("no worker churn between frames"), AB::WorkerGeneration(Name), Second);
	AB::Close(Name, C);
	USpoutBPFunctionLibrary::CloseSender(Name);
	FlushRenderingCommands();
	TestEqual(TEXT("closed: no worker left"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore);
	return true;
}

// ---------------------------------------------------------------------------------------------
// Completion before release. The worker gives the sender's access back only once its copy is proven
// finished or its device was removed: a copy that finishes after the 100 ms bound, a copy whose fence signal failed,
// and a stream closed or stopped with a copy outstanding. "Held" is checked twice: by the plugin's own state and by an
// independent holder that tries to take the sender's named access mutex. A stream's objects go to the retirement list
// and are released only once their work is proven finished.
// ---------------------------------------------------------------------------------------------

namespace SpoutAsyncBetaTests
{
	/** Whether another holder could take Name's Spout access mutex now: taken with no wait and given back at once, on this thread. */
	bool AccessFreeForAnotherHolder(const FName& Name)
	{
		const FString Text = Name.ToString();
		const auto Utf8 = StringCast<ANSICHAR>(*Text);
		TUniquePtr<ISpoutFrameSyncBackend> Probe = MakeSpoutSdkFrameSyncBackend();
		bool bGot = false;
		if (Probe && Probe->CreateAccessMutex(Utf8.Get()))
		{
			bGot = Probe->CheckTextureAccess(nullptr, 0);
			if (bGot)
			{
				Probe->AllowTextureAccess(nullptr);
			}
			Probe->CloseAccessMutex();
		}
		RecycleSpoutSdkFrameSyncBackend(MoveTemp(Probe));
		return bGot;
	}

	/** Polls Predicate until it is true or TimeoutSeconds passed. Returns when it became true, or a negative number. */
	template <typename FPredicate>
	double WaitUntil(FPredicate&& Predicate, double TimeoutSeconds)
	{
		const double End = FPlatformTime::Seconds() + TimeoutSeconds;
		for (;;)
		{
			if (Predicate()) return FPlatformTime::Seconds();
			if (FPlatformTime::Seconds() > End) return -1.0;
			FPlatformProcess::SleepNoStats(0.001f);
		}
	}

	/** Whether the name's asynchronous worker holds the sender's access (the plugin's own record). */
	bool WorkerHoldsAccess(const FName& Name)
	{
		const TSharedPtr<FSpoutSharedSender> E = Entry(Name);
		return E && E->FrameSync.IsAccessHeld();
	}

	FSpoutAsyncStats WorkerStats(const FName& Name)
	{
		const TSharedPtr<FSpoutSharedSender> E = Entry(Name);
		return E && E->Async ? E->Async->GetStats() : FSpoutAsyncStats();
	}

	/** Waits until the retirement list released at least Count more sets than Before (polling it), or the timeout. */
	bool WaitForReleases(int64 Before, int64 Count, double TimeoutSeconds)
	{
		return WaitUntil([&] { FSpoutGpuRetirement::Get().Poll(); return FSpoutGpuRetirement::Get().GetStats().Released >= Before + Count; }, TimeoutSeconds) > 0.0;
	}

	/** Sends the first image of a 64x32 BGRA8 source and receives it asynchronously. False when it did not arrive that way. */
	bool StartAsync(FAutomationTestBase& Test, const FName& Name, FConsumer& C, UTextureRenderTarget2D* Source)
	{
		Upload(Source, 1);
		Send(Name, Source);
		const bool bArrived = ReceiveExact(Name, C, Source);
		Test.TestTrue(TEXT("the first image arrives"), bArrived);
		const bool bAsync = Status(Name).EffectiveReceiveMode == ESpoutReceiveMode::AsyncBeta && Entry(Name) && Entry(Name)->Async;
		Test.TestTrue(TEXT("through Async Receive (Beta)"), bAsync);
		return bArrived && bAsync;
	}

	void CloseBoth(const FName& Name, FConsumer& C)
	{
		Close(Name, C);
		USpoutBPFunctionLibrary::CloseSender(Name);
		FlushRenderingCommands();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncBetaLateCopy, "SpoutPlugin.AsyncBeta.ACopyFinishingLateKeepsTheSendersAccessUntilItIsProven", SPOUT_ASYNC_BETA_FLAGS)
bool FSpoutAsyncBetaLateCopy::RunTest(const FString&)
{
	namespace AB = SpoutAsyncBetaTests;
	if (!AB::RequireSharedSlot(*this)) return false;
	FSpoutAsyncDevice::Get().ReenableForTest();
	ON_SCOPE_EXIT { SpoutAsyncReceiveTestHooks::DelayCompletionMs = 0; };
	const FName Name = AB::UniqueName(TEXT("Late"));
	AB::FConsumer C; C.Node = AB::FConsumer::ENode::Receiver; C.bAsync = true;
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AB::MakeSource(64, 32, PF_B8G8R8A8));
	if (!AB::StartAsync(*this, Name, C, Source.Get())) { AB::CloseBoth(Name, C); return false; }
	const FSpoutAsyncStats Before = AB::WorkerStats(Name);

	// The worker's next copy is not finished before 400 ms, four times its 100 ms bound.
	constexpr int32 DelayMs = 400;
	SpoutAsyncReceiveTestHooks::DelayCompletionMs = DelayMs;
	AB::Upload(Source.Get(), 2);
	AB::Send(Name, Source.Get());
	const double Held = AB::WaitUntil([&] { return AB::WorkerHoldsAccess(Name); }, 2.0);
	TestTrue(TEXT("the worker took the sender's access for the new image"), Held > 0.0);
	if (Held > 0.0)
	{
		while (FPlatformTime::Seconds() < Held + 0.2) { FPlatformProcess::SleepNoStats(0.005f); }
		TestTrue(TEXT("200 ms in, past the bound: the worker still holds access"), AB::WorkerHoldsAccess(Name));
		TestFalse(TEXT("and no other holder can take the sender's access mutex"), AB::AccessFreeForAnotherHolder(Name));
		const double Freed = AB::WaitUntil([&] { return !AB::WorkerHoldsAccess(Name); }, 5.0);
		TestTrue(TEXT("access is given back once the copy is proven finished"), Freed > 0.0);
		TestTrue(FString::Printf(TEXT("not before the copy finished (held %.0f ms; the copy took %d)"), (Freed - Held) * 1000.0, DelayMs),
			Freed > 0.0 && (Freed - Held) * 1000.0 >= DelayMs - 50.0);
	}
	const FSpoutAsyncStats After = AB::WorkerStats(Name);
	TestTrue(TEXT("the late copy was discarded, never published"), After.CompletionTimeouts >= Before.CompletionTimeouts + 1);
	TestTrue(TEXT("the image still arrives exactly, from a later copy"), AB::ReceiveExact(Name, C, Source.Get()));
	TestTrue(TEXT("still through Async Receive (Beta): one late copy does not end the stream"), AB::Status(Name).EffectiveReceiveMode == ESpoutReceiveMode::AsyncBeta);
	TestTrue(TEXT("the sender's access is free again"), AB::WaitUntil([&] { return AB::AccessFreeForAnotherHolder(Name); }, 1.0) > 0.0);
	TestEqual(TEXT("no rule violation in the slot bookkeeping"), AB::WorkerStats(Name).RuleViolations, int64(0));
	AB::CloseBoth(Name, C);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncBetaFailedSignal, "SpoutPlugin.AsyncBeta.AFailedSignalKeepsAccessUntilTheQueryProvesTheCopyThenFallsBack", SPOUT_ASYNC_BETA_FLAGS)
bool FSpoutAsyncBetaFailedSignal::RunTest(const FString&)
{
	namespace AB = SpoutAsyncBetaTests;
	if (!AB::RequireSharedSlot(*this)) return false;
	FSpoutAsyncDevice::Get().ReenableForTest();
	ON_SCOPE_EXIT
	{
		SpoutAsyncReceiveTestHooks::FailSignals = 0;
		SpoutAsyncReceiveTestHooks::DelayCompletionMs = 0;
		FSpoutAsyncDevice::Get().ReenableForTest();
	};
	AddExpectedMessage(TEXT("asynchronous receive is off for this process"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	// The pattern is a regular expression: no parentheses.
	AddExpectedMessage(TEXT("stopped .a written-fence signal failed"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const int32 WorkersBefore = FSpoutAsyncStream::GetLiveStreams();
	const FName Name = AB::UniqueName(TEXT("FailedSignal"));
	AB::FConsumer C; C.Node = AB::FConsumer::ENode::Receiver; C.bAsync = true;
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AB::MakeSource(64, 32, PF_B8G8R8A8));
	if (!AB::StartAsync(*this, Name, C, Source.Get())) { AB::CloseBoth(Name, C); return false; }
	const int64 ReleasedBefore = FSpoutGpuRetirement::Get().GetStats().Released;

	// The next copy's signal fails; its proof (the event query) is not accepted before 300 ms.
	constexpr int32 DelayMs = 300;
	SpoutAsyncReceiveTestHooks::FailSignals = 1;
	SpoutAsyncReceiveTestHooks::DelayCompletionMs = DelayMs;
	AB::Upload(Source.Get(), 2);
	AB::Send(Name, Source.Get());
	const double Held = AB::WaitUntil([&] { return AB::WorkerHoldsAccess(Name); }, 2.0);
	TestTrue(TEXT("the worker took the sender's access"), Held > 0.0);
	if (Held > 0.0)
	{
		while (FPlatformTime::Seconds() < Held + 0.15) { FPlatformProcess::SleepNoStats(0.005f); }
		TestTrue(TEXT("with no fence value to prove the copy, access is still held"), AB::WorkerHoldsAccess(Name));
		TestFalse(TEXT("and no other holder can take the sender's access mutex"), AB::AccessFreeForAnotherHolder(Name));
		const double Freed = AB::WaitUntil([&] { return !AB::WorkerHoldsAccess(Name); }, 5.0);
		TestTrue(FString::Printf(TEXT("access is given back once the query proved the copy (held %.0f ms; the proof took %d)"), (Freed - Held) * 1000.0, DelayMs),
			Freed > 0.0 && (Freed - Held) * 1000.0 >= DelayMs - 50.0);
	}
	TestTrue(TEXT("the stream failed"), AB::WaitUntil([&] { return AB::Entry(Name) && (!AB::Entry(Name)->Async || AB::Entry(Name)->Async->HasFailed()); }, 2.0) > 0.0);
	TestTrue(TEXT("a failed signal turns the asynchronous path off for the process, as before"), FSpoutAsyncDevice::Get().IsDisabled());
	TestTrue(TEXT("the image still arrives exactly, through the synchronous fallback"), AB::ReceiveExact(Name, C, Source.Get()));
	const FSpoutStreamStatus S = AB::Status(Name);
	TestTrue(TEXT("the effective mode is synchronous"), S.EffectiveReceiveMode == ESpoutReceiveMode::Synchronous);
	TestFalse(TEXT("and the status says why"), S.ReceiveModeNote.IsEmpty());
	TestEqual(TEXT("the failed worker is gone"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore);
	TestTrue(TEXT("its objects were retired and released once their work was proven finished"), AB::WaitForReleases(ReleasedBefore, 1, 5.0));
	AB::CloseBoth(Name, C);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncBetaCloseOutstanding, "SpoutPlugin.AsyncBeta.ClosingWithACopyOutstandingWaitsForItAndRetiresTheStream", SPOUT_ASYNC_BETA_FLAGS)
bool FSpoutAsyncBetaCloseOutstanding::RunTest(const FString&)
{
	namespace AB = SpoutAsyncBetaTests;
	if (!AB::RequireSharedSlot(*this)) return false;
	FSpoutAsyncDevice::Get().ReenableForTest();
	ON_SCOPE_EXIT { SpoutAsyncReceiveTestHooks::DelayCompletionMs = 0; };
	const int32 WorkersBefore = FSpoutAsyncStream::GetLiveStreams();
	const FName Name = AB::UniqueName(TEXT("CloseOutstanding"));
	AB::FConsumer C; C.Node = AB::FConsumer::ENode::Receiver; C.bAsync = true;
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AB::MakeSource(64, 32, PF_B8G8R8A8));
	if (!AB::StartAsync(*this, Name, C, Source.Get())) { AB::CloseBoth(Name, C); return false; }

	constexpr int32 DelayMs = 500;
	SpoutAsyncReceiveTestHooks::DelayCompletionMs = DelayMs;
	AB::Upload(Source.Get(), 2);
	AB::Send(Name, Source.Get());
	const double Held = AB::WaitUntil([&] { return AB::WorkerHoldsAccess(Name); }, 2.0);
	TestTrue(TEXT("the worker holds the sender's access for a copy outstanding"), Held > 0.0);
	const FSpoutGpuRetirement::FStats Before = FSpoutGpuRetirement::Get().GetStats();
	AB::Close(Name, C);
	const double Closed = FPlatformTime::Seconds();
	TestEqual(TEXT("the close joined the worker"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore);
	if (Held > 0.0)
	{
		TestTrue(FString::Printf(TEXT("the close waited until the copy was proven finished (%.0f ms after access was taken; the copy took %d)"),
			(Closed - Held) * 1000.0, DelayMs), (Closed - Held) * 1000.0 >= DelayMs - 50.0);
	}
	TestTrue(TEXT("the sender's access is free once the close returned"), AB::AccessFreeForAnotherHolder(Name));
	TestTrue(TEXT("the stream's objects went to the retirement list"), FSpoutGpuRetirement::Get().GetStats().Retired >= Before.Retired + 1);
	TestTrue(TEXT("and were released once their work was proven finished"), AB::WaitForReleases(Before.Released, 1, 5.0));
	USpoutBPFunctionLibrary::CloseSender(Name);
	FlushRenderingCommands();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncBetaStopOutstanding, "SpoutPlugin.AsyncBeta.ASessionStopWithACopyOutstandingWaitsForItAndRetiresTheStream", SPOUT_ASYNC_BETA_FLAGS)
bool FSpoutAsyncBetaStopOutstanding::RunTest(const FString&)
{
	namespace AB = SpoutAsyncBetaTests;
	if (!AB::RequireSharedSlot(*this)) return false;
	FSpoutAsyncDevice::Get().ReenableForTest();
	ON_SCOPE_EXIT { SpoutAsyncReceiveTestHooks::DelayCompletionMs = 0; };
	const int32 WorkersBefore = FSpoutAsyncStream::GetLiveStreams();
	const FName Name = AB::UniqueName(TEXT("StopOutstanding"));
	AB::FConsumer C; C.Node = AB::FConsumer::ENode::Receiver; C.bAsync = true;
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AB::MakeSource(64, 32, PF_B8G8R8A8));
	if (!AB::StartAsync(*this, Name, C, Source.Get())) { AB::CloseBoth(Name, C); return false; }

	constexpr int32 DelayMs = 500;
	SpoutAsyncReceiveTestHooks::DelayCompletionMs = DelayMs;
	AB::Upload(Source.Get(), 2);
	AB::Send(Name, Source.Get());
	const double Held = AB::WaitUntil([&] { return AB::WorkerHoldsAccess(Name); }, 2.0);
	TestTrue(TEXT("the worker holds the sender's access for a copy outstanding"), Held > 0.0);
	const FSpoutGpuRetirement::FStats Before = FSpoutGpuRetirement::Get().GetStats();
	// The session's ordered teardown (every stream of the process), then the session runs again.
	USpoutBPFunctionLibrary::CloseAllStreams();
	const double Stopped = FPlatformTime::Seconds();
	TestEqual(TEXT("the stop joined the worker"), FSpoutAsyncStream::GetLiveStreams(), WorkersBefore);
	if (Held > 0.0)
	{
		TestTrue(FString::Printf(TEXT("the stop waited until the copy was proven finished (%.0f ms after access was taken; the copy took %d)"),
			(Stopped - Held) * 1000.0, DelayMs), (Stopped - Held) * 1000.0 >= DelayMs - 50.0);
	}
	TestTrue(TEXT("the sender's access is free once the stop returned"), AB::AccessFreeForAnotherHolder(Name));
	TestTrue(TEXT("the stream's objects went to the retirement list"), FSpoutGpuRetirement::Get().GetStats().Retired >= Before.Retired + 1);
	TestTrue(TEXT("and were released once their work was proven finished"), AB::WaitForReleases(Before.Released, 1, 5.0));
	FlushRenderingCommands();
	return true;
}

#endif
