// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Test support: a frame-sync backend that reproduces the count arithmetic of the bundled Spout.dll,
 * so the automation tests exercise the plugin against the SDK's real semantics instead of against
 * a counter that happens to agree with itself.
 *
 *   EnableFrameCount   the semaphore is created with a count of 1 (or opened, when it exists); the
 *                      object's own frame count and last-seen count start again at 0.
 *   SetNewFrame        the semaphore count goes up by one.
 *   GetNewFrame        waits on the semaphore without a timeout and releases it again; the release
 *                      reports the count after the wait's decrement, which becomes the frame count.
 *                      A frame count of 0 answers "new"; one equal to the last one seen answers
 *                      "not new"; otherwise it becomes the last one seen and the answer is "new".
 *                      Without frame counting, or without a semaphore, the answer is "new" and
 *                      nothing is read.
 *   GetSenderFrame     the frame count the last GetNewFrame stored.
 *
 * So the frame count is the semaphore count minus one. The model keeps the raw semaphore count and
 * derives everything from it the way the DLL does. Only the SDK's public calls are modelled: the
 * plugin uses nothing else.
 *
 * What the model cannot show is that the real DLL behaves like this. Tests/Native/
 * SpoutNotificationTests.cpp and SpoutPlugin.Notification.BundledSdkAgreesWithTheModel do that.
 */
#pragma once

#include "CoreMinimal.h"
#include "SpoutFrameSync.h"

#if WITH_DEV_AUTOMATION_TESTS

/** One named frame-count semaphore as the bundled SDK creates it: raw count 1, plus one per publication. */
struct FSpoutSemaphoreModel
{
	int64 RawCount = 1;
};

class FSpoutSdkCountModelBackend final : public ISpoutFrameSyncBackend
{
public:
	/** Null models "no semaphore": the SDK then reports every read as new. */
	TSharedPtr<FSpoutSemaphoreModel> Semaphore = MakeShared<FSpoutSemaphoreModel>();
	/** False models frame counting switched off (the registry setting or the application). */
	bool bCountingEnabled = true;
	/** The SDK object's own fields. */
	int64 FrameCount = 0;     // m_FrameCount
	int64 LastFrameCount = 0; // m_LastFrameCount
	/** Another process is between its wait and its release during the NEXT read: that read sees the raw count this much lower. One-shot. */
	int32 ForeignDecrementAtNextRead = 0;
	/** The same, for this many reads in a row (another receiver descheduled between its wait and its release). */
	int32 ForeignDecrementForReads = 0;
	/** The same, at the read whose number (NewFrameQueries after it) equals this. One-shot. */
	int32 ForeignDecrementAtQuery = -1;
	/** This many publications are made when texture access is next attempted (published between a read and its access). One-shot. */
	int32 PublishAtNextAccess = 0;
	/** A publication happens just before the read whose number (NewFrameQueries after it) reaches this. */
	int32 PublishAtQuery = -1;
	/**
	 * Publications at given times on a test clock. Before every read of the count and every access
	 * attempt, every scheduled publication whose time has come is made. Sorted. Without a clock
	 * nothing is scheduled.
	 */
	TFunction<double()> ScheduleClock;
	TArray<double> ScheduledPublications;
	int32 NextScheduled = 0;
	/** The next this many access attempts fail (a peer holds the texture). */
	int32 FailAccessAttempts = 0;

	int32 NewFrameQueries = 0;
	int32 AccessAttempts = 0;
	int32 AccessReleases = 0;

	/** What a sender's SetNewFrame does to the semaphore. */
	void Publish(int32 Times = 1) { if (Semaphore) Semaphore->RawCount += Times; }
	/** Sets the semaphore as if this many publications had happened since it was created. */
	void SetPublications(int64 Publications) { if (Semaphore) Semaphore->RawCount = Publications + 1; }
	/** The true frame count right now, for assertions: raw count minus one. */
	int64 SdkCountNow() const { return Semaphore ? Semaphore->RawCount - 1 : -1; }
	/** Makes every scheduled publication whose time has come. */
	void ApplySchedule()
	{
		if (!ScheduleClock) return;
		const double Now = ScheduleClock();
		while (NextScheduled < ScheduledPublications.Num() && ScheduledPublications[NextScheduled] <= Now)
		{
			Publish();
			++NextScheduled;
		}
	}

	virtual bool CreateAccessMutex(const ANSICHAR*) override { return true; }
	virtual void CloseAccessMutex() override {}
	virtual void EnableFrameCount(const ANSICHAR*) override { FrameCount = 0; LastFrameCount = 0; }
	virtual void CleanupFrameCount() override { FrameCount = 0; LastFrameCount = 0; }
	virtual bool CheckTextureAccess(void*, uint32) override
	{
		// What is published by the time access is granted is in the texture.
		ApplySchedule();
		if (PublishAtNextAccess > 0)
		{
			Publish(PublishAtNextAccess);
			PublishAtNextAccess = 0;
		}
		++AccessAttempts;
		if (FailAccessAttempts > 0)
		{
			--FailAccessAttempts;
			return false;
		}
		return true;
	}
	virtual bool AllowTextureAccess(void*) override { ++AccessReleases; return true; }

	virtual void SetNewFrame() override
	{
		if (!Semaphore) return;
		++Semaphore->RawCount;
		++FrameCount;
	}

	virtual bool GetNewFrame() override
	{
		++NewFrameQueries;
		if (PublishAtQuery >= 0 && NewFrameQueries >= PublishAtQuery)
		{
			Publish();
			PublishAtQuery = -1;
		}
		ApplySchedule();
		if (!bCountingEnabled || !Semaphore) return true;
		int64 Reported = 0; // "framecount = 0" before the wait
		const int64 Visible = VisibleRawCount();
		if (Visible > 0) Reported = Visible - 1; // the wait succeeded; the release reports the count after the decrement
		FrameCount = Reported;
		if (Reported == 0) return true;
		if (Reported == LastFrameCount) return false;
		LastFrameCount = Reported;
		return true;
	}

	virtual int64 GetSenderFrame() override { return FrameCount; }
	virtual bool IsFrameCountEnabled() override { return bCountingEnabled; }

private:
	int64 VisibleRawCount()
	{
		int32 Lower = ForeignDecrementAtNextRead;
		ForeignDecrementAtNextRead = 0;
		if (ForeignDecrementAtQuery >= 0 && NewFrameQueries == ForeignDecrementAtQuery)
		{
			ForeignDecrementAtQuery = -1;
			Lower = FMath::Max(Lower, 1);
		}
		if (ForeignDecrementForReads > 0)
		{
			--ForeignDecrementForReads;
			Lower = FMath::Max(Lower, 1);
		}
		return Semaphore->RawCount - Lower;
	}
};

#endif
