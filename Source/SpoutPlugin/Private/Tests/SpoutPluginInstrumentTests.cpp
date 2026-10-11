// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// Measurement builds only (SPOUT_WITH_INSTRUMENT): the measurement log's column line and its rows
// agree, field for field. In a release build this file compiles to nothing, and the test below
// does not exist; the registered-name check of a release build expects it to be absent.
#include "Misc/AutomationTest.h"
#include "SpoutInstrument.h"
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS && SPOUT_WITH_INSTRUMENT

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutInstrumentColumns, "SpoutPlugin.Instrument.RecordFieldsMatchTheColumnLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FSpoutInstrumentColumns::RunTest(const FString&)
{
	TArray<FString> Columns;
	FString(SpoutInstrument::ColumnHeader()).ParseIntoArray(Columns, TEXT(","), false);
	TestEqual(TEXT("instrumentVersion 6 has 49 columns"), Columns.Num(), 49);
	TestEqual(TEXT("the first column"), Columns[0], FString(TEXT("kind")));
	TestEqual(TEXT("column 47 is the last of version 5"), Columns[46], FString(TEXT("syncOpens")));
	TestEqual(TEXT("then the completion flag"), Columns[47], FString(TEXT("completed")));
	TestEqual(TEXT("and the read under access"), Columns.Last(), FString(TEXT("newUnderAccess")));

	// A row whose every field has a distinct value, so a field written in the wrong place shows.
	SpoutInstrument::FRecord R;
	R.Kind = TEXT('C'); R.Stream = 3; R.FrameRT = 1001; R.TDetect = 1002; R.NewFrame = 1; R.SenderFrame = 1003; R.Pending = 0;
	R.TDrain0 = 1004; R.TDrain1 = 1005; R.TWrap0 = 1006; R.TWrap1 = 1007; R.TAccess0 = 1008; R.TAcquire = 1009; R.Acquired = 1;
	R.TCopyRec = 1010; R.TGpuDone = 1011; R.TPublish = 1012; R.TRelease = 1013; R.TRestore = 1014;
	R.GpuT0 = 1015; R.GpuT1 = 1016; R.GpuFreq = 1017; R.GpuDisjoint = 0; R.Hr = 0x12;
	R.WaitDecision = 9; R.WaitOutcome = 1; R.TWait0 = 1018; R.TWait1 = 1019; R.WaitPolls = 4; R.WaitBoundUs = 1000.0;
	R.PredLoUs = 100.0; R.PredHiUs = 600.0; R.PeriodUs = 16666.7; R.CountAfterWait = 1020; R.CountUnderAccess = 1021;
	R.BudgetBeforeUs = 10.0; R.BudgetAfterUs = 460.0; R.Flags = 43; R.PredSuspensions = 2; R.PredWastedWaits = 5; R.PredJitterUs = 250.0;
	R.AckBefore = -2; R.Delivered = 1; R.ReceivedBefore = 1022; R.ReceivedAfter = 1023; R.FrameRequests = 6; R.SyncOpens = 7;
	R.Completed = 1; R.NewUnderAccess = 0;
	FString Line = SpoutInstrument::FormatRecordLine(R);
	TestTrue(TEXT("a row ends with a line break"), Line.EndsWith(TEXT("\n")));
	Line.TrimEndInline();
	TArray<FString> Fields;
	Line.ParseIntoArray(Fields, TEXT(","), false);
	if (!TestEqual(TEXT("a row has one field per column"), Fields.Num(), Columns.Num())) return false;
	auto Field = [&Columns, &Fields](const TCHAR* Name) { const int32 I = Columns.IndexOfByKey(FString(Name)); return I == INDEX_NONE ? FString(TEXT("<no such column>")) : Fields[I]; };
	const TCHAR* const Expected[][2] = {
		{ TEXT("kind"), TEXT("C") }, { TEXT("stream"), TEXT("3") }, { TEXT("frameRT"), TEXT("1001") }, { TEXT("tDetect"), TEXT("1002") }, { TEXT("newFrame"), TEXT("1") },
		{ TEXT("senderFrame"), TEXT("1003") }, { TEXT("pending"), TEXT("0") }, { TEXT("tDrain0"), TEXT("1004") }, { TEXT("tDrain1"), TEXT("1005") },
		{ TEXT("tWrap0"), TEXT("1006") }, { TEXT("tWrap1"), TEXT("1007") }, { TEXT("tAccess0"), TEXT("1008") }, { TEXT("tAcquire"), TEXT("1009") }, { TEXT("acquired"), TEXT("1") },
		{ TEXT("tCopyRec"), TEXT("1010") }, { TEXT("tGpuDone"), TEXT("1011") }, { TEXT("tPublish"), TEXT("1012") }, { TEXT("tRelease"), TEXT("1013") }, { TEXT("tRestore"), TEXT("1014") },
		{ TEXT("gpuT0"), TEXT("1015") }, { TEXT("gpuT1"), TEXT("1016") }, { TEXT("gpuFreq"), TEXT("1017") }, { TEXT("gpuDisjoint"), TEXT("0") }, { TEXT("hr"), TEXT("0x00000012") },
		{ TEXT("waitDecision"), TEXT("9") }, { TEXT("waitOutcome"), TEXT("1") }, { TEXT("tWait0"), TEXT("1018") }, { TEXT("tWait1"), TEXT("1019") }, { TEXT("waitPolls"), TEXT("4") },
		{ TEXT("waitBoundUs"), TEXT("1000.0") }, { TEXT("predLoUs"), TEXT("100.0") }, { TEXT("predHiUs"), TEXT("600.0") }, { TEXT("periodUs"), TEXT("16666.7") },
		{ TEXT("countAfterWait"), TEXT("1020") }, { TEXT("countUnderAccess"), TEXT("1021") }, { TEXT("budgetBeforeUs"), TEXT("10.0") }, { TEXT("budgetAfterUs"), TEXT("460.0") },
		{ TEXT("flags"), TEXT("43") }, { TEXT("predSuspensions"), TEXT("2") }, { TEXT("predWastedWaits"), TEXT("5") }, { TEXT("predJitterUs"), TEXT("250.0") },
		{ TEXT("ackBefore"), TEXT("-2") }, { TEXT("delivered"), TEXT("1") }, { TEXT("receivedBefore"), TEXT("1022") }, { TEXT("receivedAfter"), TEXT("1023") },
		{ TEXT("frameRequests"), TEXT("6") }, { TEXT("syncOpens"), TEXT("7") }, { TEXT("completed"), TEXT("1") }, { TEXT("newUnderAccess"), TEXT("0") } };
	TestEqual(TEXT("every column has an expected value in this test"), int32(UE_ARRAY_COUNT(Expected)), Columns.Num());
	for (const auto& Pair : Expected)
	{
		TestEqual(FString::Printf(TEXT("column %s"), Pair[0]), Field(Pair[0]), FString(Pair[1]));
	}
	return true;
}
#endif
