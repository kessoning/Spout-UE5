// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// The Project Settings page (USpoutSettings): where it appears, its defaults, how its values reach the console
// variables (project-setting priority, so the console, the command line and [ConsoleVariables] still win), that it
// loads and saves through Unreal's config system, and the default name of a newly placed Spout Sender actor.
// Every console variable and the page's class default are put back as they were.
#include "Misc/AutomationTest.h"
#if SPOUT_WITH_TESTS && PLATFORM_WINDOWS
#include "SpoutLimits.h"
#include "SpoutSenderActor.h"
#include "SpoutSettings.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#define SettingsTestFlags (EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	/** Keeps a console variable's value and source, and puts both back when the scope ends. */
	struct FScopedConsoleVariable
	{
		IConsoleVariable* Variable;
		FString Value;
		EConsoleVariableFlags Source;

		explicit FScopedConsoleVariable(const TCHAR* Name)
			: Variable(IConsoleManager::Get().FindConsoleVariable(Name))
			, Value(Variable ? Variable->GetString() : FString())
			, Source(Variable ? static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask) : ECVF_SetByConstructor)
		{
		}

		~FScopedConsoleVariable()
		{
			if (Variable)
			{
				// The console's priority is the highest, so this set always applies; the source is restored after it.
				Variable->Set(*Value, ECVF_SetByConsole);
				Variable->ClearFlags(ECVF_SetByMask);
				Variable->SetFlags(Source);
			}
		}

		EConsoleVariableFlags CurrentSource() const
		{
			return static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSettingsPageAndDefaults, "SpoutPlugin.Settings.PageAndDefaults", SettingsTestFlags)
bool FSpoutSettingsPageAndDefaults::RunTest(const FString&)
{
	const USpoutSettings& Settings = *GetDefault<USpoutSettings>();
	TestEqual(TEXT("listed under Plugins"), Settings.GetCategoryName(), FName(TEXT("Plugins")));
#if WITH_EDITOR
	TestEqual(TEXT("named Spout"), Settings.GetSectionText().ToString(), FString(TEXT("Spout")));
#endif
	const UClass* Class = USpoutSettings::StaticClass();
	TestTrue(TEXT("saved to the project's DefaultEngine.ini"), Class->HasAllClassFlags(CLASS_Config | CLASS_DefaultConfig) && Class->ClassConfigName == FName(TEXT("Engine")));
	for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		TestTrue(*FString::Printf(TEXT("%s is saved"), *It->GetName()), It->HasAnyPropertyFlags(CPF_Config));
#if WITH_EDITOR
		TestFalse(*FString::Printf(TEXT("%s has a tooltip"), *It->GetName()), It->GetToolTipText().IsEmpty());
#endif
	}

	const USpoutSettings* Defaults = Class->GetDefaultObject<USpoutSettings>();
	TestTrue(TEXT("startup policy Play only"), Defaults->StartupPolicy == ESpoutStartupPolicy::PlayOnly);
	TestEqual(TEXT("default sender name"), Defaults->GetDefaultSenderName(), FString(TEXT("UE Spout Sender")));
	// The page's defaults are the variables' own, so an untouched page changes nothing.
	const TPair<const TCHAR*, int32> Expected[] = {
		{ TEXT("r.Spout.AccessWaitMs"), SpoutLimits::DefaultAccessWaitMs },
		{ TEXT("r.Spout.MaxActiveStreams"), SpoutLimits::DefaultMaxActiveStreams },
		{ TEXT("r.Spout.MaxPendingCommands"), SpoutLimits::DefaultMaxPendingCommands },
		{ TEXT("r.Spout.MaxPendingPerStream"), SpoutLimits::DefaultMaxPendingPerStream },
		{ TEXT("r.Spout.MaxConsumersPerReceiver"), SpoutLimits::DefaultMaxConsumersPerReceiver },
		{ TEXT("r.Spout.MaxTextureDimension"), SpoutLimits::DefaultMaxTextureDimension },
		{ TEXT("r.Spout.MaxFrameMiB"), SpoutLimits::DefaultMaxFrameMiB },
		{ TEXT("r.Spout.MaxRetainedMiB"), SpoutLimits::DefaultMaxRetainedMiB },
	};
	const int32 PageDefaults[] = { Defaults->AccessWaitMs, Defaults->MaxActiveStreams, Defaults->MaxPendingCommands, Defaults->MaxPendingPerStream,
		Defaults->MaxConsumersPerReceiver, Defaults->MaxTextureDimension, Defaults->MaxFrameMiB, Defaults->MaxRetainedMiB };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Expected); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("page default for %s"), Expected[Index].Key), PageDefaults[Index], Expected[Index].Value);
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Expected[Index].Key);
		if (TestNotNull(*FString::Printf(TEXT("%s registered"), Expected[Index].Key), Variable)
			&& (Variable->GetFlags() & ECVF_SetByMask) == ECVF_SetByConstructor)
		{
			TestEqual(*FString::Printf(TEXT("%s untouched holds the same default"), Expected[Index].Key), Variable->GetInt(), Expected[Index].Value);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSettingsReachTheVariables, "SpoutPlugin.Settings.ValuesReachTheConsoleVariables", SettingsTestFlags)
bool FSpoutSettingsReachTheVariables::RunTest(const FString&)
{
	// Every variable of the page is put back afterwards, whatever this test sets.
	TArray<TUniquePtr<FScopedConsoleVariable>> Kept;
	for (const TCHAR* Name : { TEXT("r.Spout.AccessWaitMs"), TEXT("r.Spout.MaxActiveStreams"), TEXT("r.Spout.MaxPendingCommands"),
		TEXT("r.Spout.MaxPendingPerStream"), TEXT("r.Spout.MaxConsumersPerReceiver"), TEXT("r.Spout.MaxTextureDimension"),
		TEXT("r.Spout.MaxFrameMiB"), TEXT("r.Spout.MaxRetainedMiB") })
	{
		Kept.Add(MakeUnique<FScopedConsoleVariable>(Name));
		if (!TestNotNull(*FString::Printf(TEXT("%s registered"), Name), Kept.Last()->Variable)) return false;
	}
	const FScopedConsoleVariable& Wait = *Kept[0];
	const FScopedConsoleVariable& Streams = *Kept[1];
	if (Streams.Source > ECVF_SetByProjectSetting || Wait.Source > ECVF_SetByProjectSetting)
	{
		AddInfo(TEXT("These variables were set with a higher priority before the test; the page cannot change them here."));
		return true;
	}

	USpoutSettings* Page = NewObject<USpoutSettings>(GetTransientPackage());
	Page->MaxActiveStreams = 77;
	Page->ApplyToConsoleVariables(true);
	TestEqual(TEXT("a changed value reaches its variable"), Streams.Variable->GetInt(), 77);
	TestEqual(TEXT("and the plugin reads it"), SpoutLimits::MaxActiveStreams(), 77);
	TestTrue(TEXT("at project-setting priority"), Streams.CurrentSource() == ECVF_SetByProjectSetting);
	TestTrue(TEXT("a value left at its default is not set"), Wait.CurrentSource() == Wait.Source);

	// The console takes precedence over the page.
	Streams.Variable->Set(5, ECVF_SetByConsole);
	Page->MaxActiveStreams = 99;
	Page->ApplyToConsoleVariables(true);
	TestEqual(TEXT("a value set in the console stays"), Streams.Variable->GetInt(), 5);

#if WITH_EDITOR
	// Editing the page: the edited value is set even back at its default, so the change takes effect without a
	// restart, and only that variable changes.
	Streams.Variable->ClearFlags(ECVF_SetByMask);
	Streams.Variable->SetFlags(ECVF_SetByProjectSetting);
	USpoutSettings* Defaults = GetMutableDefault<USpoutSettings>();
	const int32 Before = Defaults->MaxActiveStreams;
	Defaults->MaxActiveStreams = SpoutLimits::DefaultMaxActiveStreams;
	FPropertyChangedEvent Edit(USpoutSettings::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(USpoutSettings, MaxActiveStreams)));
	Defaults->PostEditChangeProperty(Edit);
	Defaults->MaxActiveStreams = Before;
	TestEqual(TEXT("an edit back to the default applies at once"), Streams.Variable->GetInt(), SpoutLimits::DefaultMaxActiveStreams);
	TestTrue(TEXT("an edit sets only its own variable"), Wait.CurrentSource() == Wait.Source);
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSettingsLoadAndSave, "SpoutPlugin.Settings.LoadAndSave", SettingsTestFlags)
bool FSpoutSettingsLoadAndSave::RunTest(const FString&)
{
	// A file of its own, so the project's configuration is never touched.
	const FString File = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SpoutSettingsTest.ini")));
	const FString Section = USpoutSettings::StaticClass()->GetPathName();
	const FString Text = FString::Printf(TEXT("[%s]\nStartupPolicy=EditorAndPlay\nDefaultSenderName=Stage Left\nMaxActiveStreams=77\n"), *Section);
	if (!TestTrue(TEXT("test file written"), FFileHelper::SaveStringToFile(Text, *File))) return false;
	GConfig->LoadFile(File);

	USpoutSettings* Loaded = NewObject<USpoutSettings>(GetTransientPackage());
	Loaded->LoadConfig(nullptr, *File);
	TestTrue(TEXT("startup policy loads"), Loaded->StartupPolicy == ESpoutStartupPolicy::EditorAndPlay);
	TestEqual(TEXT("default sender name loads"), Loaded->DefaultSenderName, FString(TEXT("Stage Left")));
	TestEqual(TEXT("a limit loads"), Loaded->MaxActiveStreams, 77);
	TestEqual(TEXT("a value absent from the file keeps its default"), Loaded->MaxRetainedMiB, SpoutLimits::DefaultMaxRetainedMiB);

	USpoutSettings* Saved = NewObject<USpoutSettings>(GetTransientPackage());
	Saved->AccessWaitMs = 7;
	Saved->MaxRetainedMiB = 2048;
	Saved->DefaultSenderName = TEXT("Stage Right");
	Saved->SaveConfig(CPF_Config, *File, GConfig, false);
	USpoutSettings* Reloaded = NewObject<USpoutSettings>(GetTransientPackage());
	Reloaded->LoadConfig(nullptr, *File);
	TestEqual(TEXT("the access wait is saved"), Reloaded->AccessWaitMs, 7);
	TestEqual(TEXT("a limit is saved"), Reloaded->MaxRetainedMiB, 2048);
	TestEqual(TEXT("the default sender name is saved"), Reloaded->DefaultSenderName, FString(TEXT("Stage Right")));
	TestEqual(TEXT("the class default is left alone"), GetDefault<USpoutSettings>()->AccessWaitMs, SpoutLimits::DefaultAccessWaitMs);

	GConfig->UnloadFile(File);
	IFileManager::Get().Delete(*File);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSettingsDefaultSenderName, "SpoutPlugin.Settings.NewSenderActorsUseTheDefaultName", SettingsTestFlags)
bool FSpoutSettingsDefaultSenderName::RunTest(const FString&)
{
	USpoutSettings* Defaults = GetMutableDefault<USpoutSettings>();
	const FString Before = Defaults->DefaultSenderName;
	Defaults->DefaultSenderName = TEXT("Stage Left");

	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
	ASpoutSenderActor* First = World ? World->SpawnActor<ASpoutSenderActor>() : nullptr;
	ASpoutSenderActor* Second = World ? World->SpawnActor<ASpoutSenderActor>() : nullptr;
	if (TestNotNull(TEXT("first sender placed"), First) && TestNotNull(TEXT("second sender placed"), Second))
	{
		TestEqual(TEXT("a new sender takes the project's default name"), First->SpoutName, FName(TEXT("Stage Left")));
		TestEqual(TEXT("the next one is numbered"), Second->SpoutName, FName(TEXT("Stage Left 2")));
	}

	Defaults->DefaultSenderName = TEXT("   ");
	TestEqual(TEXT("an empty default name falls back"), Defaults->GetDefaultSenderName(), FString(TEXT("UE Spout Sender")));
	Defaults->DefaultSenderName = Before;
	if (World)
	{
		World->DestroyWorld(false);
	}
	return true;
}
#endif
