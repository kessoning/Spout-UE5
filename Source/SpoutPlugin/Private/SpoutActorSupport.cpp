// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#include "SpoutActorSupport.h"

#include "GameFramework/Actor.h"
#include "SpoutBPFunctionLibrary.h"
#include "SpoutD3DContext.h"
#include "SpoutSdkSafety.h"
#include "SpoutSenderRegistry.h"
#include "SpoutValidation.h"

namespace SpoutActorSupport
{
	namespace
	{
		TMap<FName, TWeakObjectPtr<const AActor>>& SenderClaims()
		{
			static TMap<FName, TWeakObjectPtr<const AActor>> Claims;
			return Claims;
		}
	}

	bool IsPublishedByThisProcess(FName Name)
	{
		check(IsInGameThread());
		if (Name.IsNone()) return false;
		if (FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Sender)) return true;
		return USpoutBPFunctionLibrary::GetSpoutStreamStatus(Name, ESpoutType::Sender).bActive;
	}

	bool IsAdvertised(FName Name)
	{
		FSenderStruct Info;
		return !Name.IsNone() && USpoutBPFunctionLibrary::GetSenderInfo(Name, Info) && Info.bIsAlive;
	}

	bool ChooseExternalSender(FName& OutName)
	{
		check(IsInGameThread());
		TArray<FSenderStruct> Senders;
		if (!USpoutBPFunctionLibrary::SpoutInfo(Senders) || Senders.Num() == 0) return false;

		// Spout's "active sender" is the one most recently started or selected on this machine.
		FName Active;
		{
			FSpoutD3DContext& Context = FSpoutD3DContext::Get();
			if (FSpoutSafeSenderNames* Names = Context.GetSenderNames())
			{
				FScopeLock Lock(&Context.GetSpoutMutex());
				ANSICHAR Buffer[SpoutMaxSenderNameLen] = {};
				if (Names->GetActiveSender(Buffer, UE_ARRAY_COUNT(Buffer)))
				{
					Buffer[UE_ARRAY_COUNT(Buffer) - 1] = '\0';
					if (Buffer[0]) Active = FName(UTF8_TO_TCHAR(Buffer));
				}
			}
		}
		if (!Active.IsNone() && !IsPublishedByThisProcess(Active))
		{
			for (const FSenderStruct& Sender : Senders)
			{
				if (Sender.Name == Active)
				{
					OutName = Active;
					return true;
				}
			}
		}
		for (const FSenderStruct& Sender : Senders)
		{
			if (!IsPublishedByThisProcess(Sender.Name))
			{
				OutName = Sender.Name;
				return true;
			}
		}
		return false;
	}

	TArray<FString> ListAdvertisedSenderNames()
	{
		TArray<FString> Result;
		TArray<FSenderStruct> Senders;
		if (USpoutBPFunctionLibrary::SpoutInfo(Senders))
		{
			for (const FSenderStruct& Sender : Senders)
			{
				Result.Add(Sender.Name.ToString());
			}
		}
		Result.Sort();
		return Result;
	}

	bool ClaimSenderName(FName Name, const AActor* Actor, const AActor*& OutHolder)
	{
		check(IsInGameThread());
		OutHolder = nullptr;
		TWeakObjectPtr<const AActor>& Holder = SenderClaims().FindOrAdd(Name);
		if (Holder.IsValid() && Holder.Get() != Actor)
		{
			OutHolder = Holder.Get();
			return false;
		}
		Holder = Actor;
		return true;
	}

	void ReleaseSenderName(FName Name, const AActor* Actor)
	{
		check(IsInGameThread());
		if (const TWeakObjectPtr<const AActor>* Holder = SenderClaims().Find(Name))
		{
			if (!Holder->IsValid() || Holder->Get() == Actor)
			{
				SenderClaims().Remove(Name);
			}
		}
	}
}
