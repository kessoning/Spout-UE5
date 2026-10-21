// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5
#include "SpoutAsyncBetaAssetCommandlet.h"
#include "SpoutAsyncBetaAssetToolShared.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/LinkerLoad.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

USpoutAsyncBetaAssetCommandlet::USpoutAsyncBetaAssetCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

namespace SpoutAsyncBetaAsset
{
	static TSharedRef<FJsonObject> DescribeVariables(UBlueprint* BP)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Vars;
		for (const FBPVariableDescription& V : BP->NewVariables)
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("name"), V.VarName.ToString());
			O->SetStringField(TEXT("friendlyName"), V.FriendlyName);
			O->SetStringField(TEXT("category"), V.Category.ToString());
			O->SetStringField(TEXT("type"), V.VarType.PinCategory.ToString());
			O->SetStringField(TEXT("defaultValue"), V.DefaultValue);
			O->SetBoolField(TEXT("instanceEditable"), (V.PropertyFlags & CPF_Edit) != 0 && (V.PropertyFlags & CPF_DisableEditOnInstance) == 0);
			O->SetBoolField(TEXT("blueprintReadOnly"), (V.PropertyFlags & CPF_BlueprintReadOnly) != 0);
			O->SetStringField(TEXT("tooltip"), V.HasMetaData(FBlueprintMetadata::MD_Tooltip) ? V.GetMetaData(FBlueprintMetadata::MD_Tooltip) : FString());
			Vars.Add(MakeShared<FJsonValueObject>(O));
		}
		Out->SetArrayField(TEXT("variables"), Vars);
		return Out;
	}

	static TSharedRef<FJsonObject> DescribeProperty(UBlueprint* BP)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		UClass* Cls = BP->GeneratedClass;
		FProperty* P = Cls ? Cls->FindPropertyByName(VarName) : nullptr;
		O->SetBoolField(TEXT("exists"), P != nullptr);
		if (P)
		{
			O->SetBoolField(TEXT("cpfEdit"), P->HasAnyPropertyFlags(CPF_Edit));
			O->SetBoolField(TEXT("disableEditOnInstance"), P->HasAnyPropertyFlags(CPF_DisableEditOnInstance));
			O->SetBoolField(TEXT("blueprintVisible"), P->HasAnyPropertyFlags(CPF_BlueprintVisible));
			O->SetStringField(TEXT("displayNameMetadata"), P->GetMetaData(TEXT("DisplayName")));
			O->SetStringField(TEXT("detailsDisplayName"), P->GetDisplayNameText().ToString());
			O->SetStringField(TEXT("category"), P->GetMetaData(TEXT("Category")));
			O->SetStringField(TEXT("tooltip"), P->GetMetaData(TEXT("ToolTip")));
			if (const FBoolProperty* B = CastField<FBoolProperty>(P))
			{
				O->SetBoolField(TEXT("classDefault"), B->GetPropertyValue_InContainer(Cls->GetDefaultObject()));
			}
		}
		return O;
	}

	static TArray<TSharedPtr<FJsonValue>> DescribeReceiveNodes(UBlueprint* BP)
	{
		TArray<TSharedPtr<FJsonValue>> Nodes;
		TArray<UK2Node_CallFunction*> Calls;
		FBlueprintEditorUtils::GetAllNodesOfClass(BP, Calls);
		for (UK2Node_CallFunction* N : Calls)
		{
			const FName Fn = N->FunctionReference.GetMemberName();
			if (Fn != FName(TEXT("SpoutReceiverWithMaterial")) && Fn != FName(TEXT("SpoutReceiver"))) continue;
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("function"), Fn.ToString());
			O->SetStringField(TEXT("graph"), N->GetGraph() ? N->GetGraph()->GetName() : FString());
			UEdGraphPin* Pin = N->FindPin(PinName);
			O->SetBoolField(TEXT("hasAsyncReceivePin"), Pin != nullptr);
			if (Pin)
			{
				O->SetStringField(TEXT("pinDefaultValue"), Pin->DefaultValue);
				O->SetStringField(TEXT("pinDisplayName"), N->GetPinDisplayName(Pin).ToString());
				O->SetBoolField(TEXT("pinAdvanced"), Pin->bAdvancedView);
				TArray<TSharedPtr<FJsonValue>> Links;
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					UEdGraphNode* Owner = Linked ? Linked->GetOwningNode() : nullptr;
					FString What = Owner ? Owner->GetClass()->GetName() : TEXT("?");
					if (const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Owner))
					{
						What += TEXT(":") + Get->VariableReference.GetMemberName().ToString();
					}
					Links.Add(MakeShared<FJsonValueString>(What));
				}
				O->SetArrayField(TEXT("pinLinkedTo"), Links);
			}
			Nodes.Add(MakeShared<FJsonValueObject>(O));
		}
		return Nodes;
	}

	static void Compile(UBlueprint* BP, const TSharedRef<FJsonObject>& Into)
	{
		FCompilerResultsLog Results;
		Results.SetSourcePath(BP->GetPathName());
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
		Into->SetNumberField(TEXT("compileErrors"), Results.NumErrors);
		Into->SetNumberField(TEXT("compileWarnings"), Results.NumWarnings);
		Into->SetBoolField(TEXT("upToDate"), BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings);
		TArray<TSharedPtr<FJsonValue>> Lines;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			Lines.Add(MakeShared<FJsonValueString>(Message->ToText().ToString()));
		}
		Into->SetArrayField(TEXT("compileMessages"), Lines);
	}

	static TSharedRef<FJsonObject> Describe(UBlueprint* BP)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("asset"), BP->GetPathName());
		J->SetStringField(TEXT("runningEngine"), FEngineVersion::Current().ToString());
		if (FLinkerLoad* Linker = BP->GetLinker())
		{
			J->SetStringField(TEXT("packageSavedByEngine"), Linker->Summary.SavedByEngineVersion.ToString());
			J->SetNumberField(TEXT("packageFileVersionUE5"), Linker->Summary.GetFileVersionUE().FileVersionUE5);
		}
		J->SetObjectField(TEXT("asLoaded"), DescribeVariables(BP));
		J->SetArrayField(TEXT("receiveNodesAsLoaded"), DescribeReceiveNodes(BP));
		// Reconstructed against the plugin compiled in this process: what an old graph becomes when it is opened.
		FBlueprintEditorUtils::RefreshAllNodes(BP);
		J->SetArrayField(TEXT("receiveNodesReconstructed"), DescribeReceiveNodes(BP));
		Compile(BP, J);
		J->SetObjectField(TEXT("asyncReceiveBetaProperty"), DescribeProperty(BP));
		return J;
	}

	static bool Migrate(UBlueprint* BP, const TSharedRef<FJsonObject>& J)
	{
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, VarName) != INDEX_NONE)
		{
			J->SetStringField(TEXT("problem"), TEXT("the variable already exists: the example was migrated before"));
			return false;
		}
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		if (!FBlueprintEditorUtils::AddMemberVariable(BP, VarName, Type, TEXT("false")))
		{
			J->SetStringField(TEXT("problem"), TEXT("AddMemberVariable failed"));
			return false;
		}
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, VarName);
		FBPVariableDescription& Var = BP->NewVariables[Index];
		Var.FriendlyName = FriendlyName;
		Var.PropertyFlags |= CPF_Edit | CPF_BlueprintVisible;
		Var.PropertyFlags &= ~(CPF_DisableEditOnInstance | CPF_BlueprintReadOnly);
		FBlueprintEditorUtils::SetBlueprintVariableCategory(BP, VarName, nullptr, FText::FromString(Category), true);
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, VarName, nullptr, FBlueprintMetadata::MD_Tooltip, Tooltip);

		// The receive nodes as the current plugin declares them, then the variable into their new input.
		FBlueprintEditorUtils::RefreshAllNodes(BP);
		TArray<UK2Node_CallFunction*> Calls;
		FBlueprintEditorUtils::GetAllNodesOfClass(BP, Calls);
		int32 Connected = 0;
		for (UK2Node_CallFunction* N : Calls)
		{
			const FName Fn = N->FunctionReference.GetMemberName();
			if (Fn != FName(TEXT("SpoutReceiverWithMaterial")) && Fn != FName(TEXT("SpoutReceiver"))) continue;
			UEdGraphPin* Pin = N->FindPin(PinName);
			UEdGraph* Graph = N->GetGraph();
			if (!Pin || !Graph)
			{
				J->SetStringField(TEXT("problem"), TEXT("a receive node has no Async Receive (Beta) input: the plugin compiled in this process is not the new one"));
				return false;
			}
			FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
			UK2Node_VariableGet* Get = Creator.CreateNode(false);
			Get->VariableReference.SetSelfMember(VarName);
			Get->NodePosX = N->NodePosX - 320;
			Get->NodePosY = N->NodePosY + 260;
			Creator.Finalize();
			if (!GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Get->GetValuePin(), Pin))
			{
				J->SetStringField(TEXT("problem"), TEXT("the variable could not be connected to the Async Receive (Beta) input"));
				return false;
			}
			N->AdvancedPinDisplay = ENodeAdvancedPins::Shown;
			++Connected;
		}
		J->SetNumberField(TEXT("receiveNodesConnected"), Connected);
		if (Connected == 0)
		{
			J->SetStringField(TEXT("problem"), TEXT("the example has no receive node"));
			return false;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
		Compile(BP, After);
		After->SetObjectField(TEXT("variables"), DescribeVariables(BP));
		After->SetArrayField(TEXT("receiveNodes"), DescribeReceiveNodes(BP));
		After->SetObjectField(TEXT("asyncReceiveBetaProperty"), DescribeProperty(BP));
		J->SetObjectField(TEXT("afterMigration"), After);
		if (After->GetNumberField(TEXT("compileErrors")) != 0 || !After->GetBoolField(TEXT("upToDate")))
		{
			J->SetStringField(TEXT("problem"), TEXT("the migrated example does not compile cleanly"));
			return false;
		}
		UPackage* Package = BP->GetOutermost();
		Package->MarkPackageDirty();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, BP, *File, Args);
		J->SetStringField(TEXT("savedFile"), File);
		J->SetBoolField(TEXT("saved"), bSaved);
		if (!bSaved) J->SetStringField(TEXT("problem"), TEXT("the package could not be saved"));
		return bSaved;
	}

	static TArray<TSharedPtr<FJsonValue>> DescribeLevel(UWorld* World)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		if (!World) return Out;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (!A->GetClass()->GetName().StartsWith(TEXT("BP_SpoutReceiverExample"))) continue;
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("actor"), A->GetName());
			O->SetStringField(TEXT("label"), A->GetActorLabel());
			if (const FBoolProperty* B = FindFProperty<FBoolProperty>(A->GetClass(), VarName)) O->SetBoolField(TEXT("asyncReceiveBeta"), B->GetPropertyValue_InContainer(A));
			else O->SetStringField(TEXT("asyncReceiveBeta"), TEXT("no such property"));
			Out.Add(MakeShared<FJsonValueObject>(O));
		}
		return Out;
	}

	/**
	 * One example actor in a new level, saved. The example receives the fixed name "Spout", so a level holds one: with
	 * bAsync its Async Receive (Beta) is set to true on the placed instance; without it the instance is left at the
	 * class default (false), so the saved level also shows that default.
	 */
	static bool MakeLevel(const FString& MapPath, bool bAsync, const FString& Label, const TSharedRef<FJsonObject>& J)
	{
		UClass* Cls = LoadClass<AActor>(nullptr, ExampleClassPath);
		if (!Cls || !FindFProperty<FBoolProperty>(Cls, VarName))
		{
			J->SetStringField(TEXT("problem"), TEXT("the example class or its Async Receive (Beta) variable is missing"));
			return false;
		}
		UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
		if (!World)
		{
			J->SetStringField(TEXT("problem"), TEXT("no new level"));
			return false;
		}
		AActor* A = World->SpawnActor<AActor>(Cls, FTransform(FVector(400, 0, 100)));
		if (!A)
		{
			J->SetStringField(TEXT("problem"), TEXT("the example actor could not be placed"));
			return false;
		}
		A->Modify();
		A->SetActorLabel(Label);
		if (bAsync)
		{
			FindFProperty<FBoolProperty>(Cls, VarName)->SetPropertyValue_InContainer(A, true);
		}
		J->SetBoolField(TEXT("instanceSetToAsync"), bAsync);
		J->SetArrayField(TEXT("placed"), DescribeLevel(World));
		const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(World, MapPath);
		J->SetBoolField(TEXT("saved"), bSaved);
		J->SetStringField(TEXT("map"), MapPath);
		if (!bSaved) J->SetStringField(TEXT("problem"), TEXT("the level could not be saved"));
		return bSaved;
	}
}

int32 USpoutAsyncBetaAssetCommandlet::Main(const FString& Params)
{
	using namespace SpoutAsyncBetaAsset;
	FString Mode, OutPath, MapPath, Label = TEXT("AsyncBetaReceiver");
	FParse::Value(*Params, TEXT("Mode="), Mode);
	FParse::Value(*Params, TEXT("Label="), Label);
	const bool bAsyncInstance = FParse::Param(*Params, TEXT("AsyncOn"));
	FParse::Value(*Params, TEXT("Out="), OutPath);
	FParse::Value(*Params, TEXT("Map="), MapPath);
	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetStringField(TEXT("mode"), Mode);
	bool bOk = false;
	if (Mode == TEXT("MakeLevel"))
	{
		bOk = !MapPath.IsEmpty() && MakeLevel(MapPath, bAsyncInstance, Label, J);
	}
	else
	{
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, ExampleAssetPath);
		if (!BP)
		{
			J->SetStringField(TEXT("problem"), TEXT("the example Blueprint could not be loaded"));
		}
		else if (Mode == TEXT("Inspect"))
		{
			J->SetObjectField(TEXT("example"), Describe(BP));
			bOk = J->GetObjectField(TEXT("example"))->GetNumberField(TEXT("compileErrors")) == 0;
			if (!MapPath.IsEmpty())
			{
				UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(MapPath);
				J->SetArrayField(TEXT("levelActors"), DescribeLevel(World));
				J->SetBoolField(TEXT("levelLoaded"), World != nullptr);
				bOk = bOk && World != nullptr;
			}
		}
		else if (Mode == TEXT("Migrate"))
		{
			J->SetObjectField(TEXT("before"), Describe(BP));
			bOk = Migrate(BP, J);
		}
		else
		{
			J->SetStringField(TEXT("problem"), TEXT("unknown -Mode (Inspect, Migrate or MakeLevel)"));
		}
	}
	J->SetBoolField(TEXT("ok"), bOk);
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(J, Writer);
	if (!OutPath.IsEmpty()) FFileHelper::SaveStringToFile(Text, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Display, TEXT("SpoutAsyncBetaAsset %s: %s"), *Mode, bOk ? TEXT("ok") : TEXT("FAILED"));
	return bOk ? 0 : 1;
}
