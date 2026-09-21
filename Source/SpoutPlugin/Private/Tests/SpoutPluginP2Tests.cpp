// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
#include "SpoutSession.h"
#include "SpoutLimits.h"
#include "SpoutTextureUtils.h"
#include "SpoutReceiverOwnership.h"
#include "SpoutSenderRegistry.h"
#include "HAL/IConsoleManager.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/GarbageCollection.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

namespace
{
	struct FLimitScope
	{
		IConsoleVariable* Variable;
		int32 Before;
		FLimitScope(const TCHAR* Name, int32 Value) : Variable(IConsoleManager::Get().FindConsoleVariable(Name)), Before(Variable->GetInt()) { Variable->Set(Value, ECVF_SetByCode); }
		~FLimitScope() { Variable->Set(Before, ECVF_SetByCode); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP2BoundedSession, "SpoutPlugin.P2.BoundedSessionAndCancelledQueue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSpoutP2BoundedSession::RunTest(const FString&)
{
	FLimitScope Active(TEXT("r.Spout.MaxActiveStreams"), 2), Pending(TEXT("r.Spout.MaxPendingCommands"), 3), PerStream(TEXT("r.Spout.MaxPendingPerStream"), 2);
	FSpoutSession Session;
	const FName A(TEXT("P2_A")), B(TEXT("P2_B"));
	const auto Old = Session.OpenStream(A, ESpoutType::Sender);
	auto First = Session.TryQueue(Old), Second = Session.TryQueue(Old);
	TestTrue(TEXT("two queued requests admitted"), First.IsValid() && Second.IsValid());
	TestEqual(TEXT("queue reservation alone is not an accepted transfer"), Session.GetStreamStatus(A,ESpoutType::Sender).AcceptedRequests, int64(0));
	Session.MarkQueued(Old);
	TestEqual(TEXT("only committed commands are counted as accepted"), Session.GetStreamStatus(A,ESpoutType::Sender).AcceptedRequests, int64(1));
	TestFalse(TEXT("per stream queue bounded"), Session.TryQueue(Old).IsValid());
	Session.CancelStream(A, ESpoutType::Sender);
	TestEqual(TEXT("cancel retires active record immediately"), Session.NumStreams(), 0);
	const auto Reopened = Session.OpenStream(A, ESpoutType::Sender);
	TestFalse(TEXT("retirement cannot revive old token"), Session.IsCurrent(Old));
	auto Third = Session.TryQueue(Reopened);
	TestTrue(TEXT("new generation accepted within global bound"), Third.IsValid());
	const auto Other = Session.OpenStream(B, ESpoutType::Receiver);
	TestFalse(TEXT("cancelled backlog still consumes global budget"), Session.TryQueue(Other).IsValid());
	TestFalse(TEXT("active roles bounded"), Session.OpenStream(FName(TEXT("P2_C")), ESpoutType::Sender).IsSet());
	First.Reset(); Second.Reset(); Third.Reset();
	TestEqual(TEXT("leases drain to zero"), Session.GetRuntimeStatus().PendingCommands, 0);
	Session.CancelStream(A, ESpoutType::Sender); Session.CancelStream(B, ESpoutType::Receiver);
	for (int32 Index = 0; Index < 10000; ++Index)
	{
		const FName Name(*FString::Printf(TEXT("P2_Dynamic_%d"), Index));
		const auto Token = Session.OpenStream(Name, ESpoutType::Sender);
		if (!Token.IsSet()) return false;
		Session.CancelStream(Name, ESpoutType::Sender);
		if (Session.IsCurrent(Token)) return false;
	}
	TestEqual(TEXT("10000 names leave no session tombstones"), Session.NumStreams(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP2RetainedBudget, "SpoutPlugin.P2.RetainedOutputBudget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSpoutP2RetainedBudget::RunTest(const FString&)
{
	USpoutBPFunctionLibrary::CloseAllStreams(); FlushRenderingCommands(); CollectGarbage(RF_NoFlags); FlushRenderingCommands();
	const int64 Before = SpoutLimits::RetainedBytes();
	FLimitScope Budget(TEXT("r.Spout.MaxRetainedMiB"), int32((Before + 1024*1024-1)/(1024*1024)) + 1);
	UTexture2D* Texture = nullptr;
	TestTrue(TEXT("first image created"), SpoutTextureUtils::EnsureTransientTexture(Texture,256,128,PF_B8G8R8A8,false));
	if (!TestNotNull(TEXT("first allocation succeeded"), Texture)) return false;
	TStrongObjectPtr<UTexture2D> Retained(Texture);
	TestEqual(TEXT("CPU mip payload is included in the reservation"), Texture->GetPlatformData()->Mips[0].BulkData.GetBulkDataSize(), int64(256*128*4));
	Texture->UpdateResource(); FlushRenderingCommands();
	TestTrue(TEXT("recreated output has a writable resource"), Texture->GetResource() != nullptr);
	TestTrue(TEXT("replacement allocation created"), SpoutTextureUtils::EnsureTransientTexture(Texture,128,128,PF_B8G8R8A8,false));
	TStrongObjectPtr<UTexture2D> Replacement(Texture);
	UTexture2D* TooLarge = nullptr;
	SpoutTextureUtils::EnsureTransientTexture(TooLarge,256,256,PF_B8G8R8A8,false);
	TestNull(TEXT("caller-held old output consumes budget"), TooLarge);
	Retained.Reset(); FlushRenderingCommands(); CollectGarbage(RF_NoFlags); FlushRenderingCommands();
	SpoutTextureUtils::EnsureTransientTexture(TooLarge,256,256,PF_B8G8R8A8,false);
	TestNotNull(TEXT("GC returns capacity for a new image"), TooLarge);
	Replacement.Reset(); Texture = nullptr; TooLarge = nullptr;
	CollectGarbage(RF_NoFlags); FlushRenderingCommands();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP2FailedDiscovery, "SpoutPlugin.P2.FailedDiscoveryRetainsNoRecords", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSpoutP2FailedDiscovery::RunTest(const FString&)
{
	USpoutBPFunctionLibrary::CloseAllStreams();
	const int32 SessionBefore = FSpoutSession::Get().NumStreams(), RegistryBefore = FSpoutSenderRegistry::Get().Num();
	UTexture2D* Texture = nullptr;
	for (int32 Index = 0; Index < 2000; ++Index)
	{
		const FName Name(*FString::Printf(TEXT("P2_Missing_%u_%d"), FPlatformProcess::GetCurrentProcessId(), Index));
		if (USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(Name,nullptr,NAME_None,Texture,nullptr,ESpoutColorSpace::Auto)) return false;
	}
	TestEqual(TEXT("failed discovery leaves no native records"), FSpoutSenderRegistry::Get().Num(), RegistryBefore);
	TestEqual(TEXT("failed discovery leaves no session records"), FSpoutSession::Get().NumStreams(), SessionBefore);
	TestNull(TEXT("failed discovery allocates no output"), Texture);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP2RetryPolicy, "SpoutPlugin.P2.NativeFailureRetryPolicy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSpoutP2RetryPolicy::RunTest(const FString&)
{
	FSpoutSharedSender Stream(FName(TEXT("P2_Retry")), ESpoutType::Receiver);
	TestTrue(TEXT("new identity can connect"), Stream.CanRetry(16,16,DXGI_FORMAT_B8G8R8A8_UNORM));
	Stream.NativeFailure(E_ACCESSDENIED);
	TestFalse(TEXT("failure prevents an immediate native retry"), Stream.CanRetry(16,16,DXGI_FORMAT_B8G8R8A8_UNORM));
	for (int32 Index=0; Index<20; ++Index) Stream.NativeFailure(E_FAIL);
	TestEqual(TEXT("backoff is capped at one second"), Stream.RetryDelay, 1.0);
	TestEqual(TEXT("failures are observable"), Stream.NativeFailures.load(), int64(21));
	TestTrue(TEXT("changed native identity can reconnect immediately"), Stream.CanRetry(32,16,DXGI_FORMAT_B8G8R8A8_UNORM));
	Stream.NativeSuccess();
	TestEqual(TEXT("recovery clears the current error"), Stream.LastNativeError.load(), int64(0));
	TestEqual(TEXT("recovery retains historical failure count"), Stream.NativeFailures.load(), int64(21));
	return true;
}
#endif
