/**
 * Implements the Blueprint API for Spout by routing game-thread calls into the D3D/Spout
 * subsystems and enqueuing render-thread work where required.
 */
#include "SpoutBPFunctionLibrary.h"
#include "SpoutModule.h"

#include "SpoutD3DContext.h"
#include "SpoutReceiver.h"
#include "SpoutSender.h"
#include "SpoutSenderRegistry.h"
#include "SpoutTextureUtils.h"

#include "RHI.h"
#include "RenderingThread.h"

void USpoutBPFunctionLibrary::EnsureSpoutInitialized()
{
	// Safe to call from any thread; guarded by an internal mutex.
	FSpoutD3DContext::Get().InitializeIfNeeded();
}

void USpoutBPFunctionLibrary::CloseAllStreams()
{
	// Render-thread work may still reference shared D3D objects; drain before releasing.
	FlushRenderingCommands();
	FSpoutSender::Shutdown();
	FSpoutSenderRegistry::Get().Clear();
}

void USpoutBPFunctionLibrary::GlobalShutdown()
{
	CloseAllStreams();
	FSpoutD3DContext::Get().Shutdown();
}

int32 USpoutBPFunctionLibrary::SetMaxSenders(int32 Max)
{
	EnsureSpoutInitialized();
	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	if (spoutSenderNames* SenderNames = Context.GetSenderNames())
	{
		// Spout sender registry is not thread-safe; guard with the shared mutex.
		FScopeLock SpoutLock(&Context.GetSpoutMutex());
		SenderNames->SetMaxSenders(Max);
	}
	return Max;
}

UTextureRenderTarget2D* USpoutBPFunctionLibrary::CreateTextureRenderTarget2D(int32 Width, int32 Height, EPixelFormat Format, bool bForceLinearGamma)
{
	return SpoutTextureUtils::CreateRenderTarget2D(Width, Height, Format, bForceLinearGamma);
}

bool USpoutBPFunctionLibrary::SpoutSender(FName SpoutName, ESpoutSendTextureFrom SendTextureFrom, UTextureRenderTarget2D* TextureRenderTarget2D)
{
	return FSpoutSender::Send(SpoutName, SendTextureFrom, TextureRenderTarget2D);
}

void USpoutBPFunctionLibrary::CloseSender(FName SpoutName)
{
	FSpoutSender::Close(SpoutName);
}

void USpoutBPFunctionLibrary::CloseReceiver(FName SpoutName, UTexture2D*& ReceivedTexture, UMaterialInstanceDynamic*& ReceivedMaterial)
{
	// Drop the receiver's registry entry (releases its shared/staging textures and cached wraps),
	// then destroy the transient texture on the game thread (unroot + release render resource).
	// Any in-flight render command keeps the underlying RHI alive via its captured ref-counted
	// handle, so this is safe to call while a receive may still be queued.
	FSpoutReceiver::Close(SpoutName);

	// Clear the MID reference before destroying the texture so the material does not retain a
	// binding to a texture whose render resource is being released. The MID is owned by the caller's
	// graph (the plugin never roots it), so dropping this reference lets it be collected once the
	// caller releases its own.
	ReceivedMaterial = nullptr;

	SpoutTextureUtils::DestroyTexture(ReceivedTexture);
}

bool USpoutBPFunctionLibrary::SpoutReceiver(
	const FName SpoutName,
	UMaterialInterface* InputMaterial,
	FName TextureParameterName,
	UMaterialInstanceDynamic*& OutMat,
	UTexture2D*& OutTexture,
	UTextureRenderTarget2D* OptionalOutputRenderTarget
)
{
	// OptionalOutputRenderTarget is reserved for future GPU-side copy paths.
	return FSpoutReceiver::Receive(SpoutName, InputMaterial, TextureParameterName, OutMat, OutTexture, OptionalOutputRenderTarget);
}

bool USpoutBPFunctionLibrary::SpoutInfo(TArray<FSenderStruct>& Senders)
{
	Senders.Empty();
	EnsureSpoutInitialized();

	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	spoutSenderNames* SenderNames = Context.GetSenderNames();
	if (!SenderNames)
	{
		return false;
	}

	// Access to the global Spout sender list is serialized via SpoutMutex.
	FScopeLock SpoutLock(&Context.GetSpoutMutex());
	int32 Count = SenderNames->GetSenderCount();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		char NameBuf[256];
		unsigned int W = 0;
		unsigned int H = 0;
		HANDLE SharedHandle = nullptr;

		SenderNames->GetSenderNameInfo(Index, NameBuf, 256, W, H, SharedHandle);

		HANDLE Handle;
		unsigned long Format;
		SenderNames->GetSenderInfo(NameBuf, W, H, Handle, Format);
		static_cast<void>(Format);

		FSenderStruct SenderInfo;
		SenderInfo.Name = FName(NameBuf);
		SenderInfo.Width = W;
		SenderInfo.Height = H;
		SenderInfo.bIsAlive = true;
		Senders.Add(SenderInfo);
	}
	return true;
}

bool USpoutBPFunctionLibrary::GetSenderInfo(FName SpoutName, FSenderStruct& OutSenderStruct)
{
	EnsureSpoutInitialized();
	unsigned int W = 0;
	unsigned int H = 0;
	HANDLE Handle = nullptr;
	unsigned long Format = 0;

	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	FScopeLock SpoutLock(&Context.GetSpoutMutex());
	if (spoutSenderNames* SenderNames = Context.GetSenderNames())
	{
		// Spout returns a shared handle + format for the sender if it exists.
		if (!SenderNames->GetSenderInfo(TCHAR_TO_UTF8(*SpoutName.ToString()), W, H, Handle, Format))
		{
			return false;
		}
	}
	else
	{
		return false;
	}

	OutSenderStruct.Name = SpoutName;
	OutSenderStruct.Width = W;
	OutSenderStruct.Height = H;
	OutSenderStruct.bIsAlive = true;
	static_cast<void>(Format);
	return true;
}

void USpoutBPFunctionLibrary::GetMaxSenders(int32& Max)
{
	EnsureSpoutInitialized();
	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	if (spoutSenderNames* SenderNames = Context.GetSenderNames())
	{
		// Spout registry uses global state; protect access.
		FScopeLock SpoutLock(&Context.GetSpoutMutex());
		Max = SenderNames->GetMaxSenders();
	}
}
