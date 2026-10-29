// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

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
 *   - the GameViewportSenderSmoke scenario: a sender fed from the REAL game viewport's back
 *     buffer (ESpoutSendTextureFrom::GameViewport) advertises externally, its shared texture
 *     opens on an independent D3D11 device, its cross-process frame count advances over multiple
 *     real presents, it survives one controlled resolution change, and it disappears after close
 *     - all observed through FViewportSenderProbe rather than through the plugin's own state;
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
			ColorDecode,
			ViewportSenderStart,
			ViewportSenderAdvertise,
			ViewportSenderFrames,
			ViewportSenderResize,
			ViewportSenderResize2,
			ViewportSenderClose,
			ViewportAttachDetachStress,
			ViewportPostCloseResize,
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

		/**
		 * Color Space sRGB on a 10-bit sender: a 10-bit render target holding every code in each channel and
		 * all four alpha codes is sent and received with the default receive settings and Color Space sRGB; the received
		 * texture must be 16-bit float and hold the exact sRGB decode of every texel, alpha unchanged. In a packaged build
		 * this also proves the decode shader was cooked into the global shader map.
		 */
		bool StepColorDecode();

		/**
		 * GameViewportSenderSmoke.
		 *
		 * The source is explicitly ESpoutSendTextureFrom::GameViewport: publication happens inside
		 * the plugin's back-buffer present callback, not from any render target this harness owns.
		 * Because one queued request publishes at most one present, the frames/resize steps
		 * re-queue the send every tick, exactly as a real per-tick Blueprint sender does.
		 *
		 * Evidence is deliberately external (FViewportSenderProbe): a sender that never really
		 * publishes cannot advertise in the shared registry, cannot be opened from its share
		 * handle on an unrelated device, and cannot move its cross-process frame semaphore.
		 */
		bool StepViewportSenderStart();
		bool StepViewportSenderAdvertise();
		bool StepViewportSenderFrames();
		bool StepViewportSenderResize();
		bool StepViewportSenderResize2();
		bool StepViewportSenderClose();

		/**
		 * Attach/detach lifecycle stress.
		 *
		 * Each cycle drives the producer through a FULL detach and re-attach: CloseAllStreams runs
		 * the ordered shutdown (which removes the back-buffer delegate — on UE 5.3 from the
		 * rendering thread, with a completion wait), and the next GameViewport send re-registers
		 * it. Repeating that proves the bounded waits on both the registration and removal paths
		 * complete every time instead of deadlocking or timing out, and that a re-attached
		 * producer still publishes.
		 */
		bool StepViewportAttachDetachStress();

		bool StepViewportPostCloseResize();

		bool StepShutdownWithActivity();

		/** Queues one GameViewport publish; fails the run (and returns false) on rejection. */
		bool QueueViewportSend();

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
		// Regression probe: a clear issued in the frame the factory creates a target must survive.
		TStrongObjectPtr<UTextureRenderTarget2D> FactoryProbe;
		int32 FactoryProbeFrame = 0;
		bool bFactoryProbeChecked = false;
		TStrongObjectPtr<UTexture2D> ReceivedTexture;
		TStrongObjectPtr<UMaterialInstanceDynamic> ReceivedMaterial;

		FName LifecycleSenderName;
		FName ColorDecodeSenderName;
		TStrongObjectPtr<UTextureRenderTarget2D> ColorDecodeSource;
		TStrongObjectPtr<UTexture2D> ColorDecodeTexture;
		TStrongObjectPtr<UMaterialInstanceDynamic> ColorDecodeMaterial;
		FName ShutdownSenderName;
		FName WarmupSenderName;

		// --- GameViewportSenderSmoke state -------------------------------------------------------

		/** Unique per process, like the other sender names, so stale senders cannot collide. */
		FName ViewportSenderName;

		/** The external observer. Created in ViewportSenderStart, closed in ViewportSenderClose. */
		TUniquePtr<class FViewportSenderProbe> ViewportProbe;

		// External observations. Every one comes from the probe, none from the plugin.
		int32 ViewportObservedAdvances = 0;
		long ViewportInitialSenderFrame = 0;
		long ViewportLastSenderFrame = 0;
		double ViewportFirstAdvanceSeconds = 0.0;
		double ViewportLastAdvanceSeconds = 0.0;
		int32 ViewportResizeObservedAdvances = 0;
		bool bViewportResizeDimensionsSeen = false;
		int32 ViewportResize2ObservedAdvances = 0;
		bool bViewportResize2DimensionsSeen = false;
		int32 CompletedViewportRestartCycles = 0;
		double ViewportRestartWorstCycleSeconds = 0.0;
		double ViewportRestartCycleStartSeconds = 0.0;
	};
}
