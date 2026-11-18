// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#include "SpoutSettings.h"

#include "SpoutLimits.h"
#include "SpoutModule.h"

#include "HAL/IConsoleManager.h"

#define LOCTEXT_NAMESPACE "SpoutSettings"

namespace
{
	/** One setting of the page that is a console variable. */
	struct FSpoutConsoleSetting
	{
		const TCHAR* Variable;
		int32 USpoutSettings::* Value;
		int32 Default;
		FName Property;
	};

	const TArray<FSpoutConsoleSetting>& ConsoleSettings()
	{
		static const TArray<FSpoutConsoleSetting> Settings = {
			{ TEXT("r.Spout.AccessWaitMs"), &USpoutSettings::AccessWaitMs, SpoutLimits::DefaultAccessWaitMs, GET_MEMBER_NAME_CHECKED(USpoutSettings, AccessWaitMs) },
			{ TEXT("r.Spout.MaxActiveStreams"), &USpoutSettings::MaxActiveStreams, SpoutLimits::DefaultMaxActiveStreams, GET_MEMBER_NAME_CHECKED(USpoutSettings, MaxActiveStreams) },
			{ TEXT("r.Spout.MaxPendingCommands"), &USpoutSettings::MaxPendingCommands, SpoutLimits::DefaultMaxPendingCommands, GET_MEMBER_NAME_CHECKED(USpoutSettings, MaxPendingCommands) },
			{ TEXT("r.Spout.MaxPendingPerStream"), &USpoutSettings::MaxPendingPerStream, SpoutLimits::DefaultMaxPendingPerStream, GET_MEMBER_NAME_CHECKED(USpoutSettings, MaxPendingPerStream) },
			{ TEXT("r.Spout.MaxConsumersPerReceiver"), &USpoutSettings::MaxConsumersPerReceiver, SpoutLimits::DefaultMaxConsumersPerReceiver, GET_MEMBER_NAME_CHECKED(USpoutSettings, MaxConsumersPerReceiver) },
			{ TEXT("r.Spout.MaxTextureDimension"), &USpoutSettings::MaxTextureDimension, SpoutLimits::DefaultMaxTextureDimension, GET_MEMBER_NAME_CHECKED(USpoutSettings, MaxTextureDimension) },
			{ TEXT("r.Spout.MaxFrameMiB"), &USpoutSettings::MaxFrameMiB, SpoutLimits::DefaultMaxFrameMiB, GET_MEMBER_NAME_CHECKED(USpoutSettings, MaxFrameMiB) },
			{ TEXT("r.Spout.MaxRetainedMiB"), &USpoutSettings::MaxRetainedMiB, SpoutLimits::DefaultMaxRetainedMiB, GET_MEMBER_NAME_CHECKED(USpoutSettings, MaxRetainedMiB) },
		};
		return Settings;
	}

	/**
	 * Sets one variable at project-setting priority. Unreal ignores a lower-priority set, and warns when the value came
	 * from code or the console; the check here keeps that quiet and says it in the plugin's own words instead.
	 */
	void ApplySetting(const FSpoutConsoleSetting& Setting, int32 Value, bool bReport)
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Setting.Variable);
		if (!Variable)
		{
			return;
		}
		const uint32 Source = static_cast<uint32>(Variable->GetFlags()) & ECVF_SetByMask;
		if (Source > static_cast<uint32>(ECVF_SetByProjectSetting))
		{
			if (bReport)
			{
				UE_LOG(LogSpoutPlugin, Log,
					TEXT("Project Settings > Spout: %s stays at %s, set by %s, which takes precedence over the project setting (%d)."),
					Setting.Variable, *Variable->GetString(), GetConsoleVariableSetByName(static_cast<EConsoleVariableFlags>(Source)), Value);
			}
			return;
		}
		Variable->Set(Value, ECVF_SetByProjectSetting);
	}
}

USpoutSettings::USpoutSettings()
	: DefaultSenderName(TEXT("UE Spout Sender"))
	, AccessWaitMs(SpoutLimits::DefaultAccessWaitMs)
	, MaxActiveStreams(SpoutLimits::DefaultMaxActiveStreams)
	, MaxPendingCommands(SpoutLimits::DefaultMaxPendingCommands)
	, MaxPendingPerStream(SpoutLimits::DefaultMaxPendingPerStream)
	, MaxConsumersPerReceiver(SpoutLimits::DefaultMaxConsumersPerReceiver)
	, MaxTextureDimension(SpoutLimits::DefaultMaxTextureDimension)
	, MaxFrameMiB(SpoutLimits::DefaultMaxFrameMiB)
	, MaxRetainedMiB(SpoutLimits::DefaultMaxRetainedMiB)
{
}

FString USpoutSettings::GetDefaultSenderName() const
{
	const FString Trimmed = DefaultSenderName.TrimStartAndEnd();
	return Trimmed.IsEmpty() ? FString(TEXT("UE Spout Sender")) : Trimmed;
}

void USpoutSettings::ApplyToConsoleVariables(bool bOnlyChangedValues) const
{
	for (const FSpoutConsoleSetting& Setting : ConsoleSettings())
	{
		const int32 Value = this->*Setting.Value;
		if (bOnlyChangedValues && Value == Setting.Default)
		{
			continue;
		}
		ApplySetting(Setting, Value, false);
	}
}

FName USpoutSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

void USpoutSettings::PostInitProperties()
{
	Super::PostInitProperties();
	// The class default holds the saved values by now. Only values that differ from the defaults are set, so a project
	// that never changed this page leaves every variable exactly as Unreal registered it.
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		ApplyToConsoleVariables(true);
	}
}

#if WITH_EDITOR
FText USpoutSettings::GetSectionText() const
{
	return LOCTEXT("SectionText", "Spout");
}

FText USpoutSettings::GetSectionDescription() const
{
	return LOCTEXT("SectionDescription",
		"Spout texture sharing: when streams may run, the default sender name, the access wait and the stream and memory limits. "
		"A console variable given in an ini file's [ConsoleVariables], on the command line or in the console takes precedence over this page.");
}

void USpoutSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		return;
	}
	const FName Changed = PropertyChangedEvent.GetMemberPropertyName();
	for (const FSpoutConsoleSetting& Setting : ConsoleSettings())
	{
		// An edited value is set even when it is back at the default, so the change takes effect without a restart.
		if (Changed == Setting.Property)
		{
			ApplySetting(Setting, this->*Setting.Value, true);
		}
	}
}
#endif

#undef LOCTEXT_NAMESPACE
