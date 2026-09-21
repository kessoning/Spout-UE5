// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/Material.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_CallFunction.h"
#include "UObject/StrongObjectPtr.h"
#include "K2Node_Event.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP2ShippedAssets, "SpoutPlugin.P2.ShippedAssetsLoadAndCompile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSpoutP2ShippedAssets::RunTest(const FString&)
{
	for (const TCHAR* Name : {TEXT("BP_SpoutSenderExample_"),TEXT("BP_SpoutReceiverExample")})
	{
		const FString Path = FString::Printf(TEXT("/SpoutPlugin/Content/%s.%s"),Name,Name);
		TStrongObjectPtr<UBlueprint> Blueprint(LoadObject<UBlueprint>(nullptr,*Path));
		if (!TestNotNull(*Path,Blueprint.Get())) return false;
		FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
		TestTrue(TEXT("shipped Blueprint compiles"), Blueprint->Status != BS_Error && Blueprint->GeneratedClass != nullptr);
		TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs);
		bool HasEndPlay=false, HasTransfer=false, HasClose=false, HasMaterialCreation=false;
		const bool Sender=Blueprint->GetName()==TEXT("BP_SpoutSenderExample_");
		for (const UEdGraph* Graph : Graphs)
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (const auto* Event=Cast<UK2Node_Event>(Node))
					HasEndPlay |= Event->EventReference.GetMemberName()==TEXT("ReceiveEndPlay");
				if (const auto* Call = Cast<UK2Node_CallFunction>(Node))
				{
					AddInfo(FString::Printf(TEXT("Example %s calls %s"),Name,*Call->FunctionReference.GetMemberName().ToString()));
					const FName Function=Call->FunctionReference.GetMemberName();
					if (Function==(Sender ? TEXT("SpoutSender") : TEXT("SpoutReceiverWithMaterial")))
					{
						HasTransfer=true;
						const UEdGraphPin* Input=Call->FindPin(Sender ? TEXT("TextureRenderTarget2D") : TEXT("ConsumerMaterial"));
						TestTrue(TEXT("example transfer has its required source input"), Input && (Input->DefaultObject || Input->LinkedTo.Num()>0));
					}
					HasClose |= Function==(Sender ? TEXT("CloseSender") : TEXT("CloseReceiver"));
					if (Function==TEXT("CreateDynamicMaterialInstance"))
					{
						const UEdGraphPin* Source=Call->FindPin(TEXT("SourceMaterial"));
						HasMaterialCreation=Source && Source->DefaultObject;
					}
				}
			}
		}
		TestTrue(TEXT("example transfers and closes on EndPlay"), HasEndPlay && HasTransfer && HasClose);
		if (!Sender) TestTrue(TEXT("receiver creates its consumer MID from the shipped material"), HasMaterialCreation);
	}
	TestNotNull(TEXT("shipped material"), LoadObject<UMaterial>(nullptr,TEXT("/SpoutPlugin/Content/M_SpoutMaterial.M_SpoutMaterial")));
	TestNotNull(TEXT("shipped sender target"), LoadObject<UTextureRenderTarget2D>(nullptr,TEXT("/SpoutPlugin/Content/RT_SpoutSenderTest_.RT_SpoutSenderTest_")));
	TestNotNull(TEXT("shipped receiver target"), LoadObject<UTextureRenderTarget2D>(nullptr,TEXT("/SpoutPlugin/Content/RT_SpoutReceiverTest.RT_SpoutReceiverTest")));
	return true;
}
#endif
