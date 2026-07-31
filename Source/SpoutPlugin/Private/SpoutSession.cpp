/**
 * Implements the lifecycle and cancellation model.
 */
#include "SpoutSession.h"

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

	// Per-stream generations start at 1, so a token can never carry a zero Stream while its Session
	// is set. Every later Cancel bumps this value and strands the tokens that captured the old one.
	uint64& Generation = StreamGenerations.FindOrAdd(TPair<FName, ESpoutType>(Name, Type), 1);

	FSpoutStreamToken Token;
	Token.Session = SessionGeneration;
	Token.Stream = Generation;
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

void FSpoutSession::CancelStream(FName Name, ESpoutType Type)
{
	FScopeLock Lock(&Mutex);

	// FindOrAdd rather than Find: cancelling a stream that has no entry yet still has to leave a
	// generation behind, otherwise a token issued a moment earlier on another thread would compare
	// equal against a freshly created entry and survive the cancel.
	uint64& Generation = StreamGenerations.FindOrAdd(TPair<FName, ESpoutType>(Name, Type), 1);
	++Generation;
}

void FSpoutSession::BeginStop()
{
	FScopeLock Lock(&Mutex);

	State = ESpoutSessionState::Stopping;

	// One increment invalidates every outstanding token for every stream. Work already sitting in
	// the render-command queue becomes a no-op from this instant, before anything is released.
	++SessionGeneration;
}

void FSpoutSession::EndStop()
{
	FScopeLock Lock(&Mutex);

	// Safe to forget: every token that referenced these generations also referenced the previous
	// session generation, which BeginStop already left behind.
	StreamGenerations.Reset();
	State = ESpoutSessionState::Running;
}

uint64 FSpoutSession::GetStreamGeneration(FName Name, ESpoutType Type) const
{
	FScopeLock Lock(&Mutex);
	const uint64* Generation = StreamGenerations.Find(TPair<FName, ESpoutType>(Name, Type));
	return Generation ? *Generation : 0;
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
