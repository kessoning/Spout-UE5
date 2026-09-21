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
