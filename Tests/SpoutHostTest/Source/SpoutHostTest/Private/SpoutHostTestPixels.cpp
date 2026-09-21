// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#include "SpoutHostTestPixels.h"
#include "SpoutBPFunctionLibrary.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/MeshComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"
#include "Windows/WindowsHWrapper.h"

namespace SpoutHostTest
{
	TArray<FColor> ReadPixels(FTextureResource* Resource, int32 Width, int32 Height)
	{
		TArray<FColor> Pixels;
		ENQUEUE_RENDER_COMMAND(SpoutHostPixelRead)([Resource, Width, Height, &Pixels](FRHICommandListImmediate& Cmd)
		{
			if (!Resource || !Resource->GetTextureRHI()) return;
			FReadSurfaceDataFlags Flags(RCM_UNorm); Flags.SetLinearToGamma(false);
			Cmd.Transition(FRHITransitionInfo(Resource->GetTextureRHI(), ERHIAccess::Unknown, ERHIAccess::CopySrc));
			Cmd.ReadSurfaceData(Resource->GetTextureRHI(), FIntRect(0, 0, Width, Height), Pixels, Flags);
			Cmd.Transition(FRHITransitionInfo(Resource->GetTextureRHI(), ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		});
		FlushRenderingCommands();
		return Pixels;
	}

	static TArray<FColor> Pattern(int32 W, int32 H, int32 Sequence)
	{
		TArray<FColor> Pixels; Pixels.SetNumUninitialized(W * H);
		for (int32 Y=0; Y<H; ++Y) for (int32 X=0; X<W; ++X)
			Pixels[Y*W+X] = FColor((X*17+Sequence)&255, (Y*29+Sequence)&255, (X+Y+Sequence*13)&255,
				(X==0 && Y==0) ? 255 : ((X*3+Y*7+Sequence)&255));
		return Pixels;
	}

	static bool FinishPeer(FProcHandle& Peer, double Timeout)
	{
		if (!Peer.IsValid()) return false;
		const double Deadline = FPlatformTime::Seconds() + Timeout;
		while (FPlatformProcess::IsProcRunning(Peer) && FPlatformTime::Seconds() < Deadline) FPlatformProcess::Sleep(0.01f);
		int32 Exit = -1;
		const bool Passed = FPlatformProcess::GetProcReturnCode(Peer, &Exit) && Exit == 0;
		if (FPlatformProcess::IsProcRunning(Peer)) FPlatformProcess::TerminateProc(Peer);
		FPlatformProcess::CloseProc(Peer);
		return Passed;
	}

	bool QualifyShippedExamples(UWorld* World, FString& Error)
	{
		FString Exe;
		if (!World || !FParse::Value(FCommandLine::Get(),TEXT("SpoutNativePeer="),Exe)) { Error=TEXT("example qualification needs a game world and native peer"); return false; }
		const FName SenderName(TEXT("UESpout_Test_Example")), ReceiverName(TEXT("Spout"));
		FSenderStruct Existing;
		if (USpoutBPFunctionLibrary::GetSenderInfo(SenderName,Existing) || USpoutBPFunctionLibrary::GetSenderInfo(ReceiverName,Existing))
		{ Error=TEXT("shipped example name already belongs to another sender; qualification cannot take it over"); return false; }
		UClass* SenderClass=LoadClass<AActor>(nullptr,TEXT("/SpoutPlugin/Content/BP_SpoutSenderExample_.BP_SpoutSenderExample__C"));
		UClass* ReceiverClass=LoadClass<AActor>(nullptr,TEXT("/SpoutPlugin/Content/BP_SpoutReceiverExample.BP_SpoutReceiverExample_C"));
		auto* SenderTarget=LoadObject<UTextureRenderTarget2D>(nullptr,TEXT("/SpoutPlugin/Content/RT_SpoutSenderTest_.RT_SpoutSenderTest_"));
		auto* ReceiverTarget=LoadObject<UTextureRenderTarget2D>(nullptr,TEXT("/SpoutPlugin/Content/RT_SpoutReceiverTest.RT_SpoutReceiverTest"));
		auto* MaterialAsset=LoadObject<UMaterial>(nullptr,TEXT("/SpoutPlugin/Content/M_SpoutMaterial.M_SpoutMaterial"));
		if (!SenderClass || !ReceiverClass || !SenderTarget || !ReceiverTarget || !MaterialAsset) { Error=TEXT("a shipped example asset is missing from the consumer package"); return false; }
		// Inject deterministic pixels into the example's authored input. No Blueprint graph
		// or package is rewritten; this controls the image source, like an external capture.
		SenderTarget->InitCustomFormat(256,16,PF_B8G8R8A8,true); SenderTarget->UpdateResource();
		ReceiverTarget->InitCustomFormat(32,16,PF_B8G8R8A8,true); ReceiverTarget->UpdateResource();
		AActor* Sender=World->SpawnActor<AActor>(SenderClass);
		if (!Sender) { Error=TEXT("shipped sender cannot spawn"); return false; }
		ON_SCOPE_EXIT { Sender->Destroy(); USpoutBPFunctionLibrary::CloseSender(SenderName); FlushRenderingCommands(); };
		TInlineComponentArray<USceneCaptureComponent2D*> Captures(Sender);
		for (auto* Capture : Captures) { Capture->bCaptureEveryFrame=false; Capture->bCaptureOnMovement=false; }
		const FString Output=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("example-sender-peer.json"));
		FProcHandle Reader;
		const double SenderDeadline=FPlatformTime::Seconds()+15.0;
		for(int32 Sequence=1; FPlatformTime::Seconds()<SenderDeadline; ++Sequence)
		{
			auto Pixels=Pattern(256,16,Sequence); auto* Resource=SenderTarget->GameThread_GetRenderTargetResource();
			ENQUEUE_RENDER_COMMAND(SpoutExamplePattern)([Resource,Pixels=MoveTemp(Pixels)](FRHICommandListImmediate& Cmd)
			{ Cmd.UpdateTexture2D(Resource->GetTextureRHI(),0,FUpdateTextureRegion2D(0,0,0,0,256,16),1024,reinterpret_cast<const uint8*>(Pixels.GetData())); });
			Sender->Tick(1.0f/60.0f); FlushRenderingCommands();
			if (Sequence==1) { const FString Args=FString::Printf(TEXT("receive %s \"%s\" linear"),*SenderName.ToString(),*Output); Reader=FPlatformProcess::CreateProc(*Exe,*Args,false,true,true,nullptr,0,nullptr,nullptr); }
			if (!Reader.IsValid() || !FPlatformProcess::IsProcRunning(Reader)) break;
			FPlatformProcess::Sleep(0.01f);
		}
		if (!FinishPeer(Reader,0.1)) { Error=TEXT("shipped sender Blueprint failed external pixel qualification"); return false; }
		Sender->Destroy(); FlushRenderingCommands();
		if (USpoutBPFunctionLibrary::GetSenderInfo(SenderName,Existing)) { Error=TEXT("shipped sender EndPlay did not close its published name"); return false; }
		const FString EventPrefix=TEXT("SpoutExample_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString StopName=EventPrefix+TEXT("_Stop"), ReadyName=EventPrefix+TEXT("_Ready");
		HANDLE Stop=CreateEventW(nullptr,true,false,*StopName), Ready=CreateEventW(nullptr,true,false,*ReadyName);
		if (!Stop || !Ready) { if(Stop) CloseHandle(Stop); if(Ready) CloseHandle(Ready); Error=TEXT("example peer event creation failed"); return false; }
		const FString Args=FString::Printf(TEXT("send Spout %s %s"),*StopName,*ReadyName);
		FProcHandle Writer=FPlatformProcess::CreateProc(*Exe,*Args,false,true,true,nullptr,0,nullptr,nullptr);
		AActor* Receiver=World->SpawnActor<AActor>(ReceiverClass);
		ON_SCOPE_EXIT { if(Receiver) Receiver->Destroy(); UTexture2D* Texture=nullptr; UMaterialInstanceDynamic* Material=nullptr; USpoutBPFunctionLibrary::CloseReceiver(ReceiverName,Texture,Material); SetEvent(Stop); FinishPeer(Writer,3.0); CloseHandle(Ready); CloseHandle(Stop); FlushRenderingCommands(); };
		if (!Receiver || !Writer.IsValid() || WaitForSingleObject(Ready,10000)!=WAIT_OBJECT_0) { Error=TEXT("shipped receiver or external sender failed to start"); return false; }
		int32 Verified=0, Last=-1; TSet<int32> Widths;
		const double ReceiverDeadline=FPlatformTime::Seconds()+12.0;
		while (Verified<64 && FPlatformTime::Seconds()<ReceiverDeadline)
		{
			Receiver->Tick(1.0f/60.0f); FlushRenderingCommands();
			const auto Pixels=ReadPixels(ReceiverTarget->GetResource(),ReceiverTarget->SizeX,ReceiverTarget->SizeY);
			if (Pixels.Num() && Pixels[0].A==255)
			{
				if (Pixels!=Pattern(ReceiverTarget->SizeX,ReceiverTarget->SizeY,Pixels[0].R)) { Error=TEXT("shipped receiver output contains corrupted pixels"); return false; }
				Widths.Add(ReceiverTarget->SizeX);
				if (Last!=Pixels[0].R) { Last=Pixels[0].R; ++Verified; }
			}
			FPlatformProcess::Sleep(0.01f);
		}
		TInlineComponentArray<UMeshComponent*> Meshes(Receiver);
		bool Bound=false;
		for(auto* Mesh : Meshes) if(auto* MID=Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(0))) Bound |= MID->K2_GetTextureParameterValue(TEXT("SpoutTexture")) != nullptr;
		if (Verified<64 || Widths.Num()<2 || !Bound) { Error=FString::Printf(TEXT("shipped receiver failed sustained Blueprint/material/resize qualification (%d frames, %d sizes, bound=%d)"),Verified,Widths.Num(),Bound); return false; }
		Receiver->Destroy(); FlushRenderingCommands();
		if (USpoutBPFunctionLibrary::GetSpoutStreamStatus(ReceiverName,ESpoutType::Receiver).bActive) { Error=TEXT("shipped receiver EndPlay did not close its session"); return false; }
		SetEvent(Stop);
		if (!FinishPeer(Writer,3.0)) { Error=TEXT("example external sender shutdown failed"); return false; }
		return true;
	}

	bool QualifyExternalPixels(FString& Error)
	{
		FString Exe;
		if (!FParse::Value(FCommandLine::Get(), TEXT("SpoutNativePeer="), Exe) || !IFileManager::Get().FileExists(*Exe))
		{ Error = TEXT("external pixel qualification requires -SpoutNativePeer=<built oracle>"); return false; }
		const FString Prefix = TEXT("SpoutHostPixels_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		for (bool Linear : {true, false})
		{
			const FName Name(*(Prefix + (Linear ? TEXT("_Linear") : TEXT("_SRGB"))));
			TStrongObjectPtr<UTextureRenderTarget2D> Source(USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(256, 16, PF_B8G8R8A8, Linear));
			if (!Source.IsValid()) { Error=TEXT("pixel source allocation failed"); return false; }
			ON_SCOPE_EXIT { USpoutBPFunctionLibrary::CloseSender(Name); FlushRenderingCommands(); };
			const FString Output = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / (Name.ToString()+TEXT(".json")));
			FProcHandle Peer;
			const double Deadline = FPlatformTime::Seconds()+15.0;
			for (int32 Sequence=1; FPlatformTime::Seconds()<Deadline; ++Sequence)
			{
				auto Pixels=Pattern(256,16,Sequence);
				if (FParse::Param(FCommandLine::Get(), TEXT("SpoutInjectPixelFailure"))) Pixels[1].B ^= 1;
				auto* Resource=Source->GameThread_GetRenderTargetResource();
				ENQUEUE_RENDER_COMMAND(SpoutHostPattern)([Resource, Pixels=MoveTemp(Pixels)](FRHICommandListImmediate& Cmd)
				{ Cmd.UpdateTexture2D(Resource->GetTextureRHI(),0,FUpdateTextureRegion2D(0,0,0,0,256,16),256*4,reinterpret_cast<const uint8*>(Pixels.GetData())); });
				USpoutBPFunctionLibrary::SpoutSender(Name,ESpoutSendTextureFrom::TextureRenderTarget2D,Source.Get());
				FlushRenderingCommands();
				if (Sequence==1)
				{
					const FString Args=FString::Printf(TEXT("receive %s \"%s\" %s"),*Name.ToString(),*Output,Linear?TEXT("linear"):TEXT("srgb"));
					Peer=FPlatformProcess::CreateProc(*Exe,*Args,false,true,true,nullptr,0,nullptr,nullptr);
					if (!Peer.IsValid()) break;
				}
				if (!FPlatformProcess::IsProcRunning(Peer)) break;
				FPlatformProcess::Sleep(0.005f);
			}
			if (!FinishPeer(Peer,0.1)) { Error=TEXT("native receiver rejected ramp/channel/alpha/metadata or stalled"); return false; }
		}
		auto* CVar=IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.GPUReceiver"));
		if (!CVar) { Error=TEXT("receiver path selector missing"); return false; }
		const int32 Old=CVar->GetInt(); const auto Priority=static_cast<EConsoleVariableFlags>(CVar->GetFlags() & ECVF_SetByMask);
		ON_SCOPE_EXIT { CVar->Set(Old,ECVF_SetByCode); CVar->ClearFlags(ECVF_SetByMask); CVar->SetFlags(Priority); };
		for (int32 GPU : {1,0})
		{
			CVar->Set(GPU,ECVF_SetByCode);
			const FString Name=Prefix+FString::Printf(TEXT("_Native%d"),GPU), StopName=Name+TEXT("_Stop"), ReadyName=Name+TEXT("_Ready");
			HANDLE Stop=CreateEventW(nullptr,true,false,*StopName), Ready=CreateEventW(nullptr,true,false,*ReadyName);
			if (!Stop || !Ready) { if(Stop) CloseHandle(Stop); if(Ready) CloseHandle(Ready); Error=TEXT("peer event creation failed"); return false; }
			const FString Args=FString::Printf(TEXT("send %s %s %s"),*Name,*StopName,*ReadyName);
			FProcHandle Peer=FPlatformProcess::CreateProc(*Exe,*Args,false,true,true,nullptr,0,nullptr,nullptr);
			UTexture2D* Texture=nullptr; UMaterialInstanceDynamic* Material=nullptr;
			ON_SCOPE_EXIT { USpoutBPFunctionLibrary::CloseReceiver(FName(*Name),Texture,Material); FlushRenderingCommands(); CloseHandle(Ready); CloseHandle(Stop); };
			const bool ReadyToRead=Peer.IsValid() && WaitForSingleObject(Ready,10000)==WAIT_OBJECT_0;
			int32 Verified=0, Last=-1; TSet<int32> Widths; bool Match=true;
			const double Deadline=FPlatformTime::Seconds()+10.0;
			while (ReadyToRead && (Verified<8 || Widths.Num()<2) && FPlatformTime::Seconds()<Deadline)
			{
				if (USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(FName(*Name),nullptr,NAME_None,Texture,nullptr,ESpoutColorSpace::Linear) && Texture)
				{
					const auto Pixels=ReadPixels(Texture->GetResource(),Texture->GetSizeX(),Texture->GetSizeY());
					if (Pixels.Num() && Pixels[0].A==255)
					{
						Match=Pixels==Pattern(Texture->GetSizeX(),Texture->GetSizeY(),Pixels[0].R);
						if (!Match) break;
						Widths.Add(Texture->GetSizeX());
						if (Last!=Pixels[0].R) { Last=Pixels[0].R; ++Verified; }
					}
				}
				FPlatformProcess::Sleep(0.01f);
			}
			SetEvent(Stop); const bool Stopped=FinishPeer(Peer,3.0);
			if (!Stopped || !Match || Verified<8 || Widths.Num()<2) { Error=TEXT("native sender pixels, progress, resize, or shutdown failed"); return false; }
		}
		return true;
	}
}
