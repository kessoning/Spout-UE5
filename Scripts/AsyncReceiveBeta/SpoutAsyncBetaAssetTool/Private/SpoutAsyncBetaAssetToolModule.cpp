// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5
#include "SpoutAsyncBetaAssetToolShared.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "IDetailsView.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyPath.h"
#include "RenderingThread.h"
#include "Slate/WidgetRenderer.h"
#include "TextureResource.h"
#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SVirtualWindow.h"

/**
 * SpoutAsyncBeta.DetailsShot <map> <actor label> <out.png> [quit]
 *
 * Opens the level in the running editor, builds the editor's own Details panel (the PropertyEditor module's details
 * view, the widget the Level Editor shows) for the labelled actor, in a virtual window that Slate ticks like any other
 * window (so the tree builds its expanded rows), renders that real widget and saves it as a PNG, then optionally quits.
 * Needs the full editor (Slate and an RHI), not a commandlet. Used only to capture screenshots.
 */
namespace SpoutAsyncBetaDetailsShot
{
	/** One capture in progress: the details view lives in a virtual window that Slate ticks, so its rows are built. */
	struct FShot
	{
		TSharedPtr<SVirtualWindow> Window;
		TSharedPtr<IDetailsView> View;
		TWeakObjectPtr<AActor> Actor;
		FString OutPath;
		bool bQuit = false;
		int32 Frames = 0;
	};
	static TUniquePtr<FShot> GShot;
	static const FVector2D ShotSize(720.0f, 420.0f);

	static void Finish()
	{
		FWidgetRenderer Renderer(true, true);
		UTextureRenderTarget2D* Target = FWidgetRenderer::CreateTargetFor(ShotSize, TF_Bilinear, true);
		Target->AddToRoot();
		for (int32 Pass = 0; Pass < 3; ++Pass)
		{
			Renderer.DrawWindow(Target, GShot->Window->GetHittestGrid(), GShot->Window.ToSharedRef(), 1.0f, ShotSize, 0.016f);
			FlushRenderingCommands();
		}
		TArray<FColor> Pixels;
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		const bool bRead = Resource && Resource->ReadPixels(Pixels) && Pixels.Num() == int32(ShotSize.X) * int32(ShotSize.Y);
		Target->RemoveFromRoot();
		bool bSaved = false;
		if (bRead)
		{
			for (FColor& C : Pixels) C.A = 255;
			bSaved = FImageUtils::SaveImageByExtension(*GShot->OutPath, FImageView(Pixels.GetData(), int32(ShotSize.X), int32(ShotSize.Y)));
		}
		AActor* Actor = GShot->Actor.Get();
		const FBoolProperty* Setting = Actor ? FindFProperty<FBoolProperty>(Actor->GetClass(), SpoutAsyncBetaAsset::VarName) : nullptr;
		UE_LOG(LogTemp, Display, TEXT("SpoutAsyncBeta.DetailsShot: %s %s after %d ticked frames (Async Receive (Beta) on this instance = %s)"),
			bSaved ? TEXT("saved") : TEXT("could NOT save"), *GShot->OutPath, GShot->Frames,
			Setting ? (Setting->GetPropertyValue_InContainer(Actor) ? TEXT("true") : TEXT("false")) : TEXT("absent"));
		FSlateApplication::Get().UnregisterVirtualWindow(GShot->Window.ToSharedRef());
		const bool bQuit = GShot->bQuit;
		GShot.Reset();
		if (bQuit) FPlatformMisc::RequestExit(false);
	}

	static bool Tick(float)
	{
		if (!GShot) return false;
		// The Experimental sub-group starts collapsed: once the tree is built, open the path to the setting the way the
		// editor does when it points at a property (the row is shown highlighted).
		if (++GShot->Frames == 10)
		{
			AActor* Actor = GShot->Actor.Get();
			if (FProperty* Setting = Actor ? FindFProperty<FProperty>(Actor->GetClass(), SpoutAsyncBetaAsset::VarName) : nullptr)
			{
				GShot->View->HighlightProperty(*FPropertyPath::CreateEmpty()->ExtendPath(FPropertyInfo(Setting)));
			}
		}
		if (GShot->Frames < 45) return true;
		Finish();
		return false;
	}

	static void Run(const TArray<FString>& Args)
	{
		if (Args.Num() < 3 || GShot)
		{
			UE_LOG(LogTemp, Error, TEXT("SpoutAsyncBeta.DetailsShot <map> <actor label> <out.png> [quit] (one at a time)"));
			return;
		}
		UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(Args[0]);
		AActor* Actor = nullptr;
		for (TActorIterator<AActor> It(World); World && It; ++It)
		{
			if ((*It)->GetActorLabel() == Args[1]) { Actor = *It; break; }
		}
		if (!Actor)
		{
			UE_LOG(LogTemp, Error, TEXT("SpoutAsyncBeta.DetailsShot: no actor labelled '%s' in %s"), *Args[1], *Args[0]);
			return;
		}
		FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		FDetailsViewArgs ViewArgs;
		ViewArgs.bAllowSearch = false;
		ViewArgs.bHideSelectionTip = true;
		ViewArgs.NameAreaSettings = FDetailsViewArgs::ObjectsUseNameArea;
		GShot = MakeUnique<FShot>();
		GShot->View = PropertyEditor.CreateDetailView(ViewArgs);
		GShot->View->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateLambda([](const FPropertyAndParent& Item)
		{
			return Item.Property.GetMetaData(TEXT("Category")).StartsWith(TEXT("Spout"));
		}));
		GShot->View->SetObject(Actor, true);
		GShot->Window = SNew(SVirtualWindow).Size(ShotSize);
		GShot->Window->SetContent(GShot->View.ToSharedRef());
		FSlateApplication::Get().RegisterVirtualWindow(GShot->Window.ToSharedRef());
		GShot->Actor = Actor;
		GShot->OutPath = Args[2];
		GShot->bQuit = Args.Num() > 3 && Args[3] == TEXT("quit");
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
	}

	static FAutoConsoleCommand Command(TEXT("SpoutAsyncBeta.DetailsShot"), TEXT("SpoutAsyncBeta.DetailsShot <map> <actor label> <out.png> [quit]: renders the editor's Details panel of that actor (its Spout categories) to a PNG; with quit, exits the editor afterwards."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
IMPLEMENT_MODULE(FDefaultModuleImpl, SpoutAsyncBetaAssetTool);
