// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"
#if SPOUT_WITH_TESTS && WITH_EDITOR
#include "SpoutBPFunctionLibrary.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"
#include "K2Node_Event.h"

namespace
{
	const TCHAR* const UnlitDisplayPath = TEXT("/SpoutPlugin/Content/M_SpoutDisplayUnlit.M_SpoutDisplayUnlit");

	/** True when Pin is fed by a getter of the Blueprint variable Variable. */
	bool FedByVariable(const UEdGraphPin* Pin, FName Variable)
	{
		if (!Pin) return false;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			const UK2Node_VariableGet* Get = Linked ? Cast<UK2Node_VariableGet>(Linked->GetOwningNode()) : nullptr;
			if (Get && Get->VariableReference.GetMemberName() == Variable) return true;
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP2ShippedAssets, "SpoutPlugin.P2.ShippedAssetsLoadAndCompile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSpoutP2ShippedAssets::RunTest(const FString&)
{
	FString SenderExampleName;
	for (const TCHAR* Name : {TEXT("BP_SpoutSenderExample_"),TEXT("BP_SpoutReceiverExample")})
	{
		const FString Path = FString::Printf(TEXT("/SpoutPlugin/Content/%s.%s"),Name,Name);
		TStrongObjectPtr<UBlueprint> Blueprint(LoadObject<UBlueprint>(nullptr,*Path));
		if (!TestNotNull(*Path,Blueprint.Get())) return false;
		FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
		TestTrue(TEXT("shipped Blueprint compiles"), Blueprint->Status != BS_Error && Blueprint->GeneratedClass != nullptr);
		TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs);
		bool HasEndPlay=false, HasTransfer=false, HasClose=false, HasMaterialCreation=false, HasSignal=false;
		bool NameOnTransfer=false, NameOnClose=false, TargetIsOptional=false;
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
						if (Sender)
						{
							if (const UEdGraphPin* NamePin=Call->FindPin(TEXT("SpoutName"))) SenderExampleName=NamePin->DefaultValue;
						}
						else
						{
							NameOnTransfer=FedByVariable(Call->FindPin(TEXT("SpoutName")),TEXT("SpoutName"));
							const UEdGraphPin* Target=Call->FindPin(TEXT("OptionalOutputRenderTarget"));
							TargetIsOptional=Target && !Target->DefaultObject && FedByVariable(Target,TEXT("OutputRenderTarget"));
						}
					}
					if (Function==(Sender ? TEXT("CloseSender") : TEXT("CloseReceiver")))
					{
						HasClose=true;
						if (!Sender) NameOnClose=FedByVariable(Call->FindPin(TEXT("SpoutName")),TEXT("SpoutName"));
					}
					if (Function==TEXT("CreateDynamicMaterialInstance"))
					{
						const UEdGraphPin* Source=Call->FindPin(TEXT("SourceMaterial"));
						HasMaterialCreation=Source && Source->DefaultObject && Source->DefaultObject->GetPathName()==UnlitDisplayPath;
					}
					if (Function==TEXT("SetScalarParameterValue"))
					{
						const UEdGraphPin* Parameter=Call->FindPin(TEXT("ParameterName"));
						HasSignal |= Parameter && Parameter->DefaultValue==TEXT("HasSignal");
					}
				}
			}
		}
		TestTrue(TEXT("example transfers and closes on EndPlay"), HasEndPlay && HasTransfer && HasClose);
		if (!Sender)
		{
			TestTrue(TEXT("receiver creates its consumer MID from the unlit display material"), HasMaterialCreation);
			TestTrue(TEXT("receiver shows the picture once a receive is accepted (HasSignal)"), HasSignal);
			TestTrue(TEXT("receiver receives and closes the name in its Spout Name variable"), NameOnTransfer && NameOnClose);
			TestTrue(TEXT("receiver passes the render target in its Output Render Target variable, not a fixed one"), TargetIsOptional);
			const UObject* Defaults=Blueprint->GeneratedClass->GetDefaultObject();
			const FNameProperty* NameProperty=FindFProperty<FNameProperty>(Blueprint->GeneratedClass,TEXT("SpoutName"));
			const FObjectPropertyBase* TargetProperty=FindFProperty<FObjectPropertyBase>(Blueprint->GeneratedClass,TEXT("OutputRenderTarget"));
			TestTrue(TEXT("Spout Name and Output Render Target are editable on a placed example"), NameProperty && TargetProperty
				&& NameProperty->HasAnyPropertyFlags(CPF_Edit) && !NameProperty->HasAnyPropertyFlags(CPF_DisableEditOnInstance)
				&& TargetProperty->HasAnyPropertyFlags(CPF_Edit) && !TargetProperty->HasAnyPropertyFlags(CPF_DisableEditOnInstance));
			if (NameProperty) TestEqual(TEXT("by default the receiver example receives the sender example's name"), NameProperty->GetPropertyValue_InContainer(Defaults).ToString(), SenderExampleName);
			if (TargetProperty) TestNull(TEXT("by default it passes no output render target"), TargetProperty->GetObjectPropertyValue_InContainer(Defaults));
		}
	}
	TestEqual(TEXT("the sender example sends UESpout_Test_Example"), SenderExampleName, FString(TEXT("UESpout_Test_Example")));
	TestNotNull(TEXT("shipped material"), LoadObject<UMaterial>(nullptr,TEXT("/SpoutPlugin/Content/M_SpoutMaterial.M_SpoutMaterial")));
	TestNotNull(TEXT("shipped display material"), LoadObject<UMaterial>(nullptr,UnlitDisplayPath));
	TestNotNull(TEXT("shipped sender target"), LoadObject<UTextureRenderTarget2D>(nullptr,TEXT("/SpoutPlugin/Content/RT_SpoutSenderTest_.RT_SpoutSenderTest_")));
	TestNotNull(TEXT("shipped receiver target"), LoadObject<UTextureRenderTarget2D>(nullptr,TEXT("/SpoutPlugin/Content/RT_SpoutReceiverTest.RT_SpoutReceiverTest")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP2ExamplesConnect, "SpoutPlugin.P2.ExamplesConnectWithTheirDefaults", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSpoutP2ExamplesConnect::RunTest(const FString&)
{
	// Both examples as shipped, in a game world: their BeginPlay, Tick and EndPlay run as in Play, with real transfers
	// through this process. The receiver example must find the sender example by its default name.
	const FName Name(TEXT("UESpout_Test_Example"));
	FSenderStruct Existing;
	if (USpoutBPFunctionLibrary::GetSenderInfo(Name, Existing))
	{
		AddWarning(TEXT("Another application sends UESpout_Test_Example; the examples cannot be tested against each other now."));
		return true;
	}
	UClass* SenderClass = LoadClass<AActor>(nullptr, TEXT("/SpoutPlugin/Content/BP_SpoutSenderExample_.BP_SpoutSenderExample__C"));
	UClass* ReceiverClass = LoadClass<AActor>(nullptr, TEXT("/SpoutPlugin/Content/BP_SpoutReceiverExample.BP_SpoutReceiverExample_C"));
	const UTextureRenderTarget2D* SenderTarget = LoadObject<UTextureRenderTarget2D>(nullptr, TEXT("/SpoutPlugin/Content/RT_SpoutSenderTest_.RT_SpoutSenderTest_"));
	if (!TestNotNull(TEXT("sender example"), SenderClass) || !TestNotNull(TEXT("receiver example"), ReceiverClass) || !TestNotNull(TEXT("sender example's render target"), SenderTarget)) return false;
	const FObjectPropertyBase* TextureProperty = FindFProperty<FObjectPropertyBase>(ReceiverClass, TEXT("SpoutReceivedTexture"));
	const FObjectPropertyBase* MaterialProperty = FindFProperty<FObjectPropertyBase>(ReceiverClass, TEXT("SpoutConsumerMaterial"));
	if (!TestNotNull(TEXT("received texture variable"), TextureProperty) || !TestNotNull(TEXT("consumer material variable"), MaterialProperty)) return false;

	// A game world with its own engine world context for the length of the test, as Play has. Actors spawned after
	// InitializeActorsForPlay are initialized as in Play, which End Play needs: Unreal routes it only to initialized
	// actors. Before UE 5.6 that call requires the world context.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); FlushRenderingCommands(); };
	World->InitializeActorsForPlay(FURL());
	// Blueprint events of a world that was not started by Play run only under this guard in the editor.
	FEditorScriptExecutionGuard ScriptGuard;
	AActor* Sender = World->SpawnActor<AActor>(SenderClass);
	AActor* Receiver = World->SpawnActor<AActor>(ReceiverClass);
	if (!TestNotNull(TEXT("sender example placed"), Sender) || !TestNotNull(TEXT("receiver example placed"), Receiver)) return false;
	Sender->DispatchBeginPlay();
	Receiver->DispatchBeginPlay();

	const FIntPoint Expected(SenderTarget->SizeX, SenderTarget->SizeY);
	UTexture2D* Received = nullptr;
	for (int32 Attempt = 0; Attempt < 300; ++Attempt)
	{
		Sender->Tick(1.f / 60.f);
		FlushRenderingCommands();
		Receiver->Tick(1.f / 60.f);
		FlushRenderingCommands();
		Received = Cast<UTexture2D>(TextureProperty->GetObjectPropertyValue_InContainer(Receiver));
		if (Received && Received->GetSizeX() == Expected.X && Received->GetSizeY() == Expected.Y) break;
		FPlatformProcess::Sleep(0.005f);
	}
	if (!TestNotNull(TEXT("the receiver example receives the sender example"), Received)) return false;
	TestEqual(TEXT("at the size of the sender example's render target"), FIntPoint(Received->GetSizeX(), Received->GetSizeY()), Expected);
	UMaterialInstanceDynamic* Display = Cast<UMaterialInstanceDynamic>(MaterialProperty->GetObjectPropertyValue_InContainer(Receiver));
	if (TestNotNull(TEXT("the receiver example's display material"), Display))
	{
		TestEqual(TEXT("made from the unlit display material"), Display->Parent ? Display->Parent->GetPathName() : FString(), FString(UnlitDisplayPath));
		TestEqual(TEXT("showing the picture (HasSignal)"), Display->K2_GetScalarParameterValue(TEXT("HasSignal")), 1.f);
		TestTrue(TEXT("bound to the received texture"), Display->K2_GetTextureParameterValue(TEXT("SpoutTexture")) == Received);
	}

	// End Play closes both names.
	Receiver->Destroy();
	Sender->Destroy();
	FlushRenderingCommands();
	TestFalse(TEXT("the receiver example closed its receiver"), USpoutBPFunctionLibrary::GetSpoutStreamStatus(Name, ESpoutType::Receiver).bActive);
	TestFalse(TEXT("the sender example closed its sender"), USpoutBPFunctionLibrary::GetSpoutStreamStatus(Name, ESpoutType::Sender).bActive);
	return true;
}
#endif
