// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Adds a "Spout" category to the Place Actors panel holding the Spout Sender and Spout Receiver actors, and gives both
 * classes the plugin's icon. Editor only; nothing here is part of a packaged game.
 */
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "IPlacementModeModule.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/CoreDelegates.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Brushes/SlateImageBrush.h"
#include "Editor.h"
#include "Misc/EngineVersionComparison.h"
#include "SpoutSenderActor.h"
#include "SpoutReceiverActor.h"

#define LOCTEXT_NAMESPACE "SpoutPluginEditor"

class FSpoutPluginEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RegisterStyle();
		if (GEngine && GEngine->IsInitialized())
		{
			RegisterPlacement();
		}
		else
		{
			PostEngineInitHandle = PostEngineInit().AddRaw(this, &FSpoutPluginEditorModule::RegisterPlacement);
		}
	}

	virtual void ShutdownModule() override
	{
		PostEngineInit().Remove(PostEngineInitHandle);
		if (IPlacementModeModule::IsAvailable())
		{
			IPlacementModeModule& Placement = IPlacementModeModule::Get();
			for (const FPlacementModeID& ID : PlacedItems)
			{
				Placement.UnregisterPlaceableItem(ID);
			}
			Placement.UnregisterPlacementCategory(CategoryHandle);
		}
		PlacedItems.Reset();
		if (Style.IsValid())
		{
			FSlateStyleRegistry::UnRegisterSlateStyle(*Style);
			Style.Reset();
		}
	}

private:
	// UE 5.8 replaced the delegate member with an accessor; UE 5.3-5.7 only have the member.
	static auto& PostEngineInit()
	{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		return FCoreDelegates::OnPostEngineInit;
#else
		return FCoreDelegates::GetOnPostEngineInit();
#endif
	}

	void RegisterStyle()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpoutPlugin"));
		if (!Plugin.IsValid()) return;
		const FString Icon = Plugin->GetBaseDir() / TEXT("Resources/Icon128.png");
		Style = MakeShared<FSlateStyleSet>(TEXT("SpoutPluginEditorStyle"));
		const FVector2D Icon16(16.0, 16.0);
		const FVector2D Icon64(64.0, 64.0);
		for (const TCHAR* ClassName : { TEXT("SpoutSenderActor"), TEXT("SpoutReceiverActor") })
		{
			Style->Set(*FString::Printf(TEXT("ClassIcon.%s"), ClassName), new FSlateImageBrush(Icon, Icon16));
			Style->Set(*FString::Printf(TEXT("ClassThumbnail.%s"), ClassName), new FSlateImageBrush(Icon, Icon64));
		}
		Style->Set(TEXT("SpoutPlugin.PlacementCategory"), new FSlateImageBrush(Icon, Icon16));
		FSlateStyleRegistry::RegisterSlateStyle(*Style);
	}

	void RegisterPlacement()
	{
		if (!GEditor) return;
		// Get() loads the Place Actors module when needed: this module starts before it, so testing IsAvailable() here
		// would skip the registration.
		IPlacementModeModule& Placement = IPlacementModeModule::Get();
		FPlacementCategoryInfo Info(LOCTEXT("SpoutCategory", "Spout"),
			FSlateIcon(TEXT("SpoutPluginEditorStyle"), TEXT("SpoutPlugin.PlacementCategory")),
			CategoryHandle, TEXT("PMSpout"), 45);
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
		Info.ShortDisplayName = LOCTEXT("SpoutCategoryShort", "Spout"); // field added in UE 5.5
#endif
		Placement.RegisterPlacementCategory(Info);

		auto Add = [&](UClass* Class, const FText& DisplayName, int32 SortOrder)
		{
			TOptional<FPlacementModeID> ID = Placement.RegisterPlaceableItem(CategoryHandle, MakeShared<FPlaceableItem>(
				*Class,
				FAssetData(Class),
				FName(*FString::Printf(TEXT("ClassThumbnail.%s"), *Class->GetName())),
				FName(*FString::Printf(TEXT("ClassIcon.%s"), *Class->GetName())),
				TOptional<FLinearColor>(),
				TOptional<int32>(SortOrder),
				DisplayName));
			if (ID.IsSet()) PlacedItems.Add(ID.GetValue());
		};
		Add(ASpoutSenderActor::StaticClass(), LOCTEXT("SpoutSender", "Spout Sender"), 10);
		Add(ASpoutReceiverActor::StaticClass(), LOCTEXT("SpoutReceiver", "Spout Receiver"), 20);
	}

	const FName CategoryHandle = TEXT("Spout");
	TArray<FPlacementModeID> PlacedItems;
	TSharedPtr<FSlateStyleSet> Style;
	FDelegateHandle PostEngineInitHandle;
};

IMPLEMENT_MODULE(FSpoutPluginEditorModule, SpoutPluginEditor)

#undef LOCTEXT_NAMESPACE
