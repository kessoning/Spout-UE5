// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// Ownership rules of the placeable Spout Sender / Spout Receiver actors, against the real registry and ownership
// tracker. Sessions are set up directly (no external sender is needed): the rules under test are the release decisions.
#include "Misc/AutomationTest.h"
#if SPOUT_WITH_TESTS && PLATFORM_WINDOWS
#include "SpoutActorSupport.h"
#include "SpoutReceiver.h"
#include "SpoutReceiverActor.h"
#include "SpoutReceiverOwnership.h"
#include "SpoutSenderActor.h"
#include "SpoutSenderRegistry.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"
#include "Materials/Material.h"

#define ActorTestFlags (EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	TSharedPtr<FSpoutSharedSender> OpenEntry(FName Name)
	{
		return FSpoutSenderRegistry::Get().FindOrAdd(Name, ESpoutType::Receiver);
	}

	bool EntryExists(FName Name)
	{
		return FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Receiver).IsValid();
	}

	void CloseForTest(FName Name)
	{
		UTexture2D* Texture = nullptr;
		UMaterialInstanceDynamic* Material = nullptr;
		USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material);
		FlushRenderingCommands();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutActorsLeasesShareAndCloseOnLast, "SpoutPlugin.Actors.LeasesShareAndCloseOnLast", ActorTestFlags)
bool FSpoutActorsLeasesShareAndCloseOnLast::RunTest(const FString&)
{
	const FName Name(TEXT("SpoutActorsTest_Leases_7c21"));
	TStrongObjectPtr<UTexture2D> A(NewObject<UTexture2D>()), B(NewObject<UTexture2D>());
	TSharedPtr<FSpoutSharedSender> Entry = OpenEntry(Name);
	Entry->ManagedConsumers.Add(FObjectKey(A.Get()));
	Entry->ManagedConsumers.Add(FObjectKey(B.Get()));
	TestFalse(TEXT("releasing one of two actors keeps the session"), FSpoutReceiver::ReleaseManagedConsumer(Name, A.Get(), nullptr, NAME_None, nullptr));
	TestTrue(TEXT("session still open"), EntryExists(Name));
	TestFalse(TEXT("releasing the same actor again changes nothing"), FSpoutReceiver::ReleaseManagedConsumer(Name, A.Get(), nullptr, NAME_None, nullptr));
	TestTrue(TEXT("session still open after a repeated release"), EntryExists(Name));
	TestTrue(TEXT("releasing the last actor closes the session"), FSpoutReceiver::ReleaseManagedConsumer(Name, B.Get(), nullptr, NAME_None, nullptr));
	TestFalse(TEXT("session closed"), EntryExists(Name));
	TestFalse(TEXT("a release after close is a no-op"), FSpoutReceiver::ReleaseManagedConsumer(Name, B.Get(), nullptr, NAME_None, nullptr));
	FlushRenderingCommands();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutActorsUnmanagedCallerKeepsSession, "SpoutPlugin.Actors.UnmanagedCallerKeepsSession", ActorTestFlags)
bool FSpoutActorsUnmanagedCallerKeepsSession::RunTest(const FString&)
{
	const FName Name(TEXT("SpoutActorsTest_Unmanaged_7c21"));
	TStrongObjectPtr<UTexture2D> A(NewObject<UTexture2D>());
	TSharedPtr<FSpoutSharedSender> Entry = OpenEntry(Name);
	Entry->ManagedConsumers.Add(FObjectKey(A.Get()));
	Entry->bUnmanagedConsumer = true; // a Blueprint/C++ receive was accepted on this session
	TestFalse(TEXT("the actor does not close a session a Blueprint caller used"), FSpoutReceiver::ReleaseManagedConsumer(Name, A.Get(), nullptr, NAME_None, nullptr));
	TestTrue(TEXT("session left for the Blueprint caller's Close Receiver"), EntryExists(Name));
	CloseForTest(Name);
	TestFalse(TEXT("Close Receiver still closes it"), EntryExists(Name));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutActorsRefusedOnlySessionIsReleased, "SpoutPlugin.Actors.RefusedOnlySessionIsReleased", ActorTestFlags)
bool FSpoutActorsRefusedOnlySessionIsReleased::RunTest(const FString&)
{
	// An entry that only refused calls created (no accepted receive, so no lease and no unmanaged mark).
	const FName Name(TEXT("SpoutActorsTest_Refused_7c21"));
	TStrongObjectPtr<UTexture2D> A(NewObject<UTexture2D>());
	OpenEntry(Name);
	TestTrue(TEXT("an actor whose attempts were refused still releases an otherwise unused session"), FSpoutReceiver::ReleaseManagedConsumer(Name, A.Get(), nullptr, NAME_None, nullptr));
	TestFalse(TEXT("session closed"), EntryExists(Name));
	FlushRenderingCommands();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutActorsSharedOutputTargetKept, "SpoutPlugin.Actors.SharedOutputTargetReservationKept", ActorTestFlags)
bool FSpoutActorsSharedOutputTargetKept::RunTest(const FString&)
{
	const FName Name(TEXT("SpoutActorsTest_Output_7c21"));
	TStrongObjectPtr<UTexture2D> A(NewObject<UTexture2D>()), B(NewObject<UTexture2D>());
	TStrongObjectPtr<UTextureRenderTarget2D> Target(NewObject<UTextureRenderTarget2D>());
	TSharedPtr<FSpoutSharedSender> Entry = OpenEntry(Name);
	const TWeakObjectPtr<UObject> Key(Target.Get());
	Entry->ManagedConsumers.Add(FObjectKey(A.Get()));
	Entry->ManagedConsumers.Add(FObjectKey(B.Get()));
	auto& Use = Entry->OutputTargetUsers.FindOrAdd(Key);
	Use.Managed.Add(FObjectKey(A.Get()));
	Use.Managed.Add(FObjectKey(B.Get()));
	Entry->OutputReservations.Add(Key, FSpoutResourceReservation::TryReserve(1024));
	TestTrue(TEXT("reservation made"), Entry->OutputReservations.Contains(Key));

	FSpoutReceiver::ReleaseManagedOutputTarget(Name, A.Get(), Target.Get());
	TestTrue(TEXT("another actor still uses the target: its reservation stays"), Entry->OutputReservations.Contains(Key));
	FSpoutReceiver::ReleaseManagedOutputTarget(Name, A.Get(), Target.Get());
	TestTrue(TEXT("repeating the release changes nothing"), Entry->OutputReservations.Contains(Key));
	Entry->OutputTargetUsers.FindOrAdd(Key).bUnmanaged = true;
	FSpoutReceiver::ReleaseManagedOutputTarget(Name, B.Get(), Target.Get());
	TestTrue(TEXT("a Blueprint caller still uses the target: its reservation stays"), Entry->OutputReservations.Contains(Key));
	Entry->OutputTargetUsers.FindOrAdd(Key).bUnmanaged = false;
	Entry->OutputTargetUsers.FindOrAdd(Key).Managed.Add(FObjectKey(B.Get()));
	FSpoutReceiver::ReleaseManagedOutputTarget(Name, B.Get(), Target.Get());
	TestFalse(TEXT("the last user's release drops the reservation"), Entry->OutputReservations.Contains(Key));
	TestTrue(TEXT("releasing an output target never closes the session"), EntryExists(Name));
	CloseForTest(Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutActorsCloseReceiverUnchanged, "SpoutPlugin.Actors.CloseReceiverStillClosesForEveryone", ActorTestFlags)
bool FSpoutActorsCloseReceiverUnchanged::RunTest(const FString&)
{
	const FName Name(TEXT("SpoutActorsTest_Close_7c21"));
	TStrongObjectPtr<UTexture2D> A(NewObject<UTexture2D>());
	TSharedPtr<FSpoutSharedSender> Entry = OpenEntry(Name);
	Entry->ManagedConsumers.Add(FObjectKey(A.Get()));
	CloseForTest(Name);
	TestFalse(TEXT("Close Receiver closes a session actors use, as before"), EntryExists(Name));
	TestFalse(TEXT("the actor's later release is a harmless no-op"), FSpoutReceiver::ReleaseManagedConsumer(Name, A.Get(), nullptr, NAME_None, nullptr));
	const FSpoutReceiverSessionState Info = FSpoutReceiver::GetSessionState(Name);
	TestFalse(TEXT("no session reported after close"), Info.bExists);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutActorsSenderNameClaims, "SpoutPlugin.Actors.SenderNameClaims", ActorTestFlags)
bool FSpoutActorsSenderNameClaims::RunTest(const FString&)
{
	const FName Name(TEXT("SpoutActorsTest_Claim_7c21"));
	const AActor* First = GetDefault<ASpoutSenderActor>();
	const AActor* Second = GetDefault<ASpoutReceiverActor>();
	const AActor* Holder = nullptr;
	TestTrue(TEXT("first claim succeeds"), SpoutActorSupport::ClaimSenderName(Name, First, Holder));
	TestFalse(TEXT("a second actor cannot take the name"), SpoutActorSupport::ClaimSenderName(Name, Second, Holder));
	TestTrue(TEXT("and is told who holds it"), Holder == First);
	TestTrue(TEXT("re-claiming one's own name succeeds"), SpoutActorSupport::ClaimSenderName(Name, First, Holder));
	SpoutActorSupport::ReleaseSenderName(Name, Second);
	TestFalse(TEXT("another actor's release does not drop the claim"), SpoutActorSupport::ClaimSenderName(Name, Second, Holder));
	SpoutActorSupport::ReleaseSenderName(Name, First);
	TestTrue(TEXT("after the holder's release the name is free"), SpoutActorSupport::ClaimSenderName(Name, Second, Holder));
	SpoutActorSupport::ReleaseSenderName(Name, Second);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutActorsDefaults, "SpoutPlugin.Actors.Defaults", ActorTestFlags)
bool FSpoutActorsDefaults::RunTest(const FString&)
{
	const ASpoutSenderActor* Sender = GetDefault<ASpoutSenderActor>();
	TestTrue(TEXT("sender defaults to Game Viewport Only"), Sender->SendFrom == ESpoutSendTextureFrom::GameViewportOnly);
	TestTrue(TEXT("sender enabled by default"), Sender->bEnabled);
	TestFalse(TEXT("sender has a default name"), Sender->SpoutName.IsNone());
	TestTrue(TEXT("sender is placeable"), !ASpoutSenderActor::StaticClass()->HasAnyClassFlags(CLASS_NotPlaceable | CLASS_Abstract));

	const ASpoutReceiverActor* Receiver = GetDefault<ASpoutReceiverActor>();
	TestTrue(TEXT("receiver Sender Name empty (auto) by default"), Receiver->SenderName.IsNone());
	TestTrue(TEXT("receiver Color Space Auto"), Receiver->ColorSpace == ESpoutColorSpace::Auto);
	TestTrue(TEXT("receiver Async Receive on"), Receiver->bAsyncReceive);
	TestNull(TEXT("receiver has no Output Render Target by default"), Receiver->OutputRenderTarget.Get());
	TestNotNull(TEXT("receiver's default display material resolves (M_SpoutDisplayUnlit)"), Receiver->DisplayMaterial.Get());
	TestTrue(TEXT("receiver is placeable"), !ASpoutReceiverActor::StaticClass()->HasAnyClassFlags(CLASS_NotPlaceable | CLASS_Abstract));
	if (const UMaterial* Base = Receiver->DisplayMaterial ? Receiver->DisplayMaterial->GetMaterial() : nullptr)
	{
		TestTrue(TEXT("display material is unlit"), Base->GetShadingModels().HasOnlyShadingModel(MSM_Unlit));
	}
#if WITH_EDITOR
	for (UClass* Class : { ASpoutSenderActor::StaticClass(), ASpoutReceiverActor::StaticClass() })
	{
		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Edit) && !It->HasAnyPropertyFlags(CPF_EditConst))
			{
				TestFalse(*FString::Printf(TEXT("%s.%s has a tooltip"), *Class->GetName(), *It->GetName()), It->GetToolTipText().IsEmpty());
			}
		}
	}
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutActorsSessionMembershipAndIdentity, "SpoutPlugin.Actors.SessionMembershipAndIdentity", ActorTestFlags)
bool FSpoutActorsSessionMembershipAndIdentity::RunTest(const FString&)
{
	const FName Name(TEXT("SpoutActorsTest_Identity_7c21"));
	TStrongObjectPtr<UTexture2D> Holder(NewObject<UTexture2D>()), Refused(NewObject<UTexture2D>());
	TSharedPtr<FSpoutSharedSender> Entry = OpenEntry(Name);
	Entry->ManagedConsumers.Add(FObjectKey(Holder.Get()));
	const FSpoutReceiverSessionState ForHolder = FSpoutReceiver::GetSessionState(Name, Holder.Get());
	const FSpoutReceiverSessionState ForRefused = FSpoutReceiver::GetSessionState(Name, Refused.Get());
	TestTrue(TEXT("the leaseholder is a member of the session"), ForHolder.bIsManagedConsumer);
	TestFalse(TEXT("an actor without an accepted receive is not, although the session has a consumer"), ForRefused.bIsManagedConsumer);
	TestEqual(TEXT("both see one consumer"), ForRefused.ManagedConsumers, 1);
	TestTrue(TEXT("the session has an identity"), ForHolder.SessionId != 0);
	TestEqual(TEXT("the identity is stable while the session lives"), FSpoutReceiver::GetSessionState(Name).SessionId, ForHolder.SessionId);
	CloseForTest(Name); // a Blueprint Close Receiver
	OpenEntry(Name);    // reopened by the next receive
	const FSpoutReceiverSessionState Reopened = FSpoutReceiver::GetSessionState(Name, Holder.Get());
	TestTrue(TEXT("a reopened session has a new identity"), Reopened.SessionId != 0 && Reopened.SessionId != ForHolder.SessionId);
	TestFalse(TEXT("membership does not carry over to the reopened session"), Reopened.bIsManagedConsumer);
	CloseForTest(Name);
	return true;
}
#endif
