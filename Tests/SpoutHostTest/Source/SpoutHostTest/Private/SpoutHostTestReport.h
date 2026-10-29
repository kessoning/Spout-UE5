// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * The machine-readable result channel for the packaged-host smoke and performance runs.
 *
 * Why this exists at all: a Shipping game is built with NO_LOGGING, so UE_LOG compiles to nothing
 * and log scraping - the obvious way to read a headless run - reports exactly the same "no markers
 * found" for a passing Shipping host as for one that crashed on the first frame. Every marker is
 * therefore written to a file the harness owns, flushed on every line, and mirrored to the log only
 * where the log still exists.
 *
 * Three channels, in decreasing order of authority:
 *   1. <ResultDir>/markers.log  - one marker per line, appended and flushed immediately, so a run
 *                                 killed by the watchdog still shows exactly how far it got.
 *   2. <ResultDir>/result.json  - the structured verdict, written once during shutdown.
 *   3. the process exit code    - 0 pass, non-zero fail.
 *
 * The scripts read all three and require them to agree. Nothing infers a pass from the process
 * merely having started.
 */
#pragma once

#include "CoreMinimal.h"

namespace SpoutHostTest
{
	/**
	 * Marker vocabulary.
	 *
	 * Fixed strings rather than formatted text: the scripts match them exactly, and a marker that
	 * drifts by one character would silently stop being found. The nine pass/fail markers come
	 * first; the rest are supporting evidence (resolved DLL path, per-phase counts) and are
	 * prefixed the same way so one grep finds everything.
	 */
	namespace Markers
	{
		inline const TCHAR* SmokeBegin           = TEXT("SPOUT_SMOKE_BEGIN");
		inline const TCHAR* PluginModuleLoaded   = TEXT("SPOUT_PLUGIN_MODULE_LOADED");
		inline const TCHAR* DllLoaded            = TEXT("SPOUT_DLL_LOADED");
		inline const TCHAR* SenderCreatePass     = TEXT("SPOUT_SENDER_CREATE_PASS");
		inline const TCHAR* ReceiverCreatePass   = TEXT("SPOUT_RECEIVER_CREATE_PASS");
		inline const TCHAR* LevelTravelPass      = TEXT("SPOUT_LEVEL_TRAVEL_PASS");
		inline const TCHAR* ShutdownBegin        = TEXT("SPOUT_SHUTDOWN_BEGIN");
		inline const TCHAR* SmokePass            = TEXT("SPOUT_SMOKE_PASS");
		inline const TCHAR* SmokeFailPrefix      = TEXT("SPOUT_SMOKE_FAIL:");

		inline const TCHAR* ApiInitPass          = TEXT("SPOUT_PLUGIN_API_INIT_PASS");
		inline const TCHAR* SameNameRestartPass  = TEXT("SPOUT_SAME_NAME_RESTART_PASS");
		inline const TCHAR* NoStaleSessionPass   = TEXT("SPOUT_NO_STALE_SESSION_PASS");
		inline const TCHAR* ColorDecodePass      = TEXT("SPOUT_COLOR_DECODE_PASS");
		inline const TCHAR* ShutdownComplete     = TEXT("SPOUT_SHUTDOWN_COMPLETE");
		inline const TCHAR* PerfBegin            = TEXT("SPOUT_PERF_BEGIN");
		inline const TCHAR* PerfPass             = TEXT("SPOUT_PERF_PASS");
		inline const TCHAR* PerfFailPrefix       = TEXT("SPOUT_PERF_FAIL:");

		// GameViewportSenderSmoke. Every pass marker below is backed by an EXTERNAL
		// observation through FViewportSenderProbe (shared registry, opened shared texture,
		// frame-count semaphore), never by the plugin's own return values alone.
		inline const TCHAR* ViewportBegin           = TEXT("SPOUT_VIEWPORT_BEGIN");
		inline const TCHAR* ViewportPathValue       = TEXT("SPOUT_VIEWPORT_PATH");
		inline const TCHAR* ViewportDelegateThreadValue = TEXT("SPOUT_VIEWPORT_DELEGATE_THREAD");
		inline const TCHAR* ViewportRestartPass     = TEXT("SPOUT_VIEWPORT_RESTART_PASS");
		inline const TCHAR* ViewportAdvertisedPass  = TEXT("SPOUT_VIEWPORT_ADVERTISED_PASS");
		inline const TCHAR* ViewportTextureOpenPass = TEXT("SPOUT_VIEWPORT_TEXTURE_OPEN_PASS");
		inline const TCHAR* ViewportFramesPass      = TEXT("SPOUT_VIEWPORT_FRAMES_PASS");
		inline const TCHAR* ViewportResizePass      = TEXT("SPOUT_VIEWPORT_RESIZE_PASS");
		inline const TCHAR* ViewportClosePass       = TEXT("SPOUT_VIEWPORT_CLOSE_PASS");
		inline const TCHAR* ViewportSenderPass      = TEXT("SPOUT_VIEWPORT_SENDER_PASS");
	}

	/**
	 * Process-wide reporter. Created by the module on startup, torn down on module shutdown.
	 *
	 * Thread affinity: game thread only. Everything that writes markers runs from the ticker or from
	 * module startup/shutdown, all of which are game thread.
	 */
	class FReport
	{
	public:
		/**
		 * Resolves the output directory and opens markers.log.
		 *
		 * Directory precedence: -SpoutResultDir=<path> on the command line, then
		 * ProjectSavedDir()/SpoutHostTest. The scripts always pass the switch, so evidence lands in
		 * the run's own evidence tree rather than inside the packaged host where a later row would
		 * overwrite it.
		 */
		static void Initialize();
		static void Shutdown();

		/** Null before Initialize and after Shutdown; callers must check. */
		static FReport* Get() { return Instance; }

		/** Appends one marker line and flushes. Also UE_LOGs it where logging exists. */
		void Mark(const FString& Marker);

		/** Mark("NAME:value"), for the markers that carry a payload. */
		void MarkValue(const TCHAR* Marker, const FString& Value);

		/** Records a key/value pair for result.json without emitting a marker line. */
		void SetField(const FString& Key, const FString& Value);

		/**
		 * Records the first failure and nothing after it.
		 *
		 * First rather than last deliberately: later failures are usually consequences of the first,
		 * and the reason string is what a human reads before anything else.
		 */
		void Fail(const FString& Reason);

		bool HasFailed() const { return bFailed; }
		const FString& GetFailureReason() const { return FailureReason; }

		/** The resolved evidence directory, so the perf harness writes alongside the markers. */
		const FString& GetResultDir() const { return ResultDir; }

		/**
		 * Writes result.json.
		 *
		 * Safe to call more than once (the shutdown path and the watchdog path both do); the last
		 * call wins, and the file is written atomically enough for a reader that only opens it after
		 * the process exits.
		 */
		void WriteResultJson(const FString& RunKind, bool bPassed);

	private:
		static FReport* Instance;

		FString ResultDir;
		FString MarkerFilePath;
		FString ResultFilePath;

		TArray<TPair<FString, FString>> Fields;
		TArray<FString> EmittedMarkers;

		bool bFailed = false;
		FString FailureReason;

		void AppendMarkerLine(const FString& Line);
	};

	/** JSON string escaping. The harness writes JSON by hand to avoid a Json module dependency. */
	FString EscapeJson(const FString& In);
}
