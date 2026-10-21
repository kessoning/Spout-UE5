// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// The asynchronous receive, Async Receive (Beta): off by default and requested per receiver session. Its
// per-session contract (default, independent names, same-name conflicts, close and reopen, fallback) is tested with
// real transfers in SpoutPluginAsyncBetaTests.cpp.
//
// What these tests cover: the retired process-wide setting; the worker's poll of the notification and that it
// leaves the pending-frame state alone; the slot bookkeeping compiled with Unreal's own toolchain (its full
// test is native, Tests/Native/SpoutSlotPoolTests.cpp); and that a stream which cannot be made is refused
// with a reason, takes no Spout access and leaves nothing behind.
//
// What they do not cover: a worker transferring from a sender in another process. Nothing here is a performance
// statement.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "SpoutAsyncReceive.h"
#include "SpoutFrameSync.h"
#include "SpoutLimits.h"
#include "SpoutSdkCountModel.h"
#include "SpoutSlotPool.h"
#include "HAL/IConsoleManager.h"
#include "RenderingThread.h"

#define SPOUT_ASYNC_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncOffByDefault, "SpoutPlugin.Async.OffByDefault", SPOUT_ASYNC_FLAGS)
bool FSpoutAsyncOffByDefault::RunTest(const FString&)
{
	// r.Spout.AsyncReceive is retired: still registered so old command lines do not fail, 0 by default, and
	// setting it only logs that it is ignored. That it selects nothing is tested with real transfers in
	// SpoutPlugin.AsyncBeta.DefaultIsSynchronousAndTheRetiredSettingIsIgnored.
	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.AsyncReceive"));
	if (!Variable)
	{
		AddError(TEXT("r.Spout.AsyncReceive is not registered: old command lines that set it would fail"));
		return false;
	}
	const FString Before = Variable->GetString();
	const EConsoleVariableFlags BeforePriority = static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask);
	if ((Variable->GetFlags() & ECVF_SetByMask) == ECVF_SetByConstructor)
	{
		TestEqual(TEXT("untouched, the retired setting is 0"), Variable->GetInt(), 0);
	}
	else
	{
		AddInfo(TEXT("r.Spout.AsyncReceive was set before this test; its default is checked in the source."));
	}
	TestTrue(TEXT("its help says it is retired and ignored"), FString(Variable->GetHelp()).Contains(TEXT("Retired and ignored")));
	// One warning for the 1 below, and one more when restoring a value that was not 0.
	AddExpectedMessage(TEXT("r.Spout.AsyncReceive is retired and ignored"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains,
		FCString::Atoi(*Before) != 0 ? 2 : 1);
	Variable->Set(TEXT("1"), ECVF_SetByCode);
	Variable->Set(TEXT("0"), ECVF_SetByCode);
	Variable->Set(*Before, ECVF_SetByCode);
	Variable->ClearFlags(ECVF_SetByMask);
	Variable->SetFlags(BeforePriority);

	// The diagnostic beside it: 0 unless set, and never outside what DXGI accepts.
	IConsoleVariable* Priority = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.AsyncReceive.GpuPriority"));
	if (!Priority)
	{
		AddError(TEXT("r.Spout.AsyncReceive.GpuPriority is not registered"));
		return false;
	}
	const FString PriorityBefore = Priority->GetString();
	if ((Priority->GetFlags() & ECVF_SetByMask) == ECVF_SetByConstructor)
	{
		TestEqual(TEXT("untouched, the worker device's GPU priority is left alone"), SpoutLimits::AsyncReceiveGpuPriority(), 0);
	}
	Priority->Set(TEXT("7"), ECVF_SetByCode);
	TestEqual(TEXT("7 is the highest"), SpoutLimits::AsyncReceiveGpuPriority(), 7);
	Priority->Set(TEXT("100"), ECVF_SetByCode);
	TestEqual(TEXT("above the range is clamped"), SpoutLimits::AsyncReceiveGpuPriority(), 7);
	Priority->Set(TEXT("-100"), ECVF_SetByCode);
	TestEqual(TEXT("below the range is clamped"), SpoutLimits::AsyncReceiveGpuPriority(), -7);
	Priority->Set(*PriorityBefore, ECVF_SetByCode);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncPoll, "SpoutPlugin.Async.TheWorkersPollTouchesNoPendingState", SPOUT_ASYNC_FLAGS)
bool FSpoutAsyncPoll::RunTest(const FString&)
{
	TUniquePtr<FSpoutSdkCountModelBackend> Owned = MakeUnique<FSpoutSdkCountModelBackend>();
	FSpoutSdkCountModelBackend* Fake = Owned.Get();
	Fake->SetPublications(100);
	FSpoutFrameSync Sync(MoveTemp(Owned));

	bool bUsable = true;
	TestFalse(TEXT("nothing open: no notification"), Sync.PollNotification(bUsable));
	TestFalse(TEXT("nothing open: no usable count"), bUsable);

	Sync.Open("SpoutAsyncPoll");
	const int32 Queries = Fake->NewFrameQueries;
	TestTrue(TEXT("the first poll sees what the sender published"), Sync.PollNotification(bUsable));
	TestTrue(TEXT("with a usable count"), bUsable);
	TestEqual(TEXT("one SDK read per poll"), Fake->NewFrameQueries, Queries + 1);
	TestFalse(TEXT("a poll makes no frame pending"), Sync.HasPendingFrame());
	TestFalse(TEXT("a second poll finds nothing new"), Sync.PollNotification(bUsable));
	TestTrue(TEXT("the count is still usable"), bUsable);
	Fake->Publish();
	TestTrue(TEXT("a publication is seen by the next poll"), Sync.PollNotification(bUsable));
	TestFalse(TEXT("and only once"), Sync.PollNotification(bUsable));
	TestFalse(TEXT("still nothing is pending"), Sync.HasPendingFrame());
	TestEqual(TEXT("polling attempted no texture access"), Fake->AccessAttempts, 0);
	TestFalse(TEXT("and holds none"), Sync.IsAccessHeld());

	// The same publication cannot be seen by both readers: the poll consumed it. That is why a stream on the
	// asynchronous path leaves the synchronous read alone.
	Fake->Publish();
	TestTrue(TEXT("the poll takes a publication"), Sync.PollNotification(bUsable));
	TestFalse(TEXT("which the synchronous read then does not report again"), Sync.ConsumeNewFrame());

	// A sender without a frame count: the SDK answers true every time, and the count is not usable.
	Fake->bCountingEnabled = false;
	TestTrue(TEXT("without a count every poll answers true"), Sync.PollNotification(bUsable));
	TestFalse(TEXT("and reports no usable count"), bUsable);
	TestTrue(TEXT("again"), Sync.PollNotification(bUsable));

	Sync.Close();
	TestFalse(TEXT("after close: no notification"), Sync.PollNotification(bUsable));
	TestFalse(TEXT("after close: no usable count"), bUsable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncSlotPool, "SpoutPlugin.Async.SlotPoolRulesUnderUnreal", SPOUT_ASYNC_FLAGS)
bool FSpoutAsyncSlotPool::RunTest(const FString&)
{
	using namespace SpoutAsync;
	// The bookkeeping's full test is native. This one compiles the same header with Unreal's toolchain and
	// walks the four rare cases once each.
	FSlotPool Pool;
	FConsumed Taken;
	TestEqual(TEXT("three slots"), Pool.GetNumSlots(), 3);
	TestTrue(TEXT("nothing to read at the start"), Pool.Consume(1, 1, Taken) == EConsume::Nothing);

	// Replaced unread: two publications between two reads.
	TestEqual(TEXT("first publication"), Pool.Publish(Pool.BeginWrite(0), 1, 1, 10), uint64(1));
	TestEqual(TEXT("second publication"), Pool.Publish(Pool.BeginWrite(0), 1, 2, 20), uint64(2));
	TestEqual(TEXT("the first was replaced unread"), Pool.GetCounters().ReplacedUnread, int64(1));
	TestTrue(TEXT("the reader gets the newer one"), Pool.Consume(1, 1, Taken) == EConsume::Consumed && Taken.Tag == 20 && Taken.WrittenValue == 2 && Taken.ReadValue == 1);

	// No free slot: two reads queued and not completed, a third image ready.
	Pool.Publish(Pool.BeginWrite(0), 1, 3, 30);
	TestTrue(TEXT("a second read is queued"), Pool.Consume(1, 2, Taken) == EConsume::Consumed);
	Pool.Publish(Pool.BeginWrite(0), 1, 4, 40);
	TestEqual(TEXT("with no read completed there is no slot to write"), Pool.BeginWrite(0), -1);
	TestEqual(TEXT("counted as a missing slot"), Pool.GetCounters().NoFreeSlot, int64(1));
	TestEqual(TEXT("the worker may wait for read 1"), Pool.OldestPendingReadValue(), uint64(1));
	const int32 AfterRead = Pool.BeginWrite(1);
	TestTrue(TEXT("once read 1 completed its slot is written"), AfterRead >= 0 && Pool.GetSlot(AfterRead).State == ESlotState::Writing);

	// Refused access and a completion that timed out: the slot is free and nothing was published.
	const int64 PublishedBefore = Pool.GetCounters().Published;
	Pool.AbandonWrite(AfterRead, EAbandon::AccessRefused);
	const int32 Again = Pool.BeginWrite(1);
	TestEqual(TEXT("the abandoned slot is offered again"), Again, AfterRead);
	Pool.AbandonWrite(Again, EAbandon::CompletionTimeout);
	TestEqual(TEXT("one refused access"), Pool.GetCounters().Abandoned[static_cast<int32>(EAbandon::AccessRefused)], int64(1));
	TestEqual(TEXT("one completion timeout"), Pool.GetCounters().Abandoned[static_cast<int32>(EAbandon::CompletionTimeout)], int64(1));
	TestEqual(TEXT("neither published anything"), Pool.GetCounters().Published, PublishedBefore);
	TestEqual(TEXT("a later copy into that slot is published"), Pool.Publish(Pool.BeginWrite(1), 1, 5, 50), uint64(5));
	TestEqual(TEXT("and counted as following a write that was given up"), Pool.GetCounters().PublishedAfterReuse, int64(1));

	// Another generation is dropped unread; a retired pool accepts nothing.
	TestTrue(TEXT("an image of another generation is dropped"), Pool.Consume(2, 3, Taken) == EConsume::DroppedOldGeneration && Taken.Slot == -1);
	Pool.Retire();
	TestEqual(TEXT("a retired pool accepts no write"), Pool.BeginWrite(99), -1);
	TestFalse(TEXT("and is not destroyable while read 2 has not completed"), Pool.CanDestroy(1));
	TestTrue(TEXT("but is once it has"), Pool.CanDestroy(2));
	TestEqual(TEXT("no rule was broken"), Pool.GetCounters().RuleViolations, int64(0));

	// What starts a transfer.
	FTransferGate Gate;
	TestFalse(TEXT("nothing is owed before the stream opens"), Gate.IsOwed());
	Gate.Reset();
	TestTrue(TEXT("open owes the current image"), Gate.IsOwed());
	Gate.OnPublished();
	Gate.ObserveCounted(false);
	TestFalse(TEXT("a completed copy is not repeated"), Gate.IsOwed());
	Gate.ObserveCounted(true);
	TestTrue(TEXT("a notification owes a transfer"), Gate.IsOwed());
	Gate.OnPublished();
	for (int32 K = 0; K < 100; ++K) Gate.ObserveUncounted();
	TestFalse(TEXT("without a count and without a read nothing starts"), Gate.IsOwed());
	Gate.OnConsumed();
	Gate.ObserveUncounted();
	TestTrue(TEXT("one read allows one transfer"), Gate.IsOwed());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAsyncRefusals, "SpoutPlugin.Async.AStreamThatCannotBeMadeIsRefusedWithAReason", SPOUT_ASYNC_FLAGS)
bool FSpoutAsyncRefusals::RunTest(const FString&)
{
	TUniquePtr<FSpoutSdkCountModelBackend> Owned = MakeUnique<FSpoutSdkCountModelBackend>();
	FSpoutSdkCountModelBackend* Fake = Owned.Get();
	Fake->SetPublications(100);
	FSpoutFrameSync Sync(MoveTemp(Owned));
	Sync.Open("SpoutAsyncRefusal");

	struct FOutcome
	{
		bool bMade = true;
		FString Why;
	};
	TSharedRef<FOutcome, ESPMode::ThreadSafe> Unsupported = MakeShared<FOutcome, ESPMode::ThreadSafe>();
	TSharedRef<FOutcome, ESPMode::ThreadSafe> NoTexture = MakeShared<FOutcome, ESPMode::ThreadSafe>();
	FSpoutFrameSync* SyncPtr = &Sync;
	ENQUEUE_RENDER_COMMAND(SpoutAsyncRefusalTest)([SyncPtr, Unsupported, NoTexture](FRHICommandListImmediate&)
	{
		FSpoutAsyncStream::FIdentity Identity;
		Identity.Width = 64;
		Identity.Height = 32;
		// A format the asynchronous receive does not handle.
		Identity.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		Unsupported->bMade = FSpoutAsyncStream::Create(FName(TEXT("SpoutAsyncRefusal")), -1, *SyncPtr, Identity, PF_FloatRGBA, Unsupported->Why).IsValid();
		// A format it handles, and a null handle: refused before anything is reserved or opened.
		Identity.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		NoTexture->bMade = FSpoutAsyncStream::Create(FName(TEXT("SpoutAsyncRefusal")), -1, *SyncPtr, Identity, PF_B8G8R8A8, NoTexture->Why).IsValid();
	});
	FlushRenderingCommands();

	TestFalse(TEXT("an unsupported format is refused"), Unsupported->bMade);
	TestEqual(TEXT("for its format"), Unsupported->Why, FString(TEXT("the prototype handles BGRA8 and RGBA8 senders only")));
	TestFalse(TEXT("a sender without a texture is refused"), NoTexture->bMade);
	TestEqual(TEXT("for its null handle"), NoTexture->Why, FString(TEXT("the sender's shared texture handle is null")));
	AddInfo(FString::Printf(TEXT("refused: '%s' and '%s'"), *Unsupported->Why, *NoTexture->Why));
	TestEqual(TEXT("a refused stream attempted no Spout access"), Fake->AccessAttempts, 0);
	TestFalse(TEXT("and holds none"), Sync.IsAccessHeld());
	TestFalse(TEXT("nor made a frame pending"), Sync.HasPendingFrame());
	Sync.Close();
	return true;
}

#endif
