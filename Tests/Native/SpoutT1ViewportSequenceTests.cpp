// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
//
// Native tests of the viewport check's phases (Source/SpoutPlugin/Private/Tests/SpoutT1ViewportSequence.h),
// compiled from the plugin's own header without Unreal and without a GPU.
//
// The point: the resize is requested exactly once per size, in the tick that enters the Resize phase, whether the
// next tick runs in the same frame or in a later one; never when the viewport already has the size; and the two sizes
// are sent, received and verified in order.
#include "Tests/SpoutT1ViewportSequence.h"
#include <iostream>
#include <vector>

using namespace SpoutT1;

static int Failures = 0;
static int Checks = 0;
static void Check(bool Value, const char* Message)
{
	++Checks;
	if (!Value) { std::cout << "FAIL " << Message << std::endl; ++Failures; }
}

static int Count(const std::vector<EAction>& Actions, EAction A)
{
	int N = 0;
	for (EAction X : Actions) N += X == A ? 1 : 0;
	return N;
}

struct FDriver
{
	FViewportSequence Sequence;
	FInput In;
	std::vector<EAction> All;
	explicit FDriver(FSize Start) : Sequence({ FSize{1280, 720}, FSize{640, 360} }) { In.Viewport = Start; }
	std::vector<EAction> Tick(int FrameStep, double TimeStep)
	{
		In.Frame += FrameStep;
		In.Now += TimeStep;
		std::vector<EAction> Out;
		Sequence.Tick(In, Out);
		All.insert(All.end(), Out.begin(), Out.end());
		return Out;
	}
};

/** The whole two-size sequence. FrameStep 0: every next tick runs in the same frame as the one before. */
static void TwoSizes(int FrameStep, const char* Label)
{
	FDriver D(FSize{800, 600});
	std::vector<EAction> A = D.Tick(FrameStep, 0.016);
	Check(A.size() == 1 && A[0] == EAction::RequestResize, Label);
	for (int I = 0; I < 50; ++I) D.Tick(FrameStep, 0.016);  // the window has not changed yet
	Check(D.Sequence.GetResizeRequests(0) == 1 && Count(D.All, EAction::RequestResize) == 1, "size 0 is asked for exactly once while the viewport still differs");
	D.In.Viewport = FSize{1280, 720};
	D.Tick(1, 0.016);
	Check(D.Sequence.GetPhase() == EPhase::Settle, "the reached size moves to Settle");
	for (int I = 0; I < 9; ++I) D.Tick(1, 0.016);
	Check(D.Sequence.GetPhase() == EPhase::Settle, "Settle lasts ten frames");
	A = D.Tick(1, 0.016);
	Check(Count(A, EAction::SendViewport) == 1 && D.Sequence.GetPhase() == EPhase::AwaitCapture, "the send is the entry action of AwaitCapture");
	for (int I = 0; I < 5; ++I) D.Tick(FrameStep, 0.016);
	Check(D.Sequence.GetSends(0) == 1, "one send for size 0");
	D.In.bCaptureTaken = true;
	A = D.Tick(1, 0.016);
	Check(Count(A, EAction::Receive) == 1 && D.Sequence.GetPhase() == EPhase::Receive, "the first receive is the entry action of Receive");
	A = D.Tick(1, 0.016);
	Check(Count(A, EAction::Receive) == 1, "receives repeat while nothing is delivered");
	D.In.bDelivered = true;
	A = D.Tick(FrameStep, 0.016);
	Check(A.size() == 2 && A[0] == EAction::Verify && A[1] == EAction::RequestResize, "verify, then size 1 is asked for in the same tick");
	D.In.bDelivered = false;
	D.In.bCaptureTaken = false;
	for (int I = 0; I < 50; ++I) D.Tick(FrameStep, 0.016);
	Check(D.Sequence.GetResizeRequests(1) == 1, "size 1 is asked for exactly once");
	D.In.Viewport = FSize{640, 360};
	D.Tick(1, 0.016);
	for (int I = 0; I < 10; ++I) D.Tick(1, 0.016);
	Check(D.Sequence.GetSends(1) == 1, "one send for size 1");
	D.In.bCaptureTaken = true;
	D.Tick(1, 0.016);
	D.In.bDelivered = true;
	A = D.Tick(1, 0.016);
	Check(A.size() == 2 && A[0] == EAction::Verify && A[1] == EAction::Finish, "the second verify finishes");
	Check(Count(D.All, EAction::RequestResize) == 2 && Count(D.All, EAction::SendViewport) == 2 && Count(D.All, EAction::Verify) == 2,
		"two sizes: two requests, two sends, two verifications");
	Check(Count(D.All, EAction::Fail) == 0, "no failure");
}

static void AlreadySized()
{
	FDriver D(FSize{1280, 720});
	std::vector<EAction> A = D.Tick(1, 0.016);
	Check(A.empty() && D.Sequence.GetResizeRequests(0) == 0, "a viewport that already has the size is not asked for it");
}

static void Timeouts()
{
	FDriver D(FSize{800, 600});
	D.Tick(1, 0.0);
	for (int I = 0; I < 9; ++I) D.Tick(1, 1.0);
	Check(D.Sequence.GetPhase() == EPhase::Resize, "still waiting before 10 s");
	std::vector<EAction> A = D.Tick(1, 1.5);
	Check(Count(A, EAction::Fail) == 1 && D.Sequence.GetResizeRequests(0) == 1, "a size never reached fails after 10 s, asked for once");

	FDriver C(FSize{1280, 720});
	C.Tick(1, 0.0);
	for (int I = 0; I < 11; ++I) C.Tick(1, 0.0);
	Check(C.Sequence.GetPhase() == EPhase::AwaitCapture, "in AwaitCapture");
	A = C.Tick(1, 5.5);
	Check(Count(A, EAction::Fail) == 1, "a missing capture fails after 5 s");

	FDriver R(FSize{1280, 720});
	R.Tick(1, 0.0);
	for (int I = 0; I < 11; ++I) R.Tick(1, 0.0);
	R.In.bCaptureTaken = true;
	R.Tick(1, 0.0);
	A = R.Tick(1, 5.5);
	Check(Count(A, EAction::Fail) == 1, "a frame never received fails after 5 s");
}

int main()
{
	TwoSizes(0, "same-frame ticks: size 0 is asked for in the first tick");
	TwoSizes(1, "next-frame ticks: size 0 is asked for in the first tick");
	TwoSizes(7, "ticks several frames apart: size 0 is asked for in the first tick");
	AlreadySized();
	Timeouts();
	std::cout << "viewport sequence checks " << Checks << ", failures " << Failures << std::endl;
	return Failures ? 1 : 0;
}
