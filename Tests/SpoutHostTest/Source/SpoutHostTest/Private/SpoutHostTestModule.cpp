// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#include "SpoutHostTestModule.h"

#include "SpoutHostTestPerf.h"
#include "SpoutHostTestReport.h"
#include "SpoutHostTestSmoke.h"

#include "Misc/CommandLine.h"

DEFINE_LOG_CATEGORY(LogSpoutHostTest);

#define LOCTEXT_NAMESPACE "FSpoutHostTestModule"

void FSpoutHostTestModule::StartupModule()
{
	// Only arm the evidence channel when a harness was actually requested, so an editor session
	// opening this project for the automation suite does not scatter marker files around.
	if (SpoutHostTest::FSmokeRunner::IsRequested() || SpoutHostTest::FPerfHarness::IsRequested())
	{
		SpoutHostTest::FReport::Initialize();
	}

	SpoutHostTest::FSmokeRunner::Start();
	SpoutHostTest::FPerfHarness::Start();
}

void FSpoutHostTestModule::ShutdownModule()
{
	const bool bSmokeRan = SpoutHostTest::FSmokeRunner::WasStarted();
	const bool bPerfRan = SpoutHostTest::FPerfHarness::WasStarted();

	SpoutHostTest::FSmokeRunner::Stop();
	SpoutHostTest::FPerfHarness::Stop();

	SpoutHostTest::FReport* Report = SpoutHostTest::FReport::Get();
	if (Report != nullptr && (bSmokeRan || bPerfRan))
	{
		Report->Mark(SpoutHostTest::Markers::ShutdownComplete);

		const FString RunKind = bSmokeRan ? TEXT("smoke") : TEXT("performance");
		const bool bHarnessPassed = bSmokeRan
			? SpoutHostTest::FSmokeRunner::HasPassed()
			: SpoutHostTest::FPerfHarness::HasPassed();

		// The pass marker is emitted here and nowhere else. Reaching this line means the sequence
		// completed AND the engine tore down without taking the process with it.
		if (bHarnessPassed && !Report->HasFailed())
		{
			Report->Mark(SpoutHostTest::Markers::SmokePass);
			Report->WriteResultJson(RunKind, /*bPassed*/ true);
		}
		else
		{
			if (!Report->HasFailed())
			{
				// Neither passed nor recorded a reason: the run was cut short before any step could
				// decide. Say exactly that instead of inventing a cause.
				Report->Fail(TEXT("run ended before the sequence completed and no step reported a reason"));
			}
			Report->WriteResultJson(RunKind, /*bPassed*/ false);
		}
	}

	SpoutHostTest::FReport::Shutdown();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_PRIMARY_GAME_MODULE(FSpoutHostTestModule, SpoutHostTest, "SpoutHostTest");
