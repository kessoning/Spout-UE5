// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// The experimental new-frame wait (r.Spout.FrameWaitMs, off by default).
//
// The predictor is pure interval arithmetic and is driven here with made-up times. The polling loop
// is driven through FSpoutFrameSync with FSpoutSdkCountModelBackend, which reproduces the bundled
// SDK's count arithmetic (SpoutSdkCountModel.h). The notification state has its own tests in
// SpoutPluginNotificationTests.cpp and, without Unreal, in Tests/Native/SpoutNotificationTests.cpp.
//
// Three predictor policies are driven, so that every difference is pinned on all sides:
//   Baseline   EWaitRule::UpperEdge   + ERecovery::Original
//   Overlap    EWaitRule::Overlap     + ERecovery::Original
//   Corrected  EWaitRule::Conditional + ERecovery::Adaptive   (the plugin's default)
//
// What these tests pin:
//   - the settings are off by default, are clamped, and the wait implies the recheck;
//   - the window against the waiting interval under each rule;
//   - accurately observed publications outside a carried window widen the allowance (bounded,
//     decaying) instead of suspending waits that are getting their frames;
//   - a late publication keeps the sender's grid;
//   - slower sources (30, 40, 50, 57 fps read at 60 Hz) are not waited for at all, which is a
//     tradeoff: the freshness a wait could give at 30 fps is pinned beside it;
//   - wasted-wait scoring, suspension, escalation and its reset by a success, probes of an escalated
//     suspension, and that a sender restart does not clear a suspension;
//   - stale (paused) predictions, restarts, a changing rate and an irregular source;
//   - the count read under access is observed at the time it was read;
//   - the loop: publication, timeout, abort (not charged as wasted), Close, access held, a frame
//     already pending, another receiver's read during the wait, the count going away; two timeouts
//     suspend; the budget is shared, shortens the bound the rule sees, and an empty budget refuses.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
#include "SpoutFrameSync.h"
#include "SpoutFrameWait.h"
#include "SpoutLimits.h"
#include "SpoutSdkCountModel.h"
#include "Algo/BinarySearch.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"

#define SPOUT_FRAME_WAIT_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

namespace
{
	using SpoutFrameWait::EDecision;
	using SpoutFrameWait::EOutcome;
	using SpoutFrameWait::ERecovery;
	using SpoutFrameWait::EWaitRule;
	using SpoutFrameWait::FPublishPredictor;
	using SpoutFrameWait::FPrediction;
	using SpoutFrameWait::FTuning;

	/** Restores a console variable on scope exit. */
	struct FWaitCVarScope
	{
		IConsoleVariable* Variable;
		FString Before;
		explicit FWaitCVarScope(const TCHAR* Name) : Variable(IConsoleManager::Get().FindConsoleVariable(Name)), Before(Variable ? Variable->GetString() : FString()) {}
		~FWaitCVarScope() { if (Variable) Variable->Set(*Before, ECVF_SetByCode); }
		void Set(const TCHAR* Value) { if (Variable) Variable->Set(Value, ECVF_SetByCode); }
	};

	void* const WaitFakeTexture = reinterpret_cast<void*>(static_cast<UPTRINT>(0xB100));

	enum class EPolicy : int32 { Baseline, Overlap, Corrected };
	const EPolicy AllPolicies[] = { EPolicy::Baseline, EPolicy::Overlap, EPolicy::Corrected };

	void Configure(FPublishPredictor& P, EPolicy Policy)
	{
		switch (Policy)
		{
		case EPolicy::Baseline: P.SetRule(EWaitRule::UpperEdge); P.SetRecovery(ERecovery::Original); break;
		case EPolicy::Overlap: P.SetRule(EWaitRule::Overlap); P.SetRecovery(ERecovery::Original); break;
		default: P.SetRule(EWaitRule::Conditional); P.SetRecovery(ERecovery::Adaptive); break;
		}
	}

	const TCHAR* PolicyName(EPolicy Policy)
	{
		return Policy == EPolicy::Baseline ? TEXT("baseline") : (Policy == EPolicy::Overlap ? TEXT("overlap") : TEXT("corrected"));
	}

	/** Per read of a simulation: -1 when the read found a new frame, else the decision; and the outcome of a wait. */
	struct FSimRead
	{
		int32 Decision = -1;
		int32 Outcome = int32(EOutcome::NotAttempted);
		bool bProbe = false; // the wait was a probe of a suspension
		double EarliestAfterRead = 0.0; // the predicted window relative to the read, seconds (0 when none)
		double LatestAfterRead = 0.0;
		double NextPublicationAfterRead = 0.0; // where it actually came (retrospective)
	};

	/**
	 * A sender publishing at given times and a receiver reading at given times, with the predictor in
	 * between and the wait modelled exactly as the plugin performs it (polls every PollS until the
	 * count moves above the count at the read, or the bound elapses). Publishes must be sorted.
	 * Without a predictor the receiver never waits (waiting off). Also measured: the
	 * publications never acquired (Skipped) and, at every read, the age of the frame held after it
	 * (read time + time waited - its publication time), so delivery, freshness and wasted waiting are
	 * reported separately.
	 */
	struct FSimResult
	{
		int32 ReadsWithoutNewFrame = 0;
		int32 Waits = 0;
		int32 Published = 0;
		int32 Wasted = 0;
		int32 NotImminent = 0;
		int32 NotImminentWithinBound = 0; // refused, but the next publication came within the bound after the read
		int32 SlowerSource = 0;
		int32 Overdue = 0;
		int32 Suspended = 0;
		int32 Warmup = 0;
		int32 Probes = 0;
		int32 Skipped = 0;
		double AgeSumSeconds = 0.0;
		int32 AgeReads = 0;
		double MaxLatestMinusTruth = -1e9; // how far the predicted upper bound exceeded the true publication, on successful waits
		TArray<FSimRead> PerRead;
		double MeanAgeSeconds() const { return AgeReads > 0 ? AgeSumSeconds / AgeReads : 0.0; }
	};

	/** Counts: the SDK frame count of each publication (a sender restart starts it again); null = 101, 102, ... */
	FSimResult SimulateWith(FPublishPredictor* Predictor, const TArray<double>& Publishes, const TArray<double>& Reads, double WaitS, double PollS, const TArray<int64>* Counts = nullptr)
	{
		FSimResult R;
		R.PerRead.SetNum(Reads.Num());
		auto IndexAt = [&Publishes](double T) { return int32(Algo::UpperBound(Publishes, T)) - 1; };
		auto CountAt = [&IndexAt, Counts](double T)
		{
			const int32 I = IndexAt(T);
			return I < 0 ? int64(100) : (Counts ? (*Counts)[I] : int64(101 + I));
		};
		auto NextAfter = [&Publishes](double T)
		{
			const int32 I = Algo::UpperBound(Publishes, T);
			return I < Publishes.Num() ? Publishes[I] : 1e30;
		};
		int64 Consumed = -1;
		int32 Held = -1;
		auto Acquire = [&R, &Held](int32 I)
		{
			if (Held >= 0 && I > Held) R.Skipped += I - Held - 1;
			Held = I;
		};
		for (int32 Index = 0; Index < Reads.Num(); ++Index)
		{
			const double Read = Reads[Index];
			double Waited = 0.0;
			if (Predictor) Predictor->BeginRead(Read);
			const int64 C = CountAt(Read);
			if (Predictor) Predictor->ObserveCount(Read, C);
			if (C != Consumed)
			{
				Consumed = C;
				Acquire(IndexAt(Read));
			}
			else
			{
				++R.ReadsWithoutNewFrame;
				if (Predictor)
				{
					const FPrediction P = Predictor->Predict(Read, WaitS);
					R.PerRead[Index].Decision = int32(P.Decision);
					R.PerRead[Index].bProbe = P.bProbe;
					R.PerRead[Index].EarliestAfterRead = P.EarliestSeconds > 0.0 ? P.EarliestSeconds - Read : 0.0;
					R.PerRead[Index].LatestAfterRead = P.LatestSeconds > 0.0 ? P.LatestSeconds - Read : 0.0;
					R.PerRead[Index].NextPublicationAfterRead = NextAfter(Read) - Read;
					switch (P.Decision)
					{
					case EDecision::NotImminent:
						++R.NotImminent;
						if (NextAfter(Read) <= Read + WaitS) ++R.NotImminentWithinBound;
						break;
					case EDecision::SlowerSource: ++R.SlowerSource; break;
					case EDecision::Overdue: ++R.Overdue; break;
					case EDecision::Suspended: ++R.Suspended; break;
					case EDecision::Warmup: ++R.Warmup; break;
					case EDecision::Wait:
					{
						++R.Waits;
						if (P.bProbe) ++R.Probes;
						bool bGot = false;
						for (double T = Read + PollS; ; T += PollS)
						{
							const double Poll = FMath::Min(T, Read + WaitS);
							const int64 Now = CountAt(Poll);
							Predictor->ObserveCount(Poll, Now);
							if (Now > C)
							{
								bGot = true;
								Consumed = Now;
								Acquire(IndexAt(Poll));
								Waited = Poll - Read;
								// the publication that ended the wait is the first one after the read
								R.MaxLatestMinusTruth = FMath::Max(R.MaxLatestMinusTruth, P.LatestSeconds - NextAfter(Read));
								break;
							}
							if (Poll >= Read + WaitS)
							{
								Waited = WaitS;
								break;
							}
						}
						if (bGot) { ++R.Published; Predictor->ObserveWaitSucceeded(); R.PerRead[Index].Outcome = int32(EOutcome::Published); }
						else { ++R.Wasted; Predictor->ObserveWaitTimedOut(); R.PerRead[Index].Outcome = int32(EOutcome::TimedOut); }
						break;
					}
					default: break;
					}
				}
			}
			if (Held >= 0)
			{
				R.AgeSumSeconds += Read + Waited - Publishes[Held];
				++R.AgeReads;
			}
		}
		return R;
	}

	FSimResult Simulate(FPublishPredictor& Predictor, const TArray<double>& Publishes, const TArray<double>& Reads, double WaitS, double PollS, const TArray<int64>* Counts = nullptr)
	{
		return SimulateWith(&Predictor, Publishes, Reads, WaitS, PollS, Counts);
	}

	/** The same reads with waiting off. */
	FSimResult SimulateOff(const TArray<double>& Publishes, const TArray<double>& Reads)
	{
		return SimulateWith(nullptr, Publishes, Reads, 0.001, 0.00015);
	}

	/**
	 * A small deterministic generator (a fixed formula and a fixed order of draws), so every expectation built on it
	 * is reproducible.
	 */
	struct FTestLcg
	{
		uint32 State;
		explicit FTestLcg(uint32 Seed) : State(Seed & 0x7fffffffu) {}
		double Next()
		{
			State = (1103515245u * State + 12345u) & 0x7fffffffu;
			return double(State) / 2147483648.0;
		}
		double Range(double Lo, double Hi) { return Lo + (Hi - Lo) * Next(); }
	};

	/** Reads at 60 Hz, 0.2 ms before each 60 Hz slot +- 0.25 ms (uniform). */
	TArray<double> JitteredReads(int32 N, uint32 Seed)
	{
		const double T = 1.0 / 60.0;
		FTestLcg G(Seed);
		TArray<double> Out;
		Out.Reserve(N);
		for (int32 K = 0; K < N; ++K) Out.Add(10.0 + K * T + -0.0002 + G.Range(-0.00025, 0.00025));
		return Out;
	}

	enum class ERecoveryKind : int32 { Stable, Pause, Restart, Rate };

	struct FRecoverySender
	{
		TArray<double> Publishes;
		TArray<int64> Counts;
		bool bUseCounts = false;
		int32 StableIndex = 0; // the read (and 60 Hz slot) from which the sender is regular at 60 fps
	};

	/**
	 * A sender by 60 Hz slot: irregular (+-6 ms, uniform) before IrregularUntil, then regular with 6 % of slots
	 * 0.2 to 0.5 ms late. Pause: the 300 slots after IrregularUntil are not published (5 s). Restart: the count
	 * starts again at 1 there. Rate: for 1200 slots after IrregularUntil only the even slots are published
	 * (30 fps for 20 s), then 60 fps again.
	 */
	FRecoverySender MakeRecoverySender(ERecoveryKind Kind, uint32 Seed, int32 N = 6610, int32 IrregularUntil = 3000)
	{
		const double T = 1.0 / 60.0;
		FTestLcg G(Seed);
		FRecoverySender S;
		int32 RestartRank = INDEX_NONE;
		for (int32 I = 0; I < N; ++I)
		{
			const double Base = 10.0 + I * T;
			double Time = Base;
			if (I < IrregularUntil)
			{
				Time = Base + G.Range(-0.006, 0.006);
			}
			else
			{
				const double U = G.Next();
				Time = Base + (U < 0.06 ? G.Range(0.0002, 0.0005) : 0.0);
			}
			if (Kind == ERecoveryKind::Pause && I >= IrregularUntil && I < IrregularUntil + 300) continue;
			if (Kind == ERecoveryKind::Rate && I >= IrregularUntil && I < IrregularUntil + 1200 && (I % 2) == 1) continue;
			if (Kind == ERecoveryKind::Restart && I >= IrregularUntil && RestartRank == INDEX_NONE) RestartRank = S.Publishes.Num();
			S.Publishes.Add(Time);
			S.Counts.Add(RestartRank == INDEX_NONE ? int64(101 + S.Publishes.Num() - 1) : int64(1 + S.Publishes.Num() - 1 - RestartRank));
		}
		S.bUseCounts = Kind == ERecoveryKind::Restart;
		S.StableIndex = IrregularUntil + (Kind == ERecoveryKind::Pause ? 300 : (Kind == ERecoveryKind::Rate ? 1200 : 0));
		return S;
	}

	/** Counts, over reads [First, Last), the reads with the given decision (and, when given, outcome). */
	int32 CountReads(const FSimResult& R, int32 First, int32 Last, EDecision Decision, int32 Outcome = -1)
	{
		int32 N = 0;
		for (int32 I = FMath::Max(0, First); I < FMath::Min(Last, R.PerRead.Num()); ++I)
		{
			if (R.PerRead[I].Decision == int32(Decision) && (Outcome < 0 || R.PerRead[I].Outcome == Outcome)) ++N;
		}
		return N;
	}

	int32 CountDecided(const FSimResult& R, int32 First, int32 Last)
	{
		int32 N = 0;
		for (int32 I = FMath::Max(0, First); I < FMath::Min(Last, R.PerRead.Num()); ++I) if (R.PerRead[I].Decision >= 0) ++N;
		return N;
	}

	TArray<double> Regular(double Start, double Period, int32 N)
	{
		TArray<double> Out;
		Out.Reserve(N);
		for (int32 I = 0; I < N; ++I) Out.Add(Start + Period * I);
		return Out;
	}

	/** Reads 0.2 ms after each publication for the first Switch reads, 0.2 ms before it afterwards (the alignment band). */
	TArray<double> BandReads(double Start, double Period, int32 N, int32 Switch)
	{
		TArray<double> Out;
		Out.Reserve(N);
		for (int32 I = 0; I < N; ++I) Out.Add(Start + Period * I + (I < Switch ? 0.0002 : -0.0002));
		return Out;
	}

	/**
	 * N publications on a regular grid, each seen at the old count WidthS before it and at the new count
	 * at its instant, one read each (without read times). Returns the next nominal publication. The window
	 * is then [next - WidthS - slack, next + slack], slack = 0.25 ms + 2 x WidthS / (N - 1).
	 */
	double PrimeRegular(FPublishPredictor& P, double Start, double Period, int32 N, double WidthS, int64 BaseCount = 500)
	{
		for (int32 I = 0; I < N; ++I)
		{
			const double T = Start + Period * I;
			P.BeginRead();
			P.ObserveCount(T - WidthS, BaseCount + I - 1);
			P.ObserveCount(T, BaseCount + I);
		}
		return Start + Period * N;
	}

	/** A frame sync on the SDK count model, with 1000 publications already made. */
	TUniquePtr<FSpoutFrameSync> MakeModelSync(FSpoutSdkCountModelBackend*& OutFake, const ANSICHAR* Name)
	{
		TUniquePtr<FSpoutSdkCountModelBackend> Owned = MakeUnique<FSpoutSdkCountModelBackend>();
		OutFake = Owned.Get();
		OutFake->SetPublications(1000);
		TUniquePtr<FSpoutFrameSync> Sync = MakeUnique<FSpoutFrameSync>(MoveTemp(Owned));
		Sync->Open(Name);
		return Sync;
	}

	uint64 CyclesFor(double Seconds) { return uint64(Seconds / FPlatformTime::GetSecondsPerCycle64() + 0.5); }

	/**
	 * A model sync with the wait enabled by the caller, one frame already copied and a read that found
	 * nothing new: the state in which the receiver asks for a wait. The count at that read is 1000.
	 */
	TUniquePtr<FSpoutFrameSync> MakeWaitingSync(FSpoutSdkCountModelBackend*& OutFake, const ANSICHAR* Name)
	{
		TUniquePtr<FSpoutFrameSync> Sync = MakeModelSync(OutFake, Name);
		// The two reads are one reader period apart on a test clock: read in quick succession on the real
		// one, the predictor would take the reader for much faster than a 60 fps source and decline to
		// wait for it. The real clock is restored for the wait itself.
		TSharedRef<double> Clock = MakeShared<double>(1000.0);
		Sync->SetClockForTests([Clock]() { return *Clock; });
		Sync->ConsumeNewFrame();     // 1000 is new
		Sync->CompleteTransfer();    // and copied
		*Clock += 1.0 / 60.0;
		Sync->ConsumeNewFrame();     // the read that finds nothing new
		Sync->SetClockForTests(TFunction<double()>());
		return Sync;
	}

	/**
	 * Puts the sync on a clock that advances one microsecond each time it is read. A wait on it takes
	 * the same course however the test thread is scheduled, so a decision that depends on a few tenths
	 * of a millisecond can be tested inside the 1 ms limit.
	 */
	void UseSteppingClock(FSpoutFrameSync& Sync)
	{
		TSharedRef<double> Clock = MakeShared<double>(2000.0);
		Sync.SetClockForTests([Clock]() { *Clock += 0.000001; return *Clock; });
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitSettings, "SpoutPlugin.FrameWait.OffByDefaultAndClamped", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitSettings::RunTest(const FString&)
{
	FWaitCVarScope Wait(TEXT("r.Spout.FrameWaitMs")), Budget(TEXT("r.Spout.FrameWaitBudgetMs")), Poll(TEXT("r.Spout.FrameWaitPollUs")), Recheck(TEXT("r.Spout.FrameCountRecheck"));
	if (!Wait.Variable || !Budget.Variable || !Poll.Variable || !Recheck.Variable)
	{
		AddError(TEXT("the frame-wait settings are not registered"));
		return false;
	}
	for (IConsoleVariable* V : { Wait.Variable, Budget.Variable, Poll.Variable, Recheck.Variable })
		TestTrue(TEXT("read on the render thread, so render-thread safe"), (V->GetFlags() & ECVF_RenderThreadSafe) != 0);

	Wait.Set(TEXT("0")); Recheck.Set(TEXT("0"));
	TestEqual(TEXT("the wait is off at 0"), SpoutLimits::FrameWaitSeconds(), 0.0);
	TestFalse(TEXT("and the recheck with it"), SpoutLimits::FrameCountRecheck());
	if ((Wait.Variable->GetFlags() & ECVF_SetByMask) == ECVF_SetByConstructor) AddInfo(TEXT("r.Spout.FrameWaitMs was untouched before this test: its default is off."));

	// The limit is one millisecond, for the wait of one receive call and for the budget of one render
	// frame. A setting above it is read as the limit, in every build configuration.
	TestEqual(TEXT("the limit of both settings is 1 ms"), SpoutLimits::MaxFrameWaitMs, 1.0f);
	TestEqual(TEXT("the budget's default is the limit"), SpoutLimits::DefaultFrameWaitBudgetMs, 1.0f);
	struct FCase { const TCHAR* Text; double ExpectedMs; };
	const FCase Cases[] = { { TEXT("1"), 1.0 }, { TEXT("0.5"), 0.5 }, { TEXT("0.25"), 0.25 }, { TEXT("1.001"), 1.0 }, { TEXT("2"), 1.0 }, { TEXT("4"), 1.0 },
		{ TEXT("1000"), 1.0 }, { TEXT("-1"), 0.0 }, { TEXT("-1000"), 0.0 } };
	for (const FCase& Case : Cases)
	{
		Wait.Set(Case.Text);
		TestTrue(FString::Printf(TEXT("'%s' resolves to %.2f ms"), Case.Text, Case.ExpectedMs), FMath::IsNearlyEqual(SpoutLimits::FrameWaitSeconds() * 1000.0, Case.ExpectedMs, 1e-6));
		Budget.Set(Case.Text);
		TestTrue(FString::Printf(TEXT("budget '%s' resolves to %.2f ms"), Case.Text, Case.ExpectedMs), FMath::IsNearlyEqual(SpoutLimits::FrameWaitBudgetSeconds() * 1000.0, Case.ExpectedMs, 1e-6));
	}
	const TCHAR* const Odd[] = { TEXT("abc"), TEXT(""), TEXT("nan"), TEXT("inf"), TEXT("1e30"), TEXT("-1e30") };
	for (const TCHAR* Text : Odd)
	{
		Wait.Set(Text);
		Budget.Set(Text);
		const double Ms = SpoutLimits::FrameWaitSeconds() * 1000.0, BudgetMs = SpoutLimits::FrameWaitBudgetSeconds() * 1000.0;
		TestTrue(FString::Printf(TEXT("'%s' stays within 0..1 ms"), Text), Ms >= 0.0 && Ms <= 1.0);
		TestTrue(FString::Printf(TEXT("budget '%s' stays within 0..1 ms"), Text), BudgetMs >= 0.0 && BudgetMs <= 1.0);
	}

	Wait.Set(TEXT("1"));
	TestTrue(TEXT("the wait turns the recheck on"), SpoutLimits::FrameCountRecheck());
	Wait.Set(TEXT("0")); Recheck.Set(TEXT("1"));
	TestTrue(TEXT("the recheck can be on by itself"), SpoutLimits::FrameCountRecheck());

	Poll.Set(TEXT("1"));
	TestTrue(TEXT("the poll interval is clamped below"), FMath::IsNearlyEqual(SpoutLimits::FrameWaitPollSeconds() * 1e6, 50.0, 1e-6));
	Poll.Set(TEXT("100000"));
	TestTrue(TEXT("and above"), FMath::IsNearlyEqual(SpoutLimits::FrameWaitPollSeconds() * 1e6, 1000.0, 1e-6));

	FPublishPredictor Default;
	TestEqual(TEXT("the plugin's predictor uses the conditional rule"), int32(Default.GetRule()), int32(EWaitRule::Conditional));
	TestEqual(TEXT("and the adaptive recovery"), int32(Default.GetRecovery()), int32(ERecovery::Adaptive));
	TestTrue(TEXT("the jitter allowance starts at its floor"), FMath::IsNearlyEqual(Default.GetJitterSeconds(), FTuning::JitterAllowanceSeconds, 1e-12));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitPredictorBand, "SpoutPlugin.FrameWait.Predictor.WarmupBandAndHalfRateSourceUnderEachPolicy", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitPredictorBand::RunTest(const FString&)
{
	const double T = 1.0 / 60.0, W = 0.001, PollS = 0.00015;

	for (const EPolicy Policy : AllPolicies)
	{
		const TCHAR* Name = PolicyName(Policy);

		// Warm-up: fewer publications than the period needs.
		{
			FPublishPredictor P;
			Configure(P, Policy);
			TArray<double> Pub = Regular(10.0, T, 20);
			TArray<double> Reads;
			for (int32 I = 0; I < 20; ++I) Reads.Add(10.0 + T * I + 0.005);
			Reads.Add(Reads.Last() + T); // one read with nothing new
			const FSimResult R = Simulate(P, Pub, Reads, W, PollS);
			TestEqual(FString::Printf(TEXT("%s: no wait before the period is known"), Name), R.Waits, 0);
			TestEqual(FString::Printf(TEXT("%s: the read without a new frame is a warm-up"), Name), R.Warmup, 1);
		}

		// In band: for 4 s every read comes 0.2 ms after a publication; then the read moves to 0.2 ms
		// before it. Every read that then finds nothing new is waited for and gets its frame.
		{
			FPublishPredictor P;
			Configure(P, Policy);
			const FSimResult R = Simulate(P, Regular(10.0, T, 400), BandReads(10.0, T, 380, 240), W, PollS);
			TestTrue(FString::Printf(TEXT("%s: reads just before a publication found nothing new"), Name), R.ReadsWithoutNewFrame > 100);
			TestEqual(FString::Printf(TEXT("%s: every one of them was waited for"), Name), R.Waits, R.ReadsWithoutNewFrame);
			TestEqual(FString::Printf(TEXT("%s: every wait got its frame"), Name), R.Published, R.Waits);
			TestEqual(FString::Printf(TEXT("%s: none was wasted"), Name), R.Wasted, 0);
			TestTrue(FString::Printf(TEXT("%s: the predicted upper bound covered the true publication"), Name), R.MaxLatestMinusTruth >= 0.0);
		}

		// A source at half the reader's rate: every other read finds nothing new.
		//   baseline:  never a wait.
		//   overlap:   at phases 0.2 to 14 ms the wide window overlaps the bound, so it waits, and every such
		//              wait times out; only the suspension stops it. At 16.4 ms it waits and gets the frame.
		//   corrected: never a wait at any phase: the source is slower than the reader (SlowerSource), so
		//              every publication is delivered without waiting and a wait could only make a frame
		//              fresher (at 16.4 ms by about 16 ms), at the cost of waits that fail at most phases.
		for (const double PhaseMs : { 0.2, 3.0, 8.0, 14.0, 16.4 })
		{
			FPublishPredictor P;
			Configure(P, Policy);
			TArray<double> Reads;
			for (int32 I = 0; I < 380; ++I) Reads.Add(10.0 + T * I + PhaseMs / 1000.0);
			const FSimResult R = Simulate(P, Regular(10.0, 2.0 * T, 200), Reads, W, PollS);
			const bool bDueInBound = PhaseMs > 16.0;
			TestTrue(FString::Printf(TEXT("%s, half-rate at %.1f ms: many reads find nothing new"), Name, PhaseMs), R.ReadsWithoutNewFrame > 150);
			if (Policy == EPolicy::Baseline)
			{
				TestEqual(FString::Printf(TEXT("baseline, half-rate at %.1f ms: no wait is attempted"), PhaseMs), R.Waits, 0);
			}
			else if (Policy == EPolicy::Corrected)
			{
				TestEqual(FString::Printf(TEXT("corrected, half-rate at %.1f ms: no wait is attempted"), PhaseMs), R.Waits, 0);
				TestTrue(FString::Printf(TEXT("corrected, half-rate at %.1f ms: refused as a slower source"), PhaseMs), R.SlowerSource > 100);
			}
			else if (bDueInBound)
			{
				TestTrue(TEXT("overlap, half-rate at 16.4 ms: the reads before a publication are waited for"), R.Waits > 100);
				TestEqual(TEXT("overlap, half-rate at 16.4 ms: every wait gets its frame"), R.Published, R.Waits);
			}
			else
			{
				TestEqual(FString::Printf(TEXT("overlap, half-rate at %.1f ms: no wait gets a frame"), PhaseMs), R.Published, 0);
				TestTrue(FString::Printf(TEXT("overlap, half-rate at %.1f ms: it does wait, and wastes it (the regression the correction removes)"), PhaseMs), R.Wasted > 0);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitOverlapWindows, "SpoutPlugin.FrameWait.Predictor.WindowAgainstTheWaitingInterval", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitOverlapWindows::RunTest(const FString&)
{
	// 60 publications, each seen 50 us before and at its instant: the window for the next one is
	// [Next - 0.3017 ms, Next + 0.2517 ms] (slack 0.25 ms + 2 x 50 us / 59).
	const double T = 1.0 / 60.0, W = 0.001, Width = 0.00005;
	FPublishPredictor P;
	const double Next = PrimeRegular(P, 10.0, T, 60, Width);
	const double Slack = 0.00025 + 2.0 * Width / 59.0;
	{
		const FPrediction Pr = P.Predict(Next - 0.002, W);
		TestTrue(TEXT("the period is the grid's"), FMath::IsNearlyEqual(Pr.PeriodSeconds, T, 1e-9));
		TestTrue(TEXT("earliest edge as constructed"), FMath::IsNearlyEqual(Pr.EarliestSeconds, Next - Width - Slack, 1e-7));
		TestTrue(TEXT("latest edge as constructed"), FMath::IsNearlyEqual(Pr.LatestSeconds, Next + Slack, 1e-7));
	}

	struct FCase { const TCHAR* What; double NowMinusNextMs; double BoundMs; EDecision Upper; EDecision Overlap; EDecision Conditional; };
	const FCase Cases[] = {
		{ TEXT("window entirely after now + bound"),                         -2.0, 1.0, EDecision::NotImminent, EDecision::NotImminent, EDecision::NotImminent },
		{ TEXT("window straddles now + bound (earliest inside, latest out)"), -1.1, 1.0, EDecision::NotImminent, EDecision::Wait,        EDecision::Wait },
		{ TEXT("window entirely inside the bound"),                           -0.5, 1.0, EDecision::Wait,        EDecision::Wait,        EDecision::Wait },
		{ TEXT("window just past, within the overdue tolerance"),              1.0, 1.0, EDecision::Wait,        EDecision::Wait,        EDecision::Wait },
		{ TEXT("window past by more than the overdue tolerance"),              5.0, 1.0, EDecision::Overdue,     EDecision::Overdue,     EDecision::Overdue },
		{ TEXT("a bound of 0.3 ms (the budget's remainder) not reaching it"), -0.8, 0.3, EDecision::NotImminent, EDecision::NotImminent, EDecision::NotImminent },
		{ TEXT("the same read with the full 1 ms bound"),                     -0.8, 1.0, EDecision::NotImminent, EDecision::Wait,        EDecision::Wait },
	};
	for (const FCase& Case : Cases)
	{
		const double Now = Next + Case.NowMinusNextMs / 1000.0;
		P.SetRule(EWaitRule::UpperEdge);
		TestEqual(FString::Printf(TEXT("upper-edge: %s"), Case.What), int32(P.Predict(Now, Case.BoundMs / 1000.0).Decision), int32(Case.Upper));
		P.SetRule(EWaitRule::Overlap);
		TestEqual(FString::Printf(TEXT("overlap: %s"), Case.What), int32(P.Predict(Now, Case.BoundMs / 1000.0).Decision), int32(Case.Overlap));
		P.SetRule(EWaitRule::Conditional);
		TestEqual(FString::Printf(TEXT("conditional: %s"), Case.What), int32(P.Predict(Now, Case.BoundMs / 1000.0).Decision), int32(Case.Conditional));
	}

	// A wide window that lies mostly after now (the state of a slower source between reads): the
	// publications are each known only to within 16 ms. 10 ms before the nominal publication, the
	// window after now is about 10.8 ms long, of which the bound covers 1 ms (a share of about 0.09).
	{
		FPublishPredictor Wide;
		const double WideWidth = 0.016;
		const double WideNext = PrimeRegular(Wide, 10.0, T, 60, WideWidth);
		const double Now = WideNext - 0.010;
		Wide.SetRule(EWaitRule::UpperEdge);
		TestEqual(TEXT("wide window mostly after now, upper-edge: refused"), int32(Wide.Predict(Now, W).Decision), int32(EDecision::NotImminent));
		Wide.SetRule(EWaitRule::Overlap);
		TestEqual(TEXT("wide window mostly after now, overlap: waited (its earliest edge is in the past)"), int32(Wide.Predict(Now, W).Decision), int32(EDecision::Wait));
		Wide.SetRule(EWaitRule::Conditional);
		const FPrediction Pr = Wide.Predict(Now, W);
		TestEqual(TEXT("wide window mostly after now, conditional: refused"), int32(Pr.Decision), int32(EDecision::NotImminent));
		TestTrue(TEXT("and its share is about 0.09, below the 0.15 needed"), Pr.ConditionalShare > 0.05 && Pr.ConditionalShare < FTuning::MinConditionalShare);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitOverlapRescue, "SpoutPlugin.FrameWait.Predictor.ReadsTheUpperEdgeRuleRefusedAreWaitedFor", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitOverlapRescue::RunTest(const FString&)
{
	// The reader hovers 0.25 ms before each publication, so each read finds the frame published just after
	// the previous read already new, and no wait observes a publication: the predictor knows each one only
	// to within a reader period. Every 20th read is 0.4 ms late (after its publication). The first read
	// after warm-up that then finds nothing new has a window about a reader period wide, almost all of it
	// in the past, ending about 1.2 ms after the read; its publication is 0.25 ms away. The upper-edge rule
	// refuses it; the overlap and the conditional rule wait and get it (the mechanism, not its frequency).
	const double T = 1.0 / 60.0, W = 0.001, PollS = 0.00015;
	const TArray<double> Pub = Regular(10.0, T, 700);
	TArray<double> Reads;
	for (int32 I = 0; I < 660; ++I) Reads.Add(10.0 + T * I - 0.00025 + (I % 20 == 10 ? 0.0004 : 0.0));

	FPublishPredictor Base, Over, Corr;
	Configure(Base, EPolicy::Baseline);
	Configure(Over, EPolicy::Overlap);
	Configure(Corr, EPolicy::Corrected);
	const FSimResult RB = Simulate(Base, Pub, Reads, W, PollS);
	const FSimResult RO = Simulate(Over, Pub, Reads, W, PollS);
	const FSimResult RC = Simulate(Corr, Pub, Reads, W, PollS);

	int32 First = INDEX_NONE;
	for (int32 I = 0; I < RB.PerRead.Num(); ++I)
	{
		if (RB.PerRead[I].Decision >= 0 && RB.PerRead[I].Decision != int32(EDecision::Warmup)) { First = I; break; }
	}
	if (First == INDEX_NONE) { AddError(TEXT("no read without a new frame after warm-up")); return false; }
	const FSimRead& U = RB.PerRead[First];
	AddInfo(FString::Printf(TEXT("read %d: window %.3f .. %.3f ms after the read, publication at %.3f ms"), First, U.EarliestAfterRead * 1000.0, U.LatestAfterRead * 1000.0, U.NextPublicationAfterRead * 1000.0));
	TestTrue(TEXT("that read's window is wide: it starts before the read and ends after the bound"), U.EarliestAfterRead < 0.0 && U.LatestAfterRead > W);
	TestTrue(TEXT("and its publication came within the bound"), U.NextPublicationAfterRead > 0.0 && U.NextPublicationAfterRead <= W);
	TestEqual(TEXT("baseline: refused (not imminent)"), U.Decision, int32(EDecision::NotImminent));
	TestEqual(TEXT("overlap: waited"), RO.PerRead[First].Decision, int32(EDecision::Wait));
	TestEqual(TEXT("overlap: and got the frame"), RO.PerRead[First].Outcome, int32(EOutcome::Published));
	TestEqual(TEXT("corrected: waited"), RC.PerRead[First].Decision, int32(EDecision::Wait));
	TestEqual(TEXT("corrected: and got the frame"), RC.PerRead[First].Outcome, int32(EOutcome::Published));
	TestEqual(TEXT("corrected: no read refused in the whole run"), RC.NotImminent, 0);
	TestEqual(TEXT("corrected: every wait got its frame"), RC.Published, RC.Waits);
	TestEqual(TEXT("corrected: none was wasted"), RC.Wasted, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitInconsistency, "SpoutPlugin.FrameWait.Predictor.InaccurateTimingWidensTheAllowanceInsteadOfSuspending", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitInconsistency::RunTest(const FString&)
{
	// Inaccurate timing: in band, every third publication 0.7 ms late. Every wait gets its frame (the
	// publication is at most 0.9 ms after the read), but the late ones fall outside a carried window that
	// allows 0.25 ms of jitter.
	const double T = 1.0 / 60.0, W = 0.001, PollS = 0.00015;
	TArray<double> Pub;
	for (int32 I = 0; I < 1300; ++I) Pub.Add(10.0 + T * I + ((I >= 240 && I % 3 == 1) ? 0.0007 : 0.0));
	const TArray<double> Reads = BandReads(10.0, T, 1260, 240);

	FPublishPredictor Over, Corr;
	Configure(Over, EPolicy::Overlap);
	Configure(Corr, EPolicy::Corrected);
	const FSimResult RO = Simulate(Over, Pub, Reads, W, PollS);
	const FSimResult RC = Simulate(Corr, Pub, Reads, W, PollS);
	AddInfo(FString::Printf(TEXT("overlap: %d waits, %d got the frame, %d wasted, %lld suspensions (%lld for inconsistency)"), RO.Waits, RO.Published, RO.Wasted, Over.GetSuspensions(), Over.GetSuspensionsForInconsistency()));
	AddInfo(FString::Printf(TEXT("corrected: %d waits, %d got the frame, %d wasted, %lld suspensions, %lld inconsistent observations, allowance %.3f ms"), RC.Waits, RC.Published, RC.Wasted, Corr.GetSuspensions(), Corr.GetInconsistentObservations(), Corr.GetJitterSeconds() * 1000.0));
	TestTrue(TEXT("overlap: it was suspended for inconsistency"), Over.GetSuspensionsForInconsistency() >= 1);
	TestEqual(TEXT("overlap: although no wait was wasted"), RO.Wasted, 0);
	TestEqual(TEXT("overlap: and every wait got its frame"), RO.Published, RO.Waits);
	TestTrue(TEXT("corrected: the late publications were seen as inconsistent"), Corr.GetInconsistentObservations() > 0);
	TestEqual(TEXT("corrected: never suspended"), Corr.GetSuspensions(), int64(0));
	TestTrue(TEXT("corrected: it kept waiting"), RC.Waits > 900);
	TestEqual(TEXT("corrected: every wait got its frame"), RC.Published, RC.Waits);
	TestTrue(TEXT("corrected: the allowance widened, within its cap"), Corr.GetJitterSeconds() > FTuning::JitterAllowanceSeconds && Corr.GetJitterSeconds() <= FTuning::JitterAllowanceMaxSeconds);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitLateAnchor, "SpoutPlugin.FrameWait.Predictor.ALatePublicationKeepsTheSendersGrid", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitLateAnchor::RunTest(const FString&)
{
	// A predictor that re-anchors on one late publication places the next window 1 to 3 ms late, while the
	// next publication comes on time.
	// Here: a regular history, publication 60 seen 2.0 ms late (tightly, as a wait would see it), then a read
	// 0.25 ms before the on-time publication 61.
	const double T = 1.0 / 60.0, W = 0.001, Width = 0.00005;
	for (const EPolicy Policy : { EPolicy::Overlap, EPolicy::Corrected })
	{
		FPublishPredictor P;
		Configure(P, Policy);
		const double Nominal60 = PrimeRegular(P, 10.0, T, 60, Width);
		const double Late = Nominal60 + 0.002;
		P.ObserveCount(Late - 0.00015, 559);
		P.ObserveCount(Late, 560);
		const double Now = Nominal60 + T - 0.00025;
		const FPrediction Pr = P.Predict(Now, W);
		AddInfo(FString::Printf(TEXT("%s: window %.3f .. %.3f ms after the read (the publication is at +0.25 ms), allowance %.3f ms, decision %d"),
			PolicyName(Policy), (Pr.EarliestSeconds - Now) * 1000.0, (Pr.LatestSeconds - Now) * 1000.0, P.GetJitterSeconds() * 1000.0, int32(Pr.Decision)));
		TestEqual(FString::Printf(TEXT("%s: one inconsistent observation"), PolicyName(Policy)), P.GetInconsistentObservations(), int64(1));
		if (Policy == EPolicy::Overlap)
		{
			TestTrue(TEXT("overlap: re-anchored on the late publication, the window starts more than the bound after the read"), Pr.EarliestSeconds > Now + W);
			TestEqual(TEXT("overlap: refused"), int32(Pr.Decision), int32(EDecision::NotImminent));
		}
		else
		{
			TestTrue(TEXT("corrected: the window still contains the on-time slot"), Pr.EarliestSeconds <= Now + 0.00025 && Pr.LatestSeconds >= Now + 0.00025);
			TestEqual(TEXT("corrected: waited"), int32(Pr.Decision), int32(EDecision::Wait));
			TestEqual(TEXT("corrected: not suspended"), P.GetSuspensions(), int64(0));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitSlower, "SpoutPlugin.FrameWait.Predictor.SlowerSourcesAreNotWaitedFor", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitSlower::RunTest(const FString&)
{
	// Sources slower than the 60 Hz reader, 20 s at each of 9 phases: never a wait (corrected); every
	// publication is delivered without one. Equal-rate (59.94 fps, 0.1 % slower) and faster (60.05 fps)
	// sources are not treated as slower.
	const double T = 1.0 / 60.0, W = 0.001, PollS = 0.00015;
	for (const double Fps : { 30.0, 40.0, 50.0, 57.0, 59.94, 60.05 })
	{
		const double Period = 1.0 / Fps;
		int32 Waits = 0, Wasted = 0, Slower = 0, OverWasted = 0;
		for (int32 Phase = 0; Phase < 9; ++Phase)
		{
			const TArray<double> Pub = Regular(10.0, Period, int32(1200 * T / Period) + 5);
			TArray<double> Reads;
			for (int32 I = 0; I < 1200; ++I) Reads.Add(10.0 + T * I + Phase * 0.0018);
			FPublishPredictor Corr, Over;
			Configure(Corr, EPolicy::Corrected);
			Configure(Over, EPolicy::Overlap);
			const FSimResult RC = Simulate(Corr, Pub, Reads, W, PollS);
			const FSimResult RO = Simulate(Over, Pub, Reads, W, PollS);
			Waits += RC.Waits;
			Wasted += RC.Wasted;
			Slower += RC.SlowerSource;
			OverWasted += RO.Wasted;
		}
		AddInfo(FString::Printf(TEXT("%.2f fps: corrected %d waits, %d wasted, %d slower-source refusals; overlap %d wasted"), Fps, Waits, Wasted, Slower, OverWasted));
		if (Fps < 59.0)
		{
			TestEqual(FString::Printf(TEXT("%.0f fps: no wait at any phase (corrected)"), Fps), Waits, 0);
			TestTrue(FString::Printf(TEXT("%.0f fps: refused as a slower source"), Fps), Slower > 0);
		}
		else
		{
			TestEqual(FString::Printf(TEXT("%.2f fps: not a slower source"), Fps), Slower, 0);
			TestTrue(FString::Printf(TEXT("%.2f fps: at most 5 %% of its waits wasted"), Fps), Wasted * 20 <= FMath::Max(1, Waits));
		}
		if (FMath::IsNearlyEqual(Fps, 30.0)) TestTrue(TEXT("30 fps: the overlap policy wasted waits (the known regression)"), OverWasted > 0);
	}

	// The read interval is a smoothed estimate, so uneven frame pacing can make an equal-rate source look slower for
	// a moment. Reads of a 60 fps-capped game on a 165 Hz vsync grid (intervals of two or three refreshes, 12.1 or
	// 18.2 ms) and an equal-rate 60 fps source, 10 phases of 60 s: at most 1 % of the reads that find nothing new are
	// refused as a slower source.
	{
		const double Refresh = 1.0 / 165.0;
		int32 Slower = 0, WithoutNew = 0;
		for (int32 Phase = 0; Phase < 10; ++Phase)
		{
			TArray<double> Reads;
			for (int32 I = 0; I < 3600; ++I) Reads.Add(FMath::CeilToDouble((10.0 + T * I + Phase * 0.0017) / Refresh) * Refresh);
			FPublishPredictor Corr;
			Configure(Corr, EPolicy::Corrected);
			const FSimResult RC = Simulate(Corr, Regular(10.0, T, 3700), Reads, W, PollS);
			Slower += RC.SlowerSource;
			WithoutNew += RC.ReadsWithoutNewFrame;
		}
		AddInfo(FString::Printf(TEXT("equal-rate source, reads on a 165 Hz grid: %d of %d reads without a new frame refused as a slower source"), Slower, WithoutNew));
		TestTrue(TEXT("equal-rate source with uneven read pacing: at most 1 % refused as slower"), WithoutNew > 1000 && Slower * 100 <= WithoutNew);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitSuspension, "SpoutPlugin.FrameWait.Predictor.SuspensionEscalationAndRecovery", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitSuspension::RunTest(const FString&)
{
	const double T = 1.0 / 60.0, W = 0.001, Width = 0.00005;

	// Scoring: a wasted wait adds 3, a successful one takes 1 off, 6 suspends.
	{
		FPublishPredictor P;
		double Next = PrimeRegular(P, 10.0, T, 60, Width);
		TestEqual(TEXT("primed: a read 0.3 ms before the next publication waits"), int32(P.Predict(Next - 0.0003, W).Decision), int32(EDecision::Wait));
		P.ObserveWaitTimedOut();
		TestFalse(TEXT("one wasted wait does not suspend"), P.IsSuspended());
		P.ObserveWaitSucceeded(); P.ObserveWaitSucceeded(); P.ObserveWaitSucceeded();
		P.ObserveWaitTimedOut();
		TestFalse(TEXT("successful waits in between earn the credit back: still not suspended"), P.IsSuspended());
		P.ObserveWaitTimedOut();
		TestTrue(TEXT("a second wasted wait in a row suspends"), P.IsSuspended());
		TestEqual(TEXT("one suspension"), P.GetSuspensions(), int64(1));
		TestEqual(TEXT("the first suspension lasts SuspendReads reads"), P.GetSuspendedReadsLeft(), FTuning::SuspendReads);
		TestEqual(TEXT("the decision is Suspended"), int32(P.Predict(Next - 0.0003, W).Decision), int32(EDecision::Suspended));

		// The sender stays regular while suspended; the suspension lasts SuspendReads reads.
		int64 Count = 500 + 59;
		double Pub = Next;
		for (int64 I = 1; I < FTuning::SuspendReads; ++I)
		{
			P.BeginRead();
			P.ObserveCount(Pub - Width, Count);
			P.ObserveCount(Pub, ++Count);
			Pub += T;
		}
		TestTrue(TEXT("still suspended one read before the end"), P.IsSuspended());
		P.BeginRead();
		P.ObserveCount(Pub - Width, Count);
		P.ObserveCount(Pub, ++Count);
		Pub += T;
		TestFalse(TEXT("the suspension ends after SuspendReads reads"), P.IsSuspended());
		TestEqual(TEXT("recovered: a read 0.3 ms before the next publication waits again"), int32(P.Predict(Pub - 0.0003, W).Decision), int32(EDecision::Wait));
	}

	// Escalation: each suspension that follows another without a successful wait in between is twice as
	// long, up to MaxSuspendReads; a successful wait restores the base length. Original keeps 120.
	for (const EPolicy Policy : { EPolicy::Overlap, EPolicy::Corrected })
	{
		FPublishPredictor P;
		Configure(P, Policy);
		PrimeRegular(P, 10.0, T, 60, Width);
		TArray<int64> Lengths;
		for (int32 K = 0; K < 9; ++K)
		{
			P.ObserveWaitTimedOut();
			P.ObserveWaitTimedOut();
			Lengths.Add(P.GetSuspendedReadsLeft());
			for (int64 I = 0, N = P.GetSuspendedReadsLeft(); I < N; ++I) P.BeginRead();
		}
		FString Text;
		for (const int64 L : Lengths) Text += FString::Printf(TEXT("%lld "), L);
		AddInfo(FString::Printf(TEXT("%s: suspension lengths %s"), PolicyName(Policy), *Text));
		if (Policy == EPolicy::Overlap)
		{
			TestTrue(TEXT("original recovery: every suspension is 120 reads"), Lengths[0] == 120 && Lengths[8] == 120);
		}
		else
		{
			const int64 Expected[] = { 120, 240, 480, 960, 1920, 3840, 7680, 7680, 7680 };
			for (int32 K = 0; K < 9; ++K) TestEqual(FString::Printf(TEXT("corrected: suspension %d lasts %lld reads"), K + 1, Expected[K]), Lengths[K], Expected[K]);
			P.ObserveWaitSucceeded();
			TestEqual(TEXT("corrected: a successful wait restores the base length"), P.GetNextSuspendReads(), FTuning::SuspendReads);
		}
	}

	// A sender that restarts (its count starts again) during a suspension does not clear it.
	{
		FPublishPredictor P;
		PrimeRegular(P, 10.0, T, 60, Width);
		P.ObserveWaitTimedOut();
		P.ObserveWaitTimedOut();
		TestTrue(TEXT("suspended"), P.IsSuspended());
		const int64 ResetsBefore = P.GetResets();
		P.BeginRead();
		P.ObserveCount(11.5, 3);
		TestEqual(TEXT("the restart reset the history"), P.GetResets(), ResetsBefore + 1);
		TestTrue(TEXT("but not the suspension"), P.IsSuspended());
		TestEqual(TEXT("nor the escalation"), P.GetNextSuspendReads(), 2 * FTuning::SuspendReads);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitJitter, "SpoutPlugin.FrameWait.Predictor.TheJitterAllowanceIsBoundedAndDecays", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitJitter::RunTest(const FString&)
{
	const double T = 1.0 / 60.0, Width = 0.00005;
	FPublishPredictor P;
	const double Next = PrimeRegular(P, 10.0, T, 60, Width);
	P.ObserveCount(Next + 0.010 - 0.00015, 559);
	P.ObserveCount(Next + 0.010, 560);
	TestTrue(TEXT("a publication 10 ms late widens the allowance only to its cap"), FMath::IsNearlyEqual(P.GetJitterSeconds(), FTuning::JitterAllowanceMaxSeconds, 1e-9));
	int64 Count = 560;
	for (int32 I = 1; I <= 256; ++I)
	{
		const double Pub = Next + 0.010 + T * I;
		P.ObserveCount(Pub - Width, Count);
		P.ObserveCount(Pub, ++Count);
	}
	const double Expected = FTuning::JitterAllowanceSeconds + (FTuning::JitterAllowanceMaxSeconds - FTuning::JitterAllowanceSeconds) * FMath::Pow(1.0 - 1.0 / FTuning::JitterDecayPublications, 256.0);
	AddInfo(FString::Printf(TEXT("after 256 consistent publications the allowance is %.3f ms (expected %.3f)"), P.GetJitterSeconds() * 1000.0, Expected * 1000.0));
	TestTrue(TEXT("it decays back towards the floor while the sender behaves"), FMath::IsNearlyEqual(P.GetJitterSeconds(), Expected, 1e-6));
	TestEqual(TEXT("and was never a reason to suspend"), P.GetSuspensions(), int64(0));
	P.Reset();
	TestTrue(TEXT("a reset (sender restart) returns it to the floor"), FMath::IsNearlyEqual(P.GetJitterSeconds(), FTuning::JitterAllowanceSeconds, 1e-12));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitReadTime, "SpoutPlugin.FrameWait.Predictor.ACountIsObservedWhenItWasRead", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitReadTime::RunTest(const FString&)
{
	// The count read under access is observed when it is read, not after the copy's completion wait. Under GPU
	// load that wait can last a whole frame, so "the count was still N then" could be false, and a false claim
	// pushes the lower bound of the next publication past its true time.
	const double T = 1.0 / 60.0, Width = 0.00005;
	for (const bool bAtReadTime : { false, true })
	{
		FPublishPredictor P;
		const double Next = PrimeRegular(P, 10.0, T, 60, Width);   // publication 60 (count 560) at Next
		const double CountRead = Next - 0.001;                      // count 559 read under access 1 ms before it
		const double GpuDone = Next + 0.002;                        // the completion wait ends 2 ms after it
		P.ObserveCount(bAtReadTime ? CountRead : GpuDone, 559);
		P.ObserveCount(Next + 0.003, 560);                          // the next read sees 560
		TestEqual(bAtReadTime ? TEXT("observed at the read time: consistent") : TEXT("observed at completion: a false claim makes it inconsistent"),
			P.GetInconsistentObservations(), int64(bAtReadTime ? 0 : 1));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitStale, "SpoutPlugin.FrameWait.Predictor.AStalePredictionIsNotWaitedForAndWaitingRecovers", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitStale::RunTest(const FString&)
{
	// In band, then the sender pauses for 5 s (publications 400 to 699 missing) and resumes on its grid.
	// After the resume one late read catches up with the sender, and the reads are in band again.
	const double T = 1.0 / 60.0, W = 0.001, PollS = 0.00015;
	TArray<double> Pub;
	for (int32 I = 0; I < 1100; ++I) if (I < 400 || I >= 700) Pub.Add(10.0 + T * I);
	TArray<double> Reads;
	for (int32 I = 0; I < 1060; ++I) Reads.Add(10.0 + T * I + ((I < 240 || I == 760) ? 0.0002 : -0.0002));

	for (const EPolicy Policy : AllPolicies)
	{
		const TCHAR* Name = PolicyName(Policy);
		FPublishPredictor P;
		Configure(P, Policy);
		const FSimResult R = Simulate(P, Pub, Reads, W, PollS);
		const int32 PausedWaits = CountReads(R, 400, 700, EDecision::Wait);
		const int32 PausedOverdue = CountReads(R, 400, 700, EDecision::Overdue);
		const int32 After = CountDecided(R, 761, Reads.Num());
		const int32 AfterPublished = CountReads(R, 761, Reads.Num(), EDecision::Wait, int32(EOutcome::Published));
		AddInfo(FString::Printf(TEXT("%s: during the pause %d waits, %d overdue; after the resume %d decisions, %d waits that got their frame"), Name, PausedWaits, PausedOverdue, After, AfterPublished));
		TestTrue(FString::Printf(TEXT("%s: a pause costs at most one wasted wait"), Name), PausedWaits <= 1 && CountReads(R, 400, 700, EDecision::Wait, int32(EOutcome::Published)) == 0);
		TestTrue(FString::Printf(TEXT("%s: the stale prediction is recognised as overdue"), Name), PausedOverdue >= 298);
		TestTrue(FString::Printf(TEXT("%s: after the resume reads find nothing new again"), Name), After > 250);
		TestEqual(FString::Printf(TEXT("%s: and every one is waited for and gets its frame"), Name), AfterPublished, After);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitChanging, "SpoutPlugin.FrameWait.Predictor.RestartingAndChangingRateSources", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitChanging::RunTest(const FString&)
{
	const double T = 1.0 / 60.0, W = 0.001, PollS = 0.00015;

	// Rate changing 60 -> 30 -> 60 fps in 20 s blocks (reads 0..1199, 1200..2399, 2400..3599), reads in band.
	// A reader 0.2 ms before each publication that did not wait at the previous read finds the frame
	// published just after that read, one frame old, at every read: it never finds nothing new, so it never
	// decides anything (nothing is lost; each frame is a frame staler). The corrected policy does not wait
	// in the 30 fps block, so in the last block it would stay in that state; the alignment puts the read
	// just before the publication again (reads 2400 to 2639 after it, then before), as at the start. The
	// few waits in the 30 fps block come at the change, while the period window still holds 60 fps samples.
	{
		TArray<double> Pub;
		for (double Tp = 10.0; Tp < 10.0 + 3600 * T + 0.1; )
		{
			Pub.Add(Tp);
			Tp += (int32((Tp - 10.0) / 20.0) % 2 == 0) ? T : 2.0 * T;
		}
		TArray<double> Reads;
		for (int32 I = 0; I < 3600; ++I) Reads.Add(10.0 + T * I + ((I < 240 || (I >= 2400 && I < 2640)) ? 0.0002 : -0.0002));
		FPublishPredictor Over, Corr;
		Configure(Over, EPolicy::Overlap);
		Configure(Corr, EPolicy::Corrected);
		const FSimResult RO = Simulate(Over, Pub, Reads, W, PollS);
		const FSimResult RC = Simulate(Corr, Pub, Reads, W, PollS);
		const int32 SlowBlockWaits = CountReads(RC, 1200, 2400, EDecision::Wait);
		const int32 LastBlockDecided = CountDecided(RC, 2640, 3600);
		const int32 LastBlockPublished = CountReads(RC, 2640, 3600, EDecision::Wait, int32(EOutcome::Published));
		AddInfo(FString::Printf(TEXT("overlap: %d waits, %d wasted, %lld suspensions (%lld for inconsistency); corrected: %d waits, %d wasted, %lld suspensions, %d waits in the 30 fps block, last block %d decisions of which %d waits got the frame"),
			RO.Waits, RO.Wasted, Over.GetSuspensions(), Over.GetSuspensionsForInconsistency(), RC.Waits, RC.Wasted, Corr.GetSuspensions(), SlowBlockWaits, LastBlockDecided, LastBlockPublished));
		TestEqual(TEXT("corrected: never suspended for inconsistency"), Corr.GetSuspensionsForInconsistency(), int64(0));
		TestTrue(TEXT("corrected: few wasted waits across the changes"), RC.Wasted <= 6);
		TestTrue(TEXT("corrected: few waits in the 30 fps block, all at the change"), SlowBlockWaits <= 10 && CountReads(RC, 1500, 2400, EDecision::Wait) == 0);
		TestTrue(TEXT("corrected: back at 60 fps and in band, reads find nothing new again"), LastBlockDecided > 900);
		TestEqual(TEXT("corrected: and every one is waited for and gets its frame"), LastBlockPublished, LastBlockDecided);
		TestTrue(TEXT("overlap: suspended for inconsistency at the changes (the behaviour corrected)"), Over.GetSuspensionsForInconsistency() > 0);
	}

	// Restarting every 10 s (the count starts again), reads in band: each restart resets what was learnt;
	// waiting resumes after the warm-up, and no wait is wasted.
	for (const EPolicy Policy : { EPolicy::Overlap, EPolicy::Corrected })
	{
		FPublishPredictor P;
		Configure(P, Policy);
		const TArray<double> Pub = Regular(10.0, T, 3605);
		const TArray<double> Reads = BandReads(10.0, T, 3600, 240);
		int64 Consumed = -1;
		int32 Waits = 0, Got = 0, Wasted = 0;
		for (const double Read : Reads)
		{
			P.BeginRead(Read);
			const int32 N = Algo::UpperBound(Pub, Read);
			const int64 C = N - (N / 600) * 600; // publications since the last restart
			P.ObserveCount(Read, C);
			if (C != Consumed) { Consumed = C; continue; }
			if (P.Predict(Read, W).Decision != EDecision::Wait) continue;
			++Waits;
			const double Next = N < Pub.Num() ? Pub[N] : 1e30;
			if (Next <= Read + W)
			{
				const int64 C2 = (N + 1) - ((N + 1) / 600) * 600;
				P.ObserveCount(Next + PollS, C2);
				P.ObserveWaitSucceeded();
				++Got;
				Consumed = C2;
			}
			else
			{
				P.ObserveCount(Read + W, C);
				P.ObserveWaitTimedOut();
				++Wasted;
			}
		}
		AddInfo(FString::Printf(TEXT("%s, restarting every 10 s: %d waits, %d got the frame, %d wasted, %lld resets"), PolicyName(Policy), Waits, Got, Wasted, P.GetResets()));
		TestTrue(FString::Printf(TEXT("%s: the restarts reset the predictor"), PolicyName(Policy)), P.GetResets() >= 5);
		TestTrue(FString::Printf(TEXT("%s: waiting resumes after each restart"), PolicyName(Policy)), Got > 100);
		TestEqual(FString::Printf(TEXT("%s: no wait wasted"), PolicyName(Policy)), Wasted, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitPredictorDecay, "SpoutPlugin.FrameWait.Predictor.PausedIrregularAndRestartingSendersStopBeingWaitedFor", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitPredictorDecay::RunTest(const FString&)
{
	const double T = 1.0 / 60.0, W = 0.001, PollS = 0.00015;

	for (const EPolicy Policy : AllPolicies)
	{
		const TCHAR* Name = PolicyName(Policy);

		// Paused: a regular source read 5 ms after each publication stops for five seconds.
		{
			FPublishPredictor P;
			Configure(P, Policy);
			TArray<double> Reads;
			for (int32 I = 0; I < 420; ++I) Reads.Add(10.0 + T * I + 0.005);
			const FSimResult R = Simulate(P, Regular(10.0, T, 120), Reads, W, PollS);
			TestEqual(FString::Printf(TEXT("%s: 300 reads find nothing new"), Name), R.ReadsWithoutNewFrame, 300);
			TestTrue(FString::Printf(TEXT("%s: a pause costs at most one wasted wait"), Name), R.Wasted <= 1);
			TestEqual(FString::Printf(TEXT("%s: and no wait succeeds"), Name), R.Published, 0);
			TestTrue(FString::Printf(TEXT("%s: the rest are recognised as overdue"), Name), R.Overdue >= 298);
		}

		// Irregular: publication times jittered by up to +-6 ms. Wasted waits are bounded by the
		// wasted-wait score: between two suspensions at most (successful waits + 8) / 3 are wasted (plus timed-out probes).
		{
			FPublishPredictor P;
			Configure(P, Policy);
			FRandomStream Rng(12345);
			TArray<double> Pub;
			for (int32 I = 0; I < 1300; ++I) Pub.Add(10.0 + T * I + Rng.FRandRange(-0.006f, 0.006f));
			Pub.Sort();
			TArray<double> Reads;
			for (int32 I = 0; I < 1200; ++I) Reads.Add(10.0 + T * I + 0.002);
			const FSimResult R = Simulate(P, Pub, Reads, W, PollS);
			TestTrue(FString::Printf(TEXT("%s: the irregular source does leave reads without a new frame"), Name), R.ReadsWithoutNewFrame > 100);
			// Every probe that timed out is also wasted; probes are not scored (the score is 0 throughout a suspension).
			const int32 Bound = (R.Published + 8 * (int32(P.GetSuspensions()) + 1)) / 3 + 1 + int32(P.GetProbes() - P.GetProbeSuccesses());
			AddInfo(FString::Printf(TEXT("%s, irregular source over 20 s: %d reads without a new frame, %d waits, %d wasted (bound %d), %d suspended decisions, %lld suspensions"),
				Name, R.ReadsWithoutNewFrame, R.Waits, R.Wasted, Bound, R.Suspended, P.GetSuspensions()));
			TestTrue(FString::Printf(TEXT("%s: wasted waits stay within the suspension bound"), Name), R.Wasted <= Bound);
		}
	}

	// Restart: the count starts again; everything learnt is dropped.
	{
		FPublishPredictor P;
		for (int32 I = 0; I < 100; ++I) { P.BeginRead(); P.ObserveCount(10.0 + T * I, 500 + I); }
		TestTrue(TEXT("a regular history gives a period"), P.Predict(10.0 + T * 100, W).PeriodSeconds > 0.0);
		const int64 ResetsBefore = P.GetResets();
		P.ObserveCount(12.0, 3);
		TestEqual(TEXT("a count that starts again resets the predictor"), P.GetResets(), ResetsBefore + 1);
		TestEqual(TEXT("and the next decision is a warm-up"), int32(P.Predict(12.0, W).Decision), int32(EDecision::Warmup));
	}

	// One below the last count is the instant of another process's wait/release read: ignored.
	{
		FPublishPredictor P;
		for (int32 I = 0; I < 100; ++I) P.ObserveCount(10.0 + T * I, 500 + I);
		P.ObserveCount(10.0 + T * 100, 598);
		TestEqual(TEXT("counted as a glitch"), P.GetGlitches(), int64(1));
		TestEqual(TEXT("the last count is unchanged"), P.GetLastCount(), int64(599));
	}

	// No count at all: nothing to predict.
	{
		FPublishPredictor P;
		P.ObserveCount(10.0, -1);
		TestEqual(TEXT("without a count there is no decision to wait"), int32(P.Predict(10.0, W).Decision), int32(EDecision::NoCount));
	}

	// A fine poll bounds the publication to the poll interval, and the next prediction is that tight.
	{
		FPublishPredictor P;
		for (int32 I = 0; I < 100; ++I) P.ObserveCount(10.0 + T * I + 0.005, 500 + I);
		const double Obs = 10.0 + T * 100 + 0.0003;
		P.ObserveCount(Obs - 0.00015, 599);
		P.ObserveCount(Obs, 600);
		const FPrediction Pr = P.Predict(Obs + T - 0.0004, W);
		const double WidthMs = (Pr.LatestSeconds - Pr.EarliestSeconds) * 1000.0;
		AddInfo(FString::Printf(TEXT("after a 0.15 ms poll observation the predicted interval is %.3f ms wide"), WidthMs));
		TestTrue(TEXT("the prediction is about as tight as the poll plus the allowances"), WidthMs < 1.5);
		TestEqual(TEXT("and a read 0.4 ms before it waits"), int32(Pr.Decision), int32(EDecision::Wait));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitProbe, "SpoutPlugin.FrameWait.Predictor.AnEscalatedSuspensionIsProbed", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitProbe::RunTest(const FString&)
{
	// One read per publication of a regular sender, 0.3 ms before it (where the rule waits), then the
	// publication seen at its instant. Wait outcomes are reported directly.
	const double T = 1.0 / 60.0, W = 0.001, Width = 0.00005;
	FPublishPredictor P; // the plugin's default: conditional rule, adaptive recovery
	double Next = PrimeRegular(P, 10.0, T, 60, Width);
	int64 Count = 559;
	auto ReadAt = [&P, &Next, W](double BeforePublication) { return P.Predict(Next - BeforePublication, W); };
	auto Publish = [&P, &Next, &Count, T, Width]()
	{
		P.ObserveCount(Next - Width, Count);
		P.ObserveCount(Next, ++Count);
		Next += T;
	};

	// The base suspension is never probed: its first probe would fall at its end.
	P.ObserveWaitTimedOut();
	P.ObserveWaitTimedOut();
	TestEqual(TEXT("two wasted waits: the base suspension"), P.GetSuspendedReadsLeft(), FTuning::SuspendReads);
	int32 NotSuspended = 0;
	for (int32 I = 1; I < 120; ++I)
	{
		P.BeginRead();
		const FPrediction Pr = ReadAt(0.0003);
		if (Pr.Decision != EDecision::Suspended || Pr.bProbe) ++NotSuspended;
		Publish();
	}
	TestEqual(TEXT("base suspension: reads 1 to 119 are all Suspended, none probed"), NotSuspended, 0);
	P.BeginRead();
	{
		const FPrediction Pr = ReadAt(0.0003);
		TestTrue(TEXT("read 120: the suspension is over, an ordinary wait (not a probe)"), Pr.Decision == EDecision::Wait && !Pr.bProbe);
	}
	Publish();

	// Two more wasted waits without a success in between: an escalated suspension of 240 reads.
	P.BeginRead();
	P.ObserveWaitTimedOut();
	P.ObserveWaitTimedOut();
	Publish();
	TestEqual(TEXT("escalated suspension: 240 reads"), P.GetSuspendedReadsLeft(), int64(240));
	TestEqual(TEXT("the next one would be 480"), P.GetNextSuspendReads(), int64(480));
	for (int32 I = 1; I < 120; ++I)
	{
		P.BeginRead();
		const FPrediction Pr = ReadAt(0.0003);
		if (Pr.Decision != EDecision::Suspended || Pr.bProbe) ++NotSuspended;
		Publish();
	}
	TestEqual(TEXT("escalated suspension: no probe before one interval"), NotSuspended, 0);
	P.BeginRead();
	{
		TestEqual(TEXT("probe allowed now, but a read 8 ms before the publication is not one the rule would wait at: Suspended"), int32(ReadAt(0.008).Decision), int32(EDecision::Suspended));
		TestEqual(TEXT("nor a read 10 ms after the predicted publication (overdue): Suspended"), int32(ReadAt(-0.010).Decision), int32(EDecision::Suspended));
		const FPrediction Pr = ReadAt(0.0003);
		TestTrue(TEXT("the read 0.3 ms before it is a probe"), Pr.Decision == EDecision::Wait && Pr.bProbe);
		const int64 WastedBefore = P.GetWastedWaits();
		P.ObserveWaitTimedOut();
		TestTrue(TEXT("a probe that timed out: still suspended"), P.IsSuspended());
		TestEqual(TEXT("the suspension's end is unchanged"), P.GetSuspendedReadsLeft(), int64(120));
		TestEqual(TEXT("the next probe is one interval later"), P.GetReadsUntilProbe(), FTuning::ProbeIntervalReads);
		TestEqual(TEXT("the escalation is unchanged"), P.GetNextSuspendReads(), int64(480));
		TestEqual(TEXT("it is counted as a wasted wait"), P.GetWastedWaits(), WastedBefore + 1);
		TestEqual(TEXT("one probe, no success"), P.GetProbes(), int64(1));
	}
	Publish();
	for (int32 I = 1; I < 120; ++I)
	{
		P.BeginRead();
		const FPrediction Pr = ReadAt(0.0003);
		if (Pr.Decision != EDecision::Suspended || Pr.bProbe) ++NotSuspended;
		Publish();
	}
	TestEqual(TEXT("between probes: Suspended"), NotSuspended, 0);
	P.BeginRead();
	{
		const FPrediction Pr = ReadAt(0.0003);
		TestTrue(TEXT("read 240: the suspension ran out, an ordinary wait"), Pr.Decision == EDecision::Wait && !Pr.bProbe);
		P.ObserveWaitTimedOut();
	}
	Publish();
	P.BeginRead();
	P.ObserveWaitTimedOut();
	Publish();
	TestEqual(TEXT("two more wasted waits: a suspension of 480 reads"), P.GetSuspendedReadsLeft(), int64(480));
	for (int32 I = 1; I < 120; ++I)
	{
		P.BeginRead();
		const FPrediction Pr = ReadAt(0.0003);
		if (Pr.Decision != EDecision::Suspended || Pr.bProbe) ++NotSuspended;
		Publish();
	}
	TestEqual(TEXT("480-read suspension: Suspended until the first probe"), NotSuspended, 0);
	P.BeginRead();
	{
		const FPrediction Pr = ReadAt(0.0003);
		TestTrue(TEXT("the first probe of the 480-read suspension"), Pr.Decision == EDecision::Wait && Pr.bProbe);
		P.ObserveWaitSucceeded();
		TestFalse(TEXT("a probe that got its frame ends the suspension at once (360 reads early)"), P.IsSuspended());
		TestTrue(TEXT("two probes, one success"), P.GetProbes() == 2 && P.GetProbeSuccesses() == 1);
		TestEqual(TEXT("the escalation is kept after a probe success"), P.GetNextSuspendReads(), int64(960));
	}
	Publish();
	P.BeginRead();
	{
		const FPrediction Pr = ReadAt(0.0003);
		TestTrue(TEXT("the next read: an ordinary wait"), Pr.Decision == EDecision::Wait && !Pr.bProbe);
		P.ObserveWaitSucceeded();
		TestEqual(TEXT("an ordinary success restores the base length"), P.GetNextSuspendReads(), FTuning::SuspendReads);
	}
	Publish();

	// The original recovery: never a probe, and a suspension that begins during a successful wait (at an
	// inconsistent poll) stays.
	{
		FPublishPredictor O;
		Configure(O, EPolicy::Overlap);
		const double N60 = PrimeRegular(O, 10.0, T, 60, Width);
		O.ObserveCount(N60 + 0.002 - 0.00015, 559);
		O.ObserveCount(N60 + 0.002, 560);                    // 2 ms late: inconsistent (score 4), re-anchored on it
		O.ObserveCount(N60 + T - 0.00015, 560);
		O.ObserveCount(N60 + T, 561);                        // on time: early against the re-anchored window (score 8)
		O.BeginRead();
		const double N62 = N60 + 2.0 * T;
		const FPrediction Pr = O.Predict(N62 - 0.0003, W);
		TestEqual(TEXT("original: the read before publication 62 waits"), int32(Pr.Decision), int32(EDecision::Wait));
		O.ObserveCount(N62 + 0.00075, 561);
		O.ObserveCount(N62 + 0.0009, 562);                   // 0.9 ms late, within the bound: inconsistent (score 12)
		TestTrue(TEXT("original: the poll that saw the frame suspended the predictor"), O.IsSuspended() && O.GetSuspensionsForInconsistency() == 1);
		O.ObserveWaitSucceeded();
		TestTrue(TEXT("original: the successful wait does not end that suspension"), O.IsSuspended());
		TestEqual(TEXT("original: no probe"), O.GetProbes(), int64(0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitRecovery, "SpoutPlugin.FrameWait.Predictor.UsefulWaitingRecoversDuringAnEscalatedSuspension", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitRecovery::RunTest(const FString&)
{
	// A sender irregular (+-6 ms) for 50 s leaves the predictor in an escalated suspension; then it
	// becomes regular: directly, after a 5 s pause, after a restart (the count starts again), or after 20 s at 30 fps.
	// Reads 0.2 ms before each slot +- 0.25 ms. Recovery means useful waiting again: reads that find nothing new
	// are waited for and get their frames, not merely that frames arrive or that the countdown ran out.
	// Allowance: the probe interval plus 60 reads; after the rate change also the 4 s period window (240 reads),
	// which must forget the 30 fps samples before the rule waits again.
	const double W = 0.001, PollS = 0.00015;
	struct FCase { ERecoveryKind Kind; const TCHAR* Name; int32 Allowance; };
	const FCase Cases[] = {
		{ ERecoveryKind::Stable, TEXT("irregular, then regular"), int32(FTuning::ProbeIntervalReads) + 60 },
		{ ERecoveryKind::Pause, TEXT("irregular, paused 5 s, then regular"), int32(FTuning::ProbeIntervalReads) + 60 },
		{ ERecoveryKind::Restart, TEXT("irregular, restarted, then regular"), int32(FTuning::ProbeIntervalReads) + 60 },
		{ ERecoveryKind::Rate, TEXT("irregular, 30 fps for 20 s, then 60 fps"), 240 + int32(FTuning::ProbeIntervalReads) + 60 },
	};
	for (const FCase& Case : Cases)
	{
		const FRecoverySender S = MakeRecoverySender(Case.Kind, 3);
		const TArray<double> Reads = JitteredReads(6600, 1003);
		const TArray<int64>* Counts = S.bUseCounts ? &S.Counts : nullptr;
		FPublishPredictor Before;
		TArray<double> Prefix;
		Prefix.Append(Reads.GetData(), S.StableIndex);
		Simulate(Before, S.Publishes, Prefix, W, PollS, Counts);
		const int64 Left = Before.GetSuspendedReadsLeft();

		FPublishPredictor P;
		const FSimResult R = Simulate(P, S.Publishes, Reads, W, PollS, Counts);
		int32 FirstOk = INDEX_NONE, TimeoutsAfter = 0, DecidedTail = 0, OkTail = 0;
		bool bFirstWasProbe = false;
		for (int32 I = S.StableIndex; I < R.PerRead.Num(); ++I)
		{
			const FSimRead& X = R.PerRead[I];
			const bool bOk = X.Decision == int32(EDecision::Wait) && X.Outcome == int32(EOutcome::Published);
			if (bOk && FirstOk == INDEX_NONE) { FirstOk = I; bFirstWasProbe = X.bProbe; }
			if (X.Outcome == int32(EOutcome::TimedOut)) ++TimeoutsAfter;
			if (I >= S.StableIndex + 600 && X.Decision >= 0) { ++DecidedTail; if (bOk) ++OkTail; }
		}
		AddInfo(FString::Printf(TEXT("%s: suspension left when the sender became regular %lld reads; first successful wait after %d reads (%s); from 10 s after, %d of %d decided reads waited and got the frame; probes %lld (%lld got the frame)"),
			Case.Name, Left, FirstOk == INDEX_NONE ? -1 : FirstOk - S.StableIndex, bFirstWasProbe ? TEXT("a probe") : TEXT("not a probe"), OkTail, DecidedTail, P.GetProbes(), P.GetProbeSuccesses()));
		TestTrue(FString::Printf(TEXT("%s: an escalated suspension of at least 960 reads (16 s) was still running"), Case.Name), Left >= 960);
		TestTrue(FString::Printf(TEXT("%s: useful waiting resumed within %d reads"), Case.Name, Case.Allowance), FirstOk != INDEX_NONE && FirstOk - S.StableIndex <= Case.Allowance);
		TestTrue(FString::Printf(TEXT("%s: it was a probe that ended the suspension"), Case.Name), bFirstWasProbe);
		TestEqual(FString::Printf(TEXT("%s: no wait timed out once the sender was regular"), Case.Name), TimeoutsAfter, 0);
		TestTrue(FString::Printf(TEXT("%s: from 10 s after, reads find nothing new and are decided"), Case.Name), DecidedTail > 1500);
		TestEqual(FString::Printf(TEXT("%s: and every one is waited for and gets its frame"), Case.Name), OkTail, DecidedTail);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitPersistentWaste, "SpoutPlugin.FrameWait.Predictor.PersistentlyWastefulWaitingStaysBounded", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitPersistentWaste::RunTest(const FString&)
{
	// A sender irregular (+-6 ms) for 120 s. The probes cost waits. The bound: between two suspensions
	// the wasted-wait score allows (ordinary successes + 8) / 3 wasted waits, and every probe that timed out is wasted
	// too (probes are not scored). For this scenario also at most 3 wasted waits per probe interval (120 reads).
	// That is a check of this scenario, not a bound for every source (the scoring tolerates about one wasted wait in
	// four between suspensions).
	const double W = 0.001, PollS = 0.00015;
	for (uint32 Seed = 1; Seed <= 3; ++Seed)
	{
		const FRecoverySender S = MakeRecoverySender(ERecoveryKind::Stable, Seed, 7210, MAX_int32);
		const TArray<double> Reads = JitteredReads(7200, 1000 + Seed);
		FPublishPredictor P;
		const FSimResult R = Simulate(P, S.Publishes, Reads, W, PollS);
		int32 OrdinaryPublished = 0;
		for (const FSimRead& X : R.PerRead)
		{
			if (X.Decision == int32(EDecision::Wait) && X.Outcome == int32(EOutcome::Published) && !X.bProbe) ++OrdinaryPublished;
		}
		const int64 Bound = (OrdinaryPublished + 8 * (P.GetSuspensions() + 1)) / 3 + 1 + (P.GetProbes() - P.GetProbeSuccesses());
		AddInfo(FString::Printf(TEXT("seed %u: %d waits, %d wasted (bound %lld), %lld suspensions, %lld probes (%lld got the frame), %.2f wasted per 120 reads"),
			Seed, R.Waits, R.Wasted, Bound, P.GetSuspensions(), P.GetProbes(), P.GetProbeSuccesses(), R.Wasted * 120.0 / Reads.Num()));
		TestTrue(FString::Printf(TEXT("seed %u: the probes were exercised"), Seed), P.GetProbes() > 0);
		TestTrue(FString::Printf(TEXT("seed %u: wasted waits within the scoring and probe bound"), Seed), R.Wasted <= Bound);
		TestTrue(FString::Printf(TEXT("seed %u: this scenario wastes at most 3 waits per probe interval"), Seed), int64(R.Wasted) * FTuning::ProbeIntervalReads <= 3 * int64(Reads.Num()));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitSlowerTradeoff, "SpoutPlugin.FrameWait.Predictor.DecliningASlowerSourceIsWaitingOff", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitSlowerTradeoff::RunTest(const FString&)
{
	// What the slower-source decline costs, pinned on both sides. Declining is exactly waiting off for
	// such a source: the same frames are delivered (and the same ones lost), at the same age. A lower average rate
	// does not mean every frame is delivered (a 60 fps sender that drops every 10th frame), and at a phase where
	// reads come just before the publications a wait would make 30 fps frames much fresher (the overlap policy, which
	// waits there, shows it). Delivery, freshness and wasted waiting are reported separately.
	const double T = 1.0 / 60.0, W = 0.001, PollS = 0.00015;
	{
		TArray<double> Pub;
		for (int32 I = 0; I < 610; ++I) Pub.Add(10.0 + I / 30.0);
		const TArray<double> Reads = JitteredReads(1200, 5);
		const FSimResult Off = SimulateOff(Pub, Reads);
		FPublishPredictor Corr, Over;
		Configure(Over, EPolicy::Overlap);
		const FSimResult RC = Simulate(Corr, Pub, Reads, W, PollS);
		const FSimResult RO = Simulate(Over, Pub, Reads, W, PollS);
		AddInfo(FString::Printf(TEXT("30 fps, reads just before the publications: waiting off skipped %d, mean age %.3f ms; corrected %d waits, skipped %d, mean age %.3f ms; overlap %d waits (%d timed out), skipped %d, mean age %.3f ms"),
			Off.Skipped, Off.MeanAgeSeconds() * 1000.0, RC.Waits, RC.Skipped, RC.MeanAgeSeconds() * 1000.0, RO.Waits, RO.Wasted, RO.Skipped, RO.MeanAgeSeconds() * 1000.0));
		TestEqual(TEXT("30 fps: corrected, no wait"), RC.Waits, 0);
		TestTrue(TEXT("30 fps: declined as a slower source"), RC.SlowerSource > 0);
		TestEqual(TEXT("30 fps: delivery equal to waiting off"), RC.Skipped, Off.Skipped);
		TestTrue(TEXT("30 fps: frame age equal to waiting off"), FMath::IsNearlyEqual(RC.MeanAgeSeconds(), Off.MeanAgeSeconds(), 1e-12));
		TestTrue(TEXT("30 fps: a policy that waits here gets frames more than 5 ms fresher on average (the freshness the decline gives up)"), RO.Waits > 0 && RO.MeanAgeSeconds() < Off.MeanAgeSeconds() - 0.005);
	}
	{
		TArray<double> Pub;
		for (int32 I = 0; I < 1210; ++I) if (I % 10 != 9) Pub.Add(10.0 + I * T);
		const TArray<double> Reads = JitteredReads(1200, 6);
		const FSimResult Off = SimulateOff(Pub, Reads);
		FPublishPredictor Corr;
		const FSimResult RC = Simulate(Corr, Pub, Reads, W, PollS);
		AddInfo(FString::Printf(TEXT("60 fps dropping every 10th frame: waiting off skipped %d, mean age %.3f ms; corrected %d waits, %d slower-source declines, skipped %d, mean age %.3f ms"),
			Off.Skipped, Off.MeanAgeSeconds() * 1000.0, RC.Waits, RC.SlowerSource, RC.Skipped, RC.MeanAgeSeconds() * 1000.0));
		TestTrue(TEXT("dropping sender: slower on average, yet frames are lost with waiting off"), Off.Skipped > 0);
		TestEqual(TEXT("dropping sender: corrected, no wait"), RC.Waits, 0);
		TestEqual(TEXT("dropping sender: the same frames lost as with waiting off"), RC.Skipped, Off.Skipped);
		TestTrue(TEXT("dropping sender: frame age equal to waiting off"), FMath::IsNearlyEqual(RC.MeanAgeSeconds(), Off.MeanAgeSeconds(), 1e-12));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitLoop, "SpoutPlugin.FrameWait.TheWaitPollsOutsideAccessAndEndsOnPublicationTimeoutAbortOrClose", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitLoop::RunTest(const FString&)
{
	FWaitCVarScope Wait(TEXT("r.Spout.FrameWaitMs")), Recheck(TEXT("r.Spout.FrameCountRecheck"));
	if (!Wait.Variable || !Recheck.Variable) { AddError(TEXT("the frame-wait settings are not registered")); return false; }
	Wait.Set(TEXT("1"));
	const double T = 1.0 / 60.0;
	auto Never = []() { return false; };

	// Published: the count moves just before the third poll.
	{
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeWaitingSync(Fake, "SpoutFrameWaitPublished");
		TestFalse(TEXT("nothing is pending before the wait"), Sync->HasPendingFrame());
		Sync->PrimePredictorForTests(T, 0.0003, Fake->SdkCountNow());
		Fake->PublishAtQuery = Fake->NewFrameQueries + 3;
		FSpoutFrameWaitReport Report;
		const double Begin = FPlatformTime::Seconds();
		const bool bPending = Sync->WaitForPublication(0.001, 0.00015, Never, Report);
		const double Ms = (FPlatformTime::Seconds() - Begin) * 1000.0;
		TestTrue(TEXT("a publication inside the bound ends the wait with a frame pending"), bPending);
		TestEqual(TEXT("decision: wait"), int32(Report.Decision), int32(EDecision::Wait));
		TestEqual(TEXT("outcome: published"), int32(Report.Outcome), int32(EOutcome::Published));
		TestEqual(TEXT("it took three polls"), Report.Polls, 3);
		TestEqual(TEXT("the count at the read"), Report.CountBefore, int64(1000));
		TestEqual(TEXT("the count seen last is one higher"), Report.CountAfter, int64(1001));
		TestEqual(TEXT("no texture access was attempted by the wait"), Fake->AccessAttempts, 0);
		TestFalse(TEXT("and none is held"), Sync->IsAccessHeld());
		TestTrue(TEXT("the wait ended well inside 50 ms"), Ms < 50.0);
		TestEqual(TEXT("the report carries the predictor's totals"), Report.PredictorWastedWaits, int64(0));
		TestTrue(TEXT("and its jitter allowance (within the floor and the cap)"), Report.PredictorJitterSeconds >= FTuning::JitterAllowanceSeconds - 1e-12 && Report.PredictorJitterSeconds <= FTuning::JitterAllowanceMaxSeconds + 1e-12);
		TestTrue(TEXT("the poll that saw the publication latched it: the frame is pending without another call"), Sync->HasPendingFrame());
		TestTrue(TEXT("a wait that was made"), Report.WasWaitMade());
		int64 Attempts = 0, Published = 0, TimedOut = 0;
		Sync->GetFrameWaitStats(Attempts, Published, TimedOut);
		TestTrue(TEXT("counted as one wait that published"), Attempts == 1 && Published == 1 && TimedOut == 0);
		TestTrue(TEXT("the copy that follows is a delivery"), Sync->CompleteTransfer());
		AddInfo(FString::Printf(TEXT("published after %d polls, %.3f ms"), Report.Polls, Ms));
	}

	// Timed out, twice: the second wasted wait suspends waiting.
	{
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeWaitingSync(Fake, "SpoutFrameWaitTimeout");
		FSpoutFrameWaitReport Report;
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			Sync->PrimePredictorForTests(T, 0.0003, Fake->SdkCountNow());
			if (Pass == 1)
			{
				// Priming forgets the score of the first timeout; give it back so this one is the second in a row.
				Sync->WaitForPublication(0.001, 0.00015, Never, Report);
				Sync->PrimePredictorForTests(T, 0.0003, Fake->SdkCountNow());
			}
			const double Begin = FPlatformTime::Seconds();
			const bool bPending = Sync->WaitForPublication(0.001, 0.00015, Never, Report);
			const double Ms = (FPlatformTime::Seconds() - Begin) * 1000.0;
			TestFalse(TEXT("no publication: nothing pending"), bPending);
			TestEqual(TEXT("outcome: timed out"), int32(Report.Outcome), int32(EOutcome::TimedOut));
			TestTrue(TEXT("it lasted about the bound"), Ms >= 0.9 && Ms < 50.0);
			TestTrue(TEXT("it polled"), Report.Polls >= 1);
			TestFalse(TEXT("and nothing is pending afterwards"), Sync->HasPendingFrame());
			TestFalse(TEXT("a wait that was made is not followed by a transfer without a notification"), Sync->BeginTransferWithoutNotification(Report.WasWaitMade()));
			if (Pass == 0) break;
		}
	}
	{
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeWaitingSync(Fake, "SpoutFrameWaitSuspend");
		Sync->PrimePredictorForTests(T, 0.0003, Fake->SdkCountNow());
		FSpoutFrameWaitReport First, Second, Third;
		Sync->WaitForPublication(0.001, 0.00015, Never, First);
		Sync->WaitForPublication(0.001, 0.00015, Never, Second);
		TestEqual(TEXT("the first timed out"), int32(First.Outcome), int32(EOutcome::TimedOut));
		// The first timeout leaves the window in the past; whether the second waits depends on the overdue test.
		// What must hold is that after two wasted waits no further wait is made.
		if (Second.Outcome == EOutcome::TimedOut)
		{
			const int32 Queries = Fake->NewFrameQueries;
			TestFalse(TEXT("after two wasted waits the third call does not wait"), Sync->WaitForPublication(0.001, 0.00015, Never, Third));
			TestEqual(TEXT("decision: suspended"), int32(Third.Decision), int32(EDecision::Suspended));
			TestEqual(TEXT("and it did not poll"), Fake->NewFrameQueries, Queries);
			TestEqual(TEXT("the report carries the suspension"), Third.PredictorSuspensions, int64(1));
		}
		else
		{
			TestTrue(TEXT("the second call declined a publication already overdue"), Second.Decision == EDecision::Overdue || Second.Decision == EDecision::NotImminent);
		}
	}

	// The bound decides: a window 0.8 ms ahead (about 0.50 to 1.05 ms) is out of reach of 0.3 ms and
	// within reach of the 1 ms limit. On the stepping clock, so that a delay of the test thread
	// cannot move the window against the read.
	{
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeWaitingSync(Fake, "SpoutFrameWaitBound");
		UseSteppingClock(*Sync);
		FSpoutFrameWaitReport Report;
		Sync->PrimePredictorForTests(T, 0.0008, Fake->SdkCountNow());
		const int32 Queries = Fake->NewFrameQueries;
		TestFalse(TEXT("a 0.3 ms bound does not reach the window"), Sync->WaitForPublication(0.0003, 0.00015, Never, Report));
		TestEqual(TEXT("decision: not imminent"), int32(Report.Decision), int32(EDecision::NotImminent));
		TestEqual(TEXT("a declined wait does not read the count at all"), Fake->NewFrameQueries, Queries);
		TestFalse(TEXT("and is not a wait that was made"), Report.WasWaitMade());
		Sync->PrimePredictorForTests(T, 0.0008, Fake->SdkCountNow());
		Fake->PublishAtQuery = Fake->NewFrameQueries + 3;
		TestTrue(TEXT("the 1 ms limit reaches enough of the window: waited and published"), Sync->WaitForPublication(0.001, 0.00015, Never, Report));
		TestEqual(TEXT("decision: wait"), int32(Report.Decision), int32(EDecision::Wait));
		TestEqual(TEXT("outcome: published"), int32(Report.Outcome), int32(EOutcome::Published));
		TestEqual(TEXT("at the third poll, 0.45 ms into the wait"), Report.Polls, 3);
	}

	// Aborted: the stream was cancelled. Not a wasted wait: the next call may wait again.
	{
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeWaitingSync(Fake, "SpoutFrameWaitAbort");
		UseSteppingClock(*Sync);     // the third abort check comes 0.45 ms into a 1 ms wait whatever the scheduler does
		Sync->PrimePredictorForTests(T, 0.0003, Fake->SdkCountNow());
		FSpoutFrameWaitReport Report;
		int32 AbortChecks = 0;
		TestFalse(TEXT("an aborted wait leaves nothing pending"), Sync->WaitForPublication(0.001, 0.00015, [&AbortChecks]() { return ++AbortChecks >= 3; }, Report));
		TestEqual(TEXT("outcome: aborted"), int32(Report.Outcome), int32(EOutcome::Aborted));
		TestEqual(TEXT("it polled twice before the abort"), Report.Polls, 2);
		TestEqual(TEXT("an abort is not counted as a wasted wait"), Report.PredictorWastedWaits, int64(0));
		TestFalse(TEXT("an abort before the first poll leaves nothing pending either"), Sync->WaitForPublication(0.001, 0.00015, []() { return true; }, Report));
		TestEqual(TEXT("and polls nothing"), Report.Polls, 0);
	}

	// Access held: refused without a single poll.
	{
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeWaitingSync(Fake, "SpoutFrameWaitAccessHeld");
		Sync->PrimePredictorForTests(T, 0.0003, Fake->SdkCountNow());
		FSpoutFrameWaitReport Report;
		{
			FSpoutTextureAccessScope Access(*Sync, WaitFakeTexture);
			TestTrue(TEXT("access acquired"), Access.IsAcquired());
			const int32 Queries = Fake->NewFrameQueries;
			TestFalse(TEXT("a wait while access is held is refused"), Sync->WaitForPublication(0.001, 0.00015, Never, Report));
			TestEqual(TEXT("decision: access held"), int32(Report.Decision), int32(EDecision::AccessHeld));
			TestEqual(TEXT("the count was not even read"), Fake->NewFrameQueries, Queries);
		}
		TestFalse(TEXT("access released"), Sync->IsAccessHeld());
	}

	// A frame already pending: no wait is needed.
	{
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeWaitingSync(Fake, "SpoutFrameWaitAlreadyNew");
		Fake->Publish();
		TestTrue(TEXT("the read finds the publication"), Sync->ConsumeNewFrame());
		FSpoutFrameWaitReport Report;
		const int32 Queries = Fake->NewFrameQueries;
		TestTrue(TEXT("a pending frame is reported at once"), Sync->WaitForPublication(0.001, 0.00015, Never, Report));
		TestEqual(TEXT("decision: already new"), int32(Report.Decision), int32(EDecision::AlreadyNew));
		TestEqual(TEXT("without a poll"), Fake->NewFrameQueries, Queries);
	}

	// Another receiver's read during the wait: the count is one low at a poll. Not a publication.
	{
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeWaitingSync(Fake, "SpoutFrameWaitTransient");
		Sync->PrimePredictorForTests(T, 0.0003, Fake->SdkCountNow());
		Fake->ForeignDecrementAtNextRead = 1;
		FSpoutFrameWaitReport Report;
		TestFalse(TEXT("a count one low at a poll does not end the wait with a frame"), Sync->WaitForPublication(0.001, 0.00015, Never, Report));
		TestEqual(TEXT("the wait ran to its end"), int32(Report.Outcome), int32(EOutcome::TimedOut));
		TestFalse(TEXT("nothing is pending"), Sync->HasPendingFrame());
		TestEqual(TEXT("the poll was counted as another receiver's read"), Sync->GetNotificationStats().Transients, int64(1));
		// The same with the publication coming after it.
		FSpoutSdkCountModelBackend* Fake2 = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync2 = MakeWaitingSync(Fake2, "SpoutFrameWaitTransientThenPublished");
		Sync2->PrimePredictorForTests(T, 0.0003, Fake2->SdkCountNow());
		Fake2->ForeignDecrementAtNextRead = 1;
		Fake2->PublishAtQuery = Fake2->NewFrameQueries + 3;
		TestTrue(TEXT("the publication after it still ends the wait"), Sync2->WaitForPublication(0.001, 0.00015, Never, Report));
		TestEqual(TEXT("at the third poll"), Report.Polls, 3);
	}

	// The count goes away during the wait: the frame is pending, as on every read without a count.
	{
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeWaitingSync(Fake, "SpoutFrameWaitCountLost");
		Sync->PrimePredictorForTests(T, 0.0003, Fake->SdkCountNow());
		Fake->bCountingEnabled = false;
		FSpoutFrameWaitReport Report;
		TestTrue(TEXT("a count that went away ends the wait with the frame pending"), Sync->WaitForPublication(0.001, 0.00015, Never, Report));
		TestEqual(TEXT("outcome: count lost"), int32(Report.Outcome), int32(EOutcome::CountLost));
		TestEqual(TEXT("at the first poll"), Report.Polls, 1);
	}

	// No count at all: there is nothing to wait for. (A sender without a count makes every read new,
	// so the receiver never gets here; a backend that says "not new" without a count is refused too.)
	{
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeModelSync(Fake, "SpoutFrameWaitNoCount");
		Sync->ConsumeNewFrame();
		Sync->CompleteTransfer();
		Fake->bCountingEnabled = false;
		Fake->LastFrameCount = 0;
		Sync->ConsumeNewFrame();      // new, as always without a count
		Sync->CompleteTransfer();
		FSpoutFrameWaitReport Report;
		const int32 Queries = Fake->NewFrameQueries;
		TestFalse(TEXT("without a count no wait is made"), Sync->WaitForPublication(0.001, 0.00015, Never, Report));
		TestEqual(TEXT("decision: no count"), int32(Report.Decision), int32(EDecision::NoCount));
		TestEqual(TEXT("and nothing is polled"), Fake->NewFrameQueries, Queries);
	}

	// Closed underneath the wait.
	{
		FSpoutSdkCountModelBackend* Fake = nullptr;
		TUniquePtr<FSpoutFrameSync> Sync = MakeWaitingSync(Fake, "SpoutFrameWaitClosed");
		Sync->PrimePredictorForTests(T, 0.0003, Fake->SdkCountNow());
		FSpoutFrameSync* Raw = Sync.Get();
		bool bClosed = false;
		FSpoutFrameWaitReport Report;
		const bool bPending = Sync->WaitForPublication(0.001, 0.00015, [Raw, &bClosed]() { if (!bClosed) { Raw->Close(); bClosed = true; } return false; }, Report);
		TestFalse(TEXT("a wait on a stream closed underneath it leaves nothing pending"), bPending);
		TestEqual(TEXT("outcome: closed"), int32(Report.Outcome), int32(EOutcome::Closed));
		FSpoutFrameWaitReport After;
		TestFalse(TEXT("a closed stream does not wait"), Sync->WaitForPublication(0.001, 0.00015, Never, After));
		TestEqual(TEXT("outcome: closed"), int32(After.Outcome), int32(EOutcome::Closed));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitBudget, "SpoutPlugin.FrameWait.TheBudgetIsSharedPerRenderFrame", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitBudget::RunTest(const FString&)
{
	// The budget state belongs to the render thread; this test assumes no receiver is live while it
	// runs and uses frame numbers no real frame will have.
	FWaitCVarScope Budget(TEXT("r.Spout.FrameWaitBudgetMs"));
	if (!Budget.Variable) { AddError(TEXT("r.Spout.FrameWaitBudgetMs is not registered")); return false; }
	Budget.Set(TEXT("1"));
	const uint64 FrameA = MAX_uint64 - 7, FrameB = MAX_uint64 - 6;
	TestTrue(TEXT("a fresh frame grants the whole request"), FMath::IsNearlyEqual(SpoutFrameWait::GrantFromFrameBudget(0.001, FrameA), 0.001, 1e-9));
	SpoutFrameWait::ChargeFrameBudget(0.0006, FrameA);
	TestTrue(TEXT("a second stream gets what is left"), FMath::IsNearlyEqual(SpoutFrameWait::GrantFromFrameBudget(0.001, FrameA), 0.0004, 1e-9));
	SpoutFrameWait::ChargeFrameBudget(0.0007, FrameA); // waits can overrun their grant; the charge is what happened
	TestEqual(TEXT("a third stream gets nothing"), SpoutFrameWait::GrantFromFrameBudget(0.001, FrameA), 0.0);
	TestTrue(TEXT("the next render frame starts again"), FMath::IsNearlyEqual(SpoutFrameWait::GrantFromFrameBudget(0.001, FrameB), 0.001, 1e-9));
	Budget.Set(TEXT("0"));
	TestEqual(TEXT("a zero budget grants nothing"), SpoutFrameWait::GrantFromFrameBudget(0.001, FrameB), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutFrameWaitBudgetedWait, "SpoutPlugin.FrameWait.TheBudgetedWaitGrantsChargesAndRefusesWhenSpent", SPOUT_FRAME_WAIT_FLAGS)
bool FSpoutFrameWaitBudgetedWait::RunTest(const FString&)
{
	// SpoutFrameWait::RunBudgetedWait is what the receiver calls for each read without a new frame.
	FWaitCVarScope Budget(TEXT("r.Spout.FrameWaitBudgetMs")), Wait(TEXT("r.Spout.FrameWaitMs"));
	if (!Budget.Variable || !Wait.Variable) { AddError(TEXT("the frame-wait settings are not registered")); return false; }
	Budget.Set(TEXT("1"));
	const uint64 FrameA = MAX_uint64 - 17, FrameB = MAX_uint64 - 16, FrameC = MAX_uint64 - 15, FrameD = MAX_uint64 - 14;

	// A stub wait that records its grant and reports a given duration.
	TArray<double> Grants;
	double UseSeconds = 0.0;
	auto Stub = [&Grants, &UseSeconds](double GrantedSeconds, FSpoutFrameWaitReport& Report)
	{
		Grants.Add(GrantedSeconds);
		Report.Decision = EDecision::Wait;
		Report.Outcome = EOutcome::Published;
		Report.WaitBeginCycles = 1000;
		Report.WaitEndCycles = 1000 + CyclesFor(UseSeconds);
		return true;
	};
	FSpoutFrameWaitReport Report;
	double Before = -1.0, After = -1.0;

	TestFalse(TEXT("a request of zero does not wait"), SpoutFrameWait::RunBudgetedWait(0.0, FrameA, Stub, Report, Before, After));
	TestEqual(TEXT("and the decision stays disabled"), int32(Report.Decision), int32(EDecision::Disabled));
	TestEqual(TEXT("the wait was not called"), Grants.Num(), 0);
	TestFalse(TEXT("which is not a wait that was made"), Report.WasWaitMade());

	UseSeconds = 0.0006;
	TestTrue(TEXT("first stream: waited"), SpoutFrameWait::RunBudgetedWait(0.001, FrameA, Stub, Report, Before, After));
	TestTrue(TEXT("first stream: granted the whole 1 ms"), Grants.Num() == 1 && FMath::IsNearlyEqual(Grants[0], 0.001, 1e-9));
	TestTrue(TEXT("first stream: charged what it took"), FMath::IsNearlyEqual(Before, 0.0, 1e-9) && FMath::IsNearlyEqual(After, 0.0006, 1e-6));
	UseSeconds = 0.0007;
	SpoutFrameWait::RunBudgetedWait(0.001, FrameA, Stub, Report, Before, After);
	TestTrue(TEXT("second stream: granted only the 0.4 ms left"), Grants.Num() == 2 && FMath::IsNearlyEqual(Grants[1], 0.0004, 1e-6));
	TestTrue(TEXT("second stream: an overrun is charged as it happened"), FMath::IsNearlyEqual(After, 0.0013, 1e-6));
	TestFalse(TEXT("third stream: the budget is spent"), SpoutFrameWait::RunBudgetedWait(0.001, FrameA, Stub, Report, Before, After));
	TestEqual(TEXT("third stream: decision budget exhausted"), int32(Report.Decision), int32(EDecision::BudgetExhausted));
	TestEqual(TEXT("third stream: the wait was not called"), Grants.Num(), 2);
	TestTrue(TEXT("third stream: nothing charged"), FMath::IsNearlyEqual(Before, After, 1e-12));
	TestFalse(TEXT("third stream: no wait was made"), Report.WasWaitMade());
	UseSeconds = 0.0001;
	SpoutFrameWait::RunBudgetedWait(0.001, FrameB, Stub, Report, Before, After);
	TestTrue(TEXT("the next render frame grants the whole 1 ms again"), Grants.Num() == 3 && FMath::IsNearlyEqual(Grants[2], 0.001, 1e-9));

	// Settings above the limit change nothing: the request and the budget are both read as 1 ms, so two
	// streams still share one millisecond per render frame.
	const uint64 FrameE = MAX_uint64 - 13;
	Budget.Set(TEXT("4"));
	Wait.Set(TEXT("4"));
	TestTrue(TEXT("a wait setting of 4 ms asks for 1 ms"), FMath::IsNearlyEqual(SpoutLimits::FrameWaitSeconds(), 0.001, 1e-9));
	TestTrue(TEXT("a budget setting of 4 ms is a budget of 1 ms"), FMath::IsNearlyEqual(SpoutLimits::FrameWaitBudgetSeconds(), 0.001, 1e-9));
	Grants.Reset();
	UseSeconds = 0.0006;
	SpoutFrameWait::RunBudgetedWait(SpoutLimits::FrameWaitSeconds(), FrameE, Stub, Report, Before, After);
	TestTrue(TEXT("above the limit, first stream: granted 1 ms, not 4"), Grants.Num() == 1 && FMath::IsNearlyEqual(Grants[0], 0.001, 1e-9));
	SpoutFrameWait::RunBudgetedWait(SpoutLimits::FrameWaitSeconds(), FrameE, Stub, Report, Before, After);
	TestTrue(TEXT("above the limit, second stream: granted the 0.4 ms left of 1 ms"), Grants.Num() == 2 && FMath::IsNearlyEqual(Grants[1], 0.0004, 1e-6));
	TestFalse(TEXT("above the limit, third stream: the 1 ms is spent"), SpoutFrameWait::RunBudgetedWait(SpoutLimits::FrameWaitSeconds(), FrameE, Stub, Report, Before, After));
	TestEqual(TEXT("above the limit, third stream: decision budget exhausted"), int32(Report.Decision), int32(EDecision::BudgetExhausted));

	// With the real loop: what is left of the budget is the bound the rule sees. Everything inside the
	// 1 ms limit: a budget of 1 ms and a window about 0.8 ms ahead (0.50 to 1.05 ms), on the stepping
	// clock so that a delay of the test thread cannot change a decision.
	Budget.Set(TEXT("1"));
	Wait.Set(TEXT("1"));
	const double T = 1.0 / 60.0;
	auto Never = []() { return false; };
	FSpoutSdkCountModelBackend* Fake = nullptr;
	TUniquePtr<FSpoutFrameSync> Sync = MakeWaitingSync(Fake, "SpoutFrameWaitBudgeted");
	UseSteppingClock(*Sync);
	FSpoutFrameSync* Raw = Sync.Get();
	auto Real = [Raw, &Never](double GrantedSeconds, FSpoutFrameWaitReport& R) { return Raw->WaitForPublication(GrantedSeconds, 0.00015, Never, R); };

	SpoutFrameWait::ChargeFrameBudget(0.0007, FrameC); // other streams already waited 0.7 ms in this frame
	Sync->PrimePredictorForTests(T, 0.0008, Fake->SdkCountNow());
	TestFalse(TEXT("0.3 ms left: no frame"), SpoutFrameWait::RunBudgetedWait(SpoutLimits::FrameWaitSeconds(), FrameC, Real, Report, Before, After));
	TestTrue(TEXT("0.3 ms left: the loop was given 0.3 ms"), FMath::IsNearlyEqual(Report.BoundSeconds, 0.0003, 1e-6));
	TestEqual(TEXT("0.3 ms left: the window is out of reach, refused"), int32(Report.Decision), int32(EDecision::NotImminent));
	TestTrue(TEXT("0.3 ms left: nothing more charged"), FMath::IsNearlyEqual(Before, After, 1e-12));

	Sync->PrimePredictorForTests(T, 0.0008, Fake->SdkCountNow());
	Fake->PublishAtQuery = Fake->NewFrameQueries + 3;
	TestTrue(TEXT("a fresh frame: the full 1 ms grant reaches the window, waited and published"), SpoutFrameWait::RunBudgetedWait(SpoutLimits::FrameWaitSeconds(), FrameD, Real, Report, Before, After));
	TestTrue(TEXT("a fresh frame: the loop was given the whole 1 ms"), FMath::IsNearlyEqual(Report.BoundSeconds, 0.001, 1e-6));
	TestEqual(TEXT("a fresh frame: decision wait"), int32(Report.Decision), int32(EDecision::Wait));
	TestEqual(TEXT("a fresh frame: outcome published"), int32(Report.Outcome), int32(EOutcome::Published));
	TestTrue(TEXT("a fresh frame: the wait was charged, and no more than it could take"), After > Before && After - Before < 0.05);
	return true;
}
#endif
