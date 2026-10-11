// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#include "SpoutLimits.h"
#include "HAL/IConsoleManager.h"
#include "SpoutValidation.h"

namespace
{
	TAutoConsoleVariable<int32> GSpoutLimitActive(TEXT("r.Spout.MaxActiveStreams"),32,TEXT("Maximum active sender/receiver roles combined. Close unused names before opening more."),ECVF_Default);
	TAutoConsoleVariable<int32> GSpoutLimitPending(TEXT("r.Spout.MaxPendingCommands"),128,TEXT("Maximum queued non-viewport Spout commands, including cancelled work awaiting drain."),ECVF_Default);
	TAutoConsoleVariable<int32> GSpoutLimitPerStream(TEXT("r.Spout.MaxPendingPerStream"),4,TEXT("Maximum queued commands per stream; excess requests return false."),ECVF_Default);
	TAutoConsoleVariable<int32> GSpoutLimitConsumers(TEXT("r.Spout.MaxConsumersPerReceiver"),32,TEXT("Maximum material bindings and optional output targets per receiver (each independently bounded)."),ECVF_Default);
	TAutoConsoleVariable<int32> GSpoutLimitDimension(TEXT("r.Spout.MaxTextureDimension"),8192,TEXT("Operational dimension limit, clamped to 1..16384."),ECVF_Default);
	TAutoConsoleVariable<int32> GSpoutLimitFrameMB(TEXT("r.Spout.MaxFrameMiB"),128,TEXT("Maximum uncompressed pixel bytes per frame in MiB."),ECVF_Default);
	TAutoConsoleVariable<int32> GSpoutLimitRetainedMB(TEXT("r.Spout.MaxRetainedMiB"),1024,TEXT("Aggregate conservative reservation budget for plugin-retained images. Caller/driver allocations are additional."),ECVF_Default);
	// Read on the render thread, so it must be render-thread safe (a shadow copy updated in order
	// with rendering commands) rather than read from the game thread's value.
	TAutoConsoleVariable<int32> GSpoutAccessWaitMs(TEXT("r.Spout.AccessWaitMs"),SpoutLimits::DefaultAccessWaitMs,
		TEXT("Longest wait in milliseconds for one attempt to take a shared texture's cross-process access lock while a peer holds it, clamped to 0..10. ")
		TEXT("0 never waits: a busy texture skips that transfer at once. The wait ends as soon as the peer releases the lock. ")
		TEXT("It applies to each attempt (one per stream transfer), not per frame, runs on the render thread, and Windows scheduling can make it last longer than requested."),
		ECVF_RenderThreadSafe);
	// Experimental, off by default, read on the render thread.
	TAutoConsoleVariable<float> GSpoutFrameWaitMs(TEXT("r.Spout.FrameWaitMs"), 0.0f,
		TEXT("Experimental. 0 (default): off. Otherwise the longest time in milliseconds, clamped to 0..4, that one receive call waits for the sender's next frame ")
		TEXT("when it found none and the sender's own publication history says the next one is due within that time. ")
		TEXT("Runs on the render thread before any texture access is taken, so it never holds the sender's access lock, and it keeps that thread busy while it waits."),
		ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> GSpoutFrameWaitBudgetMs(TEXT("r.Spout.FrameWaitBudgetMs"), SpoutLimits::DefaultFrameWaitBudgetMs,
		TEXT("Experimental. Total time in milliseconds, clamped to 0..4, that all receivers together may spend in the new-frame wait within one render frame."),
		ECVF_RenderThreadSafe);
	TAutoConsoleVariable<int32> GSpoutFrameWaitPollUs(TEXT("r.Spout.FrameWaitPollUs"), SpoutLimits::DefaultFrameWaitPollUs,
		TEXT("Experimental. Interval in microseconds, clamped to 50..1000, between frame-count polls inside a new-frame wait."),
		ECVF_RenderThreadSafe);
	TAutoConsoleVariable<int32> GSpoutFrameCountRecheck(TEXT("r.Spout.FrameCountRecheck"), 0,
		TEXT("Experimental. 1: the sender's frame notification is read again while the texture access lock is held, and a receive call that found nothing new still copies ")
		TEXT("when the sender published since the previous call. 0 (default): unchanged behaviour. Always on while r.Spout.FrameWaitMs is above 0."),
		ECVF_RenderThreadSafe);
	FCriticalSection GSpoutLimitBudgetMutex;
	int64 GSpoutLimitReserved = 0, GSpoutLimitPeak = 0, GSpoutLimitRejected = 0;
}

int32 SpoutLimits::MaxActiveStreams() { return FMath::Clamp(GSpoutLimitActive.GetValueOnAnyThread(),1,1024); }
int32 SpoutLimits::MaxPendingCommands() { return FMath::Clamp(GSpoutLimitPending.GetValueOnAnyThread(),1,4096); }
int32 SpoutLimits::MaxPendingPerStream() { return FMath::Clamp(GSpoutLimitPerStream.GetValueOnAnyThread(),1,64); }
int32 SpoutLimits::MaxConsumers() { return FMath::Clamp(GSpoutLimitConsumers.GetValueOnAnyThread(),1,1024); }
bool SpoutLimits::FrameBytes(int64 Width, int64 Height, EPixelFormat Format, int64& OutBytes)
{
	OutBytes = 0;
	if (SpoutValidation::ValidateDimensions(Width,Height) != SpoutValidation::EResult::Ok
		|| SpoutValidation::ValidatePixelFormat(Format) != SpoutValidation::EResult::Ok) return false;
	const int32 Limit = FMath::Clamp(GSpoutLimitDimension.GetValueOnAnyThread(),1,SpoutValidation::MaxTextureDimension);
	const int64 Bytes = Width * Height * GPixelFormats[Format].BlockBytes;
	if (Width > Limit || Height > Limit || Bytes > int64(FMath::Clamp(GSpoutLimitFrameMB.GetValueOnAnyThread(),1,2048))*1024*1024) return false;
	OutBytes = Bytes;
	return true;
}
uint32 SpoutLimits::AccessWaitMs() { return static_cast<uint32>(FMath::Clamp(GSpoutAccessWaitMs.GetValueOnAnyThread(),0,MaxAccessWaitMs)); }
// A value that is not a finite number resolves to off.
double SpoutLimits::FrameWaitSeconds() { const float V = GSpoutFrameWaitMs.GetValueOnAnyThread(); return FMath::IsFinite(V) ? double(FMath::Clamp(V,0.0f,MaxFrameWaitMs))/1000.0 : 0.0; }
double SpoutLimits::FrameWaitBudgetSeconds() { const float V = GSpoutFrameWaitBudgetMs.GetValueOnAnyThread(); return FMath::IsFinite(V) ? double(FMath::Clamp(V,0.0f,MaxFrameWaitMs))/1000.0 : 0.0; }
double SpoutLimits::FrameWaitPollSeconds() { return double(FMath::Clamp(GSpoutFrameWaitPollUs.GetValueOnAnyThread(),MinFrameWaitPollUs,MaxFrameWaitPollUs))/1000000.0; }
bool SpoutLimits::FrameCountRecheck() { return GSpoutFrameCountRecheck.GetValueOnAnyThread() != 0 || FrameWaitSeconds() > 0.0; }
int64 SpoutLimits::RetainedBytes() { FScopeLock Lock(&GSpoutLimitBudgetMutex); return GSpoutLimitReserved; }
int64 SpoutLimits::PeakRetainedBytes() { FScopeLock Lock(&GSpoutLimitBudgetMutex); return GSpoutLimitPeak; }
int64 SpoutLimits::RejectedAllocations() { FScopeLock Lock(&GSpoutLimitBudgetMutex); return GSpoutLimitRejected; }
TSharedPtr<FSpoutResourceReservation,ESPMode::ThreadSafe> FSpoutResourceReservation::TryReserve(int64 Bytes)
{
	FScopeLock Lock(&GSpoutLimitBudgetMutex);
	const int64 Limit = int64(FMath::Clamp(GSpoutLimitRetainedMB.GetValueOnAnyThread(),1,65536))*1024*1024;
	if (Bytes <= 0 || Bytes > Limit || GSpoutLimitReserved > Limit-Bytes) { ++GSpoutLimitRejected; return nullptr; }
	GSpoutLimitReserved += Bytes; GSpoutLimitPeak = FMath::Max(GSpoutLimitPeak,GSpoutLimitReserved);
	return MakeShareable(new FSpoutResourceReservation(Bytes));
}
FSpoutResourceReservation::~FSpoutResourceReservation()
{
	FScopeLock Lock(&GSpoutLimitBudgetMutex);
	GSpoutLimitReserved -= Bytes;
	check(GSpoutLimitReserved >= 0);
}
