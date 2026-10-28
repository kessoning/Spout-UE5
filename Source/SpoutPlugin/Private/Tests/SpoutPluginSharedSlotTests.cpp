// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
//
// The shared-slot interop path (SpoutSharedSlot.h). These tests require the shared-slot path to be latched in the
// process (r.Spout.SharedSlotInterop 1, the default). The legacy wrap path is checked in a separate process with
// its own command (SpoutT1Commands.cpp), which never selects these tests.
//
//   latch       the path is fixed at the first initialization and a later setting change does not move it
//   formats     exact round trips through the plugin's own sender and receiver in BGRA8, RGBA8 (linear and sRGB),
//               RGB10A2 and RGBA16F, with no D3D11-on-12 wrap made at all
//   lifecycle   two streams of different sizes at once, a resize, a close and reopen of one name, and the
//               retirement of every replaced or closed set within a bound
//   retirement  a retired set is released only once its fence reached the recorded value, or its event-query proof
//               reported; an unprovable set is never released
//   failures    a receive copy that was late is never published and the next call delivers; a failed native signal
//               stops that stream until its identity changes, and its objects are released only after the proof; a
//               send whose fill timed out publishes nothing and the next one does; a failed send drain publishes
//               nothing, keeps its texture provable, and the next source size publishes again
//   access      a late receive keeps the peer's access until its read is proven finished; a failed send drain whose
//               proof query reports errors keeps access until the proof arrives (a probe thread plays the peer)
// The send failure and send access tests are tests of the synchronous send (FSpoutSlotSend): they pin the development
// switch r.Spout.Dev.AsyncSend to 0 for their duration and check that their sender took the synchronous path, so they
// mean the same thing whatever the process's setting (the asynchronous sender has its own tests).
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
#include "SpoutAsyncReceive.h"
#include "SpoutBPFunctionLibrary.h"
#include "SpoutD3DContext.h"
#include "SpoutSenderRegistry.h"
#include "SpoutSharedSlot.h"
#include "SpoutSlotRules.h"
#include "Async/Async.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>

#define SPOUT_SLOT_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

namespace SpoutSharedSlotTests
{
	/** Every texel of a texture, read back the same way for a source and its received copy. */
	struct FSlotPixels
	{
		TArray<FLinearColor> Linear;     // 8- and 10-bit formats: the conversion to float loses nothing
		TArray<FFloat16Color> Half;      // RGBA16F: the stored halves themselves
		bool operator==(const FSlotPixels& O) const
		{
			if (Linear.Num() != O.Linear.Num() || Half.Num() != O.Half.Num()) return false;
			for (int32 I = 0; I < Linear.Num(); ++I) if (!(Linear[I] == O.Linear[I])) return false;
			for (int32 I = 0; I < Half.Num(); ++I)
			{
				if (Half[I].R.Encoded != O.Half[I].R.Encoded || Half[I].G.Encoded != O.Half[I].G.Encoded
					|| Half[I].B.Encoded != O.Half[I].B.Encoded || Half[I].A.Encoded != O.Half[I].A.Encoded) return false;
			}
			return true;
		}
		/** At least two different texels: a uniform image cannot show that a copy happened. */
		bool IsVaried() const
		{
			for (int32 I = 1; I < Linear.Num(); ++I) if (!(Linear[I] == Linear[0])) return true;
			for (int32 I = 1; I < Half.Num(); ++I) if (Half[I].R.Encoded != Half[0].R.Encoded || Half[I].G.Encoded != Half[0].G.Encoded) return true;
			return false;
		}
	};

	void ReadTexture(FRHITexture* Texture, FSlotPixels& Out)
	{
		Out = FSlotPixels();
		if (!Texture) return;
		const int32 W = Texture->GetDesc().Extent.X, H = Texture->GetDesc().Extent.Y;
		const EPixelFormat Format = Texture->GetFormat();
		FSlotPixels* Target = &Out;
		ENQUEUE_RENDER_COMMAND(SpoutSlotTestRead)([Texture, W, H, Format, Target](FRHICommandListImmediate& Cmd)
		{
			Cmd.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
			if (Format == PF_FloatRGBA)
			{
				Cmd.ReadSurfaceFloatData(Texture, FIntRect(0, 0, W, H), Target->Half, CubeFace_PosX, 0, 0);
			}
			else
			{
				FReadSurfaceDataFlags Flags(RCM_MinMax);
				Flags.SetLinearToGamma(false);
				Cmd.ReadSurfaceData(Texture, FIntRect(0, 0, W, H), Target->Linear, Flags);
			}
			Cmd.Transition(FRHITransitionInfo(Texture, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		});
		FlushRenderingCommands();
	}

	int32 BytesPerTexel(EPixelFormat Format) { return Format == PF_FloatRGBA ? 8 : 4; }

	/** Raw texels of a pattern that changes with Sequence, in Format's own layout. */
	TArray<uint8> MakeRawPattern(EPixelFormat Format, int32 W, int32 H, int32 Sequence)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(W * H * BytesPerTexel(Format));
		for (int32 Y = 0; Y < H; ++Y)
		for (int32 X = 0; X < W; ++X)
		{
			const int32 I = Y * W + X;
			if (Format == PF_FloatRGBA)
			{
				FFloat16Color C(FLinearColor(float(X) / float(W), float(Y) / float(H), float((Sequence * 7) % 16) / 16.0f, float((X + Y) % 4) / 4.0f));
				FMemory::Memcpy(&Bytes[I * 8], &C, 8);
			}
			else if (Format == PF_A2B10G10R10)
			{
				const uint32 R = uint32(X * 37 + Sequence) & 1023u, G = uint32(Y * 53 + Sequence * 3) & 1023u, B = uint32(X + Y + Sequence * 7) & 1023u, A = uint32(X + Y) & 3u;
				const uint32 Packed = (A << 30) | (B << 20) | (G << 10) | R;
				FMemory::Memcpy(&Bytes[I * 4], &Packed, 4);
			}
			else
			{
				Bytes[I * 4 + 0] = uint8((X * 17 + Sequence) & 255);
				Bytes[I * 4 + 1] = uint8((Y * 29 + Sequence) & 255);
				Bytes[I * 4 + 2] = uint8((X + Y + Sequence * 13) & 255);
				Bytes[I * 4 + 3] = uint8((X * 3 + Y * 7 + Sequence) & 255);
			}
		}
		return Bytes;
	}

	void Upload(UTextureRenderTarget2D* Target, int32 Sequence)
	{
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		const int32 W = Target->SizeX, H = Target->SizeY;
		const EPixelFormat Format = Target->GetFormat();
		TArray<uint8> Bytes = MakeRawPattern(Format, W, H, Sequence);
		const int32 Pitch = W * BytesPerTexel(Format);
		ENQUEUE_RENDER_COMMAND(SpoutSlotTestUpload)([Resource, W, H, Pitch, Bytes = MoveTemp(Bytes)](FRHICommandListImmediate& Cmd)
		{
			Cmd.UpdateTexture2D(Resource->GetRenderTargetTexture(), 0, FUpdateTextureRegion2D(0, 0, 0, 0, W, H), Pitch, Bytes.GetData());
		});
		FlushRenderingCommands();
	}

	FRHITexture* SourceTexture(UTextureRenderTarget2D* Target)
	{
		FTextureRenderTargetResource* Resource = Target ? Target->GameThread_GetRenderTargetResource() : nullptr;
		return Resource ? Resource->GetRenderTargetTexture() : nullptr;
	}

	FRHITexture* ReceivedTexture(UTexture2D* Texture)
	{
		FTextureResource* Resource = Texture ? Texture->GetResource() : nullptr;
		return Resource ? Resource->GetTextureRHI() : nullptr;
	}

	FName UniqueName(const TCHAR* What)
	{
		return FName(*FString::Printf(TEXT("SpoutSlot_%s_%s"), What, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	/** One send of the target's current content, completed on the render thread. */
	bool Send(const FName& Name, UTextureRenderTarget2D* Source)
	{
		const bool bAccepted = USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source);
		FlushRenderingCommands();
		return bAccepted;
	}

	/** One receive call, completed on the render thread. */
	bool Receive(const FName& Name, UTexture2D*& Texture, UMaterialInstanceDynamic*& Material, ESpoutColorSpace Mode = ESpoutColorSpace::Linear)
	{
		const bool bAccepted = USpoutBPFunctionLibrary::SpoutReceiver(Name, nullptr, NAME_None, Material, Texture, nullptr, Mode, /*bAsyncReceive: the synchronous receive under test*/ false);
		FlushRenderingCommands();
		return bAccepted && Texture;
	}

	uint64 ReceivedNumber(const FName& Name)
	{
		const TSharedPtr<FSpoutSharedSender> Entry = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Receiver);
		return Entry ? Entry->ReceivedFrameNumber : 0;
	}

	/** Polls the process's retirement list until Released grew by at least Count, or TimeoutSeconds passed. */
	bool WaitForReleases(int64 ReleasedBefore, int64 Count, double TimeoutSeconds)
	{
		const double End = FPlatformTime::Seconds() + TimeoutSeconds;
		for (;;)
		{
			FlushRenderingCommands();
			FSpoutGpuRetirement::Get().Poll();
			if (FSpoutGpuRetirement::Get().GetStats().Released >= ReleasedBefore + Count) return true;
			if (FPlatformTime::Seconds() > End) return false;
			FPlatformProcess::Sleep(0.005f);
		}
	}

	bool RequireSharedSlot(FAutomationTestBase& Test)
	{
		FSpoutD3DContext::Get().InitializeIfNeeded();
		return Test.TestTrue(TEXT("this process latched the shared-slot path (r.Spout.SharedSlotInterop 1); these tests do not belong to the legacy control process"),
			SpoutInterop::IsSharedSlot());
	}

	/**
	 * Pins r.Spout.Dev.AsyncSend to 0 for a test of the synchronous send, at console priority (so a switch set from the
	 * command line, an ini file or the console does not override it), and gives the variable back its value and
	 * priority afterwards. A sender entry decides its path at its first send, so the test's own new name sends
	 * synchronously whatever the process's setting.
	 */
	struct FSynchronousSend
	{
		IConsoleVariable* Variable = nullptr;
		FString Before;
		EConsoleVariableFlags Priority = ECVF_SetByCode;
		FSynchronousSend()
		{
			Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.Dev.AsyncSend"));
			if (Variable)
			{
				Before = Variable->GetString();
				Priority = static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask);
				Variable->Set(TEXT("0"), ECVF_SetByConsole);
			}
		}
		~FSynchronousSend()
		{
			if (Variable)
			{
				Variable->Set(*Before, ECVF_SetByConsole);
				Variable->SetFlags(static_cast<EConsoleVariableFlags>((Variable->GetFlags() & ~ECVF_SetByMask) | Priority));
			}
		}
	};
}

#include "Windows/AllowWindowsPlatformTypes.h"
namespace SpoutSharedSlotTests
{
	/** A COM object whose final release is observed: what a retirement record holds in the retirement tests. */
	class FObservedUnknown final : public IUnknown
	{
	public:
		explicit FObservedUnknown(TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> InReleased) : Released(InReleased) {}
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** Out) override { if (Out) *Out = nullptr; return E_NOINTERFACE; }
		ULONG STDMETHODCALLTYPE AddRef() override { return ++Refs; }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const ULONG Left = --Refs;
			if (Left == 0) { Released->store(true); delete this; }
			return Left;
		}
	private:
		std::atomic<ULONG> Refs{1};
		// A timed-out retirement can outlive the test scope. Own its observation state.
		TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Released;
	};
}
#include "Windows/HideWindowsPlatformTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSlotLatch, "SpoutPlugin.SharedSlot.LatchedAtStartupAndNotMovedByTheSetting", SPOUT_SLOT_FLAGS)
bool FSpoutSlotLatch::RunTest(const FString&)
{
	if (!SpoutSharedSlotTests::RequireSharedSlot(*this)) return false;
	TestFalse(TEXT("the latch states why"), SpoutInterop::GetLatchReason().IsEmpty());
	IConsoleVariable* Setting = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.SharedSlotInterop"));
	if (!TestNotNull(TEXT("the setting exists"), Setting)) return false;
	const int32 Previous = Setting->GetInt();
	const EConsoleVariableFlags PreviousPriority = static_cast<EConsoleVariableFlags>(Setting->GetFlags() & ECVF_SetByMask);
	Setting->Set(0, ECVF_SetByCode);
	FSpoutD3DContext::Get().InitializeIfNeeded();
	SpoutInterop::NoteSettingChange();
	TestTrue(TEXT("a later change of the setting does not move the latched path"), SpoutInterop::IsSharedSlot());
	Setting->Set(Previous, ECVF_SetByCode);
	Setting->ClearFlags(ECVF_SetByMask);
	Setting->SetFlags(PreviousPriority);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSlotFormats, "SpoutPlugin.SharedSlot.ExactRoundTripsInEveryFormatWithoutAnyWrap", SPOUT_SLOT_FLAGS)
bool FSpoutSlotFormats::RunTest(const FString&)
{
	if (!SpoutSharedSlotTests::RequireSharedSlot(*this)) return false;
	IConsoleVariable* Path = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Spout.GPUReceiver"));
	const int32 PreviousPath = Path->GetInt();
	const EConsoleVariableFlags PreviousPriority = static_cast<EConsoleVariableFlags>(Path->GetFlags() & ECVF_SetByMask);
	// These receives ask for the synchronous receive (Receive above), so they use the synchronous paths under test
	// whatever the retired r.Spout.AsyncReceive holds.
	const int64 WrapsBefore = SpoutInterop::GetWrapCallCount();

	struct FCase { EPixelFormat Format; bool bLinear; ESpoutColorSpace Mode; };
	const FCase Cases[] = {
		{ PF_B8G8R8A8, true, ESpoutColorSpace::Linear }, { PF_B8G8R8A8, false, ESpoutColorSpace::Auto },
		{ PF_R8G8B8A8, true, ESpoutColorSpace::Linear }, { PF_R8G8B8A8, false, ESpoutColorSpace::Auto },
		{ PF_A2B10G10R10, true, ESpoutColorSpace::Linear }, { PF_FloatRGBA, true, ESpoutColorSpace::Linear } };
	// Run the same pixel oracle through both paths: native CPU staging must
	// preserve formats and colour interpretation exactly like the GPU path.
	for (int32 GPUPath : { 1, 0 })
	{
		Path->Set(GPUPath, ECVF_SetByCode);
		for (const FCase& Case : Cases)
		{
			const FString What = FString::Printf(TEXT("%s %s %s"), GPUPath ? TEXT("GPU") : TEXT("CPU"), GetPixelFormatString(Case.Format), Case.bLinear ? TEXT("linear") : TEXT("sRGB"));
			const FName Name = SpoutSharedSlotTests::UniqueName(TEXT("Format"));
			TStrongObjectPtr<UTextureRenderTarget2D> Source(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
			Source->InitCustomFormat(64, 16, Case.Format, Case.bLinear);
			UTexture2D* Texture = nullptr; UMaterialInstanceDynamic* Material = nullptr;
			for (int32 Sequence = 1; Sequence <= 3; ++Sequence)
			{
				SpoutSharedSlotTests::Upload(Source.Get(), Sequence);
				TestTrue(What + TEXT(": send accepted"), SpoutSharedSlotTests::Send(Name, Source.Get()));
				const bool bReceived = SpoutSharedSlotTests::Receive(Name, Texture, Material, Case.Mode);
				TestTrue(What + TEXT(": receive accepted"), bReceived);
				if (!bReceived) break;
				SpoutSharedSlotTests::FSlotPixels Sent, Got;
				SpoutSharedSlotTests::ReadTexture(SpoutSharedSlotTests::SourceTexture(Source.Get()), Sent);
				SpoutSharedSlotTests::ReadTexture(SpoutSharedSlotTests::ReceivedTexture(Texture), Got);
				TestTrue(What + TEXT(": the source is varied"), Sent.IsVaried());
				TestEqual(What + TEXT(": the received texture has the source's pixel format"), static_cast<int32>(Texture->GetPixelFormat()), static_cast<int32>(Case.Format));
				TestTrue(FString::Printf(TEXT("%s: sequence %d arrives exactly, every texel"), *What, Sequence), Got == Sent);
			}
			const TSharedPtr<FSpoutSharedSender> Sender = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Sender);
			const TSharedPtr<FSpoutSharedSender> Receiver = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Receiver);
			TestTrue(What + TEXT(": the sender published through the slot"), Sender && Sender->SlotTransfers.load() >= 3);
			if (GPUPath)
			{
				TestTrue(What + TEXT(": the receiver delivered through the slots"), Receiver && Receiver->SlotTransfers.load() >= 3);
			}
			else
			{
				TestTrue(What + TEXT(": the CPU receiver completed every frame"), Receiver && Receiver->ReceivedFrameNumber >= 3);
				Microsoft::WRL::ComPtr<ID3D11Device> SharedDevice, StagingDevice;
				if (Receiver && Receiver->SharedTexture) Receiver->SharedTexture->GetDevice(SharedDevice.GetAddressOf());
				if (Receiver && Receiver->StagingTexture) Receiver->StagingTexture->GetDevice(StagingDevice.GetAddressOf());
				ID3D11Device* Native = FSpoutAsyncDevice::Get().GetDevice();
				TestTrue(What + TEXT(": shared and staging textures both belong to the native device"),
					Native && SharedDevice.Get() == Native && StagingDevice.Get() == Native);
			}
			USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material);
			USpoutBPFunctionLibrary::CloseSender(Name);
			FlushRenderingCommands();
		}
	}
	TestEqual(TEXT("no D3D11-on-12 wrap was made in any of these transfers"), SpoutInterop::GetWrapCallCount(), WrapsBefore);
	Path->Set(PreviousPath, ECVF_SetByCode);
	Path->ClearFlags(ECVF_SetByMask);
	Path->SetFlags(PreviousPriority);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSlotLifecycle, "SpoutPlugin.SharedSlot.TwoStreamsResizeReopenAndRetirement", SPOUT_SLOT_FLAGS)
bool FSpoutSlotLifecycle::RunTest(const FString&)
{
	if (!SpoutSharedSlotTests::RequireSharedSlot(*this)) return false;
	const int64 ReleasedBefore = FSpoutGpuRetirement::Get().GetStats().Released;
	const int64 RetiredBefore = FSpoutGpuRetirement::Get().GetStats().Retired;
	const FName A = SpoutSharedSlotTests::UniqueName(TEXT("StreamA")), B = SpoutSharedSlotTests::UniqueName(TEXT("StreamB"));
	TStrongObjectPtr<UTextureRenderTarget2D> SourceA(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
	TStrongObjectPtr<UTextureRenderTarget2D> SourceB(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
	SourceA->InitCustomFormat(64, 32, PF_B8G8R8A8, true);
	SourceB->InitCustomFormat(48, 24, PF_A2B10G10R10, true);
	UTexture2D* TextureA = nullptr; UTexture2D* TextureB = nullptr;
	UMaterialInstanceDynamic* MaterialA = nullptr; UMaterialInstanceDynamic* MaterialB = nullptr;
	auto Check = [this](const TCHAR* Step, UTextureRenderTarget2D* Source, UTexture2D* Texture)
	{
		SpoutSharedSlotTests::FSlotPixels Sent, Got;
		SpoutSharedSlotTests::ReadTexture(SpoutSharedSlotTests::SourceTexture(Source), Sent);
		SpoutSharedSlotTests::ReadTexture(SpoutSharedSlotTests::ReceivedTexture(Texture), Got);
		TestTrue(FString::Printf(TEXT("%s: the received texture matches the source exactly"), Step), Sent.IsVaried() && Got == Sent);
	};

	// Two streams at once, interleaved.
	for (int32 Sequence = 1; Sequence <= 3; ++Sequence)
	{
		SpoutSharedSlotTests::Upload(SourceA.Get(), Sequence); SpoutSharedSlotTests::Upload(SourceB.Get(), Sequence + 40);
		SpoutSharedSlotTests::Send(A, SourceA.Get()); SpoutSharedSlotTests::Send(B, SourceB.Get());
		TestTrue(TEXT("stream A received"), SpoutSharedSlotTests::Receive(A, TextureA, MaterialA));
		TestTrue(TEXT("stream B received"), SpoutSharedSlotTests::Receive(B, TextureB, MaterialB));
		Check(TEXT("two streams: A"), SourceA.Get(), TextureA);
		Check(TEXT("two streams: B"), SourceB.Get(), TextureB);
	}

	// A resize of A: its sender's slot and its receiver's slots are replaced, the old ones retired.
	SourceA->InitCustomFormat(96, 48, PF_B8G8R8A8, true);
	SpoutSharedSlotTests::Upload(SourceA.Get(), 7);
	SpoutSharedSlotTests::Send(A, SourceA.Get());
	for (int32 Attempt = 0; Attempt < 4 && (!TextureA || TextureA->GetSizeX() != 96); ++Attempt)
	{
		SpoutSharedSlotTests::Receive(A, TextureA, MaterialA);
	}
	TestTrue(TEXT("after the resize the received texture has the new size"), TextureA && TextureA->GetSizeX() == 96 && TextureA->GetSizeY() == 48);
	Check(TEXT("after the resize"), SourceA.Get(), TextureA);
	SpoutSharedSlotTests::Upload(SourceB.Get(), 50); SpoutSharedSlotTests::Send(B, SourceB.Get()); SpoutSharedSlotTests::Receive(B, TextureB, MaterialB);
	Check(TEXT("stream B, unaffected by A's resize"), SourceB.Get(), TextureB);

	// A close and reopen of B under the same name.
	USpoutBPFunctionLibrary::CloseReceiver(B, TextureB, MaterialB);
	USpoutBPFunctionLibrary::CloseSender(B);
	FlushRenderingCommands();
	TextureB = nullptr; MaterialB = nullptr;
	SpoutSharedSlotTests::Upload(SourceB.Get(), 60); SpoutSharedSlotTests::Send(B, SourceB.Get());
	TestTrue(TEXT("stream B received after its reopen"), SpoutSharedSlotTests::Receive(B, TextureB, MaterialB));
	Check(TEXT("after the reopen"), SourceB.Get(), TextureB);

	USpoutBPFunctionLibrary::CloseReceiver(A, TextureA, MaterialA);
	USpoutBPFunctionLibrary::CloseSender(A);
	USpoutBPFunctionLibrary::CloseReceiver(B, TextureB, MaterialB);
	USpoutBPFunctionLibrary::CloseSender(B);
	FlushRenderingCommands();
	const int64 Retired = FSpoutGpuRetirement::Get().GetStats().Retired - RetiredBefore;
	// A: two sender slots and two receive sets; B: two of each (before and after the reopen).
	TestTrue(FString::Printf(TEXT("every replaced or closed set was retired (%lld)"), Retired), Retired >= 8);
	TestTrue(TEXT("and every one of them was released once its work finished, within five seconds"), SpoutSharedSlotTests::WaitForReleases(ReleasedBefore, Retired, 5.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSlotRetirement, "SpoutPlugin.SharedSlot.RetirementWaitsForTheProofOfCompletion", SPOUT_SLOT_FLAGS)
bool FSpoutSlotRetirement::RunTest(const FString&)
{
	if (!SpoutSharedSlotTests::RequireSharedSlot(*this)) return false;
	ID3D12Device* Device12 = static_cast<ID3D12Device*>(GDynamicRHI->RHIGetNativeDevice());
	if (!TestNotNull(TEXT("Unreal's D3D12 device"), Device12)) return false;

	// Each case has its own list: one failed proof must not contaminate the next case.
	// A fence condition: not released before the fence reached the recorded value.
	{
		Microsoft::WRL::ComPtr<ID3D12Fence> Fence;
		if (!TestTrue(TEXT("a test fence"), SUCCEEDED(Device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(Fence.GetAddressOf()))))) return false;
		const auto bReleased = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
		FSpoutGpuRetirement Local;
		TUniquePtr<FSpoutGpuRetirement::FRetiredSet> Set = MakeUnique<FSpoutGpuRetirement::FRetiredSet>();
		Set->What = TEXT("test: fence condition");
		Set->QueueFence = Fence;
		Set->Outstanding.QueueSignalQueued(2);
		SpoutSharedSlotTests::FObservedUnknown* Object = new SpoutSharedSlotTests::FObservedUnknown(bReleased);
		Set->Objects.Add(TRefCountPtr<IUnknown>(Object));
		Object->Release(); // the record's reference is now the only one
		Local.Retire(MoveTemp(Set));
		Local.Poll();
		TestFalse(TEXT("not released while the fence is at 0 of 2"), bReleased->load());
		Fence->Signal(1);
		Local.Poll();
		TestFalse(TEXT("not released at 1 of 2"), bReleased->load());
		Fence->Signal(2);
		Local.Poll();
		TestTrue(TEXT("released once the fence reached 2"), bReleased->load());
	}

	// An event-query proof after a failed signal: released once the query reports.
	{
		FSpoutAsyncDevice& Async = FSpoutAsyncDevice::Get();
		Microsoft::WRL::ComPtr<ID3D11Query> Query;
		D3D11_QUERY_DESC Desc = { D3D11_QUERY_EVENT, 0 };
		if (!TestTrue(TEXT("an event query on the native device"), Async.GetDevice() && SUCCEEDED(Async.GetDevice()->CreateQuery(&Desc, Query.GetAddressOf())))) return false;
		{
			FScopeLock ContextLock(&Async.GetContextMutex());
			Async.GetContext()->End(Query.Get());
			Async.GetContext()->Flush();
		}
		const auto bReleased = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
		FSpoutGpuRetirement Local;
		TUniquePtr<FSpoutGpuRetirement::FRetiredSet> Set = MakeUnique<FSpoutGpuRetirement::FRetiredSet>();
		Set->What = TEXT("test: proof query");
		Set->Proof = Query;
		Set->Outstanding.NativeSignalFailed(true);
		SpoutSharedSlotTests::FObservedUnknown* Object = new SpoutSharedSlotTests::FObservedUnknown(bReleased);
		Set->Objects.Add(TRefCountPtr<IUnknown>(Object));
		Object->Release();
		Local.Retire(MoveTemp(Set));
		const double End = FPlatformTime::Seconds() + 2.0;
		while (!bReleased->load() && FPlatformTime::Seconds() < End)
		{
			Local.Poll();
			FPlatformProcess::Sleep(0.001f);
		}
		TestTrue(TEXT("released once the event query reported"), bReleased->load());
		if (!bReleased->load())
		{
			// Observe the failure without flushing or changing the production polling behavior.
			HRESULT QueryResult = E_UNEXPECTED;
			{
				FScopeLock ContextLock(&Async.GetContextMutex());
				QueryResult = Async.GetContext()->GetData(Query.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
			}
			AddInfo(FString::Printf(TEXT("Retirement query timeout: GetData=0x%08x nativeRemoved=0x%08x unrealRemoved=0x%08x pending=%d; query was End/Flush issued and polled for the unchanged two-second bound."),
				static_cast<uint32>(QueryResult), static_cast<uint32>(Async.GetDevice()->GetDeviceRemovedReason()),
				static_cast<uint32>(Device12->GetDeviceRemovedReason()), Local.GetStats().Pending));
		}
	}

	// No proof possible: never released while the native device lives, even with every fence reached.
	{
		const auto bReleased = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
		FSpoutGpuRetirement Local;
		TUniquePtr<FSpoutGpuRetirement::FRetiredSet> Set = MakeUnique<FSpoutGpuRetirement::FRetiredSet>();
		Set->What = TEXT("test: unprovable");
		Set->Outstanding.NativeSignalFailed(false);
		SpoutSharedSlotTests::FObservedUnknown* Object = new SpoutSharedSlotTests::FObservedUnknown(bReleased);
		Set->Objects.Add(TRefCountPtr<IUnknown>(Object));
		Object->Release();
		Local.Retire(MoveTemp(Set));
		for (int32 I = 0; I < 10; ++I) Local.Poll();
		TestFalse(TEXT("an unprovable set is not released"), bReleased->load());
		TestEqual(TEXT("it is still pending"), Local.GetStats().Pending, 1);
		// It holds no GPU object: the test may drop it.
		Local.DiscardForTest();
		TestTrue(TEXT("dropped by the test"), bReleased->load());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSlotReceiveFailures, "SpoutPlugin.SharedSlot.ReceiveTimeoutAndFailedSignal", SPOUT_SLOT_FLAGS)
bool FSpoutSlotReceiveFailures::RunTest(const FString&)
{
	if (!SpoutSharedSlotTests::RequireSharedSlot(*this)) return false;
	const FName Name = SpoutSharedSlotTests::UniqueName(TEXT("ReceiveFailures"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
	Source->InitCustomFormat(64, 32, PF_B8G8R8A8, true);
	UTexture2D* Texture = nullptr; UMaterialInstanceDynamic* Material = nullptr;
	auto Matches = [&]()
	{
		SpoutSharedSlotTests::FSlotPixels Sent, Got;
		SpoutSharedSlotTests::ReadTexture(SpoutSharedSlotTests::SourceTexture(Source.Get()), Sent);
		SpoutSharedSlotTests::ReadTexture(SpoutSharedSlotTests::ReceivedTexture(Texture), Got);
		return Sent.IsVaried() && Got == Sent;
	};

	// A receive copy that finished after its bound (late): not published, the frame stays pending, the next call delivers it.
	// The access it held is covered by SpoutPlugin.SharedSlot.ReceiveHoldsPeerAccessUntilItsReadIsProven.
	SpoutSharedSlotTests::Upload(Source.Get(), 1);
	SpoutSharedSlotTests::Send(Name, Source.Get());
	SpoutSlotTestHooks::TimeoutReceiveWaits = 1;
	SpoutSharedSlotTests::Receive(Name, Texture, Material);
	TestEqual(TEXT("the timeout hook was used"), SpoutSlotTestHooks::TimeoutReceiveWaits.load(), 0);
	TSharedPtr<FSpoutSharedSender> Receiver = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Receiver);
	if (!TestTrue(TEXT("a receive set exists"), Receiver && Receiver->SlotReceive.IsValid())) return false;
	TestEqual(TEXT("a timed-out copy delivers nothing"), Receiver->ReceivedFrameNumber, uint64(0));
	TestEqual(TEXT("and its write was given up as a completion timeout"),
		Receiver->SlotReceive->GetCounters().Abandoned[static_cast<int32>(SpoutAsync::EAbandon::CompletionTimeout)], int64(1));
	TestEqual(TEXT("and never published"), Receiver->SlotReceive->GetCounters().Published, int64(0));
	TestTrue(TEXT("its value stays recorded: the timed-out copy is still accounted for"), Receiver->SlotReceive->GetOutstanding().GetNativeRequired() >= 1);
	SpoutSharedSlotTests::Receive(Name, Texture, Material);
	TestEqual(TEXT("the next call delivers the pending frame"), Receiver->ReceivedFrameNumber, uint64(1));
	TestTrue(TEXT("exactly"), Matches());

	// A failed native signal: the stream stops for this identity; its objects wait for the event-query proof.
	const int64 ReleasedBefore = FSpoutGpuRetirement::Get().GetStats().Released;
	SpoutSharedSlotTests::Upload(Source.Get(), 2);
	SpoutSharedSlotTests::Send(Name, Source.Get());
	SpoutSlotTestHooks::FailNativeSignals = 1;
	SpoutSharedSlotTests::Receive(Name, Texture, Material);
	TestEqual(TEXT("the signal hook was used"), SpoutSlotTestHooks::FailNativeSignals.load(), 0);
	TestTrue(TEXT("the receive set stopped"), Receiver->SlotReceive.IsValid() && Receiver->SlotReceive->HasFailed());
	TestEqual(TEXT("nothing was delivered"), Receiver->ReceivedFrameNumber, uint64(1));
	TestTrue(TEXT("its completion now needs the event query"), Receiver->SlotReceive->GetOutstanding().GetProof() != SpoutSlotRules::EProof::NotNeeded);
	SpoutSharedSlotTests::Receive(Name, Texture, Material);
	TestEqual(TEXT("a stopped set does not transfer"), Receiver->ReceivedFrameNumber, uint64(1));

	// A new sender identity: the stopped set is retired, a new one delivers, and the old one is released after its proof.
	Source->InitCustomFormat(80, 40, PF_B8G8R8A8, true);
	SpoutSharedSlotTests::Upload(Source.Get(), 3);
	SpoutSharedSlotTests::Send(Name, Source.Get());
	for (int32 Attempt = 0; Attempt < 4 && (!Texture || Texture->GetSizeX() != 80); ++Attempt)
	{
		SpoutSharedSlotTests::Receive(Name, Texture, Material);
	}
	TestTrue(TEXT("a new identity starts a new set"), Receiver->SlotReceive.IsValid() && !Receiver->SlotReceive->HasFailed());
	TestTrue(TEXT("which delivers exactly"), Texture && Texture->GetSizeX() == 80 && Matches());
	// Two sets were retired since: the stopped receive set and the sender's slot of the old size.
	TestTrue(TEXT("both, the stopped set included, were released once their proofs reported"), SpoutSharedSlotTests::WaitForReleases(ReleasedBefore, 2, 5.0));

	USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material);
	USpoutBPFunctionLibrary::CloseSender(Name);
	FlushRenderingCommands();
	SpoutSlotTestHooks::TimeoutReceiveWaits = 0;
	SpoutSlotTestHooks::FailNativeSignals = 0;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSlotSendFailures, "SpoutPlugin.SharedSlot.SendFillTimeoutAndFailedDrain", SPOUT_SLOT_FLAGS)
bool FSpoutSlotSendFailures::RunTest(const FString&)
{
	if (!SpoutSharedSlotTests::RequireSharedSlot(*this)) return false;
	SpoutSharedSlotTests::FSynchronousSend Synchronous;
	const FName Name = SpoutSharedSlotTests::UniqueName(TEXT("SendFailures"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
	Source->InitCustomFormat(64, 32, PF_B8G8R8A8, true);
	UTexture2D* Texture = nullptr; UMaterialInstanceDynamic* Material = nullptr;
	SpoutSharedSlotTests::FSlotPixels Expected;
	auto ReceivedEquals = [&](const SpoutSharedSlotTests::FSlotPixels& Want)
	{
		SpoutSharedSlotTests::FSlotPixels Got;
		SpoutSharedSlotTests::ReadTexture(SpoutSharedSlotTests::ReceivedTexture(Texture), Got);
		return Want.IsVaried() && Got == Want;
	};

	SpoutSharedSlotTests::Upload(Source.Get(), 1);
	SpoutSharedSlotTests::Send(Name, Source.Get());
	SpoutSharedSlotTests::Receive(Name, Texture, Material);
	SpoutSharedSlotTests::ReadTexture(SpoutSharedSlotTests::SourceTexture(Source.Get()), Expected);
	TestTrue(TEXT("the first frame arrives"), ReceivedEquals(Expected));
	TSharedPtr<FSpoutSharedSender> Sender = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Sender);
	TestTrue(TEXT("the sender took the synchronous send"), Sender && Sender->AsyncSendMode != 1 && !Sender->AsyncSend.IsValid());
	if (!TestTrue(TEXT("a send slot exists"), Sender && Sender->SlotSend.IsValid())) return false;
	const int64 PublishedBefore = Sender->SlotTransfers.load();

	// A fill that timed out publishes nothing; the receiver keeps the first frame; the next send publishes.
	SpoutSharedSlotTests::Upload(Source.Get(), 2);
	SpoutSlotTestHooks::TimeoutFilledWaits = 1;
	SpoutSharedSlotTests::Send(Name, Source.Get());
	TestEqual(TEXT("the fill-timeout hook was used"), SpoutSlotTestHooks::TimeoutFilledWaits.load(), 0);
	TestEqual(TEXT("a timed-out fill publishes nothing"), Sender->SlotTransfers.load(), PublishedBefore);
	SpoutSharedSlotTests::Receive(Name, Texture, Material);
	TestTrue(TEXT("the receiver still has the first frame"), ReceivedEquals(Expected));
	SpoutSharedSlotTests::Upload(Source.Get(), 3);
	SpoutSharedSlotTests::Send(Name, Source.Get());
	SpoutSharedSlotTests::Receive(Name, Texture, Material);
	SpoutSharedSlotTests::ReadTexture(SpoutSharedSlotTests::SourceTexture(Source.Get()), Expected);
	TestTrue(TEXT("the next send publishes and arrives exactly"), ReceivedEquals(Expected));

	// A failed drain signal: nothing published, the access is kept until the event query reports (so the texture
	// stays provable), and the slot stops until the source's size changes.
	const int64 PublishedMid = Sender->SlotTransfers.load();
	SpoutSharedSlotTests::Upload(Source.Get(), 4);
	SpoutSlotTestHooks::FailNativeSignals = 1;
	SpoutSharedSlotTests::Send(Name, Source.Get());
	TestEqual(TEXT("the signal hook was used"), SpoutSlotTestHooks::FailNativeSignals.load(), 0);
	TestEqual(TEXT("a failed drain publishes nothing"), Sender->SlotTransfers.load(), PublishedMid);
	TestTrue(TEXT("the slot stopped"), Sender->SlotSend.IsValid() && Sender->SlotSend->HasFailed());
	TestTrue(TEXT("its proof reported, so the shared texture is not marked unprovable"),
		Sender->SlotSend->GetOutstanding().GetProof() == SpoutSlotRules::EProof::Proven && !Sender->bSharedTextureWriteUnproven);
	SpoutSharedSlotTests::Upload(Source.Get(), 5);
	SpoutSharedSlotTests::Send(Name, Source.Get());
	TestEqual(TEXT("a stopped slot does not publish"), Sender->SlotTransfers.load(), PublishedMid);

	Source->InitCustomFormat(72, 36, PF_B8G8R8A8, true);
	SpoutSharedSlotTests::Upload(Source.Get(), 6);
	SpoutSharedSlotTests::Send(Name, Source.Get());
	for (int32 Attempt = 0; Attempt < 4 && (!Texture || Texture->GetSizeX() != 72); ++Attempt)
	{
		SpoutSharedSlotTests::Receive(Name, Texture, Material);
	}
	SpoutSharedSlotTests::ReadTexture(SpoutSharedSlotTests::SourceTexture(Source.Get()), Expected);
	TestTrue(TEXT("a new source size publishes again, exactly"), Texture && Texture->GetSizeX() == 72 && ReceivedEquals(Expected));

	USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material);
	USpoutBPFunctionLibrary::CloseSender(Name);
	FlushRenderingCommands();
	SpoutSlotTestHooks::TimeoutFilledWaits = 0;
	SpoutSlotTestHooks::FailNativeSignals = 0;
	return true;
}

namespace SpoutSharedSlotTests
{
	/**
	 * A peer, played by another thread: after DelaySeconds it tries to take the name's cross-process access through
	 * FrameSync (another entry of the same name), with the usual bound, and gives it back at once. 1 when it could take
	 * it, 0 when it could not.
	 */
	TFuture<int32> ProbePeerAccess(TSharedPtr<FSpoutSharedSender> Peer, void* Texture, float DelaySeconds)
	{
		return Async(EAsyncExecution::Thread, [Peer, Texture, DelaySeconds]()
		{
			FPlatformProcess::Sleep(DelaySeconds);
			FSpoutTextureAccessScope Scope(Peer->FrameSync, Texture);
			return Scope.IsAcquired() ? 1 : 0;
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSlotReceiveHoldsAccess, "SpoutPlugin.SharedSlot.ReceiveHoldsPeerAccessUntilItsReadIsProven", SPOUT_SLOT_FLAGS)
bool FSpoutSlotReceiveHoldsAccess::RunTest(const FString&)
{
	if (!SpoutSharedSlotTests::RequireSharedSlot(*this)) return false;
	const FName Name = SpoutSharedSlotTests::UniqueName(TEXT("ReceiveHold"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
	Source->InitCustomFormat(64, 32, PF_B8G8R8A8, true);
	UTexture2D* Texture = nullptr; UMaterialInstanceDynamic* Material = nullptr;
	SpoutSharedSlotTests::Upload(Source.Get(), 1);
	SpoutSharedSlotTests::Send(Name, Source.Get());
	SpoutSharedSlotTests::Receive(Name, Texture, Material);
	const TSharedPtr<FSpoutSharedSender> Sender = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Sender);
	const TSharedPtr<FSpoutSharedSender> Receiver = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Receiver);
	if (!TestTrue(TEXT("a sender with its texture and a receive set exist"), Sender && Sender->SharedTexture && Receiver && Receiver->SlotReceive.IsValid())) return false;
	const uint64 Before = Receiver->ReceivedFrameNumber;
	const int64 LateBefore = Receiver->SlotReceive->GetCounters().Abandoned[static_cast<int32>(SpoutAsync::EAbandon::CompletionTimeout)];

	// The next receive copy is late (discarded) and its read is not proven finished for 800 ms. The sender, as the peer,
	// tries to take access 200 ms in: it must be refused, because the read may still be running.
	SpoutSharedSlotTests::Upload(Source.Get(), 2);
	SpoutSharedSlotTests::Send(Name, Source.Get());
	SpoutSlotTestHooks::TimeoutReceiveWaits = 1;
	SpoutSlotTestHooks::DelayNativeCompletionMs = 800;
	TFuture<int32> During = SpoutSharedSlotTests::ProbePeerAccess(Sender, Sender->SharedTexture.Get(), 0.2f);
	const double Started = FPlatformTime::Seconds();
	SpoutSharedSlotTests::Receive(Name, Texture, Material);
	const double Took = FPlatformTime::Seconds() - Started;
	TestEqual(TEXT("while the late read was not yet proven finished, the peer could not take access"), During.Get(), 0);
	TestTrue(FString::Printf(TEXT("the receive held access until the read was proven finished (%.3f s, at least 0.8)"), Took), Took >= 0.8);
	TestEqual(TEXT("the late frame was discarded"), Receiver->ReceivedFrameNumber, Before);
	TestEqual(TEXT("as a late copy"), Receiver->SlotReceive->GetCounters().Abandoned[static_cast<int32>(SpoutAsync::EAbandon::CompletionTimeout)], LateBefore + 1);
	TestEqual(TEXT("the hooks were used"), SpoutSlotTestHooks::TimeoutReceiveWaits.load() + SpoutSlotTestHooks::DelayNativeCompletionMs.load(), 0);
	TestEqual(TEXT("once the receive returned, the peer could take access"), SpoutSharedSlotTests::ProbePeerAccess(Sender, Sender->SharedTexture.Get(), 0.0f).Get(), 1);

	SpoutSharedSlotTests::Receive(Name, Texture, Material);
	SpoutSharedSlotTests::FSlotPixels Sent, Got;
	SpoutSharedSlotTests::ReadTexture(SpoutSharedSlotTests::SourceTexture(Source.Get()), Sent);
	SpoutSharedSlotTests::ReadTexture(SpoutSharedSlotTests::ReceivedTexture(Texture), Got);
	TestEqual(TEXT("the frame, still pending, is delivered by the next call"), Receiver->ReceivedFrameNumber, Before + 1);
	TestTrue(TEXT("exactly"), Sent.IsVaried() && Got == Sent);

	USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material);
	USpoutBPFunctionLibrary::CloseSender(Name);
	FlushRenderingCommands();
	SpoutSlotTestHooks::TimeoutReceiveWaits = 0;
	SpoutSlotTestHooks::DelayNativeCompletionMs = 0;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutSlotSendHoldsAccess, "SpoutPlugin.SharedSlot.SendHoldsAccessUntilAFailedDrainIsProven", SPOUT_SLOT_FLAGS)
bool FSpoutSlotSendHoldsAccess::RunTest(const FString&)
{
	if (!SpoutSharedSlotTests::RequireSharedSlot(*this)) return false;
	SpoutSharedSlotTests::FSynchronousSend Synchronous;
	const FName Name = SpoutSharedSlotTests::UniqueName(TEXT("SendHold"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
	Source->InitCustomFormat(64, 32, PF_B8G8R8A8, true);
	UTexture2D* Texture = nullptr; UMaterialInstanceDynamic* Material = nullptr;
	SpoutSharedSlotTests::Upload(Source.Get(), 1);
	SpoutSharedSlotTests::Send(Name, Source.Get());
	SpoutSharedSlotTests::Receive(Name, Texture, Material);
	const TSharedPtr<FSpoutSharedSender> Sender = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Sender);
	const TSharedPtr<FSpoutSharedSender> Receiver = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Receiver);
	TestTrue(TEXT("the sender took the synchronous send"), Sender && Sender->AsyncSendMode != 1 && !Sender->AsyncSend.IsValid());
	if (!TestTrue(TEXT("a send slot and a receiver with its opened texture exist"), Sender && Sender->SlotSend.IsValid() && Receiver && Receiver->SharedTexture)) return false;
	const int64 PublishedBefore = Sender->SlotTransfers.load();

	// The next drain's signal fails, its proof query reports an error twice (not a removal), and the write is not proven
	// finished for 800 ms. The receiver, as the peer, tries to take access 200 ms in: it must be refused throughout,
	// and the texture being retained is no reason to give access back.
	SpoutSharedSlotTests::Upload(Source.Get(), 2);
	SpoutSlotTestHooks::FailNativeSignals = 1;
	SpoutSlotTestHooks::FailProofQueries = 2;
	SpoutSlotTestHooks::DelayNativeCompletionMs = 800;
	TFuture<int32> During = SpoutSharedSlotTests::ProbePeerAccess(Receiver, Receiver->SharedTexture.Get(), 0.2f);
	const double Started = FPlatformTime::Seconds();
	SpoutSharedSlotTests::Send(Name, Source.Get());
	const double Took = FPlatformTime::Seconds() - Started;
	TestEqual(TEXT("while the failed drain was not yet proven finished, the peer could not take access"), During.Get(), 0);
	TestTrue(FString::Printf(TEXT("the send held access until the drain was proven finished (%.3f s, at least 0.8)"), Took), Took >= 0.8);
	TestEqual(TEXT("both query errors were answered by asking again, not by giving access back"), SpoutSlotTestHooks::FailProofQueries.load(), 0);
	TestEqual(TEXT("the failed signal hook was used"), SpoutSlotTestHooks::FailNativeSignals.load(), 0);
	TestTrue(TEXT("the drain was proven finished by its event query"), Sender->SlotSend.IsValid() && Sender->SlotSend->GetOutstanding().GetProof() == SpoutSlotRules::EProof::Proven);
	TestFalse(TEXT("so the shared texture is not marked unprovable"), Sender->bSharedTextureWriteUnproven);
	TestEqual(TEXT("and nothing was announced"), Sender->SlotTransfers.load(), PublishedBefore);
	TestEqual(TEXT("once the send returned, the peer could take access"), SpoutSharedSlotTests::ProbePeerAccess(Receiver, Receiver->SharedTexture.Get(), 0.0f).Get(), 1);

	USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material);
	USpoutBPFunctionLibrary::CloseSender(Name);
	FlushRenderingCommands();
	SpoutSlotTestHooks::FailNativeSignals = 0;
	SpoutSlotTestHooks::FailProofQueries = 0;
	SpoutSlotTestHooks::DelayNativeCompletionMs = 0;
	return true;
}

#undef SPOUT_SLOT_FLAGS
#endif
