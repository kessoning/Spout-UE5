/**
 * The unattended lifecycle and level-travel smoke sequence.
 *
 * A frame-driven state machine rather than a blocking script, because everything it is testing is
 * asynchronous: the plugin queues sender registration and receiver copies onto the render thread,
 * and level travel takes several frames to tear down and rebuild a world. A blocking test would
 * either deadlock the render thread it is waiting on or, worse, "pass" without any of that work
 * having run.
 *
 * The sequence proves, in order:
 *   - the plugin module is loaded;
 *   - Spout.dll is mapped into the process, from the path the packaged layout intends;
 *   - the plugin API initialises and answers queries;
 *   - a sender can be created and closed repeatedly (default 10 cycles);
 *   - a sender genuinely REGISTERS with Spout - proven by reading it back out of the sender list,
 *     not by the queueing call returning true;
 *   - the same name can be closed and immediately restarted;
 *   - a receiver can be created and closed repeatedly against that live sender (default 10 cycles),
 *     which covers the sender-and-receiver-in-one-process case;
 *   - the level can be travelled away from and back (default 5 cycles) with a stream open;
 *   - closing leaves no stale session advertised;
 *   - the process shuts down cleanly with a sender still publishing.
 *
 * Every step that cannot be genuinely performed fails the run. Nothing is skipped into a pass.
 */
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/StrongObjectPtr.h"

class UMaterialInstanceDynamic;
class UTexture2D;
class UTextureRenderTarget2D;
class UWorld;

namespace SpoutHostTest
{
	class FSmokeRunner
	{
	public:
		/** True when -SpoutSmoke is on the command line. */
		static bool IsRequested();

		/** Registers the frame ticker. No-op unless IsRequested(). */
		static void Start();

		/** Unregisters the ticker and drops object references. Safe when never started. */
		static void Stop();

		/** Whether the sequence reached its end without a failure. Read during module shutdown. */
		static bool HasPassed();

		/** Whether a sequence was started at all, so shutdown can tell "failed" from "not run". */
		static bool WasStarted();

	private:
		enum class EStep : uint8
		{
			WaitForWorld,
			Begin,
			CheckPluginModule,
			CheckRuntimeDll,
			CheckApiInit,
			PrepareSource,
			SenderCycles,
			SenderRegistration,
			SameNameRestart,
			ReceiverCycles,
			LevelTravel,
			StaleSessionCheck,
			ShutdownWithActivity,
			Finished
		};

		bool Tick(float DeltaSeconds);

		// One handler per step. Each returns true when the step is complete and the machine should
		// advance; false means "still working, call me again next frame".
		bool StepWaitForWorld();
		bool StepBegin();
		bool StepCheckPluginModule();
		bool StepCheckRuntimeDll();
		bool StepCheckApiInit();

		/**
		 * Waits until a render target created this frame is actually publishable.
		 *
		 * A UTextureRenderTarget2D's RHI resource is created on the render thread, so on the frame
		 * it is constructed GetRenderTargetTexture() is still null and the plugin correctly refuses
		 * to publish it. That is one frame of engine warm-up, not a defect - but a cycle test that
		 * asserted on its first tick would blame the plugin for it. This step absorbs the warm-up so
		 * every later step can treat a rejection as a genuine failure.
		 */
		bool StepPrepareSource();

		bool StepSenderCycles();
		bool StepSenderRegistration();
		bool StepSameNameRestart();
		bool StepReceiverCycles();
		bool StepLevelTravel();
		bool StepStaleSessionCheck();
		bool StepShutdownWithActivity();

		void Advance(EStep Next);
		void FailAndFinish(const FString& Reason);

		/** The Game world, or null while one is being created or travelled to. */
		static UWorld* GetGameWorld();

		/** Short map name of the current Game world, or empty. */
		static FString GetCurrentMapShortName();

		/** Creates and holds the render target every sender in this run publishes. */
		bool EnsureSourceRenderTarget(int32 Width, int32 Height);

		/** Opens a sender on Name and returns whether the request was accepted. */
		bool OpenSender(FName Name);

		/** True when Name is visible in Spout's own sender list. */
		static bool IsSenderAdvertised(FName Name);

		static FSmokeRunner* Instance;

		FTSTicker::FDelegateHandle TickerHandle;

		EStep Step = EStep::WaitForWorld;
		bool bFailed = false;
		bool bPassed = false;

		// Frames spent inside the current step, used for the per-step budgets.
		int32 StepFrames = 0;

		// Wall-clock deadline for the whole run. The scripts also impose an external timeout; this
		// one exists so the host writes a diagnosable SPOUT_SMOKE_FAIL:timeout marker instead of
		// being killed with no explanation.
		double DeadlineSeconds = 0.0;
		double StartTimeSeconds = 0.0;

		// Configuration, from the command line.
		int32 SenderCycleCount = 10;
		int32 ReceiverCycleCount = 10;
		int32 TravelCycleCount = 5;
		FString MapA;
		FString MapB;

		// Per-step counters.
		int32 CompletedSenderCycles = 0;
		int32 CompletedReceiverCycles = 0;
		int32 CompletedTravelCycles = 0;
		int32 TravelPhase = 0;

		// Live objects. Strong pointers because CreateTextureRenderTarget2D deliberately does not
		// root its result and the received texture belongs to the plugin only while the receiver is
		// open - holding them here is what keeps a collection between ticks from being mistaken for
		// a plugin defect.
		TStrongObjectPtr<UTextureRenderTarget2D> SourceRenderTarget;
		TStrongObjectPtr<UTexture2D> ReceivedTexture;
		TStrongObjectPtr<UMaterialInstanceDynamic> ReceivedMaterial;

		FName LifecycleSenderName;
		FName ShutdownSenderName;
		FName WarmupSenderName;
	};
}
