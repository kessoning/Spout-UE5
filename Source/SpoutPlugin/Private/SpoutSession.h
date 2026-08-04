// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Declares the plugin's lifecycle and cancellation model.
 *
 * Work queued for the render thread has to be cancellable by a close or a teardown that happens
 * after the work was queued but before it runs. A name alone cannot express that: a render
 * command that captures only a name has no way to tell, once it runs, whether the stream it was
 * queued for is still the same stream, or still open at all.
 *
 * The model here is two monotonic counters and one state:
 *
 *   - A SESSION generation, bumped whenever a global teardown begins. Bumping it invalidates every
 *     token that any stream ever issued, which is what makes "no late work after teardown" a
 *     property of the data rather than a property of the ordering.
 *   - A per-stream generation, bumped when one stream is closed individually. This is what lets
 *     CloseSender('A') cancel A's queued send without disturbing B.
 *   - A Running/Stopping state, so work cannot be queued INTO a teardown that is already underway.
 *
 * A caller takes a token before queueing, and the queued work asks IsCurrent() before doing
 * anything observable. A token is a value: it is safe to copy into a render-command lambda and it
 * keeps nothing alive.
 */
#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "SpoutBPFunctionLibrary.h"

/** Whether the plugin is accepting new work, or draining towards a teardown. */
enum class ESpoutSessionState : uint8
{
	Running,
	Stopping
};

const TCHAR* LexToString(ESpoutSessionState State);

/**
 * Identifies one unit of queued work for one stream, at one point in the session's life.
 *
 * Captured on the game thread when work is queued and validated on the render thread before the
 * work does anything. A default-constructed token is deliberately never current, so "the session
 * refused to issue a token" and "the token went stale" collapse into the same check at the use site.
 */
struct FSpoutStreamToken
{
	/** Session generation at issue time. Zero means no token was issued. */
	uint64 Session = 0;

	/** Stream generation at issue time. */
	uint64 Stream = 0;

	FName Name;

	ESpoutType Type = ESpoutType::Sender;

	bool IsSet() const { return Session != 0; }
};

/**
 * Process-wide lifecycle and cancellation authority.
 *
 * Every method is safe to call from any thread; the whole object is guarded by one mutex, and the
 * operations are short map lookups. IsCurrent() is on the render thread's per-frame path, so it
 * does one lookup and nothing else.
 */
class FSpoutSession
{
public:
	/**
	 * The process-wide session. Everything in the plugin uses this one.
	 *
	 * The class is not a singleton by construction, deliberately: an instance is cheap and
	 * self-contained, so a separate local instance can apply the cancellation rules without
	 * touching global state that a live stream in the same process depends on.
	 */
	static FSpoutSession& Get();

	FSpoutSession() = default;

	ESpoutSessionState GetState() const;
	bool IsRunning() const;
	uint64 GetSessionGeneration() const;

	/**
	 * Issues a token for work that is about to be queued.
	 *
	 * Returns an unset token while the session is stopping, which is how a Send or Receive issued
	 * during teardown is refused rather than queued into a registry that is about to be cleared.
	 */
	FSpoutStreamToken OpenStream(FName Name, ESpoutType Type);

	/** True when the token still describes live, uncancelled work in the current session. */
	bool IsCurrent(const FSpoutStreamToken& Token) const;

	/**
	 * Cancels outstanding work for one stream.
	 *
	 * Bumps only that stream's generation, so other streams -- including a stream with the same
	 * FName in the other role -- are untouched. A later OpenStream for the same name gets a fresh
	 * token and works normally, so a close/reopen cycle is not a permanent kill.
	 */
	void CancelStream(FName Name, ESpoutType Type);

	/** Enters Stopping and bumps the session generation, staling every outstanding token at once. */
	void BeginStop();

	/** Returns to Running and forgets per-stream generations, which the session bump already staled. */
	void EndStop();

	/** Current generation for one stream, or 0 if it has never been opened. Diagnostics and tests. */
	uint64 GetStreamGeneration(FName Name, ESpoutType Type) const;

private:
	mutable FCriticalSection Mutex;
	ESpoutSessionState State = ESpoutSessionState::Running;

	// Starts at 1 so that 0 unambiguously means "no token".
	uint64 SessionGeneration = 1;

	TMap<TPair<FName, ESpoutType>, uint64> StreamGenerations;
};

/**
 * Names to publish on the next viewport back-buffer present, with the token each was queued under.
 *
 * Held as tokens rather than bare names: a name that is closed between the Blueprint call and
 * the present must not be published, and the only thing that can express that is the token.
 * Removal by name is what makes a single queued sender closable without disturbing the others.
 *
 * Self-synchronising: the game thread adds and removes, the render thread drains.
 */
class FSpoutPendingSenders
{
public:
	/** Queues (or re-queues) a name. A second Add for the same name replaces the earlier token. */
	void Add(const FSpoutStreamToken& Token);

	/** Drops one name. Returns whether it was queued. */
	bool Remove(FName Name);

	/** Moves every queued token out, leaving the set empty. */
	void Drain(TArray<FSpoutStreamToken>& OutTokens);

	void Reset();

	int32 Num() const;
	bool Contains(FName Name) const;

private:
	mutable FCriticalSection Mutex;
	TMap<FName, FSpoutStreamToken> Tokens;
};

/**
 * The ordered teardown, expressed as an interface so the order is explicit and checkable.
 *
 * Teardown correctness here is entirely a question of sequence. Flushing rendering commands
 * while the back-buffer delegate is still subscribed guarantees nothing: the render thread can
 * pass the flush marker and then invoke the delegate, which enqueues more module-owned work
 * behind it. Writing the sequence as calls through this interface means an implementation can
 * record what actually happened and compare it against the required order, instead of the
 * order being an unenforced comment.
 */
class ISpoutLifecycleSteps
{
public:
	virtual ~ISpoutLifecycleSteps() = default;

	/** 1. Stop accepting new work. */
	virtual void EnterStoppingState() = 0;

	/** 2. Unsubscribe producers and delegates, and wait for any in-flight callback to return. */
	virtual void DetachProducers() = 0;

	/** 3. Drop work that was queued but has not been picked up. */
	virtual void ClearPendingWork() = 0;

	/** 4. Drain the render thread. Everything queued before this point has run or been cancelled. */
	virtual void FlushRenderingWork() = 0;

	/** 5. Release the native registry and D3D resources, now that nothing can be using them. */
	virtual void ReleaseNativeResources() = 0;

	/** 6. Drop managed UObject ownership last, so normal GC reclaims the objects. */
	virtual void ReleaseManagedObjects() = 0;

	/** 7. Allow work again. */
	virtual void LeaveStoppingState() = 0;
};

namespace SpoutLifecycle
{
	/** Runs the seven steps in the one order that is safe. The only definition of that order. */
	void RunOrderedShutdown(ISpoutLifecycleSteps& Steps);

	/** Step names in required order, for diagnostics and for the ordering test. */
	extern const TCHAR* const OrderedStepNames[7];
}
