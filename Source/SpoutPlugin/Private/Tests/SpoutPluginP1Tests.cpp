// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
#include "SpoutBPFunctionLibrary.h"
#include "SpoutReceiver.h"
#include "SpoutReceiverOwnership.h"
#include "SpoutSenderRegistry.h"
#include "SpoutD3DContext.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Guid.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/IConsoleManager.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#if WITH_EDITOR
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "SpoutSession.h"
#endif

#define P1_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
namespace
{
	void ExpectWrapDiagnostic(FAutomationTestBase& Test)
	{
		if (FParse::Param(FCommandLine::Get(), TEXT("d3ddebug")))
			Test.AddExpectedError(TEXT("ID3D12CompatibilityDevice::ReflectSharedProperties: Resource provided was not shared by D3D11, or with a D3D11 desc."), EAutomationExpectedErrorFlags::Contains, 0);
	}
	TArray<FColor> Pattern(int32 W, int32 H, int32 Sequence)
	{
		TArray<FColor> Pixels; Pixels.SetNumUninitialized(W * H);
		for (int32 Y = 0; Y < H; ++Y) for (int32 X = 0; X < W; ++X)
			Pixels[Y * W + X] = FColor((X + Sequence) & 255, (Y * 11 + Sequence) & 255, (X * 13 + Y) & 255, (X * 3 + Y * 7 + Sequence) & 255);
		return Pixels;
	}
	void Upload(UTextureRenderTarget2D* Target, int32 Sequence)
	{
		auto* Resource = Target->GameThread_GetRenderTargetResource();
		const int32 W = Target->SizeX, H = Target->SizeY;
		auto Pixels = Pattern(W, H, Sequence);
		ENQUEUE_RENDER_COMMAND(SpoutP1Upload)([Resource, W, H, Pixels = MoveTemp(Pixels)](FRHICommandListImmediate& Cmd)
		{
			Cmd.UpdateTexture2D(Resource->GetTextureRHI(), 0, FUpdateTextureRegion2D(0, 0, 0, 0, W, H), W * 4, reinterpret_cast<const uint8*>(Pixels.GetData()));
		});
	}
	TArray<FColor> Read(FTextureResource* Resource, int32 W, int32 H)
	{
		TArray<FColor> Pixels;
		ENQUEUE_RENDER_COMMAND(SpoutP1Read)([Resource, W, H, &Pixels](FRHICommandListImmediate& Cmd)
		{
			FReadSurfaceDataFlags Flags(RCM_UNorm); Flags.SetLinearToGamma(false);
			Cmd.Transition(FRHITransitionInfo(Resource->GetTextureRHI(), ERHIAccess::Unknown, ERHIAccess::CopySrc));
			Cmd.ReadSurfaceData(Resource->GetTextureRHI(), FIntRect(0, 0, W, H), Pixels, Flags);
			Cmd.Transition(FRHITransitionInfo(Resource->GetTextureRHI(), ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		});
		FlushRenderingCommands(); return Pixels;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP1Fanout, "SpoutPlugin.P1.PausedFanoutRecreationAndCpuRetry", P1_FLAGS)
bool FSpoutP1Fanout::RunTest(const FString&)
{
	ExpectWrapDiagnostic(*this);
	auto* Path = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.GPUReceiver"));
	const int32 Old = Path->GetInt(); const auto Priority = static_cast<EConsoleVariableFlags>(Path->GetFlags() & ECVF_SetByMask);
	for (int32 GPU : {1, 0})
	{
		Path->Set(GPU, ECVF_SetByCode);
		const FName Name(*(TEXT("SpoutP1Fanout_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		TStrongObjectPtr<UTextureRenderTarget2D> Source(NewObject<UTextureRenderTarget2D>()), A(NewObject<UTextureRenderTarget2D>()), B(NewObject<UTextureRenderTarget2D>()), Bad(NewObject<UTextureRenderTarget2D>());
		Source->InitCustomFormat(256, 16, PF_B8G8R8A8, true);
		A->InitCustomFormat(32, 16, PF_B8G8R8A8, true); B->InitCustomFormat(32, 16, PF_B8G8R8A8, true);
		Bad->InitCustomFormat(8, 8, PF_FloatRGBA, true);
		Upload(Source.Get(), 19);
		USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source.Get()); FlushRenderingCommands();
		UTexture2D* Texture = nullptr; UMaterialInstanceDynamic* Material = nullptr;
		TestTrue(TEXT("texture-only consumer accepted first"), USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(Name, nullptr, NAME_None, Texture, nullptr, ESpoutColorSpace::Linear));
		FlushRenderingCommands();
		for (UTextureRenderTarget2D* Target : { A.Get(), B.Get() })
		{
			TestTrue(TEXT("late consumer accepted while sender is paused"), USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(Name, nullptr, NAME_None, Texture, Target, ESpoutColorSpace::Linear));
			TestTrue(TEXT("late target receives full ramp and alpha"), Read(Target->GetResource(), 256, 16) == Pattern(256, 16, 19));
		}
		A->UpdateResource();
		USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(Name, nullptr, NAME_None, Texture, A.Get(), ESpoutColorSpace::Linear);
		TestTrue(TEXT("recreated target catches up without upstream publication"), Read(A->GetResource(), 256, 16) == Pattern(256, 16, 19));
		if (Texture) Texture->UpdateResource();
		USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(Name, nullptr, NAME_None, Texture, nullptr, ESpoutColorSpace::Linear);
		TestTrue(TEXT("recreated shared output restores last completed image"), Texture && Read(Texture->GetResource(), 256, 16) == Pattern(256, 16, 19));
		USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(Name, nullptr, NAME_None, Texture, Bad.Get(), ESpoutColorSpace::Linear);
		TestEqual(TEXT("incompatible target width remains unchanged"), Bad->SizeX, 8);
		TestEqual(TEXT("incompatible target height remains unchanged"), Bad->SizeY, 8);
		UTexture2D* Before = Texture;
		TestFalse(TEXT("conflicting color request refused"), USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(Name, nullptr, NAME_None, Texture, nullptr, ESpoutColorSpace::SRGB));
		TestTrue(TEXT("conflict leaves output identity unchanged"), Texture == Before);
		if (!GPU)
		{
			Upload(Source.Get(), 23); USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source.Get()); FlushRenderingCommands();
			SpoutReceiverInternal::FailNextCpuReadbackForTests();
			USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(Name, nullptr, NAME_None, Texture, B.Get(), ESpoutColorSpace::Linear);
			TestTrue(TEXT("failed readback keeps previous completed output"), Read(B->GetResource(), 256, 16) == Pattern(256, 16, 19));
			USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(Name, nullptr, NAME_None, Texture, B.Get(), ESpoutColorSpace::Linear);
			TestTrue(TEXT("retry receives last upstream image without new publication"), Read(B->GetResource(), 256, 16) == Pattern(256, 16, 23));
		}
		USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material); USpoutBPFunctionLibrary::CloseSender(Name); FlushRenderingCommands();
	}
	Path->Set(Old, ECVF_SetByCode); Path->ClearFlags(ECVF_SetByMask); Path->SetFlags(Priority);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP1Bindings, "SpoutPlugin.P1.BindingOwnershipAndExplicitReuse", P1_FLAGS)
bool FSpoutP1Bindings::RunTest(const FString&)
{
	FSpoutReceiverOwnership Ownership;
	TStrongObjectPtr<UTexture2D> Original(UTexture2D::CreateTransient(8,8)), A(UTexture2D::CreateTransient(8,8)), B(UTexture2D::CreateTransient(8,8)), Replacement(UTexture2D::CreateTransient(8,8));
	TStrongObjectPtr<UMaterialInstanceDynamic> Mid(UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface), GetTransientPackage()));
	const FName Param(TEXT("SpoutTexture")), One(TEXT("SpoutP1One")), Two(TEXT("SpoutP1Two"));
	Ownership.AdoptTexture(One, A.Get()); Ownership.AdoptTexture(Two, B.Get());
	Mid->SetTextureParameterValue(Param, A.Get()); Ownership.RegisterMaterialBinding(One, Mid.Get(), Param, false, Original.Get());
	Mid->SetTextureParameterValue(Param, B.Get()); Ownership.RegisterMaterialBinding(Two, Mid.Get(), Param, false, A.Get());
	Ownership.Release(One);
	TestTrue(TEXT("old close preserves newer session binding"), Mid->K2_GetTextureParameterValue(Param) == B.Get());
	for (int32 I=0; I<1000; ++I) Ownership.RegisterMaterialBinding(Two, Mid.Get(), Param, false, B.Get());
	TestEqual(TEXT("repeated explicit material reuse stays bounded"), Ownership.NumMaterialBindings(Two), 1);
	Ownership.AdoptTexture(Two, Replacement.Get());
	TestTrue(TEXT("replacement rebinds consumers that still own the parameter"), Mid->K2_GetTextureParameterValue(Param) == Replacement.Get());
	TestTrue(TEXT("per-binding release succeeds"), Ownership.ReleaseMaterialBinding(Two, Mid.Get(), Param));
	TestTrue(TEXT("original pre-plugin texture restored"), Mid->K2_GetTextureParameterValue(Param) == Original.Get());
	TestTrue(TEXT("per-binding release keeps shared session"), Ownership.Contains(Two));
	Mid->SetTextureParameterValue(Param, Replacement.Get()); Ownership.RegisterMaterialBinding(Two, Mid.Get(), Param, false, Original.Get());
	Mid->SetTextureParameterValue(Param, Original.Get()); Ownership.Release(Two);
	TestTrue(TEXT("manual rebind survives session close"), Mid->K2_GetTextureParameterValue(Param) == Original.Get());
	UFunction* Function = USpoutBPFunctionLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(USpoutBPFunctionLibrary, SpoutReceiverWithMaterial));
	FProperty* Input = FindFProperty<FProperty>(Function, TEXT("ConsumerMaterial"));
	TestTrue(TEXT("Blueprint reuse material is a real input pin"), Input && Input->HasAnyPropertyFlags(CPF_Parm) && !Input->HasAnyPropertyFlags(CPF_OutParm));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP1Color, "SpoutPlugin.P1.SenderColorMetadataAndRawRamp", P1_FLAGS)
bool FSpoutP1Color::RunTest(const FString&)
{
	ExpectWrapDiagnostic(*this);
	for (EPixelFormat Format : {PF_B8G8R8A8, PF_R8G8B8A8})
	for (bool Linear : {true, false})
	{
		const FString Case = FString::Printf(TEXT("%s %s source"), GetPixelFormatString(Format), Linear ? TEXT("linear") : TEXT("sRGB"));
		const FName Name(*(TEXT("SpoutP1Color_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		TStrongObjectPtr<UTextureRenderTarget2D> Source(USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(256, 16, Format, Linear));
		TestTrue(Case + TEXT(": created"), Source.IsValid());
		if (!Source.IsValid()) continue;
		Upload(Source.Get(), 41); USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source.Get()); FlushRenderingCommands();
		auto Entry = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Sender);
		TestTrue(Case + TEXT(": sender registered"), Entry.IsValid());
		// Every 8-bit source is shared as the plain _UNORM format, whatever its gamma: SpoutGL
		// receivers refuse _UNORM_SRGB. The bytes carry the colour intent, the format does not.
		const DXGI_FORMAT Unorm = Format == PF_B8G8R8A8 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
		if (Entry) TestEqual(Case + TEXT(": shared as the _UNORM format"), static_cast<int32>(Entry->SharedFormat), static_cast<int32>(Unorm));
		// The upload wrote FColor (B,G,R,A) bytes. An RGBA8 target stores them in R,G,B,A order, so
		// reading it back as FColor swaps red and blue; the bytes themselves are unchanged.
		TArray<FColor> Expected = Pattern(256, 16, 41);
		if (Format == PF_R8G8B8A8) for (FColor& P : Expected) Swap(P.R, P.B);
		for (ESpoutColorSpace Mode : {ESpoutColorSpace::Auto, ESpoutColorSpace::Linear, ESpoutColorSpace::SRGB})
		{
			const FString ModeCase = Case + TEXT(", receiver ") + UEnum::GetValueAsString(Mode);
			UTexture2D* Texture=nullptr; UMaterialInstanceDynamic* Material=nullptr;
			const bool bAccepted = USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(Name, nullptr, NAME_None, Texture, nullptr, Mode);
			TestTrue(ModeCase + TEXT(": accepted"), bAccepted && Texture);
			if (Texture)
			{
				TestEqual(ModeCase + TEXT(": decode setting follows the colour-space mode"), Texture->SRGB != 0, Mode != ESpoutColorSpace::Linear);
				TestEqual(ModeCase + TEXT(": pixel format"), static_cast<int32>(Texture->GetPixelFormat()), static_cast<int32>(Format));
				TestTrue(ModeCase + TEXT(": ramp and alpha bytes unchanged"), Read(Texture->GetResource(), 256, 16) == Expected);
			}
			USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material); FlushRenderingCommands();
		}
		USpoutBPFunctionLibrary::CloseSender(Name); FlushRenderingCommands();
	}
	return true;
}
#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutP1Blueprint, "SpoutPlugin.P1.CompiledBlueprintMaterialReuse", P1_FLAGS)
bool FSpoutP1Blueprint::RunTest(const FString&)
{
	ExpectWrapDiagnostic(*this);
	const FName Name(*(TEXT("SpoutP1Blueprint_")+FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(256,16,PF_B8G8R8A8,true));
	Upload(Source.Get(),31); USpoutBPFunctionLibrary::SpoutSender(Name,ESpoutSendTextureFrom::TextureRenderTarget2D,Source.Get()); FlushRenderingCommands();
	TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(UObject::StaticClass(),GetTransientPackage(),MakeUniqueObjectName(GetTransientPackage(),UBlueprint::StaticClass()),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass()));
	if (!TestNotNull(TEXT("test Blueprint created"),Blueprint.Get())) return false;
	FEdGraphPinType MaterialType; MaterialType.PinCategory=UEdGraphSchema_K2::PC_Object; MaterialType.PinSubCategoryObject=UMaterialInstanceDynamic::StaticClass();
	FBlueprintEditorUtils::AddMemberVariable(Blueprint.Get(),TEXT("ReusableMaterial"),MaterialType);
	UEdGraph* Graph=FBlueprintEditorUtils::CreateNewGraph(Blueprint.Get(),TEXT("ReceiveFrame"),UEdGraph::StaticClass(),UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph(Blueprint.Get(),Graph,true,static_cast<UFunction*>(nullptr));
	auto* Entry=Cast<UK2Node_FunctionEntry>(Graph->Nodes[0]);
	auto* Call=NewObject<UK2Node_CallFunction>(Graph); Graph->AddNode(Call);
	Call->SetFromFunction(USpoutBPFunctionLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(USpoutBPFunctionLibrary,SpoutReceiverWithMaterial)));
	Call->AllocateDefaultPins();
	auto* Variable=NewObject<UK2Node_VariableGet>(Graph); Graph->AddNode(Variable);
	Variable->VariableReference.SetSelfMember(TEXT("ReusableMaterial")); Variable->AllocateDefaultPins();
	const auto* Schema=GetDefault<UEdGraphSchema_K2>();
	TestTrue(TEXT("execution pins connect"),Schema->TryCreateConnection(Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then),Call->GetExecPin()));
	TestTrue(TEXT("stored material connects to input"),Schema->TryCreateConnection(Variable->GetValuePin(),Call->FindPinChecked(TEXT("ConsumerMaterial"))));
	Call->FindPinChecked(TEXT("SpoutName"))->DefaultValue=Name.ToString();
	Call->FindPinChecked(TEXT("TextureParameterName"))->DefaultValue=TEXT("SpoutTexture");
	FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
	if (!TestTrue(TEXT("Blueprint compiles without error"),Blueprint->Status!=BS_Error)) return false;
	TStrongObjectPtr<UObject> Consumer(NewObject<UObject>(GetTransientPackage(),Blueprint->GeneratedClass));
	TStrongObjectPtr<UMaterialInstanceDynamic> Mid(UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface),GetTransientPackage()));
	FindFProperty<FObjectPropertyBase>(Blueprint->GeneratedClass,TEXT("ReusableMaterial"))->SetObjectPropertyValue_InContainer(Consumer.Get(),Mid.Get());
	UFunction* ReceiveFunction=Consumer->FindFunction(TEXT("ReceiveFrame"));
	if (!TestNotNull(TEXT("compiled callable function"),ReceiveFunction)) return false;
	for(int32 I=0; I<1000; ++I) { Consumer->ProcessEvent(ReceiveFunction,nullptr); if(I%20==0) FlushRenderingCommands(); }
	FlushRenderingCommands();
	CollectGarbage(RF_NoFlags);
	Consumer->ProcessEvent(ReceiveFunction,nullptr); FlushRenderingCommands();
	auto* Ownership=FSpoutReceiverOwnership::Peek();
	TestTrue(TEXT("1000 compiled calls retain one material binding"),Ownership && Ownership->NumMaterialBindings(Name)==1);
	TestTrue(TEXT("compiled calls bind the caller's reusable MID"),Mid->K2_GetTextureParameterValue(TEXT("SpoutTexture"))!=nullptr);
	USpoutBPFunctionLibrary::ReleaseReceiverMaterialBinding(Name,Mid.Get(),TEXT("SpoutTexture"));
	TestTrue(TEXT("binding count returns to zero"),Ownership && Ownership->NumMaterialBindings(Name)==0);
	UTexture2D* Texture=nullptr; UMaterialInstanceDynamic* Material=nullptr;
	USpoutBPFunctionLibrary::CloseReceiver(Name,Texture,Material); USpoutBPFunctionLibrary::CloseSender(Name); FlushRenderingCommands();
	return true;
}
#endif
#undef P1_FLAGS
#endif
