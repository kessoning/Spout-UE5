// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "IPlacementModeModule.h"
#include "SpoutReceiverActor.h"
#include "SpoutSenderActor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutActorsPlacementCategory, "SpoutPlugin.Actors.PlaceActorsCategory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSpoutActorsPlacementCategory::RunTest(const FString&)
{
	if (!IPlacementModeModule::IsAvailable())
	{
		AddError(TEXT("the Place Actors module is not loaded"));
		return false;
	}
	IPlacementModeModule& Placement = IPlacementModeModule::Get();
	const FPlacementCategoryInfo* Info = Placement.GetRegisteredPlacementCategory(TEXT("Spout"));
	TestNotNull(TEXT("a 'Spout' category is registered"), Info);
	if (Info) TestEqual(TEXT("its name"), Info->DisplayName.ToString(), FString(TEXT("Spout")));
	TArray<TSharedPtr<FPlaceableItem>> Items;
	Placement.GetItemsForCategory(TEXT("Spout"), Items);
	bool bSender = false, bReceiver = false;
	for (const TSharedPtr<FPlaceableItem>& Item : Items)
	{
		if (!Item.IsValid()) continue;
		const FString Path = Item->AssetData.GetObjectPathString();
		bSender |= Path == ASpoutSenderActor::StaticClass()->GetPathName();
		bReceiver |= Path == ASpoutReceiverActor::StaticClass()->GetPathName();
	}
	TestTrue(TEXT("Spout Sender is in the category"), bSender);
	TestTrue(TEXT("Spout Receiver is in the category"), bReceiver);
	return true;
}
#endif
