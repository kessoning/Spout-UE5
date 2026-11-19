// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/**
 * Adds a "Spout" category to the Place Actors panel holding the Spout Sender and Spout Receiver actors, gives both
 * classes the plugin's icon, tells the runtime module which level viewport a Spout Sender actor sends while it sends in
 * the editor, and has those actors say in their Status when the editor pauses them (no Realtime level viewport). Editor
 * only; nothing here is part of a packaged game.
 */
#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "EngineUtils.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "IPlacementModeModule.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/CoreDelegates.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Brushes/SlateImageBrush.h"
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "Misc/EngineVersionComparison.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "SpoutModule.h"
#include "SpoutSenderActor.h"
#include "SpoutReceiverActor.h"

#define LOCTEXT_NAMESPACE "SpoutPluginEditor"

class FSpoutPluginEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RegisterStyle();
		FSpoutModule::SetLevelViewportProvider(&ActiveLevelViewport);
		PauseCheckHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FSpoutPluginEditorModule::ShowEditorPause), PauseCheckSeconds);
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
		FSpoutModule::SetLevelViewportProvider(nullptr);
		FTSTicker::GetCoreTicker().RemoveTicker(PauseCheckHandle);
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
	/**
	 * The level viewport a Spout Sender actor sends while it sends in the editor: the one in use (the last one clicked,
	 * as Unreal's own level editing commands choose it), and whether it redraws continuously (Realtime).
	 */
	static TSharedPtr<SWidget> ActiveLevelViewport(bool& bOutRealtime)
	{
		FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient ? GCurrentLevelEditingViewportClient : GLastKeyLevelEditingViewportClient;
		if (!Client || !Client->Viewport)
		{
			return nullptr;
		}
		bOutRealtime = Client->IsRealtime();
		// A level editor viewport is always a scene viewport (SLevelViewport creates one).
		return static_cast<FSceneViewport*>(Client->Viewport)->GetViewportWidget().Pin();
	}

	/**
	 * While no viewport showing the level is Realtime, Unreal ticks the level in the editor with time only: actors do
	 * not tick, so the Spout actors working there stop streaming and cannot update their Status. Checked a few times a
	 * second; each of them then says so in its Status, and the log says it once.
	 */
	bool ShowEditorPause(float)
	{
		UWorld* World = GEditor && !GEditor->PlayWorld ? GEditor->GetEditorWorldContext().World() : nullptr;
		const double Now = FPlatformTime::Seconds();
		if (!World || LevelTicksInEditor(World))
		{
			// A later pause is logged again only once Realtime has stayed on for a while: brief redraws (selecting an
			// actor redraws a few frames) do not log it again.
			if (LevelTickingSince == 0.0) LevelTickingSince = Now;
			if (Now - LevelTickingSince >= PauseLogRearmSeconds) bPauseLogged = false;
			return true;
		}
		LevelTickingSince = 0.0;
		int32 Paused = 0;
		for (TActorIterator<ASpoutSenderActor> It(World); It; ++It) Paused += It->ShowPausedInEditor() ? 1 : 0;
		for (TActorIterator<ASpoutReceiverActor> It(World); It; ++It) Paused += It->ShowPausedInEditor() ? 1 : 0;
		if (Paused > 0 && !bPauseLogged)
		{
			bPauseLogged = true;
			UE_LOG(LogSpoutPlugin, Log, TEXT("Spout: no level viewport is Realtime, so Unreal does not update the level in the editor: %d Spout actor(s) sending or receiving there are paused. Turn Realtime on (Ctrl+R in a level viewport) to resume."), Paused);
		}
		return true;
	}

	/** As the editor decides each frame (UEditorEngine::Tick): the level's actors tick while a viewport showing it is Realtime. */
	static bool LevelTicksInEditor(const UWorld* World)
	{
		for (const FEditorViewportClient* Client : GEditor->GetAllViewportClients())
		{
			if (Client && Client->GetScene() == World->Scene && Client->IsRealtime()) return true;
		}
		return false;
	}

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

	static constexpr float PauseCheckSeconds = 0.25f;
	static constexpr double PauseLogRearmSeconds = 5.0;
	FTSTicker::FDelegateHandle PauseCheckHandle;
	double LevelTickingSince = 0.0;
	bool bPauseLogged = false;
};

IMPLEMENT_MODULE(FSpoutPluginEditorModule, SpoutPluginEditor)

#undef LOCTEXT_NAMESPACE
