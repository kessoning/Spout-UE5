/**
 * Reproducible performance baselines for the plugin's send and receive paths.
 *
 * Each scenario runs inside one packaged host process, warms up, then measures for a fixed
 * duration and writes a JSON result next to the smoke markers. Nothing is sampled from a profiler
 * the harness cannot read back deterministically: the per-frame numbers come from the engine's own
 * game/render/GPU timers, and the aggregates (mean, p95, p99, peak) are computed here so the same
 * run produces the same file on every engine in the matrix.
 *
 * The scenarios that need a peer produce their own. A second in-process sender feeds every
 * receiver scenario, which covers the "sender and receiver in one Unreal process" case and is
 * what makes these baselines runnable unattended instead of pending on an external application.
 */
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/StrongObjectPtr.h"

class UMaterialInstanceDynamic;
class UTexture2D;
class UTextureRenderTarget2D;

namespace SpoutHostTest
{
	enum class EPerfScenario : uint8
	{
		None,
		Sender1080p,
		Receiver1080p,
		Sender4K,
		Receiver4K,
		MultiStream,
		CpuFallback,
		IdleReceiver,
		ResizeChurn,
		StartupShutdown
	};

	class FPerfHarness
	{
	public:
		/** True when -SpoutPerf=<scenario> names a scenario this build knows. */
		static bool IsRequested();

		/** The requested scenario, or None. Also used to report an unknown name as a failure. */
		static EPerfScenario GetRequestedScenario();

		static void Start();
		static void Stop();

		static bool HasPassed();
		static bool WasStarted();

	private:
		struct FFrameSample
		{
			double TotalMs = 0.0;
			double GameThreadMs = 0.0;
			double RenderThreadMs = 0.0;
			double GpuMs = -1.0;
		};

		bool Tick(float DeltaSeconds);

		/** Builds the render targets and opens whatever streams the scenario needs. */
		bool SetUpScenario();

		/** One frame of the scenario's own work: publish, receive, resize, churn. */
		void DriveScenario(int32 FrameIndex);

		void RecordSample(float DeltaSeconds);
		void WriteResults();
		void FailAndFinish(const FString& Reason);

		/** Creates a render target of the scenario's resolution, or null on rejection. */
		UTextureRenderTarget2D* MakeRenderTarget(int32 Width, int32 Height) const;

		static FPerfHarness* Instance;

		FTSTicker::FDelegateHandle TickerHandle;

		EPerfScenario Scenario = EPerfScenario::None;
		FString ScenarioName;

		int32 Width = 1920;
		int32 Height = 1080;
		int32 StreamCount = 1;
		bool bUsesSender = false;
		bool bUsesReceiver = false;
		bool bGpuReceiverPath = true;

		double WarmupSeconds = 5.0;
		double MeasureSeconds = 60.0;
		double StartTimeSeconds = 0.0;
		double MeasureStartTimeSeconds = 0.0;
		bool bMeasuring = false;
		bool bFinished = false;
		bool bPassed = false;
		FString FailureReason;

		int32 FrameIndex = 0;
		int32 MeasuredFrames = 0;

		/** Every receiver scenario must genuinely receive at least once, or the run is not a baseline. */
		int32 AcceptedReceives = 0;
		int32 RejectedReceives = 0;
		int32 AcceptedSends = 0;
		int32 RejectedSends = 0;
		int32 ResizeEvents = 0;
		int32 StreamRestartEvents = 0;

		TArray<FFrameSample> Samples;

		double CpuPercentAccumulator = 0.0;
		int32 CpuPercentSamples = 0;
		uint64 PeakWorkingSetBytes = 0;

		TArray<TStrongObjectPtr<UTextureRenderTarget2D>> SourceRenderTargets;
		TArray<TStrongObjectPtr<UTexture2D>> ReceivedTextures;
		TArray<TStrongObjectPtr<UMaterialInstanceDynamic>> ReceivedMaterials;
		TArray<FName> SenderNames;
	};
}
