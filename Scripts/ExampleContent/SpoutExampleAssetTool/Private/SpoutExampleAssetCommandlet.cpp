// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5
#include "SpoutExampleAssetCommandlet.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/StaticMeshComponent.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/Material.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/LinkerLoad.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

USpoutExampleAssetCommandlet::USpoutExampleAssetCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

namespace SpoutExampleAsset
{
	const TCHAR* const SenderPath = TEXT("/SpoutPlugin/Content/BP_SpoutSenderExample_.BP_SpoutSenderExample_");
	const TCHAR* const ReceiverPath = TEXT("/SpoutPlugin/Content/BP_SpoutReceiverExample.BP_SpoutReceiverExample");
	const TCHAR* const MaterialPaths[] = { TEXT("/SpoutPlugin/Content/M_SpoutMaterial.M_SpoutMaterial"), TEXT("/SpoutPlugin/Content/M_SpoutDisplayUnlit.M_SpoutDisplayUnlit") };
	const TCHAR* const TargetPaths[] = { TEXT("/SpoutPlugin/Content/RT_SpoutSenderTest_.RT_SpoutSenderTest_"), TEXT("/SpoutPlugin/Content/RT_SpoutReceiverTest.RT_SpoutReceiverTest") };

	static FString ObjectPath(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	/** The properties of Object that differ from Archetype (all of them with no archetype), as Unreal writes them in text. */
	static TSharedRef<FJsonObject> DescribeProperties(UObject* Object, UObject* Archetype, bool bOwnOnly)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Object) return Out;
		for (TFieldIterator<FProperty> It(Object->GetClass(), bOwnOnly ? EFieldIteratorFlags::ExcludeSuper : EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!bOwnOnly && !Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)) continue;
			if (Archetype && Property->Identical_InContainer(Object, Archetype)) continue;
			FString Text;
			Property->ExportTextItem_InContainer(Text, Object, nullptr, Object, PPF_None);
			Out->SetStringField(Property->GetName(), Text.Left(600));
		}
		return Out;
	}

	static TSharedRef<FJsonObject> DescribePin(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), Pin->PinName.ToString());
		O->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
		O->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		if (const UObject* Sub = Pin->PinType.PinSubCategoryObject.Get()) O->SetStringField(TEXT("subCategoryObject"), Sub->GetName());
		if (!Pin->DefaultValue.IsEmpty()) O->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
		if (Pin->DefaultObject) O->SetStringField(TEXT("defaultObject"), ObjectPath(Pin->DefaultObject));
		if (!Pin->DefaultTextValue.IsEmpty()) O->SetStringField(TEXT("defaultText"), Pin->DefaultTextValue.ToString());
		if (Pin->bHidden) O->SetBoolField(TEXT("hidden"), true);
		TArray<TSharedPtr<FJsonValue>> Links;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			const UEdGraphNode* Owner = Linked ? Linked->GetOwningNode() : nullptr;
			Links.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s.%s"), Owner ? *Owner->GetName() : TEXT("?"), Linked ? *Linked->PinName.ToString() : TEXT("?"))));
		}
		if (Links.Num() > 0) O->SetArrayField(TEXT("linkedTo"), Links);
		return O;
	}

	static TSharedRef<FJsonObject> DescribeNode(const UEdGraphNode* Node)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), Node->GetName());
		O->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		O->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		if (!Node->NodeComment.IsEmpty()) O->SetStringField(TEXT("comment"), Node->NodeComment);
		O->SetNumberField(TEXT("x"), Node->NodePosX);
		O->SetNumberField(TEXT("y"), Node->NodePosY);
		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			O->SetStringField(TEXT("function"), Call->FunctionReference.GetMemberName().ToString());
			if (const UClass* Parent = Call->FunctionReference.GetMemberParentClass()) O->SetStringField(TEXT("functionClass"), Parent->GetName());
		}
		if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
		{
			O->SetStringField(TEXT("event"), Event->EventReference.GetMemberName().ToString());
		}
		if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
		{
			O->SetStringField(TEXT("variable"), Variable->VariableReference.GetMemberName().ToString());
		}
		TArray<TSharedPtr<FJsonValue>> Pins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin) Pins.Add(MakeShared<FJsonValueObject>(DescribePin(Pin)));
		}
		O->SetArrayField(TEXT("pins"), Pins);
		return O;
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

	static TSharedRef<FJsonObject> DescribeBlueprint(UBlueprint* BP)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("asset"), BP->GetPathName());
		J->SetStringField(TEXT("parentClass"), BP->ParentClass ? BP->ParentClass->GetName() : FString());
		if (FLinkerLoad* Linker = BP->GetLinker())
		{
			J->SetStringField(TEXT("packageSavedByEngine"), Linker->Summary.SavedByEngineVersion.ToString());
		}
		TArray<TSharedPtr<FJsonValue>> Variables;
		for (const FBPVariableDescription& V : BP->NewVariables)
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("name"), V.VarName.ToString());
			O->SetStringField(TEXT("friendlyName"), V.FriendlyName);
			O->SetStringField(TEXT("category"), V.Category.ToString());
			O->SetStringField(TEXT("type"), V.VarType.PinCategory.ToString());
			if (const UObject* Sub = V.VarType.PinSubCategoryObject.Get()) O->SetStringField(TEXT("typeObject"), Sub->GetName());
			O->SetStringField(TEXT("defaultValue"), V.DefaultValue);
			O->SetBoolField(TEXT("instanceEditable"), (V.PropertyFlags & CPF_Edit) != 0 && (V.PropertyFlags & CPF_DisableEditOnInstance) == 0);
			O->SetBoolField(TEXT("blueprintReadOnly"), (V.PropertyFlags & CPF_BlueprintReadOnly) != 0);
			O->SetStringField(TEXT("tooltip"), V.HasMetaData(FBlueprintMetadata::MD_Tooltip) ? V.GetMetaData(FBlueprintMetadata::MD_Tooltip) : FString());
			Variables.Add(MakeShared<FJsonValueObject>(O));
		}
		J->SetArrayField(TEXT("variables"), Variables);
		TArray<UEdGraph*> Graphs;
		BP->GetAllGraphs(Graphs);
		TArray<TSharedPtr<FJsonValue>> GraphsOut;
		for (const UEdGraph* Graph : Graphs)
		{
			TSharedRef<FJsonObject> G = MakeShared<FJsonObject>();
			G->SetStringField(TEXT("name"), Graph->GetName());
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node) Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(Node)));
			}
			G->SetArrayField(TEXT("nodes"), Nodes);
			GraphsOut.Add(MakeShared<FJsonValueObject>(G));
		}
		J->SetArrayField(TEXT("graphs"), GraphsOut);
		TArray<TSharedPtr<FJsonValue>> Components;
		if (BP->SimpleConstructionScript)
		{
			for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
			{
				if (!Node) continue;
				TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
				C->SetStringField(TEXT("variable"), Node->GetVariableName().ToString());
				UActorComponent* Template = Node->ComponentTemplate;
				C->SetStringField(TEXT("class"), Template ? Template->GetClass()->GetName() : FString());
				C->SetStringField(TEXT("parent"), Node->ParentComponentOrVariableName.ToString());
				if (Template) C->SetObjectField(TEXT("changed"), DescribeProperties(Template, Template->GetClass()->GetDefaultObject(), false));
				Components.Add(MakeShared<FJsonValueObject>(C));
			}
		}
		J->SetArrayField(TEXT("components"), Components);
		if (UClass* Generated = BP->GeneratedClass)
		{
			J->SetObjectField(TEXT("classDefaults"), DescribeProperties(Generated->GetDefaultObject(), nullptr, true));
		}
		Compile(BP, J);
		return J;
	}

	static TSharedRef<FJsonObject> DescribeMaterial(UMaterial* M)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("asset"), M->GetPathName());
		J->SetStringField(TEXT("domain"), StaticEnum<EMaterialDomain>()->GetNameStringByValue(M->MaterialDomain));
		J->SetStringField(TEXT("blendMode"), StaticEnum<EBlendMode>()->GetNameStringByValue(M->BlendMode));
		J->SetBoolField(TEXT("unlit"), M->GetShadingModels().IsUnlit());
		J->SetBoolField(TEXT("twoSided"), M->IsTwoSided());
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		TSharedRef<FJsonObject> Scalars = MakeShared<FJsonObject>();
		M->GetAllScalarParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			float Value = 0.0f;
			M->GetScalarParameterDefaultValue(Info, Value);
			Scalars->SetNumberField(Info.Name.ToString(), Value);
		}
		J->SetObjectField(TEXT("scalars"), Scalars);
		TSharedRef<FJsonObject> Vectors = MakeShared<FJsonObject>();
		M->GetAllVectorParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			FLinearColor Value;
			M->GetVectorParameterDefaultValue(Info, Value);
			Vectors->SetStringField(Info.Name.ToString(), Value.ToString());
		}
		J->SetObjectField(TEXT("vectors"), Vectors);
		TSharedRef<FJsonObject> Textures = MakeShared<FJsonObject>();
		M->GetAllTextureParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			UTexture* Value = nullptr;
			M->GetTextureParameterDefaultValue(Info, Value);
			Textures->SetStringField(Info.Name.ToString(), ObjectPath(Value));
		}
		J->SetObjectField(TEXT("textures"), Textures);
		return J;
	}

	static TSharedRef<FJsonObject> DescribeTarget(UTextureRenderTarget2D* Target)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("asset"), Target->GetPathName());
		J->SetStringField(TEXT("renderTargetFormat"), StaticEnum<ETextureRenderTargetFormat>()->GetNameStringByValue(Target->RenderTargetFormat));
		J->SetStringField(TEXT("pixelFormat"), GetPixelFormatString(Target->GetFormat()));
		J->SetNumberField(TEXT("sizeX"), Target->SizeX);
		J->SetNumberField(TEXT("sizeY"), Target->SizeY);
		J->SetBoolField(TEXT("autoGenerateMips"), Target->bAutoGenerateMips != 0);
		J->SetBoolField(TEXT("srgb"), Target->SRGB != 0);
		J->SetBoolField(TEXT("forceLinearGamma"), Target->bForceLinearGamma != 0);
		return J;
	}

	static bool Inspect(const TSharedRef<FJsonObject>& J)
	{
		bool bOk = true;
		TArray<TSharedPtr<FJsonValue>> Blueprints;
		for (const TCHAR* Path : { SenderPath, ReceiverPath })
		{
			UBlueprint* BP = LoadObject<UBlueprint>(nullptr, Path);
			if (!BP)
			{
				J->SetStringField(TEXT("problem"), FString::Printf(TEXT("%s could not be loaded"), Path));
				return false;
			}
			const TSharedRef<FJsonObject> Described = DescribeBlueprint(BP);
			bOk &= Described->GetNumberField(TEXT("compileErrors")) == 0;
			Blueprints.Add(MakeShared<FJsonValueObject>(Described));
		}
		J->SetArrayField(TEXT("blueprints"), Blueprints);
		TArray<TSharedPtr<FJsonValue>> Materials;
		for (const TCHAR* Path : MaterialPaths)
		{
			UMaterial* M = LoadObject<UMaterial>(nullptr, Path);
			if (M) Materials.Add(MakeShared<FJsonValueObject>(DescribeMaterial(M)));
			bOk &= M != nullptr;
		}
		J->SetArrayField(TEXT("materials"), Materials);
		TArray<TSharedPtr<FJsonValue>> Targets;
		for (const TCHAR* Path : TargetPaths)
		{
			UTextureRenderTarget2D* T = LoadObject<UTextureRenderTarget2D>(nullptr, Path);
			if (T) Targets.Add(MakeShared<FJsonValueObject>(DescribeTarget(T)));
			bOk &= T != nullptr;
		}
		J->SetArrayField(TEXT("renderTargets"), Targets);
		return bOk;
	}

	// The receiver example after Update.
	const FName NameVariable(TEXT("SpoutName"));
	const FName TargetVariable(TEXT("OutputRenderTarget"));
	const TCHAR* const SharedName = TEXT("UESpout_Test_Example");
	const TCHAR* const UnlitPath = TEXT("/SpoutPlugin/Content/M_SpoutDisplayUnlit.M_SpoutDisplayUnlit");
	const TCHAR* const NameTooltip = TEXT("The Spout sender to show. By default the name the sender example sends, so the two examples connect in Play. ")
		TEXT("Type the name another application sends under to show that sender instead. Set it before Play: End Play closes the name set then.");
	const TCHAR* const TargetTooltip = TEXT("Optional: also copy each frame into this render target, for other materials or Blueprints. Leave it empty if the ")
		TEXT("display is all you need. Its format must match the stream (RGBA8 for 8-bit, RGB10A2 for 10-bit with Color Space Auto, RGBA16f for 10-bit ")
		TEXT("with sRGB and for float streams) and Auto Generate Mips must be off; a mismatched target is not used and the Output Log says which format ")
		TEXT("it needs. It is resized to the stream. RT_SpoutReceiverTest is an 8-bit one.");
	const TCHAR* const GraphComment = TEXT("Receives the Spout sender named Spout Name (by default UESpout_Test_Example, the sender example's name) and shows ")
		TEXT("it on the plane with the unlit M_SpoutDisplayUnlit, so the level's lights do not change the picture. HasSignal shows the picture once ")
		TEXT("a receive is accepted; the last frame stays if the sender stops. Output Render Target is optional. End Play closes the receiver.");

	static UK2Node_CallFunction* FindCall(UBlueprint* BP, const TCHAR* Function)
	{
		TArray<UK2Node_CallFunction*> Calls;
		FBlueprintEditorUtils::GetAllNodesOfClass(BP, Calls);
		for (UK2Node_CallFunction* Call : Calls)
		{
			if (Call->FunctionReference.GetMemberName() == FName(Function)) return Call;
		}
		return nullptr;
	}

	static bool AddVariable(UBlueprint* BP, FName Name, const FEdGraphPinType& Type, const FString& Default, const TCHAR* Friendly, const TCHAR* Tooltip)
	{
		if (!FBlueprintEditorUtils::AddMemberVariable(BP, Name, Type, Default)) return false;
		FBPVariableDescription& Var = BP->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(BP, Name)];
		Var.FriendlyName = Friendly;
		Var.PropertyFlags |= CPF_Edit | CPF_BlueprintVisible;
		Var.PropertyFlags &= ~(CPF_DisableEditOnInstance | CPF_BlueprintReadOnly);
		FBlueprintEditorUtils::SetBlueprintVariableCategory(BP, Name, nullptr, FText::FromString(TEXT("Spout")), true);
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, Name, nullptr, FBlueprintMetadata::MD_Tooltip, Tooltip);
		return true;
	}

	/** A getter of Variable placed left of Node and connected to its input Pin (whose literal value it replaces). */
	static bool FeedFromVariable(UK2Node* Node, const TCHAR* Pin, FName Variable, int32 OffsetY)
	{
		UEdGraphPin* Input = Node ? Node->FindPin(Pin) : nullptr;
		UEdGraph* Graph = Node ? Node->GetGraph() : nullptr;
		if (!Input || !Graph) return false;
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		Input->DefaultObject = nullptr;
		Input->DefaultValue.Reset();
		FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
		UK2Node_VariableGet* Get = Creator.CreateNode(false);
		Get->VariableReference.SetSelfMember(Variable);
		Get->NodePosX = Node->NodePosX - 300;
		Get->NodePosY = Node->NodePosY + OffsetY;
		Creator.Finalize();
		return Schema->TryCreateConnection(Get->GetValuePin(), Input);
	}

	static bool Update(UBlueprint* BP, const TSharedRef<FJsonObject>& J)
	{
		auto Fail = [&J](const TCHAR* Problem) { J->SetStringField(TEXT("problem"), Problem); return false; };
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, NameVariable) != INDEX_NONE) return Fail(TEXT("the receiver example has a Spout Name variable already: it was updated before"));
		UMaterialInterface* Unlit = LoadObject<UMaterialInterface>(nullptr, UnlitPath);
		if (!Unlit) return Fail(TEXT("M_SpoutDisplayUnlit could not be loaded"));
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

		FEdGraphPinType NameType;
		NameType.PinCategory = UEdGraphSchema_K2::PC_Name;
		FEdGraphPinType TargetType;
		TargetType.PinCategory = UEdGraphSchema_K2::PC_Object;
		TargetType.PinSubCategoryObject = UTextureRenderTarget2D::StaticClass();
		if (!AddVariable(BP, NameVariable, NameType, SharedName, TEXT("Spout Name"), NameTooltip)) return Fail(TEXT("the Spout Name variable could not be added"));
		if (!AddVariable(BP, TargetVariable, TargetType, FString(), TEXT("Output Render Target"), TargetTooltip)) return Fail(TEXT("the Output Render Target variable could not be added"));

		// The nodes as the plugin compiled in this process declares them.
		FBlueprintEditorUtils::RefreshAllNodes(BP);
		UK2Node_CallFunction* Receive = FindCall(BP, TEXT("SpoutReceiverWithMaterial"));
		UK2Node_CallFunction* Close = FindCall(BP, TEXT("CloseReceiver"));
		UK2Node_CallFunction* CreateMaterial = FindCall(BP, TEXT("CreateDynamicMaterialInstance"));
		if (!Receive || !Close || !CreateMaterial) return Fail(TEXT("the receive, close or material node is missing"));
		if (!FeedFromVariable(Receive, TEXT("SpoutName"), NameVariable, -120)) return Fail(TEXT("Spout Name could not be connected to the receive node"));
		if (!FeedFromVariable(Close, TEXT("SpoutName"), NameVariable, -120)) return Fail(TEXT("Spout Name could not be connected to Close Receiver"));
		if (!FeedFromVariable(Receive, TEXT("OptionalOutputRenderTarget"), TargetVariable, 340)) return Fail(TEXT("Output Render Target could not be connected"));
		UEdGraphPin* Source = CreateMaterial->FindPin(TEXT("SourceMaterial"));
		if (!Source) return Fail(TEXT("Create Dynamic Material Instance has no Source Material input"));
		Schema->TrySetDefaultObject(*Source, Unlit);

		// The plane in the level shows the unlit material too, before Play.
		bool bPlaneSet = false;
		for (USCS_Node* Node : BP->SimpleConstructionScript ? BP->SimpleConstructionScript->GetAllNodes() : TArray<USCS_Node*>())
		{
			if (UStaticMeshComponent* Mesh = Node ? Cast<UStaticMeshComponent>(Node->ComponentTemplate) : nullptr)
			{
				Mesh->Modify();
				if (Mesh->OverrideMaterials.Num() < 1) Mesh->OverrideMaterials.SetNum(1);
				Mesh->OverrideMaterials[0] = Unlit;
				bPlaneSet = true;
			}
		}
		if (!bPlaneSet) return Fail(TEXT("the example has no plane component"));

		// After the received texture is stored: once a receive is accepted, HasSignal shows the picture.
		UEdGraphPin* StoredThen = nullptr;
		for (UEdGraphPin* Linked : Receive->GetThenPin()->LinkedTo)
		{
			if (UK2Node* Next = Linked ? Cast<UK2Node>(Linked->GetOwningNode()) : nullptr) StoredThen = Next->FindPin(UEdGraphSchema_K2::PN_Then);
		}
		if (!StoredThen || StoredThen->LinkedTo.Num() != 0) return Fail(TEXT("the receive node is not followed by a free Set Spout Received Texture"));
		UEdGraph* Graph = Receive->GetGraph();
		const UEdGraphNode* Stored = StoredThen->GetOwningNode();
		FGraphNodeCreator<UK2Node_IfThenElse> BranchCreator(*Graph);
		UK2Node_IfThenElse* Branch = BranchCreator.CreateNode(false);
		Branch->NodePosX = Stored->NodePosX + 320;
		Branch->NodePosY = Stored->NodePosY;
		BranchCreator.Finalize();
		FGraphNodeCreator<UK2Node_CallFunction> SetCreator(*Graph);
		UK2Node_CallFunction* SetSignal = SetCreator.CreateNode(false);
		SetSignal->SetFromFunction(UMaterialInstanceDynamic::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UMaterialInstanceDynamic, SetScalarParameterValue)));
		SetSignal->NodePosX = Branch->NodePosX + 260;
		SetSignal->NodePosY = Branch->NodePosY;
		SetCreator.Finalize();
		FGraphNodeCreator<UK2Node_VariableGet> MaterialCreator(*Graph);
		UK2Node_VariableGet* Material = MaterialCreator.CreateNode(false);
		Material->VariableReference.SetSelfMember(FName(TEXT("SpoutConsumerMaterial")));
		Material->NodePosX = Branch->NodePosX;
		Material->NodePosY = Branch->NodePosY + 180;
		MaterialCreator.Finalize();
		UEdGraphPin* Self = SetSignal->FindPin(UEdGraphSchema_K2::PN_Self);
		UEdGraphPin* Parameter = SetSignal->FindPin(TEXT("ParameterName"));
		UEdGraphPin* Value = SetSignal->FindPin(TEXT("Value"));
		if (!Self || !Parameter || !Value
			|| !Schema->TryCreateConnection(StoredThen, Branch->GetExecPin())
			|| !Schema->TryCreateConnection(Receive->GetReturnValuePin(), Branch->GetConditionPin())
			|| !Schema->TryCreateConnection(Branch->GetThenPin(), SetSignal->GetExecPin())
			|| !Schema->TryCreateConnection(Material->GetValuePin(), Self))
		{
			return Fail(TEXT("the HasSignal nodes could not be connected"));
		}
		Schema->TrySetDefaultValue(*Parameter, TEXT("HasSignal"));
		Schema->TrySetDefaultValue(*Value, TEXT("1.0"));

		FGraphNodeCreator<UEdGraphNode_Comment> CommentCreator(*Graph);
		UEdGraphNode_Comment* Comment = CommentCreator.CreateNode(false);
		Comment->NodeComment = GraphComment;
		Comment->NodePosX = Receive->NodePosX - 340;
		Comment->NodePosY = Receive->NodePosY - 420;
		Comment->NodeWidth = 1100;
		Comment->NodeHeight = 160;
		CommentCreator.Finalize();

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
		Compile(BP, After);
		J->SetObjectField(TEXT("after"), DescribeBlueprint(BP));
		if (After->GetNumberField(TEXT("compileErrors")) != 0 || After->GetNumberField(TEXT("compileWarnings")) != 0 || !After->GetBoolField(TEXT("upToDate")))
		{
			return Fail(TEXT("the updated example does not compile without errors and warnings"));
		}
		UObject* Defaults = BP->GeneratedClass ? BP->GeneratedClass->GetDefaultObject() : nullptr;
		const FNameProperty* NameProperty = Defaults ? FindFProperty<FNameProperty>(BP->GeneratedClass, NameVariable) : nullptr;
		const FObjectPropertyBase* TargetProperty = Defaults ? FindFProperty<FObjectPropertyBase>(BP->GeneratedClass, TargetVariable) : nullptr;
		if (!NameProperty || NameProperty->GetPropertyValue_InContainer(Defaults) != FName(SharedName)
			|| !TargetProperty || TargetProperty->GetObjectPropertyValue_InContainer(Defaults) != nullptr)
		{
			return Fail(TEXT("the class defaults are not Spout Name = UESpout_Test_Example and Output Render Target = none"));
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
		return bSaved || Fail(TEXT("the package could not be saved"));
	}
}

int32 USpoutExampleAssetCommandlet::Main(const FString& Params)
{
	using namespace SpoutExampleAsset;
	FString Mode, OutPath;
	FParse::Value(*Params, TEXT("Mode="), Mode);
	FParse::Value(*Params, TEXT("Out="), OutPath);
	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetStringField(TEXT("mode"), Mode);
	J->SetStringField(TEXT("runningEngine"), FEngineVersion::Current().ToString());
	bool bOk = false;
	if (Mode == TEXT("Inspect"))
	{
		bOk = Inspect(J);
	}
	else if (Mode == TEXT("Update"))
	{
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, ReceiverPath);
		if (BP)
		{
			J->SetObjectField(TEXT("before"), DescribeBlueprint(BP));
			bOk = Update(BP, J);
		}
		else
		{
			J->SetStringField(TEXT("problem"), TEXT("the receiver example could not be loaded"));
		}
	}
	else
	{
		J->SetStringField(TEXT("problem"), TEXT("unknown -Mode (Inspect or Update)"));
	}
	J->SetBoolField(TEXT("ok"), bOk);
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(J, Writer);
	if (!OutPath.IsEmpty()) FFileHelper::SaveStringToFile(Text, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Display, TEXT("SpoutExampleAsset %s: %s"), *Mode, bOk ? TEXT("ok") : TEXT("FAILED"));
	return bOk ? 0 : 1;
}
