// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
//
// Bounded texture-access wait (r.Spout.AccessWaitMs). Each access attempt waits at most the
// configured bound for the peer to let go, so a read that lands inside a peer's short lock window
// waits for it instead of skipping the frame. These tests pin the setting's range, its per-attempt
// use, that the game thread's status query never queues behind a wait, and the real named mutex's
// timing, release, timeout and abandonment behaviour. Keyed-mutex and cross-process cases are
// covered by Tests/Native.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
#include "SpoutFrameSync.h"
#include "SpoutLimits.h"
#include "SpoutModule.h"
#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "RenderingThread.h"
#include <atomic>
#include <thread>

#define SPOUT_ACCESS_WAIT_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

namespace
{
	const TCHAR* const AccessWaitName = TEXT("r.Spout.AccessWaitMs");

	/** Restores the setting's value on scope exit, whatever a test set it to. */
	struct FAccessWaitScope
	{
		IConsoleVariable* Variable;
		FString Before;
		FAccessWaitScope() : Variable(IConsoleManager::Get().FindConsoleVariable(AccessWaitName)), Before(Variable ? Variable->GetString() : FString()) {}
		~FAccessWaitScope() { if (Variable) Variable->Set(*Before, ECVF_SetByCode); }
		void Set(const TCHAR* Value) { Variable->Set(Value, ECVF_SetByCode); }
	};

	/** Records the wait every access attempt was given; can block inside an attempt on request. */
	class FWaitRecordingBackend final : public ISpoutFrameSyncBackend
	{
	public:
		TArray<uint32> Timeouts;
		bool bRefuse = false;
		FEvent* Entered = nullptr;   // set when an attempt starts, if non-null
		FEvent* Proceed = nullptr;   // an attempt blocks on this, if non-null
		virtual bool CreateAccessMutex(const ANSICHAR*) override { return true; }
		virtual void CloseAccessMutex() override {}
		virtual void EnableFrameCount(const ANSICHAR*) override {}
		virtual void CleanupFrameCount() override {}
		virtual bool CheckTextureAccess(void*, uint32 TimeoutMs) override
		{
			Timeouts.Add(TimeoutMs);
			if (Entered) Entered->Trigger();
			if (Proceed) Proceed->Wait(5000);
			return !bRefuse;
		}
		virtual bool AllowTextureAccess(void*) override { return true; }
		virtual void SetNewFrame() override {}
		virtual bool GetNewFrame() override { return false; }
	};

	void* const FakeTexture = reinterpret_cast<void*>(static_cast<UPTRINT>(0xB000));

	FString UniqueName(const TCHAR* Suffix)
	{
		return FString::Printf(TEXT("SpoutWait_%s_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits), Suffix);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAccessWaitSetting, "SpoutPlugin.AccessWait.DefaultIsTwoAndTheRangeIsClamped", SPOUT_ACCESS_WAIT_FLAGS)
bool FSpoutAccessWaitSetting::RunTest(const FString&)
{
	FAccessWaitScope Scope;
	if (!Scope.Variable)
	{
		AddError(TEXT("r.Spout.AccessWaitMs is not registered."));
		return false;
	}
	TestTrue(TEXT("the setting is read on the render thread, so it is render-thread safe"), (Scope.Variable->GetFlags() & ECVF_RenderThreadSafe) != 0);
	TestEqual(TEXT("the default is 2 ms"), SpoutLimits::DefaultAccessWaitMs, 2);
	TestEqual(TEXT("the maximum is 10 ms"), SpoutLimits::MaxAccessWaitMs, 10);
	if ((Scope.Variable->GetFlags() & ECVF_SetByMask) == ECVF_SetByConstructor)
	{
		TestEqual(TEXT("an untouched setting resolves to the default"), int32(SpoutLimits::AccessWaitMs()), 2);
	}
	else
	{
		AddInfo(FString::Printf(TEXT("r.Spout.AccessWaitMs was already set (to %s) before this test; the untouched default was not observed."), *Scope.Before));
	}

	struct FCase { const TCHAR* Text; int32 Expected; };
	const FCase Cases[] = {
		{ TEXT("0"), 0 }, { TEXT("1"), 1 }, { TEXT("2"), 2 }, { TEXT("9"), 9 }, { TEXT("10"), 10 },
		{ TEXT("11"), 10 }, { TEXT("1000"), 10 }, { TEXT("2147483647"), 10 },
		{ TEXT("-1"), 0 }, { TEXT("-2147483648"), 0 } };
	for (const FCase& Case : Cases)
	{
		Scope.Set(Case.Text);
		TestEqual(FString::Printf(TEXT("'%s' resolves to %d ms"), Case.Text, Case.Expected), int32(SpoutLimits::AccessWaitMs()), Case.Expected);
	}

	// Text that is not a plain integer: whatever the engine's parser makes of it, the wait stays
	// inside the safe range. Nothing can produce a negative, overflowed or infinite wait.
	const TCHAR* const Odd[] = { TEXT("abc"), TEXT(""), TEXT("2.9"), TEXT("1e9"), TEXT("99999999999"), TEXT("-99999999999"), TEXT("4294967295"), TEXT("0x7fffffff") };
	for (const TCHAR* Text : Odd)
	{
		Scope.Set(TEXT("5"));
		Scope.Set(Text);
		const uint32 Resolved = SpoutLimits::AccessWaitMs();
		TestTrue(FString::Printf(TEXT("'%s' stays within 0..10 ms"), Text), Resolved <= 10u);
		AddInfo(FString::Printf(TEXT("'%s' -> variable %s, resolved %u ms"), Text, *Scope.Variable->GetString(), Resolved));
	}

	// The render thread reads its own copy, which follows the game thread's value in command order.
	Scope.Set(TEXT("7"));
	std::atomic<uint32> OnRenderThread{ MAX_uint32 };
	ENQUEUE_RENDER_COMMAND(SpoutAccessWaitRead)([&OnRenderThread](FRHICommandListImmediate&) { OnRenderThread = SpoutLimits::AccessWaitMs(); });
	FlushRenderingCommands();
	TestEqual(TEXT("the render thread sees the value set before its command"), int32(OnRenderThread.load()), 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAccessWaitPerAttempt, "SpoutPlugin.AccessWait.EveryAttemptUsesTheCurrentSetting", SPOUT_ACCESS_WAIT_FLAGS)
bool FSpoutAccessWaitPerAttempt::RunTest(const FString&)
{
	FAccessWaitScope Scope;
	if (!Scope.Variable) { AddError(TEXT("r.Spout.AccessWaitMs is not registered.")); return false; }
	TUniquePtr<FWaitRecordingBackend> Owned = MakeUnique<FWaitRecordingBackend>();
	FWaitRecordingBackend* Fake = Owned.Get();
	FSpoutFrameSync Sync(MoveTemp(Owned));
	TestTrue(TEXT("opened"), Sync.Open("SpoutWaitPerAttempt"));

	struct FCase { const TCHAR* Text; uint32 Expected; };
	const FCase Cases[] = { { TEXT("0"), 0u }, { TEXT("2"), 2u }, { TEXT("7"), 7u }, { TEXT("25"), 10u }, { TEXT("-3"), 0u } };
	for (const FCase& Case : Cases)
	{
		Scope.Set(Case.Text);
		FSpoutTextureAccessScope Access(Sync, FakeTexture);
		TestTrue(TEXT("granted"), Access.IsAcquired());
		TestEqual(FString::Printf(TEXT("the attempt after setting '%s' waits at most %u ms"), Case.Text, Case.Expected), int32(Fake->Timeouts.Last()), int32(Case.Expected));
	}

	// A timed-out attempt owns nothing and releases nothing, whatever the bound.
	Scope.Set(TEXT("2"));
	Fake->bRefuse = true;
	{
		FSpoutTextureAccessScope Access(Sync, FakeTexture);
		TestFalse(TEXT("a timed-out bounded attempt is refused"), Access.IsAcquired());
	}
	TestEqual(TEXT("and counted as one miss"), Sync.GetFailedAcquireCount(), 1);
	TestEqual(TEXT("releases match grants"), Sync.GetReleaseCount(), Sync.GetAcquireCount());

	// After Close, no attempt reaches the backend, so nothing can wait on a closed handle.
	const int32 Before = Fake->Timeouts.Num();
	Sync.Close();
	{
		FSpoutTextureAccessScope Access(Sync, FakeTexture);
		TestFalse(TEXT("a closed stream refuses access"), Access.IsAcquired());
	}
	TestEqual(TEXT("without consulting the backend"), Fake->Timeouts.Num(), Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAccessWaitStatus, "SpoutPlugin.AccessWait.StatusQueryIsNotHeldUpByAnAccessWait", SPOUT_ACCESS_WAIT_FLAGS)
bool FSpoutAccessWaitStatus::RunTest(const FString&)
{
	TUniquePtr<FWaitRecordingBackend> Owned = MakeUnique<FWaitRecordingBackend>();
	FWaitRecordingBackend* Fake = Owned.Get();
	FSpoutFrameSync Sync(MoveTemp(Owned));
	Sync.Open("SpoutWaitStatus");
	FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* Proceed = FPlatformProcess::GetSynchEventFromPool(true);
	Fake->Entered = Entered;
	Fake->Proceed = Proceed;
	Fake->bRefuse = true;

	// Another thread sits inside an access attempt (holding the frame-sync lock) until released.
	TFuture<bool> Worker = Async(EAsyncExecution::Thread, [&Sync]()
	{
		FSpoutTextureAccessScope Access(Sync, FakeTexture);
		return Access.IsAcquired();
	});
	const bool bInside = Entered->Wait(5000);
	TestTrue(TEXT("the other thread is inside its access attempt"), bInside);

	const double Begin = FPlatformTime::Seconds();
	int64 Frames = -1, Misses = -1;
	Sync.GetStatus(Frames, Misses);
	const double StatusMs = (FPlatformTime::Seconds() - Begin) * 1000.0;
	AddInfo(FString::Printf(TEXT("status query during a blocked attempt took %.3f ms"), StatusMs));
	TestTrue(TEXT("the status query returns while an access attempt is in progress"), StatusMs < 50.0);
	TestEqual(TEXT("and reports no miss yet"), int32(Misses), 0);

	Proceed->Trigger();
	TestFalse(TEXT("the blocked attempt then times out"), Worker.Get());
	Sync.GetStatus(Frames, Misses);
	TestEqual(TEXT("and is counted"), int32(Misses), 1);
	Fake->Entered = nullptr;
	Fake->Proceed = nullptr;
	FPlatformProcess::ReturnSynchEventToPool(Entered);
	FPlatformProcess::ReturnSynchEventToPool(Proceed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutAccessWaitNamedMutex, "SpoutPlugin.AccessWait.RealNamedMutexHonoursTheBound", SPOUT_ACCESS_WAIT_FLAGS)
bool FSpoutAccessWaitNamedMutex::RunTest(const FString&)
{
	if (!FSpoutModule::IsSpoutDllLoaded())
	{
		AddError(TEXT("Spout.dll is not loaded in this process, so the real access mutex cannot be tested."));
		return false;
	}
	FAccessWaitScope Scope;
	if (!Scope.Variable) { AddError(TEXT("r.Spout.AccessWaitMs is not registered.")); return false; }
	const FString Name = UniqueName(TEXT("Named"));
	const auto Utf8 = StringCast<ANSICHAR>(*Name);
	const ANSICHAR* const NameUtf8 = Utf8.Get();

	FSpoutFrameSync Sync(MakeSpoutSdkFrameSyncBackend());
	if (!Sync.Open(NameUtf8)) { AddError(TEXT("could not open the access mutex")); return false; }

	// A holder on another thread owns the same named mutex the way a peer would. Windows mutex
	// ownership belongs to a thread, so it must be released on the holder thread.
	FEvent* Held = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* Release = FPlatformProcess::GetSynchEventFromPool(true);
	std::atomic<int32> ReleaseDelayMs{ -1 };
	std::atomic<bool> HolderOk{ false };
	std::thread Holder([&]()
	{
		TUniquePtr<ISpoutFrameSyncBackend> Peer = MakeSpoutSdkFrameSyncBackend();
		HolderOk = Peer && Peer->CreateAccessMutex(NameUtf8) && Peer->CheckTextureAccess(nullptr, 0);
		Held->Trigger();
		if (!HolderOk) return;
		Release->Wait(10000);
		// Hold for the delay by the high-resolution clock, not by sleeping: a process can run at
		// the default Windows timer resolution (about 15.6 ms), where a 2 ms sleep lasts about
		// 15 ms and would outlast the attempt under test.
		const double HoldUntil = FPlatformTime::Seconds() + FMath::Max(0, ReleaseDelayMs.load()) / 1000.0;
		while (FPlatformTime::Seconds() < HoldUntil) { FPlatformProcess::YieldThread(); }
		Peer->AllowTextureAccess(nullptr);
		Peer->CloseAccessMutex();
		RecycleSpoutSdkFrameSyncBackend(MoveTemp(Peer));
	});
	Held->Wait(10000);
	TestTrue(TEXT("the holder owns the named mutex"), HolderOk.load());

	auto TimedAttempt = [&Sync](double& OutMs)
	{
		const double Begin = FPlatformTime::Seconds();
		FSpoutTextureAccessScope Access(Sync, nullptr);
		OutMs = (FPlatformTime::Seconds() - Begin) * 1000.0;
		return Access.IsAcquired();
	};

	if (HolderOk)
	{
		// Zero wait: 50 attempts against a held mutex return at once, none granted.
		Scope.Set(TEXT("0"));
		double TotalZero = 0.0, Ms = 0.0;
		bool AnyZero = false;
		for (int32 i = 0; i < 50; ++i) { AnyZero |= TimedAttempt(Ms); TotalZero += Ms; }
		TestFalse(TEXT("zero wait never takes a held mutex"), AnyZero);
		TestTrue(TEXT("50 zero-wait attempts take under 100 ms in total"), TotalZero < 100.0);

		// Default wait: each attempt times out, waits a measurable time, and stays bounded.
		Scope.Set(TEXT("2"));
		TArray<double> Waits;
		bool AnyBounded = false;
		for (int32 i = 0; i < 10; ++i) { AnyBounded |= TimedAttempt(Ms); Waits.Add(Ms); }
		Waits.Sort();
		AddInfo(FString::Printf(TEXT("zero wait: 50 attempts %.3f ms; 2 ms wait: min %.3f, median %.3f, max %.3f ms"), TotalZero, Waits[0], Waits[5], Waits.Last()));
		TestFalse(TEXT("a bounded wait never takes a mutex the peer keeps holding"), AnyBounded);
		TestTrue(TEXT("a 2 ms attempt actually waits"), Waits[5] >= 0.25);
		TestTrue(TEXT("a 2 ms attempt ends within 100 ms"), Waits.Last() < 100.0);

		// The maximum: 10 ms, still bounded.
		Scope.Set(TEXT("1000"));
		const bool bMax = TimedAttempt(Ms);
		AddInfo(FString::Printf(TEXT("clamped maximum attempt: %.3f ms"), Ms));
		TestFalse(TEXT("the clamped maximum times out too"), bMax);
		TestTrue(TEXT("the clamped maximum ends within 200 ms"), Ms < 200.0);
		TestEqual(TEXT("four kinds of attempt, all counted as misses"), Sync.GetFailedAcquireCount(), 50 + 10 + 1);

		// A peer that lets go during the wait grants the attempt: a short hold costs no frame.
		Scope.Set(TEXT("10"));
		ReleaseDelayMs = 2;
		Release->Trigger();
		const bool bGranted = TimedAttempt(Ms);
		AddInfo(FString::Printf(TEXT("release 2 ms into a 10 ms attempt: granted=%d after %.3f ms"), bGranted ? 1 : 0, Ms));
		TestTrue(TEXT("a peer that releases within the bound grants the attempt"), bGranted);
		TestEqual(TEXT("the grant was released exactly once"), Sync.GetReleaseCount(), Sync.GetAcquireCount());
	}
	else
	{
		Release->Trigger();
	}
	Holder.join();

	// A holder thread that ends while owning the mutex abandons it. The next attempt is refused
	// (the frame may be inconsistent) but releases the abandoned ownership, and access recovers.
	Scope.Set(TEXT("2"));
	std::atomic<bool> AbandonOk{ false };
	TUniquePtr<ISpoutFrameSyncBackend> AbandonedPeer;
	std::thread Abandoner([&]()
	{
		AbandonedPeer = MakeSpoutSdkFrameSyncBackend();
		// No release: the thread exits owning the mutex, as a crashed peer would leave it. The
		// backend object outlives the thread so its handle is closed afterwards, not leaked.
		AbandonOk = AbandonedPeer && AbandonedPeer->CreateAccessMutex(NameUtf8) && AbandonedPeer->CheckTextureAccess(nullptr, 0);
	});
	Abandoner.join();
	TestTrue(TEXT("the abandoning thread owned the mutex"), AbandonOk.load());
	double Ms = 0.0;
	const bool bFirst = TimedAttempt(Ms);
	TestFalse(TEXT("an abandoned mutex discards that frame"), bFirst);
	TestTrue(TEXT("without outliving the bound"), Ms < 100.0);
	const bool bSecond = TimedAttempt(Ms);
	TestTrue(TEXT("and the next attempt recovers"), bSecond);
	TestEqual(TEXT("every grant was released exactly once"), Sync.GetReleaseCount(), Sync.GetAcquireCount());
	TestFalse(TEXT("nothing is left held"), Sync.IsAccessHeld());

	if (AbandonedPeer) { AbandonedPeer->CloseAccessMutex(); AbandonedPeer.Reset(); }
	Sync.Close();
	FPlatformProcess::ReturnSynchEventToPool(Held);
	FPlatformProcess::ReturnSynchEventToPool(Release);
	return true;
}
#endif
