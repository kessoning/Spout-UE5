// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * The phases of the viewport check (Spout.T1.Viewport, SpoutT1Commands.cpp), engine-free so that
 * Tests/Native/SpoutT1ViewportSequenceTests.cpp checks them without Unreal.
 *
 * For each size, in order: Resize (ask for the size and wait until the viewport has it), Settle (a few frames),
 * AwaitCapture (send the viewport once and wait for the read-back of the exact back buffer sent), Receive (receive
 * until that frame arrived), then Verify. After the last size, Finish.
 *
 * ENTRY ACTIONS. The resize request and the send are actions of ENTERING their phase. They are emitted in the very
 * tick in which the phase is entered, exactly once per entry, and never again while the phase lasts, whatever the
 * frame counter says: whether the next tick runs in the same frame or in a later one changes nothing. A resize is not
 * requested when the viewport already has the size.
 *
 * Not thread-safe: the game-thread ticker calls it.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace SpoutT1
{
	struct FSize
	{
		int X = 0;
		int Y = 0;
		bool operator==(const FSize& O) const { return X == O.X && Y == O.Y; }
		bool operator!=(const FSize& O) const { return !(*this == O); }
	};

	/** What the caller must do this tick, in the order given. */
	enum class EAction : std::uint8_t
	{
		RequestResize = 0,  // ask for GetWanted() once
		SendViewport = 1,   // arm the source read-back and queue one viewport send
		Receive = 2,        // one receive call
		Verify = 3,         // compare the received texture with the read-back of the sent back buffer
		Finish = 4,         // every size verified
		Fail = 5            // a phase passed its bound; GetFailure() says which
	};

	enum class EPhase : std::uint8_t
	{
		NotStarted = 0,
		Resize = 1,
		Settle = 2,
		AwaitCapture = 3,
		Receive = 4,
		Done = 5
	};

	/** What the caller observed before this tick's actions. */
	struct FInput
	{
		FSize Viewport;
		std::uint64_t Frame = 0;
		double Now = 0.0;
		/** The read-back of the sent back buffer was taken. */
		bool bCaptureTaken = false;
		/** The received texture has the sent size and a frame arrived after the send. */
		bool bDelivered = false;
	};

	struct FLimits
	{
		double ResizeSeconds = 10.0;
		std::uint64_t SettleFrames = 10;
		double CaptureSeconds = 5.0;
		double ReceiveSeconds = 5.0;
	};

	class FViewportSequence
	{
	public:
		explicit FViewportSequence(const std::vector<FSize>& InSizes, const FLimits& InLimits = FLimits())
			: Sizes(InSizes)
			, Limits(InLimits)
			, ResizeRequests(InSizes.size(), 0)
			, Sends(InSizes.size(), 0)
		{
		}

		/** One tick. Appends this tick's actions to Out. */
		void Tick(const FInput& In, std::vector<EAction>& Out)
		{
			switch (Phase)
			{
			case EPhase::NotStarted:
				if (Sizes.empty())
				{
					Phase = EPhase::Done;
					Out.push_back(EAction::Finish);
					return;
				}
				Enter(EPhase::Resize, In, Out);
				return;
			case EPhase::Resize:
				if (In.Viewport == GetWanted())
				{
					Enter(EPhase::Settle, In, Out);
				}
				else if (In.Now - PhaseStart > Limits.ResizeSeconds)
				{
					FailWith("the viewport did not reach the wanted size in time", Out);
				}
				return;
			case EPhase::Settle:
				if (In.Frame >= PhaseFrame + Limits.SettleFrames)
				{
					Enter(EPhase::AwaitCapture, In, Out);
				}
				return;
			case EPhase::AwaitCapture:
				if (In.bCaptureTaken)
				{
					Enter(EPhase::Receive, In, Out);
				}
				else if (In.Now - PhaseStart > Limits.CaptureSeconds)
				{
					FailWith("the send did not reach its copy into the slot in time", Out);
				}
				return;
			case EPhase::Receive:
				if (In.bDelivered)
				{
					Out.push_back(EAction::Verify);
					if (++SizeIndex < static_cast<int>(Sizes.size()))
					{
						Enter(EPhase::Resize, In, Out);
					}
					else
					{
						Phase = EPhase::Done;
						Out.push_back(EAction::Finish);
					}
				}
				else if (In.Now - PhaseStart > Limits.ReceiveSeconds)
				{
					FailWith("the sent frame was not received in time", Out);
				}
				else
				{
					Out.push_back(EAction::Receive);
				}
				return;
			default:
				return;
			}
		}

		EPhase GetPhase() const { return Phase; }
		int GetSizeIndex() const { return SizeIndex; }
		FSize GetWanted() const { return SizeIndex < static_cast<int>(Sizes.size()) ? Sizes[SizeIndex] : FSize(); }
		int GetResizeRequests(int Index) const { return Index >= 0 && Index < static_cast<int>(ResizeRequests.size()) ? ResizeRequests[Index] : 0; }
		int GetSends(int Index) const { return Index >= 0 && Index < static_cast<int>(Sends.size()) ? Sends[Index] : 0; }
		const char* GetFailure() const { return Failure; }

	private:
		/** The only place entry actions are emitted: once per entry, in the entering tick. */
		void Enter(EPhase Next, const FInput& In, std::vector<EAction>& Out)
		{
			Phase = Next;
			PhaseStart = In.Now;
			PhaseFrame = In.Frame;
			if (Next == EPhase::Resize && In.Viewport != GetWanted())
			{
				++ResizeRequests[SizeIndex];
				Out.push_back(EAction::RequestResize);
			}
			else if (Next == EPhase::AwaitCapture)
			{
				++Sends[SizeIndex];
				Out.push_back(EAction::SendViewport);
			}
			else if (Next == EPhase::Receive)
			{
				Out.push_back(EAction::Receive);
			}
		}

		void FailWith(const char* Why, std::vector<EAction>& Out)
		{
			Failure = Why;
			Phase = EPhase::Done;
			Out.push_back(EAction::Fail);
		}

		std::vector<FSize> Sizes;
		FLimits Limits;
		std::vector<int> ResizeRequests;
		std::vector<int> Sends;
		EPhase Phase = EPhase::NotStarted;
		int SizeIndex = 0;
		double PhaseStart = 0.0;
		std::uint64_t PhaseFrame = 0;
		const char* Failure = "";
	};
}
