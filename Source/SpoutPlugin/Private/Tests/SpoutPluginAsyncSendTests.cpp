// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5
//
// The asynchronous render-target sender (development switch r.Spout.Dev.AsyncSend; SpoutAsyncSend.h), tested with
// real transfers through the plugin's own sender and a synchronous receiver in this process, on the shared-slot path.
// What is observed is what actually ran: the entry's sender object, the stream's counters, the access mutex as another
// holder sees it, the retirement list, and every texel of the received image.
//
//   switch off   the synchronous send runs and no worker exists
//   publishing   a send is published by the worker and a flush resolves it; the counters' identity
//   access       a frame waits for access and publishes, or is abandoned after its deadline; a newer frame ends it
//   close, stop  frames owed get one attempt, the worker is joined, nothing is left holding access
//   resize       a new identity with work outstanding; a budget refusal is retried, not latched
//   failures     a fill that does not finish, a failed drain signal (access held until the query proves the copy),
//                a long native copy, a refused installation, a stream that cannot start
//   streams      two senders with one blocked; Unreal receiving its own asynchronous send
//   frame end    r.Spout.Dev.AsyncSend.DrainAfter 1, with and without a frame end
//   outcome log  r.Spout.Dev.AsyncSend.OutcomeLog: one row per frame and per refused send, reconciled with the stream's
//                summary row and its counters; ordered, contiguous and timestamped
//
// Nothing here is a performance statement.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
#include "Async/Async.h"
#include "SpoutAsyncSend.h"
#include "SpoutBPFunctionLibrary.h"
#include "SpoutD3DContext.h"
#include "SpoutFrameSync.h"
#include "SpoutLimits.h"
#include "SpoutSenderRegistry.h"
#include "SpoutSharedSlot.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderCommandFence.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#define SPOUT_ASYNC_SEND_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

namespace SpoutAsyncSendTests
{
	using SpoutSendRing::EOutcome;
	using SpoutSendRing::ENotAccepted;

	/** Sets a console variable for the scope, at console priority, and gives it back its value and priority afterwards. */
	struct FScopedVariable
	{
		IConsoleVariable* Variable = nullptr;
		FString Before;
		EConsoleVariableFlags Priority = ECVF_SetByCode;
		FScopedVariable(const TCHAR* Name, const TCHAR* Value)
		{
			Variable = IConsoleManager::Get().FindConsoleVariable(Name);
			if (Variable)
			{
				Before = Variable->GetString();
				Priority = static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask);
				Variable->Set(Value, ECVF_SetByConsole);
			}
		}
		~FScopedVariable()
		{
			if (Variable)
			{
				Variable->Set(*Before, ECVF_SetByConsole);
				Variable->SetFlags(static_cast<EConsoleVariableFlags>((Variable->GetFlags() & ~ECVF_SetByMask) | Priority));
			}
		}
	};

	/** Resets every failure injection when a test ends, whatever happened. */
	struct FHooksReset
	{
		~FHooksReset()
		{
			SpoutAsyncSendTestHooks::TimeoutFills = 0;
			SpoutAsyncSendTestHooks::DelayFillMs = 0;
			SpoutAsyncSendTestHooks::FailSignals = 0;
			SpoutAsyncSendTestHooks::DelayDrainMs = 0;
			SpoutAsyncSendTestHooks::FailInstalls = 0;
			SpoutAsyncSendTestHooks::FailCreations = 0;
		}
	};

	bool RequireSharedSlot(FAutomationTestBase& Test)
	{
		FSpoutD3DContext::Get().InitializeIfNeeded();
		return Test.TestTrue(TEXT("this process latched the shared-slot path (r.Spout.SharedSlotInterop 1), on which the asynchronous sender runs"),
			SpoutInterop::IsSharedSlot());
	}

	FName UniqueName(const TCHAR* What)
	{
		return FName(*FString::Printf(TEXT("SpoutAsyncSend_%s_%s"), What, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	UTextureRenderTarget2D* MakeSource(int32 W, int32 H, EPixelFormat Format = PF_B8G8R8A8)
	{
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		Target->InitCustomFormat(W, H, Format, true);
		Target->UpdateResourceImmediate(true);
		return Target;
	}

	/** A pattern that changes with Sequence, in BGRA8 or RGBA8 layout. */
	void Upload(UTextureRenderTarget2D* Target, int32 Sequence)
	{
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		const int32 W = Target->SizeX, H = Target->SizeY;
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(W * H * 4);
		for (int32 Y = 0; Y < H; ++Y)
		for (int32 X = 0; X < W; ++X)
		{
			const int32 I = (Y * W + X) * 4;
			Bytes[I + 0] = uint8((X * 17 + Sequence) & 255);
			Bytes[I + 1] = uint8((Y * 29 + Sequence * 5) & 255);
			Bytes[I + 2] = uint8((X + Y + Sequence * 13) & 255);
			Bytes[I + 3] = uint8((X * 3 + Y * 7 + Sequence) & 255);
		}
		ENQUEUE_RENDER_COMMAND(SpoutAsyncSendUpload)([Resource, W, H, Bytes = MoveTemp(Bytes)](FRHICommandListImmediate& Cmd)
		{
			Cmd.UpdateTexture2D(Resource->GetRenderTargetTexture(), 0, FUpdateTextureRegion2D(0, 0, 0, 0, W, H), W * 4, Bytes.GetData());
		});
		FlushRenderingCommands();
	}

	void ReadPixels(FRHITexture* Texture, TArray<FLinearColor>& Out)
	{
		Out.Reset();
		if (!Texture) return;
		const int32 W = Texture->GetDesc().Extent.X, H = Texture->GetDesc().Extent.Y;
		TArray<FLinearColor>* Target = &Out;
		ENQUEUE_RENDER_COMMAND(SpoutAsyncSendRead)([Texture, W, H, Target](FRHICommandListImmediate& Cmd)
		{
			Cmd.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
			FReadSurfaceDataFlags Flags(RCM_MinMax);
			Flags.SetLinearToGamma(false);
			Cmd.ReadSurfaceData(Texture, FIntRect(0, 0, W, H), *Target, Flags);
			Cmd.Transition(FRHITransitionInfo(Texture, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		});
		FlushRenderingCommands();
	}

	FRHITexture* SourceTexture(UTextureRenderTarget2D* Target)
	{
		FTextureRenderTargetResource* Resource = Target ? Target->GameThread_GetRenderTargetResource() : nullptr;
		return Resource ? Resource->GetRenderTargetTexture() : nullptr;
	}

	/** A send whose render command has run, without a flush: owed frames keep their normal access bound. */
	bool SendNoFlush(const FName& Name, UTextureRenderTarget2D* Source)
	{
		const bool bAccepted = USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source);
		FRenderCommandFence Fence;
		Fence.BeginFence();
		Fence.Wait();
		return bAccepted;
	}

	/**
	 * A send followed by a flush, whose end resolves every frame the asynchronous sender owes. Upload flushes too: a
	 * test that wants frames to stay owed prepares every source's content before its first SendNoFlush.
	 */
	bool Send(const FName& Name, UTextureRenderTarget2D* Source)
	{
		const bool bAccepted = USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source);
		FlushRenderingCommands();
		return bAccepted;
	}

	TSharedPtr<FSpoutSharedSender> SenderEntry(const FName& Name)
	{
		return FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Sender);
	}

	TSharedPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe> Stream(const FName& Name)
	{
		const TSharedPtr<FSpoutSharedSender> E = SenderEntry(Name);
		if (!E) return nullptr;
		FScopeLock Lock(&E->ResourceMutex);
		return E->AsyncSend;
	}

	FSpoutAsyncSendStats Stats(const FName& Name)
	{
		FSpoutAsyncSendStats S;
		SpoutGetAsyncSendStats(Name, S);
		return S;
	}

	int64 Outcome(const FSpoutAsyncSendStats& S, EOutcome O) { return S.Outcomes[static_cast<int32>(O)]; }
	int64 Refused(const FSpoutAsyncSendStats& S, ENotAccepted W) { return S.NotAccepted[static_cast<int32>(W)]; }

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

	bool WaitForReleases(int64 Before, int64 Count, double TimeoutSeconds)
	{
		return WaitUntil([&] { FSpoutGpuRetirement::Get().Poll(); return FSpoutGpuRetirement::Get().GetStats().Released >= Before + Count; }, TimeoutSeconds) > 0.0;
	}

	/** The counters' identity and the attempt account: checked after every step that matters. */
	void CheckIdentity(FAutomationTestBase& Test, const FName& Name, const TCHAR* When, int64 FailedAcquiresBefore = -1)
	{
		const FSpoutAsyncSendStats S = Stats(Name);
		Test.TestEqual(FString::Printf(TEXT("%s: accepted = published + superseded + abandoned + failed + in flight"), When),
			S.Accepted, S.Published() + S.Superseded() + S.AccessAbandoned() + S.Failed() + S.InFlight);
		Test.TestEqual(FString::Printf(TEXT("%s: no rule of the bookkeeping was broken"), When), S.RuleViolations, int64(0));
		const TSharedPtr<FSpoutSharedSender> E = SenderEntry(Name);
		if (E && FailedAcquiresBefore >= 0)
		{
			Test.TestEqual(FString::Printf(TEXT("%s: every failed attempt is one failed acquisition (AccessMisses keeps its meaning)"), When),
				int64(E->FrameSync.GetFailedAcquireCount()) - FailedAcquiresBefore, S.FailedAttempts);
		}
	}

	/** Holds a name's Spout access mutex on the calling thread, as another process's receiver would. */
	struct FAccessHolder
	{
		TUniquePtr<ISpoutFrameSyncBackend> Backend;
		bool bHeld = false;
		explicit FAccessHolder(const FName& Name)
		{
			const FString Text = Name.ToString();
			const auto Utf8 = StringCast<ANSICHAR>(*Text);
			Backend = MakeSpoutSdkFrameSyncBackend();
			if (Backend && Backend->CreateAccessMutex(Utf8.Get()))
			{
				bHeld = Backend->CheckTextureAccess(nullptr, 1000);
			}
		}
		void Release()
		{
			if (Backend)
			{
				if (bHeld) Backend->AllowTextureAccess(nullptr);
				bHeld = false;
				Backend->CloseAccessMutex();
				RecycleSpoutSdkFrameSyncBackend(MoveTemp(Backend));
			}
		}
		~FAccessHolder() { Release(); }
	};

	/**
	 * Whether another holder could take the name's access now (taken with no wait and given back at once). The probe
	 * runs on a thread of its own: a Windows mutex is recursive for the thread that holds it, so a probe on the test's
	 * thread would always succeed while FAccessHolder holds it.
	 */
	bool AccessFree(const FName& Name)
	{
		const FString Text = Name.ToString();
		TFuture<bool> Probed = Async(EAsyncExecution::Thread, [Text]()
		{
			const auto Utf8 = StringCast<ANSICHAR>(*Text);
			TUniquePtr<ISpoutFrameSyncBackend> Probe = MakeSpoutSdkFrameSyncBackend();
			bool bGot = false;
			if (Probe && Probe->CreateAccessMutex(Utf8.Get()))
			{
				bGot = Probe->CheckTextureAccess(nullptr, 0);
				if (bGot) Probe->AllowTextureAccess(nullptr);
				Probe->CloseAccessMutex();
			}
			RecycleSpoutSdkFrameSyncBackend(MoveTemp(Probe));
			return bGot;
		});
		return Probed.Get();
	}

	/** A synchronous receiver of Name in this process. */
	struct FReceiver
	{
		UTexture2D* Texture = nullptr;
		bool Call(const FName& Name)
		{
			UMaterialInstanceDynamic* Material = nullptr;
			const bool b = USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, Material, Texture, nullptr, ESpoutColorSpace::Linear, false);
			FlushRenderingCommands();
			return b;
		}
		/** Receives until the texture holds exactly the source's texels, or the timeout. */
		bool ReceiveExact(const FName& Name, UTextureRenderTarget2D* Source, double TimeoutSeconds = 5.0)
		{
			TArray<FLinearColor> Sent;
			ReadPixels(SourceTexture(Source), Sent);
			const double End = FPlatformTime::Seconds() + TimeoutSeconds;
			for (;;)
			{
				if (!Call(Name) || !Texture) return false;
				TArray<FLinearColor> Got;
				FTextureResource* Resource = Texture->GetResource();
				ReadPixels(Resource ? Resource->GetTextureRHI() : nullptr, Got);
				if (Got.Num() == Sent.Num() && Got.Num() > 0 && FMemory::Memcmp(Got.GetData(), Sent.GetData(), Got.Num() * sizeof(FLinearColor)) == 0) return true;
				if (FPlatformTime::Seconds() > End) return false;
				FPlatformProcess::Sleep(0.002f);
			}
		}
		void Close(const FName& Name)
		{
			UMaterialInstanceDynamic* Material = nullptr;
			USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material);
			FlushRenderingCommands();
		}
	};

	void CloseSender(const FName& Name)
	{
		USpoutBPFunctionLibrary::CloseSender(Name);
		FlushRenderingCommands();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendSwitchOff, "SpoutPlugin.AsyncSend.SwitchOffKeepsTheSynchronousSend", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendSwitchOff::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("0"));
	const int32 LiveBefore = FSpoutAsyncSendStream::GetLiveStreams();
	const FName Name = AS::UniqueName(TEXT("Off"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	AS::Upload(Source.Get(), 1);
	TestTrue(TEXT("the send is accepted"), AS::Send(Name, Source.Get()));
	const TSharedPtr<FSpoutSharedSender> E = AS::SenderEntry(Name);
	TestTrue(TEXT("the entry exists"), E.IsValid());
	if (E)
	{
		TestTrue(TEXT("it has no asynchronous sender"), !AS::Stream(Name).IsValid());
		TestTrue(TEXT("it used the synchronous slot send"), E->SlotSend.IsValid() && E->SlotTransfers.load() == 1);
	}
	TestEqual(TEXT("no worker was started"), FSpoutAsyncSendStream::GetLiveStreams(), LiveBefore);
	AS::FReceiver R;
	TestTrue(TEXT("the image arrives exactly"), R.ReceiveExact(Name, Source.Get()));
	R.Close(Name);
	AS::CloseSender(Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendPublishes, "SpoutPlugin.AsyncSend.ASendIsPublishedByTheWorkerAndAFlushResolvesIt", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendPublishes::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	const int32 LiveBefore = FSpoutAsyncSendStream::GetLiveStreams();
	const FName Name = AS::UniqueName(TEXT("Publish"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	AS::Upload(Source.Get(), 1);
	TestTrue(TEXT("the send is accepted"), AS::Send(Name, Source.Get()));
	TSharedPtr<FSpoutSharedSender> E = AS::SenderEntry(Name);
	if (!TestTrue(TEXT("the entry has an asynchronous sender"), E.IsValid() && AS::Stream(Name).IsValid()))
	{
		AS::CloseSender(Name);
		return false;
	}
	TestEqual(TEXT("one worker was started"), FSpoutAsyncSendStream::GetLiveStreams(), LiveBefore + 1);
	TestFalse(TEXT("the synchronous slot send was not used"), E->SlotSend.IsValid());
	FSpoutAsyncSendStats S = AS::Stats(Name);
	TestEqual(TEXT("one frame accepted"), S.Accepted, int64(1));
	TestEqual(TEXT("and, once the flush ended, published"), S.Published(), int64(1));
	TestEqual(TEXT("nothing in flight after the flush"), S.InFlight, int64(0));
	TestEqual(TEXT("the frame was announced once (FramesSubmitted)"), E->FrameSync.GetPublishedFrameCount(), 1);
	AS::CheckIdentity(*this, Name, TEXT("after one send"), 0);
	AS::FReceiver R;
	TestTrue(TEXT("the image arrives exactly"), R.ReceiveExact(Name, Source.Get()));
	// A sequence of frames, each published.
	for (int32 I = 2; I <= 6; ++I)
	{
		AS::Upload(Source.Get(), I);
		AS::Send(Name, Source.Get());
	}
	TestTrue(TEXT("the last image of a sequence arrives exactly"), R.ReceiveExact(Name, Source.Get()));
	S = AS::Stats(Name);
	TestEqual(TEXT("six frames accepted"), S.Accepted, int64(6));
	TestTrue(TEXT("every frame published or ended by a newer one or by one failed attempt during a receive"), S.Published() >= 1 && S.Failed() == 0);
	AS::CheckIdentity(*this, Name, TEXT("after a sequence"), 0);
	const FSpoutGpuRetirement::FStats Before = FSpoutGpuRetirement::Get().GetStats();
	R.Close(Name);
	// The entry lives while anything holds it (this test's reference too); its stream stops when the last one goes.
	E.Reset();
	AS::CloseSender(Name);
	TestEqual(TEXT("closing joined the worker"), FSpoutAsyncSendStream::GetLiveStreams(), LiveBefore);
	TestTrue(TEXT("its slot set was retired and released once its work was proven finished"), AS::WaitForReleases(Before.Released, 1, 5.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendWaitsForAccess, "SpoutPlugin.AsyncSend.AFrameWaitsForAccessThenPublishes", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendWaitsForAccess::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	const FName Name = AS::UniqueName(TEXT("Wait"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	AS::Upload(Source.Get(), 1);
	AS::Send(Name, Source.Get());
	const TSharedPtr<FSpoutSharedSender> E = AS::SenderEntry(Name);
	if (!TestTrue(TEXT("the first frame was published"), E.IsValid() && AS::Stats(Name).Published() == 1)) { AS::CloseSender(Name); return false; }
	const int64 FailedBefore = E->FrameSync.GetFailedAcquireCount();
	const int64 AttemptsBefore = AS::Stats(Name).FailedAttempts;
	{
		AS::FAccessHolder Holder(Name);
		TestTrue(TEXT("another holder took the access mutex"), Holder.bHeld);
		AS::Upload(Source.Get(), 2);
		const double SentAt = FPlatformTime::Seconds();
		AS::SendNoFlush(Name, Source.Get());
		const double Tried = AS::WaitUntil([&] { return AS::Stats(Name).FailedAttempts >= 2; }, 1.0);
		TestTrue(TEXT("the worker kept trying while access was held (bounded attempts)"), Tried > 0.0);
		FSpoutAsyncSendStats S = AS::Stats(Name);
		TestEqual(TEXT("the frame is still owed"), S.InFlight, int64(1));
		TestEqual(TEXT("and not published"), S.Published(), int64(1));
		TestEqual(TEXT("and not abandoned"), S.AccessAbandoned(), int64(0));
		static_cast<void>(SentAt);
		Holder.Release();
	}
	TestTrue(TEXT("once access was free the frame was published"), AS::WaitUntil([&] { return AS::Stats(Name).Published() == 2; }, 1.0) > 0.0);
	TestEqual(TEXT("not abandoned"), AS::Stats(Name).AccessAbandoned(), int64(0));
	AS::CheckIdentity(*this, Name, TEXT("after waiting for access"), FailedBefore - AttemptsBefore);
	AS::FReceiver R;
	TestTrue(TEXT("the waiting frame's image arrives exactly"), R.ReceiveExact(Name, Source.Get()));
	R.Close(Name);
	AS::CloseSender(Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendDeadline, "SpoutPlugin.AsyncSend.AFrameStillBlockedAfterItsDeadlineIsAbandoned", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendDeadline::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	const FName Name = AS::UniqueName(TEXT("Deadline"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	AS::Upload(Source.Get(), 1);
	AS::Send(Name, Source.Get());
	const TSharedPtr<FSpoutSharedSender> E = AS::SenderEntry(Name);
	const int64 Baseline = E ? int64(E->FrameSync.GetFailedAcquireCount()) - AS::Stats(Name).FailedAttempts : -1;
	{
		AS::FAccessHolder Holder(Name);
		AS::Upload(Source.Get(), 2);
		const double SentAt = FPlatformTime::Seconds();
		AS::SendNoFlush(Name, Source.Get());
		const double Ended = AS::WaitUntil([&] { return AS::Stats(Name).InFlight == 0; }, 2.0);
		TestTrue(FString::Printf(TEXT("the frame ended about 100 ms after it was ready (%.0f ms after the send)"), (Ended - SentAt) * 1000.0),
			Ended > 0.0 && Ended - SentAt >= 0.09 && Ended - SentAt < 1.0);
		TestEqual(TEXT("abandoned for access"), AS::Stats(Name).AccessAbandoned(), int64(1));
		TestEqual(TEXT("not published"), AS::Stats(Name).Published(), int64(1));
		TestFalse(TEXT("the other holder still has the access mutex: nothing was taken from it"), AS::AccessFree(Name));
	}
	AS::Upload(Source.Get(), 3);
	AS::Send(Name, Source.Get());
	TestEqual(TEXT("the next frame publishes once access is free"), AS::Stats(Name).Published(), int64(2));
	TestTrue(TEXT("the blocked frame made failed attempts"), AS::Stats(Name).FailedAttempts > 0);
	AS::CheckIdentity(*this, Name, TEXT("after an abandoned frame"), Baseline);
	AS::CloseSender(Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendNewer, "SpoutPlugin.AsyncSend.ANewerFrameEndsTheWaitingOneAndOnlyTheNewestIsPublished", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendNewer::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	const FName Name = AS::UniqueName(TEXT("Newer"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	TStrongObjectPtr<UTextureRenderTarget2D> Next(AS::MakeSource(64, 32)); // the same identity, other content
	AS::Upload(Source.Get(), 1);
	AS::Send(Name, Source.Get());
	AS::Upload(Source.Get(), 2);
	AS::Upload(Next.Get(), 3);
	const TSharedPtr<FSpoutSharedSender> E = AS::SenderEntry(Name);
	const int64 Baseline = E ? int64(E->FrameSync.GetFailedAcquireCount()) - AS::Stats(Name).FailedAttempts : -1;
	{
		AS::FAccessHolder Holder(Name);
		AS::SendNoFlush(Name, Source.Get());
		AS::SendNoFlush(Name, Next.Get());
		TestTrue(TEXT("the older frame ends once the newer one is ready"), AS::WaitUntil([&] { return AS::Stats(Name).InFlight <= 1; }, 1.0) > 0.0);
		const FSpoutAsyncSendStats S = AS::Stats(Name);
		TestEqual(TEXT("it ended superseded (no attempt made) or abandoned (an attempt failed), never published"),
			S.Superseded() + S.AccessAbandoned(), int64(1));
		Holder.Release();
	}
	TestTrue(TEXT("the newest frame is published"), AS::WaitUntil([&] { return AS::Stats(Name).Published() == 2; }, 1.0) > 0.0);
	AS::FReceiver R;
	TestTrue(TEXT("and its image is the one that arrives"), R.ReceiveExact(Name, Next.Get()));
	AS::CheckIdentity(*this, Name, TEXT("after a newer frame"), Baseline);
	R.Close(Name);
	AS::CloseSender(Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendCloseOwed, "SpoutPlugin.AsyncSend.ClosingWithAFrameOwedResolvesItAndJoinsTheWorker", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendCloseOwed::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	const int32 LiveBefore = FSpoutAsyncSendStream::GetLiveStreams();
	const FName Name = AS::UniqueName(TEXT("CloseOwed"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	AS::Upload(Source.Get(), 1);
	AS::Send(Name, Source.Get());
	TSharedPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe> Kept = AS::Stream(Name);
	const FSpoutGpuRetirement::FStats Before = FSpoutGpuRetirement::Get().GetStats();
	{
		AS::FAccessHolder Holder(Name);
		AS::Upload(Source.Get(), 2);
		AS::SendNoFlush(Name, Source.Get());
		TestTrue(TEXT("the frame is owed"), Kept && Kept->GetStats().InFlight == 1);
		const double ClosedAt = FPlatformTime::Seconds();
		USpoutBPFunctionLibrary::CloseSender(Name);
		const double Took = FPlatformTime::Seconds() - ClosedAt;
		TestTrue(FString::Printf(TEXT("the close resolved it with one attempt and returned (%.1f ms)"), Took * 1000.0), Took < 0.5);
		TestEqual(TEXT("the worker was joined"), FSpoutAsyncSendStream::GetLiveStreams(), LiveBefore + (Kept ? 1 : 0));
		if (Kept)
		{
			const FSpoutAsyncSendStats S = Kept->GetStats();
			TestEqual(TEXT("the owed frame was abandoned for access"), S.AccessAbandoned(), int64(1));
			TestEqual(TEXT("nothing is owed"), S.InFlight, int64(0));
		}
		TestFalse(TEXT("the other holder still has the access mutex"), AS::AccessFree(Name));
	}
	Kept.Reset();
	TestEqual(TEXT("the stream object is gone"), FSpoutAsyncSendStream::GetLiveStreams(), LiveBefore);
	TestTrue(TEXT("nothing of the closed sender holds the access mutex"), AS::AccessFree(Name));
	TestTrue(TEXT("its slot set was retired and released"), AS::WaitForReleases(Before.Released, 1, 5.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendStopOwed, "SpoutPlugin.AsyncSend.ASessionStopWithAFrameOwedResolvesIt", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendStopOwed::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	AS::FHooksReset Reset;
	const int32 LiveBefore = FSpoutAsyncSendStream::GetLiveStreams();
	const FName Name = AS::UniqueName(TEXT("StopOwed"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	AS::Upload(Source.Get(), 1);
	AS::Send(Name, Source.Get());
	TSharedPtr<FSpoutAsyncSendStream, ESPMode::ThreadSafe> Kept = AS::Stream(Name);
	// A native copy that takes 300 ms: the stop waits for its proof, with the access held, then publishes it.
	SpoutAsyncSendTestHooks::DelayDrainMs = 300;
	AS::Upload(Source.Get(), 2);
	AS::SendNoFlush(Name, Source.Get());
	const double Held = AS::WaitUntil([&] { const TSharedPtr<FSpoutSharedSender> E = AS::SenderEntry(Name); return E && E->FrameSync.IsAccessHeld(); }, 2.0);
	TestTrue(TEXT("the worker holds access for the long copy"), Held > 0.0);
	USpoutBPFunctionLibrary::CloseAllStreams();
	const double Stopped = FPlatformTime::Seconds();
	TestTrue(FString::Printf(TEXT("the session stop waited until the copy was proven (%.0f ms after access was taken)"), (Stopped - Held) * 1000.0),
		Held > 0.0 && Stopped - Held >= 0.25);
	if (Kept)
	{
		const FSpoutAsyncSendStats S = Kept->GetStats();
		TestEqual(TEXT("the owed frame was published before the stop ended"), S.Published(), int64(2));
		TestEqual(TEXT("nothing is owed"), S.InFlight, int64(0));
	}
	Kept.Reset();
	TestEqual(TEXT("no stream object is left"), FSpoutAsyncSendStream::GetLiveStreams(), LiveBefore);
	TestTrue(TEXT("nothing holds the access mutex"), AS::AccessFree(Name));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendResize, "SpoutPlugin.AsyncSend.AResizeWithWorkOutstandingRetiresTheOldSlots", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendResize::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	AS::FHooksReset Reset;
	const FName Name = AS::UniqueName(TEXT("Resize"));
	TStrongObjectPtr<UTextureRenderTarget2D> Small(AS::MakeSource(64, 32));
	TStrongObjectPtr<UTextureRenderTarget2D> Large(AS::MakeSource(96, 48));
	AS::Upload(Small.Get(), 1);
	AS::Send(Name, Small.Get());
	const FSpoutGpuRetirement::FStats Before = FSpoutGpuRetirement::Get().GetStats();
	AS::Upload(Small.Get(), 2);
	AS::Upload(Large.Get(), 3);
	SpoutAsyncSendTestHooks::DelayDrainMs = 300;
	AS::SendNoFlush(Name, Small.Get());
	const double Held = AS::WaitUntil([&] { const TSharedPtr<FSpoutSharedSender> E = AS::SenderEntry(Name); return E && E->FrameSync.IsAccessHeld(); }, 2.0);
	TestTrue(TEXT("a frame of the old size is draining"), Held > 0.0);
	AS::SendNoFlush(Name, Large.Get());
	FSpoutAsyncSendStats S = AS::Stats(Name);
	TestEqual(TEXT("a new slot set was made for the new size while the old one was in use"), S.SetsCreated, int64(2));
	TestEqual(TEXT("two frames owed"), S.InFlight, int64(2));
	FlushRenderingCommands();
	S = AS::Stats(Name);
	TestEqual(TEXT("the flush resolved both"), S.InFlight, int64(0));
	TestEqual(TEXT("both published, in order"), S.Published(), int64(3));
	TestTrue(TEXT("the old size's set was retired"), S.SetsRetired >= 1);
	TestTrue(TEXT("and released once its work was proven finished"), AS::WaitForReleases(Before.Released, 1, 5.0));
	AS::FReceiver R;
	TestTrue(TEXT("the new size's image arrives exactly"), R.ReceiveExact(Name, Large.Get()));
	AS::CheckIdentity(*this, Name, TEXT("after a resize"));
	R.Close(Name);
	AS::CloseSender(Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendBudget, "SpoutPlugin.AsyncSend.ABudgetRefusalIsCountedAndRetriedNotLatched", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendBudget::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	// What earlier tests left for the garbage collector is released first, so the budget below is this test's own.
	int64 Base = 0;
	{
		int64 Previous = -1;
		for (int32 I = 0; I < 50; ++I)
		{
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			FlushRenderingCommands();
			FSpoutGpuRetirement::Get().Poll();
			Base = SpoutLimits::RetainedBytes();
			if (Base == Previous) break;
			Previous = Base;
			FPlatformProcess::Sleep(0.02f);
		}
	}
	// 1024x1024 BGRA8 is 4 MiB a frame: the shared texture 8, a slot set 8. With 18 MiB above what is retained, the
	// first size fits (16); a second set while the first is in use does not (8 + 8 + 4 for 1024x512); once the first set
	// is released the second size fits (8 shared + 4 set + 4 new shared while the old one is replaced).
	const int64 MiB = 1024 * 1024;
	const int64 BudgetMiB = (Base + MiB - 1) / MiB + 18;
	AS::FScopedVariable Budget(TEXT("r.Spout.MaxRetainedMiB"), *FString::Printf(TEXT("%lld"), BudgetMiB));
	const FName Name = AS::UniqueName(TEXT("Budget"));
	TStrongObjectPtr<UTextureRenderTarget2D> Big(AS::MakeSource(1024, 1024));
	TStrongObjectPtr<UTextureRenderTarget2D> Half(AS::MakeSource(1024, 512));
	AS::Upload(Big.Get(), 1);
	AS::Send(Name, Big.Get());
	TestEqual(TEXT("the first size is published within the budget"), AS::Stats(Name).Published(), int64(1));
	AS::Upload(Big.Get(), 2);
	AS::Upload(Half.Get(), 3);
	{
		AS::FAccessHolder Holder(Name);
		AS::SendNoFlush(Name, Big.Get());
		AS::SendNoFlush(Name, Half.Get());
		const FSpoutAsyncSendStats S = AS::Stats(Name);
		TestEqual(TEXT("the old size's frame is still owed (its set in use)"), S.InFlight, int64(1));
		TestEqual(TEXT("the new size's set did not fit while the old one was in use: not accepted (budget)"), AS::Refused(S, SpoutSendRing::ENotAccepted::Budget), int64(1));
		TestTrue(TEXT("retained memory stayed within the budget"), SpoutLimits::RetainedBytes() <= BudgetMiB * MiB);
		Holder.Release();
	}
	FlushRenderingCommands();
	TestTrue(TEXT("the old set was released once its work was proven"), AS::WaitUntil([&] { FSpoutGpuRetirement::Get().Poll(); return AS::Stats(Name).SetsRetired >= 1 && FSpoutGpuRetirement::Get().GetStats().Pending == 0; }, 5.0) > 0.0);
	AS::Send(Name, Half.Get());
	const FSpoutAsyncSendStats S = AS::Stats(Name);
	TestEqual(TEXT("the same size, sent again, is accepted: the refusal was not latched"), S.Accepted, int64(3));
	TestTrue(TEXT("and published"), S.Published() >= 2);
	TestTrue(TEXT("retained memory stayed within the budget"), SpoutLimits::RetainedBytes() <= BudgetMiB * MiB);
	AS::CheckIdentity(*this, Name, TEXT("after a budget refusal"));
	AS::CloseSender(Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendFillTimeout, "SpoutPlugin.AsyncSend.AFillThatDoesNotFinishFailsAndTheNextFramePublishes", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendFillTimeout::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	AS::FHooksReset Reset;
	AddExpectedMessage(TEXT("did not finish within"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const FName Name = AS::UniqueName(TEXT("Fill"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	AS::Upload(Source.Get(), 1);
	SpoutAsyncSendTestHooks::TimeoutFills = 1;
	AS::Send(Name, Source.Get());
	FSpoutAsyncSendStats S = AS::Stats(Name);
	TestEqual(TEXT("the frame failed its fill"), AS::Outcome(S, AS::EOutcome::FailedFill), int64(1));
	TestEqual(TEXT("and was not published"), S.Published(), int64(0));
	AS::Upload(Source.Get(), 2);
	AS::Send(Name, Source.Get());
	TestEqual(TEXT("the next frame publishes"), AS::Stats(Name).Published(), int64(1));
	AS::FReceiver R;
	TestTrue(TEXT("its image arrives exactly"), R.ReceiveExact(Name, Source.Get()));
	AS::CheckIdentity(*this, Name, TEXT("after a failed fill"));
	R.Close(Name);
	AS::CloseSender(Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendFailedSignal, "SpoutPlugin.AsyncSend.AFailedDrainSignalKeepsAccessUntilTheQueryProvesTheCopy", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendFailedSignal::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	AS::FHooksReset Reset;
	AddExpectedMessage(TEXT("a fence signal on the native device failed"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const FName Name = AS::UniqueName(TEXT("Signal"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	TStrongObjectPtr<UTextureRenderTarget2D> Other(AS::MakeSource(80, 40));
	AS::Upload(Source.Get(), 1);
	AS::Send(Name, Source.Get());
	constexpr int32 DelayMs = 300;
	SpoutAsyncSendTestHooks::FailSignals = 1;
	SpoutAsyncSendTestHooks::DelayDrainMs = DelayMs;
	AS::Upload(Source.Get(), 2);
	AS::SendNoFlush(Name, Source.Get());
	const double Held = AS::WaitUntil([&] { const TSharedPtr<FSpoutSharedSender> E = AS::SenderEntry(Name); return E && E->FrameSync.IsAccessHeld(); }, 2.0);
	TestTrue(TEXT("the worker took access for the copy"), Held > 0.0);
	if (Held > 0.0)
	{
		while (FPlatformTime::Seconds() < Held + 0.15) { FPlatformProcess::SleepNoStats(0.005f); }
		const TSharedPtr<FSpoutSharedSender> E = AS::SenderEntry(Name);
		TestTrue(TEXT("with no fence value to prove the copy, access is still held"), E && E->FrameSync.IsAccessHeld());
		TestFalse(TEXT("and no other holder can take the access mutex"), AS::AccessFree(Name));
		const double Freed = AS::WaitUntil([&] { const TSharedPtr<FSpoutSharedSender> X = AS::SenderEntry(Name); return X && !X->FrameSync.IsAccessHeld(); }, 5.0);
		TestTrue(FString::Printf(TEXT("access is given back once the query proved the copy (held %.0f ms; the proof took %d)"), (Freed - Held) * 1000.0, DelayMs),
			Freed > 0.0 && (Freed - Held) * 1000.0 >= DelayMs - 50.0);
	}
	FlushRenderingCommands();
	FSpoutAsyncSendStats S = AS::Stats(Name);
	TestEqual(TEXT("the frame failed its drain and was not announced"), AS::Outcome(S, AS::EOutcome::FailedDrain), int64(1));
	TestEqual(TEXT("one announcement in all"), S.Published(), int64(1));
	AS::Upload(Source.Get(), 3);
	AS::Send(Name, Source.Get());
	S = AS::Stats(Name);
	TestEqual(TEXT("the same size is not accepted any more (its set stopped)"), AS::Refused(S, SpoutSendRing::ENotAccepted::Unpublishable), int64(1));
	AS::Upload(Other.Get(), 4);
	AS::Send(Name, Other.Get());
	TestEqual(TEXT("a new size starts again and publishes"), AS::Stats(Name).Published(), int64(2));
	AS::CheckIdentity(*this, Name, TEXT("after a failed signal"));
	AS::CloseSender(Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendLongCopy, "SpoutPlugin.AsyncSend.ALongNativeCopyKeepsAccessUntilItIsProven", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendLongCopy::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	AS::FHooksReset Reset;
	const FName Name = AS::UniqueName(TEXT("LongCopy"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	AS::Upload(Source.Get(), 1);
	AS::Send(Name, Source.Get());
	constexpr int32 DelayMs = 400;
	SpoutAsyncSendTestHooks::DelayDrainMs = DelayMs;
	AS::Upload(Source.Get(), 2);
	AS::SendNoFlush(Name, Source.Get());
	const double Held = AS::WaitUntil([&] { const TSharedPtr<FSpoutSharedSender> E = AS::SenderEntry(Name); return E && E->FrameSync.IsAccessHeld(); }, 2.0);
	TestTrue(TEXT("the worker holds access for the copy"), Held > 0.0);
	if (Held > 0.0)
	{
		while (FPlatformTime::Seconds() < Held + 0.2) { FPlatformProcess::SleepNoStats(0.005f); }
		TestFalse(TEXT("200 ms in, no other holder can take the access mutex"), AS::AccessFree(Name));
		TestEqual(TEXT("and the frame is not announced yet"), AS::Stats(Name).Published(), int64(1));
		const double Done = AS::WaitUntil([&] { return AS::Stats(Name).Published() == 2; }, 5.0);
		TestTrue(FString::Printf(TEXT("it is announced once proven (%.0f ms after access was taken; the copy took %d)"), (Done - Held) * 1000.0, DelayMs),
			Done > 0.0 && (Done - Held) * 1000.0 >= DelayMs - 50.0);
	}
	AS::FReceiver R;
	TestTrue(TEXT("its image arrives exactly"), R.ReceiveExact(Name, Source.Get()));
	AS::CheckIdentity(*this, Name, TEXT("after a long copy"));
	R.Close(Name);
	AS::CloseSender(Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendInstallRefused, "SpoutPlugin.AsyncSend.ARefusedInstallationFailsTheFrameAndIsRetried", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendInstallRefused::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	AS::FHooksReset Reset;
	const FName Name = AS::UniqueName(TEXT("Install"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	AS::Upload(Source.Get(), 1);
	SpoutAsyncSendTestHooks::FailInstalls = 1;
	AS::Send(Name, Source.Get());
	TestEqual(TEXT("the frame failed its installation"), AS::Outcome(AS::Stats(Name), AS::EOutcome::FailedInstall), int64(1));
	// After the back-off (10 ms at first) the next frame is accepted and installs.
	FPlatformProcess::Sleep(0.05f);
	AS::Upload(Source.Get(), 2);
	AS::Send(Name, Source.Get());
	TestEqual(TEXT("the next frame installs and publishes"), AS::Stats(Name).Published(), int64(1));
	AS::FReceiver R;
	TestTrue(TEXT("its image arrives exactly"), R.ReceiveExact(Name, Source.Get()));
	AS::CheckIdentity(*this, Name, TEXT("after a refused installation"));
	R.Close(Name);
	AS::CloseSender(Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendCannotStart, "SpoutPlugin.AsyncSend.AStreamThatCannotStartUsesTheSynchronousSend", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendCannotStart::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	AS::FHooksReset Reset;
	AddExpectedMessage(TEXT("could not start"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const int32 LiveBefore = FSpoutAsyncSendStream::GetLiveStreams();
	const FName Name = AS::UniqueName(TEXT("NoStart"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	AS::Upload(Source.Get(), 1);
	SpoutAsyncSendTestHooks::FailCreations = 1;
	AS::Send(Name, Source.Get());
	const TSharedPtr<FSpoutSharedSender> E = AS::SenderEntry(Name);
	TestTrue(TEXT("the entry uses the synchronous send"), E && E->AsyncSendMode == 0 && E->SlotSend.IsValid() && !AS::Stream(Name).IsValid());
	TestEqual(TEXT("no stream object is left"), FSpoutAsyncSendStream::GetLiveStreams(), LiveBefore);
	AS::Upload(Source.Get(), 2);
	AS::Send(Name, Source.Get());
	TestTrue(TEXT("and keeps it (no second attempt, no second warning)"), E && E->AsyncSendMode == 0);
	AS::FReceiver R;
	TestTrue(TEXT("the image arrives exactly"), R.ReceiveExact(Name, Source.Get()));
	R.Close(Name);
	AS::CloseSender(Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendTwoSenders, "SpoutPlugin.AsyncSend.TwoSendersWithOneBlockedPublishIndependently", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendTwoSenders::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	const FName A = AS::UniqueName(TEXT("Blocked"));
	const FName B = AS::UniqueName(TEXT("Free"));
	TStrongObjectPtr<UTextureRenderTarget2D> SourceA(AS::MakeSource(64, 32));
	TStrongObjectPtr<UTextureRenderTarget2D> SourceB(AS::MakeSource(48, 24));
	AS::Upload(SourceA.Get(), 1);
	AS::Upload(SourceB.Get(), 1);
	AS::Send(A, SourceA.Get());
	AS::Send(B, SourceB.Get());
	const TSharedPtr<FSpoutSharedSender> EA = AS::SenderEntry(A);
	const int64 BaselineA = EA ? int64(EA->FrameSync.GetFailedAcquireCount()) - AS::Stats(A).FailedAttempts : -1;
	{
		AS::FAccessHolder Holder(A);
		AS::Upload(SourceA.Get(), 2);
		AS::Upload(SourceB.Get(), 2);
		AS::SendNoFlush(A, SourceA.Get());
		AS::SendNoFlush(B, SourceB.Get());
		const double BDone = AS::WaitUntil([&] { return AS::Stats(B).Published() == 2; }, 1.0);
		TestTrue(TEXT("the free sender publishes while the other is blocked"), BDone > 0.0);
		TestEqual(TEXT("the blocked one has not"), AS::Stats(A).Published(), int64(1));
		TestTrue(TEXT("the blocked one's frame is abandoned after its deadline"), AS::WaitUntil([&] { return AS::Stats(A).AccessAbandoned() == 1; }, 1.0) > 0.0);
	}
	AS::FReceiver R;
	TestTrue(TEXT("the free sender's image arrives exactly"), R.ReceiveExact(B, SourceB.Get()));
	TestTrue(TEXT("the blocked sender made failed attempts"), AS::Stats(A).FailedAttempts > 0);
	AS::CheckIdentity(*this, A, TEXT("blocked sender"), BaselineA);
	AS::CheckIdentity(*this, B, TEXT("free sender"));
	R.Close(B);
	AS::CloseSender(A);
	AS::CloseSender(B);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendLoopback, "SpoutPlugin.AsyncSend.UnrealReceivesItsOwnAsynchronousSend", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendLoopback::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	const FName Name = AS::UniqueName(TEXT("Loopback"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	AS::Upload(Source.Get(), 1);
	AS::Send(Name, Source.Get());
	const TSharedPtr<FSpoutSharedSender> E = AS::SenderEntry(Name);
	const int64 FailedBefore = E ? E->FrameSync.GetFailedAcquireCount() : 0;
	const int64 AttemptsBefore = AS::Stats(Name).FailedAttempts;
	AS::FReceiver R;
	// The render thread's receive and the worker contend for one named mutex from two threads; each wait is bounded and
	// neither waits for the other's thread.
	for (int32 I = 2; I <= 40; ++I)
	{
		AS::Upload(Source.Get(), I);
		USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source.Get());
		R.Call(Name);
	}
	FlushRenderingCommands();
	TestTrue(TEXT("the last image arrives exactly"), R.ReceiveExact(Name, Source.Get()));
	const FSpoutAsyncSendStats S = AS::Stats(Name);
	TestEqual(TEXT("40 frames accepted"), S.Accepted, int64(40));
	TestEqual(TEXT("nothing owed"), S.InFlight, int64(0));
	TestTrue(TEXT("frames were published"), S.Published() > 1);
	if (E)
	{
		TestEqual(TEXT("every failed attempt of the worker is one failed acquisition"),
			int64(E->FrameSync.GetFailedAcquireCount()) - FailedBefore, S.FailedAttempts - AttemptsBefore);
	}
	AS::CheckIdentity(*this, Name, TEXT("after a loopback"));
	R.Close(Name);
	AS::CloseSender(Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendFrameEnd, "SpoutPlugin.AsyncSend.DrainAfterTheFrameEndOrItsFallback", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendFrameEnd::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	AS::FScopedVariable DrainAfter(TEXT("r.Spout.Dev.AsyncSend.DrainAfter"), TEXT("1"));
	const FName Name = AS::UniqueName(TEXT("FrameEnd"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	AS::Upload(Source.Get(), 1);
	AS::Send(Name, Source.Get());
	TestEqual(TEXT("a flushed frame does not wait for the frame end"), AS::Stats(Name).FrameEndFallbacks, int64(0));
	// No frame ends while a test runs: the frame waits for the fallback, then publishes.
	AS::Upload(Source.Get(), 2);
	const double SentAt = FPlatformTime::Seconds();
	AS::SendNoFlush(Name, Source.Get());
	const double Done = AS::WaitUntil([&] { return AS::Stats(Name).Published() == 2; }, 2.0);
	TestTrue(FString::Printf(TEXT("without a frame end it publishes after the 100 ms fallback (%.0f ms)"), (Done - SentAt) * 1000.0), Done > 0.0 && Done - SentAt >= 0.09);
	TestEqual(TEXT("counted as a fallback"), AS::Stats(Name).FrameEndFallbacks, int64(1));
	// With the frame end signalled behind the send, it publishes without the fallback.
	AS::Upload(Source.Get(), 3);
	AS::SendNoFlush(Name, Source.Get());
	FSpoutAsyncSendStream::QueueFrameEndSignal();
	// In a frame, the engine's end of frame submits the signal right after; here the test does it.
	ENQUEUE_RENDER_COMMAND(SpoutAsyncSendTestSubmit)([](FRHICommandListImmediate& RHICmdList)
	{
		RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
	});
	FRenderCommandFence Fence;
	Fence.BeginFence();
	Fence.Wait();
	TestTrue(TEXT("with a frame end it publishes"), AS::WaitUntil([&] { return AS::Stats(Name).Published() == 3; }, 2.0) > 0.0);
	TestEqual(TEXT("without another fallback"), AS::Stats(Name).FrameEndFallbacks, int64(1));
	AS::CheckIdentity(*this, Name, TEXT("after frame-end drains"));
	AS::CloseSender(Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendOutcomeLog, "SpoutPlugin.AsyncSend.TheOutcomeLogReconcilesWithTheCounters", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendOutcomeLog::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	const FString File = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
		FString::Printf(TEXT("SpoutAsyncSendOutcomesTest-%s.csv"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*File, false, true, true); };
	const FName Name = AS::UniqueName(TEXT("Outcomes"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	TStrongObjectPtr<UTextureRenderTarget2D> Next(AS::MakeSource(64, 32));
	AS::Upload(Source.Get(), 1);
	AS::Upload(Next.Get(), 2);
	FSpoutAsyncSendStats Final;
	uint64 Object = 0;
	{
		AS::FScopedVariable Log(TEXT("r.Spout.Dev.AsyncSend.OutcomeLog"), *File);
		const uint64 FrameBefore = GFrameCounter;
		AS::Send(Name, Source.Get());              // published
		{
			AS::FAccessHolder Holder(Name);
			AS::SendNoFlush(Name, Source.Get());   // abandoned at its deadline, or ended by the next one
			AS::WaitUntil([&] { return AS::Stats(Name).FailedAttempts > 0; }, 1.0);
			AS::SendNoFlush(Name, Next.Get());
			AS::SendNoFlush(Name, Next.Get());     // both slots in use: not accepted (no slot), or accepted if one ended
			AS::WaitUntil([&] { return AS::Stats(Name).InFlight <= 1; }, 1.0);
		}
		FlushRenderingCommands();
		AS::Send(Name, Source.Get());              // published
		Final = AS::Stats(Name);
		TestTrue(TEXT("the frames include a published and an access-abandoned or superseded one"), Final.Published() >= 2 && Final.AccessAbandoned() + Final.Superseded() >= 1);
		TestTrue(TEXT("sends were made in this test's game frames"), GFrameCounter >= FrameBefore);
		AS::CloseSender(Name);                     // the stream stops: its summary row
		FSpoutAsyncSendStream::FlushOutcomeLog();
	}
	TArray<FString> Lines;
	if (!TestTrue(TEXT("the outcome log was written"), FFileHelper::LoadFileToStringArray(Lines, *File) && Lines.Num() > 2)) return false;
	TestTrue(TEXT("it starts with its meta line"), Lines[0].StartsWith(TEXT("# SpoutAsyncSendOutcomes/1 pid=")) && Lines[0].Contains(TEXT("qpcFrequency=")));
	TestTrue(TEXT("then its column header"), Lines[1].StartsWith(TEXT("kind,stream,streamObject,set,frame,sendTick,sendQpc")));
	const FString Quoted = FString::Printf(TEXT("\"%s\""), *Name.ToString());
	TArray<TArray<FString>> Frames, Refused, Summary, Created;
	for (int32 I = 2; I < Lines.Num(); ++I)
	{
		TArray<FString> C;
		Lines[I].ParseIntoArray(C, TEXT(","), false);
		if (C.Num() < 20 || C[1] != Quoted) continue;
		if (C[0] == TEXT("F")) Frames.Add(C);
		else if (C[0] == TEXT("N")) Refused.Add(C);
		else if (C[0] == TEXT("S")) Summary.Add(C);
		else if (C[0] == TEXT("C")) Created.Add(C);
	}
	TestEqual(TEXT("one stream object started"), Created.Num(), 1);
	if (!TestEqual(TEXT("and one summary row"), Summary.Num(), 1)) return false;
	Object = FCString::Strtoui64(*Summary[0][2], nullptr, 10);
	TestEqual(TEXT("one row per accepted frame"), int64(Frames.Num()), Final.Accepted);
	TestEqual(TEXT("one row per refused send"), int64(Refused.Num()), Final.NotAccepted[0] + Final.NotAccepted[1] + Final.NotAccepted[2] + Final.NotAccepted[3]);
	TMap<FString, int64> ByOutcome;
	uint64 PreviousResolved = 0;
	bool bContiguous = true, bOrdered = true, bTimed = true, bOrigin = true;
	for (int32 I = 0; I < Frames.Num(); ++I)
	{
		const TArray<FString>& C = Frames[I];
		bContiguous &= FCString::Strtoui64(*C[4], nullptr, 10) == uint64(I + 1) && FCString::Strtoui64(*C[2], nullptr, 10) == Object;
		const uint64 SendQpc = FCString::Strtoui64(*C[6], nullptr, 10), Accept = FCString::Strtoui64(*C[8], nullptr, 10);
		const uint64 Acquired = FCString::Strtoui64(*C[11], nullptr, 10), Publish = FCString::Strtoui64(*C[12], nullptr, 10), Resolved = FCString::Strtoui64(*C[13], nullptr, 10);
		bOrigin &= FCString::Atoi64(*C[5]) > 0 && SendQpc > 0 && SendQpc <= Accept;
		bTimed &= Accept > 0 && Resolved >= Accept && (C[14] != TEXT("Published") || (Acquired >= Accept && Publish >= Acquired && Resolved >= Publish));
		bOrdered &= Resolved >= PreviousResolved;
		PreviousResolved = Resolved;
		ByOutcome.FindOrAdd(C[14]) += 1;
	}
	TestTrue(TEXT("frame rows are contiguous from 1, all of one stream object"), bContiguous);
	TestTrue(TEXT("each carries its originating game frame and QPC, before its acceptance"), bOrigin);
	TestTrue(TEXT("acceptance, access, announcement and resolution are in order"), bTimed);
	TestTrue(TEXT("frames are resolved in acceptance order"), bOrdered);
	TestEqual(TEXT("published rows equal the counter"), ByOutcome.FindRef(TEXT("Published")), Final.Published());
	TestEqual(TEXT("superseded rows equal the counter"), ByOutcome.FindRef(TEXT("Superseded")), Final.Superseded());
	TestEqual(TEXT("abandoned rows equal the counter"), ByOutcome.FindRef(TEXT("AccessAbandoned")), Final.AccessAbandoned());
	const FString Detail = Summary[0][19];
	TestTrue(TEXT("the summary row carries the same counters"), Detail.Contains(FString::Printf(TEXT("accepted=%lld;published=%lld;superseded=%lld;accessAbandoned=%lld;"),
		Final.Accepted, Final.Published(), Final.Superseded(), Final.AccessAbandoned())) && Detail.Contains(TEXT("inFlight=0;")) && Detail.Contains(TEXT("untracedFrames=0;")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSendOutcomeLogPath, "SpoutPlugin.AsyncSend.TheOutcomeLogLosesNoRowWhenItsPathChanges", SPOUT_ASYNC_SEND_FLAGS)
bool FSpoutAsyncSendOutcomeLogPath::RunTest(const FString&)
{
	namespace AS = SpoutAsyncSendTests;
	if (!AS::RequireSharedSlot(*this)) return false;
	AS::FScopedVariable Switch(TEXT("r.Spout.Dev.AsyncSend"), TEXT("1"));
	const FString Stem = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
		FString::Printf(TEXT("SpoutAsyncSendOutcomesPath-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
	const FString FileA = Stem + TEXT("-a.csv"), FileB = Stem + TEXT("-b.csv");
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*FileA, false, true, true); IFileManager::Get().Delete(*FileB, false, true, true); };
	const FName Name = AS::UniqueName(TEXT("OutcomePath"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(AS::MakeSource(64, 32));
	AS::Upload(Source.Get(), 1);
	IConsoleVariable* Log = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.Dev.AsyncSend.OutcomeLog"));
	if (!TestNotNull(TEXT("the outcome log variable exists"), Log)) return false;
	const FString Before = Log->GetString();
	ON_SCOPE_EXIT { Log->Set(*Before, ECVF_SetByConsole); };
	Log->Set(*FileA, ECVF_SetByConsole);
	for (int32 I = 0; I < 3; ++I) AS::Send(Name, Source.Get());
	Log->Set(*FileB, ECVF_SetByConsole);          // rows queued before the change go to A, later ones to B
	for (int32 I = 0; I < 2; ++I) AS::Send(Name, Source.Get());
	Log->Set(*FileA, ECVF_SetByConsole);          // back to A: appended, with no second header
	AS::Send(Name, Source.Get());
	const FSpoutAsyncSendStats Final = AS::Stats(Name);
	AS::CloseSender(Name);
	Log->Set(TEXT(""), ECVF_SetByConsole);         // off: what was queued is written first
	auto Count = [this, &Name](const FString& File, int32& OutFrames, int32& OutHeaders, int32& OutSummaries)
	{
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *File);
		const FString Quoted = FString::Printf(TEXT("\"%s\","), *Name.ToString());
		OutFrames = OutHeaders = OutSummaries = 0;
		for (const FString& L : Lines)
		{
			OutHeaders += L.StartsWith(TEXT("kind,stream,"));
			OutFrames += L.StartsWith(TEXT("F,") + Quoted);
			OutSummaries += L.StartsWith(TEXT("S,") + Quoted);
		}
	};
	int32 FramesA, HeadersA, SummariesA, FramesB, HeadersB, SummariesB;
	Count(FileA, FramesA, HeadersA, SummariesA);
	Count(FileB, FramesB, HeadersB, SummariesB);
	TestEqual(TEXT("six frames accepted"), Final.Accepted, int64(6));
	TestEqual(TEXT("every frame has exactly one row across the two files"), int64(FramesA + FramesB), Final.Accepted);
	TestTrue(TEXT("rows went to the file that was set when they were queued"), FramesA >= 3 && FramesB >= 1);
	TestEqual(TEXT("each file has its header once"), HeadersA * 10 + HeadersB, 11);
	TestEqual(TEXT("the summary row was written once, after the stream stopped"), SummariesA + SummariesB, 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
