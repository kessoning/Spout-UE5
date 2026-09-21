// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Implements the lifecycle and cancellation model.
 */
#include "SpoutSession.h"
#include "SpoutLimits.h"

const TCHAR* LexToString(ESpoutSessionState State)
{
	switch (State)
	{
	case ESpoutSessionState::Running:  return TEXT("Running");
	case ESpoutSessionState::Stopping: return TEXT("Stopping");
	default:                           return TEXT("Invalid");
	}
}

FSpoutSession& FSpoutSession::Get()
{
	static FSpoutSession Instance;
	return Instance;
}

ESpoutSessionState FSpoutSession::GetState() const
{
	FScopeLock Lock(&Mutex);
	return State;
}

bool FSpoutSession::IsRunning() const
{
	FScopeLock Lock(&Mutex);
	return State == ESpoutSessionState::Running;
}

uint64 FSpoutSession::GetSessionGeneration() const
{
	FScopeLock Lock(&Mutex);
	return SessionGeneration;
}

FSpoutStreamToken FSpoutSession::OpenStream(FName Name, ESpoutType Type)
{
	FScopeLock Lock(&Mutex);

	if (State != ESpoutSessionState::Running)
	{
		// Refusing here is what stops a Blueprint tick that races a PIE stop from queueing work
		// against a registry the teardown is about to clear.
		return FSpoutStreamToken();
	}

	// New incarnations receive globally unique generations. Removing a closed name cannot
	// make a token from its previous incarnation current when that name is opened again.
	const TPair<FName, ESpoutType> Key(Name, Type);
	uint64* Generation = StreamGenerations.Find(Key);
	if (!Generation)
	{
		if (StreamGenerations.Num() >= SpoutLimits::MaxActiveStreams()) { ++Rejected; return {}; }
		Generation = &StreamGenerations.Add(Key, NextStreamGeneration++);
		StreamStatus.Add(Key).bActive = true;
	}

	FSpoutStreamToken Token;
	Token.Session = SessionGeneration;
	Token.Stream = *Generation;
	Token.Name = Name;
	Token.Type = Type;
	return Token;
}

bool FSpoutSession::IsCurrent(const FSpoutStreamToken& Token) const
{
	if (!Token.IsSet())
	{
		return false;
	}

	FScopeLock Lock(&Mutex);

	if (State != ESpoutSessionState::Running || Token.Session != SessionGeneration)
	{
		return false;
	}

	const uint64* Generation = StreamGenerations.Find(TPair<FName, ESpoutType>(Token.Name, Token.Type));
	return Generation != nullptr && *Generation == Token.Stream;
}

bool FSpoutSession::ExecuteIfCurrent(const FSpoutStreamToken& Token, TFunctionRef<void()> Install)
{
	FScopeLock Lock(&Mutex);
	const uint64* Generation = StreamGenerations.Find(TPair<FName, ESpoutType>(Token.Name, Token.Type));
	if (State != ESpoutSessionState::Running || Token.Session != SessionGeneration
		|| !Generation || *Generation != Token.Stream)
	{
		return false;
	}
	Install();
	return true;
}

void FSpoutSession::CancelStream(FName Name, ESpoutType Type)
{
	FScopeLock Lock(&Mutex);

	StreamGenerations.Remove(TPair<FName, ESpoutType>(Name, Type));
	StreamStatus.Remove(TPair<FName, ESpoutType>(Name, Type));
}

void FSpoutSession::BeginStop()
{
	FScopeLock Lock(&Mutex);

	State = ESpoutSessionState::Stopping;

	// One increment invalidates every outstanding token for every stream. Work already sitting in
	// the render-command queue becomes a no-op from this instant, before anything is released.
	++SessionGeneration;
	StreamGenerations.Reset();
	StreamStatus.Reset();
}

void FSpoutSession::EndStop()
{
	FScopeLock Lock(&Mutex);

	// Safe to forget: every token that referenced these generations also referenced the previous
	// session generation, which BeginStop already left behind.
	StreamGenerations.Reset();
	StreamStatus.Reset();
	State = ESpoutSessionState::Running;
}

uint64 FSpoutSession::GetStreamGeneration(FName Name, ESpoutType Type) const
{
	FScopeLock Lock(&Mutex);
	const uint64* Generation = StreamGenerations.Find(TPair<FName, ESpoutType>(Name, Type));
	return Generation ? *Generation : 0;
}

int32 FSpoutSession::NumStreams() const
{
	FScopeLock Lock(&Mutex);
	return StreamGenerations.Num();
}

FSpoutSession::FQueuedWork::~FQueuedWork() { Owner->FinishQueued(Token); }

TSharedPtr<FSpoutSession::FQueuedWork, ESPMode::ThreadSafe> FSpoutSession::TryQueue(const FSpoutStreamToken& Token)
{
	FScopeLock Lock(&Mutex);
	const TPair<FName, ESpoutType> Key(Token.Name, Token.Type);
	const uint64* Generation = StreamGenerations.Find(Key);
	if (State != ESpoutSessionState::Running || Token.Session != SessionGeneration || !Generation || *Generation != Token.Stream) return nullptr;
	FSpoutStreamStatus& Status = StreamStatus.FindChecked(Key);
	const int32 Count = PendingByGeneration.FindRef(Token.Stream);
	if (Pending >= SpoutLimits::MaxPendingCommands() || Count >= SpoutLimits::MaxPendingPerStream())
	{
		++Rejected; ++Status.QueueRejections; return nullptr;
	}
	PendingByGeneration.Add(Token.Stream, Count + 1);
	++Pending; PeakPending = FMath::Max(PeakPending, Pending);
	return MakeShareable(new FQueuedWork{this, Token});
}

void FSpoutSession::MarkQueued(const FSpoutStreamToken& Token)
{
	FScopeLock Lock(&Mutex);
	const TPair<FName, ESpoutType> Key(Token.Name, Token.Type);
	if (Token.Session == SessionGeneration && StreamGenerations.FindRef(Key) == Token.Stream)
		++StreamStatus.FindChecked(Key).AcceptedRequests;
}

void FSpoutSession::FinishQueued(const FSpoutStreamToken& Token)
{
	FScopeLock Lock(&Mutex);
	int32& Count = PendingByGeneration.FindChecked(Token.Stream);
	if (--Count == 0) PendingByGeneration.Remove(Token.Stream);
	check(Pending > 0); --Pending;
}

FSpoutStreamStatus FSpoutSession::GetStreamStatus(FName Name, ESpoutType Type) const
{
	FScopeLock Lock(&Mutex);
	const TPair<FName, ESpoutType> Key(Name, Type);
	FSpoutStreamStatus Result = StreamStatus.FindRef(Key);
	Result.PendingCommands = PendingByGeneration.FindRef(StreamGenerations.FindRef(Key));
	return Result;
}

FSpoutRuntimeStatus FSpoutSession::GetRuntimeStatus() const
{
	FScopeLock Lock(&Mutex);
	FSpoutRuntimeStatus Result;
	Result.ActiveStreams = StreamGenerations.Num();
	Result.PendingCommands = Pending; Result.PeakPendingCommands = PeakPending; Result.RejectedRequests = Rejected;
	return Result;
}

void FSpoutPendingSenders::Add(const FSpoutStreamToken& Token)
{
	if (Token.Name.IsNone())
	{
		return;
	}

	FScopeLock Lock(&Mutex);
	Tokens.Add(Token.Name, Token);
}

bool FSpoutPendingSenders::Remove(FName Name)
{
	FScopeLock Lock(&Mutex);
	return Tokens.Remove(Name) > 0;
}

void FSpoutPendingSenders::Drain(TArray<FSpoutStreamToken>& OutTokens)
{
	FScopeLock Lock(&Mutex);

	OutTokens.Reset();
	OutTokens.Reserve(Tokens.Num());
	for (const TPair<FName, FSpoutStreamToken>& Pair : Tokens)
	{
		OutTokens.Add(Pair.Value);
	}
	Tokens.Reset();
}

void FSpoutPendingSenders::Reset()
{
	FScopeLock Lock(&Mutex);
	Tokens.Reset();
}

int32 FSpoutPendingSenders::Num() const
{
	FScopeLock Lock(&Mutex);
	return Tokens.Num();
}

bool FSpoutPendingSenders::Contains(FName Name) const
{
	FScopeLock Lock(&Mutex);
	return Tokens.Contains(Name);
}

namespace SpoutLifecycle
{
	const TCHAR* const OrderedStepNames[7] =
	{
		TEXT("EnterStoppingState"),
		TEXT("DetachProducers"),
		TEXT("ClearPendingWork"),
		TEXT("FlushRenderingWork"),
		TEXT("ReleaseNativeResources"),
		TEXT("ReleaseManagedObjects"),
		TEXT("LeaveStoppingState"),
	};

	void RunOrderedShutdown(ISpoutLifecycleSteps& Steps)
	{
		// The order matters. Each step is only safe because the ones above it have already run:
		//
		//   Stopping before detaching, so a producer that fires during the detach is already
		//   producing cancelled work.
		//
		//   Detaching before flushing. Flushing first lets the render thread pass the flush marker
		//   and then invoke a delegate that enqueues more module-owned work behind it, so the
		//   flush would guarantee nothing.
		//
		//   Flushing before releasing native resources, so no render command is still holding a
		//   shared texture, a staging texture or a D3D11-on-12 wrap when it is destroyed.
		//
		//   Releasing managed UObject ownership last, so the textures outlive every render command
		//   that captured their RHI resources and are then reclaimed by ordinary GC.
		Steps.EnterStoppingState();
		Steps.DetachProducers();
		Steps.ClearPendingWork();
		Steps.FlushRenderingWork();
		Steps.ReleaseNativeResources();
		Steps.ReleaseManagedObjects();
		Steps.LeaveStoppingState();
	}
}
