// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#include "SpoutHostTestPixels.h"
#include "SpoutHostTestReport.h"
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
		// The target keeps its authored gamma, so the example is qualified as it ships: an sRGB
		// target must still be published as plain BGRA8 that SpoutGL receivers accept.
		const bool bAuthoredLinear=SenderTarget->bForceLinearGamma;
		if (FReport* Report=FReport::Get()) Report->SetField(TEXT("shippedSenderTargetForceLinearGamma"),bAuthoredLinear?TEXT("true"):TEXT("false"));
		SenderTarget->InitCustomFormat(256,16,PF_B8G8R8A8,bAuthoredLinear); SenderTarget->UpdateResource();
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
		int32 Verified=0, Last=-1, AwaitingCopy=0; TSet<int32> Widths;
		const double ReceiverDeadline=FPlatformTime::Seconds()+12.0;
		while (Verified<64 && FPlatformTime::Seconds()<ReceiverDeadline)
		{
			const int32 WidthBefore=ReceiverTarget->SizeX;
			const int64 MissesBefore=USpoutBPFunctionLibrary::GetSpoutStreamStatus(ReceiverName,ESpoutType::Receiver).AccessMisses;
			Receiver->Tick(1.0f/60.0f); FlushRenderingCommands();
			const auto Pixels=ReadPixels(ReceiverTarget->GetResource(),ReceiverTarget->SizeX,ReceiverTarget->SizeY);
			if (Pixels.Num() && Pixels[0].A==255)
			{
				// Resizing the target clears it to its Clear Color. When the transfer in that call is
				// skipped because the sender holds the access mutex, the whole target still shows the
				// clear colour until the next successful transfer: a delivery not yet made, as the
				// receiver documentation states, not corruption. The test pattern is never uniform,
				// so this cannot hide a partially written frame, and sustained delivery is still
				// required below.
				const FColor Clear=ReceiverTarget->ClearColor.ToFColor(false);
				bool bAllClear=true;
				for (const FColor& P : Pixels) { if (P!=Clear) { bAllClear=false; break; } }
				if (bAllClear)
				{
					++AwaitingCopy;
				}
				else if (Pixels!=Pattern(ReceiverTarget->SizeX,ReceiverTarget->SizeY,Pixels[0].R))
				{
					// Record what was read, so a failure can be classified from its result file alone.
					int32 Uniform=0, ClearPixels=0;
					for (const FColor& P : Pixels) { Uniform+=(P==Pixels[0]); ClearPixels+=(P==Clear); }
					const FSpoutStreamStatus Status=USpoutBPFunctionLibrary::GetSpoutStreamStatus(ReceiverName,ESpoutType::Receiver);
					Error=FString::Printf(TEXT("shipped receiver output contains corrupted pixels (%dx%d, resized this call %d, verified %d, pixel0 %d/%d/%d/%d, uniform %d, clear colour %d of %d, submitted %lld, access misses this call %lld)"),
						ReceiverTarget->SizeX,ReceiverTarget->SizeY,ReceiverTarget->SizeX!=WidthBefore?1:0,Verified,Pixels[0].R,Pixels[0].G,Pixels[0].B,Pixels[0].A,
						Uniform,ClearPixels,Pixels.Num(),Status.FramesSubmitted,Status.AccessMisses-MissesBefore);
					return false;
				}
				else
				{
					Widths.Add(ReceiverTarget->SizeX);
					if (Last!=Pixels[0].R) { Last=Pixels[0].R; ++Verified; }
				}
			}
			FPlatformProcess::Sleep(0.01f);
		}
		if (FReport* Report=FReport::Get()) Report->SetField(TEXT("shippedReceiverAwaitingCopyObservations"),FString::FromInt(AwaitingCopy));
		TInlineComponentArray<UMeshComponent*> Meshes(Receiver);
		bool Bound=false;
		for(auto* Mesh : Meshes) if(auto* MID=Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(0))) Bound |= MID->K2_GetTextureParameterValue(TEXT("SpoutTexture")) != nullptr;
		if (Verified<64 || Widths.Num()<2 || !Bound) { Error=FString::Printf(TEXT("shipped receiver failed sustained Blueprint/material/resize qualification (%d frames, %d sizes, bound=%d)"),Verified,Widths.Num(),Bound); return false; }
		// Async Receive is on unless an instance turns it off. This spawned example keeps the class default, so its
		// session must have asked for, and run, Async Receive.
		const FSpoutStreamStatus ExampleStatus=USpoutBPFunctionLibrary::GetSpoutStreamStatus(ReceiverName,ESpoutType::Receiver);
		const bool bExampleAsync=ExampleStatus.RequestedReceiveMode==ESpoutReceiveMode::AsyncBeta && ExampleStatus.EffectiveReceiveMode==ESpoutReceiveMode::AsyncBeta;
		if (FReport* Report=FReport::Get()) Report->SetField(TEXT("shippedReceiverDefaultAsyncReceive"),bExampleAsync?TEXT("true"):TEXT("false"));
		if (!bExampleAsync) { Error=FString::Printf(TEXT("the shipped receiver example with its default setting did not run Async Receive (note '%s')"),*ExampleStatus.ReceiveModeNote); return false; }
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
		// Both 8-bit layouts, linear and sRGB. Every one must arrive as the plain _UNORM texture
		// with the exact bytes and alpha: the peer checks the real resource format and every pixel.
		for (EPixelFormat Format : {PF_B8G8R8A8, PF_R8G8B8A8})
		for (bool Linear : {true, false})
		{
			const FName Name(*(Prefix + (Format==PF_B8G8R8A8 ? TEXT("_BGRA") : TEXT("_RGBA")) + (Linear ? TEXT("_Linear") : TEXT("_SRGB"))));
			TStrongObjectPtr<UTextureRenderTarget2D> Source(USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(256, 16, Format, Linear));
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
					const FString Args=FString::Printf(TEXT("receive %s \"%s\" %s"),*Name.ToString(),*Output,Format==PF_B8G8R8A8?TEXT("bgra"):TEXT("rgba"));
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
			int32 Verified=0, Last=-1; TSet<int32> Widths; bool Match=true, bSynchronous=true;
			const double Deadline=FPlatformTime::Seconds()+10.0;
			while (ReadyToRead && (Verified<8 || Widths.Num()<2) && FPlatformTime::Seconds()<Deadline)
			{
				if (USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(FName(*Name),nullptr,NAME_None,Texture,nullptr,ESpoutColorSpace::Linear,/*bAsyncReceive: the synchronous GPU and CPU paths*/ false) && Texture)
				{
					const auto Pixels=ReadPixels(Texture->GetResource(),Texture->GetSizeX(),Texture->GetSizeY());
					// This receiver asked for the synchronous receive: the session must request and run it.
					const FSpoutStreamStatus Status=USpoutBPFunctionLibrary::GetSpoutStreamStatus(FName(*Name),ESpoutType::Receiver);
					bSynchronous &= Status.RequestedReceiveMode==ESpoutReceiveMode::Synchronous && Status.EffectiveReceiveMode==ESpoutReceiveMode::Synchronous;
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
			if (!bSynchronous) { Error=TEXT("a receiver that asked for the synchronous receive did not run it"); return false; }
		}
		// Async Receive, asked for explicitly by this one receiver, from the same independent native sender: exact pixels
		// across the sender's resizes (every size change is a new sender identity, so a new worker), every verified frame
		// read while the status reported the asynchronous path as effective, the request recorded throughout, no
		// fallback note, and the receiver inactive after Close Receiver.
		{
			CVar->Set(1,ECVF_SetByCode);
			const FString Name=Prefix+TEXT("_NativeAsync"), StopName=Name+TEXT("_Stop"), ReadyName=Name+TEXT("_Ready");
			HANDLE Stop=CreateEventW(nullptr,true,false,*StopName), Ready=CreateEventW(nullptr,true,false,*ReadyName);
			if (!Stop || !Ready) { if(Stop) CloseHandle(Stop); if(Ready) CloseHandle(Ready); Error=TEXT("peer event creation failed"); return false; }
			const FString Args=FString::Printf(TEXT("send %s %s %s"),*Name,*StopName,*ReadyName);
			FProcHandle Peer=FPlatformProcess::CreateProc(*Exe,*Args,false,true,true,nullptr,0,nullptr,nullptr);
			UTexture2D* Texture=nullptr; UMaterialInstanceDynamic* Material=nullptr; bool bClosed=false;
			ON_SCOPE_EXIT { if (!bClosed) USpoutBPFunctionLibrary::CloseReceiver(FName(*Name),Texture,Material); FlushRenderingCommands(); CloseHandle(Ready); CloseHandle(Stop); };
			const bool ReadyToRead=Peer.IsValid() && WaitForSingleObject(Ready,10000)==WAIT_OBJECT_0;
			// A note during a resize can be legitimate (an identity caught between the sender's new size and its new
			// texture is refused and the next identity is tried again); the session must not END in fallback.
			int32 Verified=0, Last=-1, NoteSamples=0; TSet<int32> Widths; bool Match=true, bRequested=true; FString Note;
			const double Deadline=FPlatformTime::Seconds()+15.0;
			while (ReadyToRead && (Verified<8 || Widths.Num()<2) && FPlatformTime::Seconds()<Deadline)
			{
				if (USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(FName(*Name),nullptr,NAME_None,Texture,nullptr,ESpoutColorSpace::Linear,/*bAsyncReceive*/ true) && Texture)
				{
					const auto Pixels=ReadPixels(Texture->GetResource(),Texture->GetSizeX(),Texture->GetSizeY());
					const FSpoutStreamStatus Status=USpoutBPFunctionLibrary::GetSpoutStreamStatus(FName(*Name),ESpoutType::Receiver);
					bRequested &= Status.RequestedReceiveMode==ESpoutReceiveMode::AsyncBeta;
					Note=Status.ReceiveModeNote;
					NoteSamples+=Note.IsEmpty()?0:1;
					if (Pixels.Num() && Pixels[0].A==255 && Status.EffectiveReceiveMode==ESpoutReceiveMode::AsyncBeta)
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
			USpoutBPFunctionLibrary::CloseReceiver(FName(*Name),Texture,Material); FlushRenderingCommands(); bClosed=true;
			const bool bInactive=!USpoutBPFunctionLibrary::GetSpoutStreamStatus(FName(*Name),ESpoutType::Receiver).bActive;
			if (FReport* Report=FReport::Get())
			{
				Report->SetField(TEXT("nativeAsyncBetaVerifiedFrames"),FString::FromInt(Verified));
				Report->SetField(TEXT("nativeAsyncBetaNoteSamples"),FString::FromInt(NoteSamples));
			}
			if (!Stopped || !Match || !bRequested || !Note.IsEmpty() || Verified<8 || Widths.Num()<2 || !bInactive)
			{
				Error=FString::Printf(TEXT("Async Receive from the native sender failed (stopped %d, match %d, requested %d, final note '%s', notes seen %d, verified %d, sizes %d, inactive after close %d)"),
					Stopped?1:0, Match?1:0, bRequested?1:0, *Note, NoteSamples, Verified, Widths.Num(), bInactive?1:0);
				return false;
			}
		}
		return true;
	}
}
