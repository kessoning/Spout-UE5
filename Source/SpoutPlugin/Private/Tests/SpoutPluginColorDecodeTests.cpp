// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
//
// Color Space sRGB on 10-bit frames. A 10-bit frame may carry display (sRGB-encoded) values, as an Unreal viewport
// stream does, or linear values, as a 10-bit render target does; the format cannot say which. Auto and Linear keep
// the texels as they are; sRGB decodes them into a linear 16-bit float texture.
//
// The tests run on the receive path the process latched: the shared-slot path by default, the legacy wrap in a
// process started with r.Spout.SharedSlotInterop 0, and the CPU readback (r.Spout.GPUReceiver 0) in both.
//
//   exact        every 10-bit code (a full ramp, independent channels, primaries, secondaries, darks and highlights)
//                and all four alpha codes, through both public receive functions, their material and output targets
//   rendered     (editor) an unlit material bound by the plugin, drawn into an 8-bit sRGB render target (a canvas draw:
//                no lighting, tone mapping, exposure or post process), shows the source codes' display values
//   session      one interpretation per session; Auto and Linear on 10-bit are the same one
//   lifecycle    resize, sender restart under the same name, receiver close and reopen
//   async        Async Receive asked for on a 10-bit sender falls back to the synchronous path and decodes
//   controls     sRGB on float is refused; sRGB on 8-bit is the sampler's decode
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
#include "SpoutBPFunctionLibrary.h"
#include "SpoutColorDecode.h"
#include "SpoutSenderRegistry.h"
#include "SpoutTestExpectations.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#if WITH_EDITOR
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "ShaderCompiler.h"
#endif
#include "Materials/MaterialInterface.h"
#include "Misc/Guid.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Runtime/Launch/Resources/Version.h"

#define SPOUT_DECODE_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

namespace SpoutColorDecodeTests
{
	constexpr int32 Width = 256;
	constexpr int32 Height = 9;

	struct FCode { uint32 R, G, B, A; };

	/** The code of texel (X, Y): rows 0-3 every code 0..1023 in grey, rows 4-7 independent channels, row 8 named colours. */
	FCode CodeAt(int32 X, int32 Y)
	{
		X %= Width; Y %= Height;
		if (Y < 4)
		{
			const uint32 C = uint32(Y * 256 + X);
			return { C, C, C, uint32(X & 3) };
		}
		if (Y < 8)
		{
			const uint32 R = uint32((Y - 4) * 256 + X);
			return { R, 1023u - R, (R * 7u) & 1023u, uint32((X + Y) & 3) };
		}
		static const FCode Named[] = {
			{1023, 0, 0, 3}, {0, 1023, 0, 3}, {0, 0, 1023, 3}, {1023, 1023, 0, 3}, {0, 1023, 1023, 3}, {1023, 0, 1023, 3},
			{1023, 1023, 1023, 3}, {0, 0, 0, 0}, {512, 0, 0, 3}, {0, 512, 0, 3}, {0, 0, 512, 3}, {1, 2, 4, 3}, {8, 16, 32, 3},
			{64, 128, 256, 3}, {900, 950, 1000, 2}, {1020, 1021, 1022, 1}, {300, 600, 900, 0}, {41, 42, 43, 3} };
		return Named[X % UE_ARRAY_COUNT(Named)];
	}

	/** R10G10B10A2_UNORM texels, R in the low bits. */
	TArray<uint8> Packed(int32 W, int32 H)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(W * H * 4);
		for (int32 Y = 0; Y < H; ++Y)
		for (int32 X = 0; X < W; ++X)
		{
			const FCode C = CodeAt(X, Y);
			const uint32 Value = (C.A << 30) | (C.B << 20) | (C.G << 10) | C.R;
			FMemory::Memcpy(&Bytes[(Y * W + X) * 4], &Value, 4);
		}
		return Bytes;
	}

	/** The exact IEC 61966-2-1 decode of a 10-bit code, in double precision. */
	double Decode(uint32 Code)
	{
		const double V = double(Code) / 1023.0;
		return V <= 0.04045 ? V / 12.92 : FMath::Pow((V + 0.055) / 1.055, 2.4);
	}
	double Encode(double Linear)
	{
		return Linear <= 0.0031308 ? 12.92 * Linear : 1.055 * FMath::Pow(Linear, 1.0 / 2.4) - 0.055;
	}
	/** The tolerance: 2e-4 absolute or 1e-3 relative, whichever is larger. */
	bool Near(double Got, double Want) { return FMath::Abs(Got - Want) <= FMath::Max(2e-4, 1e-3 * FMath::Abs(Want)); }

	UTextureRenderTarget2D* NewTarget(int32 W, int32 H, EPixelFormat Format, bool bForceLinearGamma = true)
	{
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		Target->InitCustomFormat(W, H, Format, bForceLinearGamma);
		Target->UpdateResourceImmediate(true);
		return Target;
	}

	void Upload(UTextureRenderTarget2D* Target)
	{
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		const int32 W = Target->SizeX, H = Target->SizeY;
		TArray<uint8> Bytes = Packed(W, H);
		ENQUEUE_RENDER_COMMAND(SpoutDecodeTestUpload)([Resource, W, H, Bytes = MoveTemp(Bytes)](FRHICommandListImmediate& Cmd)
		{
			Cmd.UpdateTexture2D(Resource->GetRenderTargetTexture(), 0, FUpdateTextureRegion2D(0, 0, 0, 0, W, H), W * 4, Bytes.GetData());
		});
		FlushRenderingCommands();
	}

	FRHITexture* TextureOf(UTexture* Texture)
	{
		if (UTextureRenderTarget2D* Target = Cast<UTextureRenderTarget2D>(Texture))
		{
			FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
			return Resource ? Resource->GetRenderTargetTexture() : nullptr;
		}
		FTextureResource* Resource = Texture ? Texture->GetResource() : nullptr;
		return Resource ? Resource->GetTextureRHI() : nullptr;
	}

	/** Every texel: halves for a 16-bit float texture, exact floats for 10-bit (RCM_MinMax), bytes for 8-bit. */
	struct FTexels
	{
		int32 W = 0, H = 0;
		TArray<FFloat16Color> Half;
		TArray<FLinearColor> Linear;
		TArray<FColor> Bytes;
	};
	FTexels Read(UTexture* Texture)
	{
		FTexels Out;
		FRHITexture* RHI = TextureOf(Texture);
		if (!RHI) return Out;
		Out.W = RHI->GetDesc().Extent.X; Out.H = RHI->GetDesc().Extent.Y;
		const EPixelFormat Format = RHI->GetFormat();
		FTexels* Target = &Out;
		ENQUEUE_RENDER_COMMAND(SpoutDecodeTestRead)([RHI, Format, Target](FRHICommandListImmediate& Cmd)
		{
			const FIntRect Rect(0, 0, Target->W, Target->H);
			Cmd.Transition(FRHITransitionInfo(RHI, ERHIAccess::Unknown, ERHIAccess::CopySrc));
			if (Format == PF_FloatRGBA)
			{
				Cmd.ReadSurfaceFloatData(RHI, Rect, Target->Half, CubeFace_PosX, 0, 0);
			}
			else if (Format == PF_A2B10G10R10)
			{
				FReadSurfaceDataFlags Flags(RCM_MinMax);
				Flags.SetLinearToGamma(false);
				Cmd.ReadSurfaceData(RHI, Rect, Target->Linear, Flags);
			}
			else
			{
				FReadSurfaceDataFlags Flags(RCM_UNorm);
				Flags.SetLinearToGamma(false);
				Cmd.ReadSurfaceData(RHI, Rect, Target->Bytes, Flags);
			}
			Cmd.Transition(FRHITransitionInfo(RHI, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		});
		FlushRenderingCommands();
		return Out;
	}

	/** Texels of a decoded texture that miss the exact decode; the first one described. */
	int32 DecodeMisses(const FTexels& Got, FString& First)
	{
		int32 Misses = Got.Half.Num() == Got.W * Got.H && Got.W > 0 ? 0 : 1;
		if (Misses) { First = TEXT("no 16-bit float texels were read"); return Misses; }
		for (int32 Y = 0; Y < Got.H; ++Y)
		for (int32 X = 0; X < Got.W; ++X)
		{
			const FCode C = CodeAt(X, Y);
			const FFloat16Color& T = Got.Half[Y * Got.W + X];
			const bool bOk = Near(T.R.GetFloat(), Decode(C.R)) && Near(T.G.GetFloat(), Decode(C.G)) && Near(T.B.GetFloat(), Decode(C.B))
				&& FMath::Abs(T.A.GetFloat() - double(C.A) / 3.0) <= 1e-3;
			if (!bOk && Misses++ == 0)
			{
				First = FString::Printf(TEXT("(%d,%d) codes %u,%u,%u,%u decoded to %.6f,%.6f,%.6f,%.4f, exact %.6f,%.6f,%.6f,%.4f"), X, Y, C.R, C.G, C.B, C.A,
					T.R.GetFloat(), T.G.GetFloat(), T.B.GetFloat(), T.A.GetFloat(), Decode(C.R), Decode(C.G), Decode(C.B), double(C.A) / 3.0);
			}
		}
		return Misses;
	}

	/** Texels of an undecoded 10-bit texture that differ from the source codes. */
	int32 RawMisses(const FTexels& Got, FString& First)
	{
		if (Got.Linear.Num() != Got.W * Got.H || Got.W == 0) { First = TEXT("no 10-bit texels were read"); return 1; }
		int32 Misses = 0;
		for (int32 Y = 0; Y < Got.H; ++Y)
		for (int32 X = 0; X < Got.W; ++X)
		{
			const FCode C = CodeAt(X, Y);
			const FLinearColor& T = Got.Linear[Y * Got.W + X];
			const bool bOk = FMath::RoundToInt(T.R * 1023.0f) == int32(C.R) && FMath::RoundToInt(T.G * 1023.0f) == int32(C.G)
				&& FMath::RoundToInt(T.B * 1023.0f) == int32(C.B) && FMath::RoundToInt(T.A * 3.0f) == int32(C.A);
			if (!bOk && Misses++ == 0) First = FString::Printf(TEXT("(%d,%d) codes %u,%u,%u,%u read as %.5f,%.5f,%.5f,%.3f"), X, Y, C.R, C.G, C.B, C.A, T.R, T.G, T.B, T.A);
		}
		return Misses;
	}

	FName UniqueName(const TCHAR* What)
	{
		return FName(*FString::Printf(TEXT("SpoutDecode_%s_%s"), What, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	bool Send(const FName& Name, UTextureRenderTarget2D* Source)
	{
		const bool bAccepted = USpoutBPFunctionLibrary::SpoutSender(Name, ESpoutSendTextureFrom::TextureRenderTarget2D, Source);
		FlushRenderingCommands();
		return bAccepted;
	}

	UMaterialInterface* PluginMaterial()
	{
		return LoadObject<UMaterialInterface>(nullptr, TEXT("/SpoutPlugin/Content/M_SpoutMaterial.M_SpoutMaterial"));
	}

	/** Receives through SpoutReceiver (the plugin creates the material from Base), completed on the render thread. */
	bool ReceiveWithInput(const FName& Name, UMaterialInterface* Base, UTexture2D*& Texture, UMaterialInstanceDynamic*& Material,
		UTextureRenderTarget2D* Output, ESpoutColorSpace Mode, bool bAsync = false)
	{
		const bool bAccepted = USpoutBPFunctionLibrary::SpoutReceiver(Name, Base, TEXT("SpoutTexture"), Material, Texture, Output, Mode, bAsync);
		FlushRenderingCommands();
		return bAccepted && Texture;
	}

	void Close(const FName& Name, UTexture2D*& Texture, UMaterialInstanceDynamic*& Material)
	{
		USpoutBPFunctionLibrary::CloseReceiver(Name, Texture, Material);
		FlushRenderingCommands();
	}

	/** Restores a console variable's value and priority when the test ends. */
	struct FPinnedVariable
	{
		IConsoleVariable* Variable;
		int32 Previous;
		EConsoleVariableFlags Priority;
		explicit FPinnedVariable(const TCHAR* Name)
			: Variable(IConsoleManager::Get().FindConsoleVariable(Name)), Previous(Variable ? Variable->GetInt() : 0)
			, Priority(Variable ? static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask) : ECVF_Default) {}
		void Set(int32 Value) { if (Variable) Variable->Set(Value, ECVF_SetByCode); }
		~FPinnedVariable()
		{
			if (!Variable) return;
			Variable->Set(Previous, ECVF_SetByCode);
			Variable->ClearFlags(ECVF_SetByMask);
			Variable->SetFlags(Priority);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutDecodeExact, "SpoutPlugin.ColorDecode.Srgb10BitDecodesExactlyThroughEveryOutput", SPOUT_DECODE_FLAGS)
bool FSpoutDecodeExact::RunTest(const FString&)
{
	using namespace SpoutColorDecodeTests;
	SpoutTestExpectations::ExpectLegacyWrapDiagnostic(*this);
	AddExpectedMessage(TEXT("incompatible output render target left untouched"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	UMaterialInterface* Base = PluginMaterial();
	if (!TestNotNull(TEXT("the plugin's material loads"), Base)) return false;
	FPinnedVariable Path(TEXT("r.Spout.GPUReceiver"));
	for (int32 GPU : { 1, 0 })
	{
		Path.Set(GPU);
		const FString What = GPU ? TEXT("GPU path") : TEXT("CPU readback");
		const FName Name = UniqueName(TEXT("Exact"));
		TStrongObjectPtr<UTextureRenderTarget2D> Source(NewTarget(Width, Height, PF_A2B10G10R10));
		Upload(Source.Get());
		TestTrue(What + TEXT(": send accepted"), Send(Name, Source.Get()));
		const TSharedPtr<FSpoutSharedSender> Sender = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Sender);
		TestTrue(What + TEXT(": published as R10G10B10A2_UNORM"), Sender && Sender->SharedFormat == DXGI_FORMAT_R10G10B10A2_UNORM);

		// SpoutReceiver: the plugin's material, the texture and a 16-bit float output target.
		TStrongObjectPtr<UTextureRenderTarget2D> OutF16(NewTarget(Width, Height, PF_FloatRGBA));
		TStrongObjectPtr<UTextureRenderTarget2D> Out10(NewTarget(Width, Height, PF_A2B10G10R10));
		UTexture2D* Texture = nullptr; UMaterialInstanceDynamic* Material = nullptr;
		if (!TestTrue(What + TEXT(": sRGB receive accepted"), ReceiveWithInput(Name, Base, Texture, Material, OutF16.Get(), ESpoutColorSpace::SRGB))) { USpoutBPFunctionLibrary::CloseSender(Name); continue; }
		TestEqual(What + TEXT(": the texture is 16-bit float"), static_cast<int32>(Texture->GetPixelFormat()), static_cast<int32>(PF_FloatRGBA));
		TestFalse(What + TEXT(": with no sampler decode on top"), Texture->SRGB != 0);
		FString First;
		const FTexels Decoded = Read(Texture);
		TestEqual(What + TEXT(": every texel is the exact decode, alpha unchanged ") + First, DecodeMisses(Decoded, First), 0);
		TestTrue(What + TEXT(": the material shows the texture"), Material && Material->K2_GetTextureParameterValue(TEXT("SpoutTexture")) == Texture);
		const FTexels Output = Read(OutF16.Get());
		bool bSame = Output.Half.Num() == Decoded.Half.Num();
		for (int32 I = 0; bSame && I < Output.Half.Num(); ++I)
		{
			bSame = Output.Half[I].R.Encoded == Decoded.Half[I].R.Encoded && Output.Half[I].G.Encoded == Decoded.Half[I].G.Encoded
				&& Output.Half[I].B.Encoded == Decoded.Half[I].B.Encoded && Output.Half[I].A.Encoded == Decoded.Half[I].A.Encoded;
		}
		TestTrue(What + TEXT(": the 16-bit float output target holds the same texels"), bSame);

		// A 10-bit output target cannot hold the decoded frame: it is refused and left untouched.
		const FTexels Before = Read(Out10.Get());
		TestTrue(What + TEXT(": the call with a 10-bit output target still delivers the texture"), ReceiveWithInput(Name, Base, Texture, Material, Out10.Get(), ESpoutColorSpace::SRGB));
		const FTexels After = Read(Out10.Get());
		bool bUntouched = Before.Linear.Num() == After.Linear.Num();
		for (int32 I = 0; bUntouched && I < After.Linear.Num(); ++I) bUntouched = Before.Linear[I] == After.Linear[I];
		TestTrue(What + TEXT(": the 10-bit output target is left untouched"), bUntouched);
		Close(Name, Texture, Material);

		// SpoutReceiverWithMaterial: the caller's own material.
		TStrongObjectPtr<UMaterialInstanceDynamic> Consumer(UMaterialInstanceDynamic::Create(Base, GetTransientPackage()));
		UTexture2D* Texture2 = nullptr;
		const bool bWith = USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(Name, Consumer.Get(), TEXT("SpoutTexture"), Texture2, nullptr, ESpoutColorSpace::SRGB, false);
		FlushRenderingCommands();
		if (TestTrue(What + TEXT(": SpoutReceiverWithMaterial accepted"), bWith && Texture2))
		{
			TestTrue(What + TEXT(": the caller's material shows the decoded texture"), Consumer->K2_GetTextureParameterValue(TEXT("SpoutTexture")) == Texture2);
			TestEqual(What + TEXT(": SpoutReceiverWithMaterial decodes every texel ") + First, DecodeMisses(Read(Texture2), First), 0);
		}
		UMaterialInstanceDynamic* NoMaterial = nullptr;
		Close(Name, Texture2, NoMaterial);

		// Auto and Linear are unchanged: the 10-bit texels as sent.
		for (ESpoutColorSpace Mode : { ESpoutColorSpace::Auto, ESpoutColorSpace::Linear })
		{
			const FString ModeWhat = What + TEXT(", ") + UEnum::GetValueAsString(Mode);
			UTexture2D* Raw = nullptr; UMaterialInstanceDynamic* RawMaterial = nullptr;
			if (TestTrue(ModeWhat + TEXT(": accepted"), ReceiveWithInput(Name, Base, Raw, RawMaterial, nullptr, Mode)))
			{
				TestEqual(ModeWhat + TEXT(": the texture stays 10-bit"), static_cast<int32>(Raw->GetPixelFormat()), static_cast<int32>(PF_A2B10G10R10));
				TestEqual(ModeWhat + TEXT(": every texel is the code sent ") + First, RawMisses(Read(Raw), First), 0);
			}
			Close(Name, Raw, RawMaterial);
		}
		USpoutBPFunctionLibrary::CloseSender(Name);
		FlushRenderingCommands();
	}
	return true;
}

#if WITH_EDITOR
namespace SpoutColorDecodeTests
{
	/**
	 * An unlit material whose emissive output is the "SpoutTexture" parameter, sampled linearly. The shipped
	 * M_SpoutMaterial is a default-lit surface material (the texture feeds Base Color), so what it shows depends on
	 * the scene's lighting, exposure and tone mapping by design; this one shows the sampled value itself.
	 */
	UMaterial* MakeUnlitTextureMaterial(UTexture* DefaultTexture)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		Material->MaterialDomain = MD_Surface;
		Material->SetShadingModel(MSM_Unlit);
		UMaterialExpressionTextureSampleParameter2D* Parameter = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
		Parameter->ParameterName = TEXT("SpoutTexture");
		Parameter->SamplerType = SAMPLERTYPE_LinearColor;
		Parameter->Texture = DefaultTexture;
		Material->GetExpressionCollection().AddExpression(Parameter);
		Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Parameter);
		Material->PostEditChange();
		Material->ForceRecompileForRendering();
		if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
		FlushRenderingCommands();
		return Material;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutDecodeRendered, "SpoutPlugin.ColorDecode.UnlitMaterialRendersTheSourceDisplayValues", SPOUT_DECODE_FLAGS)
bool FSpoutDecodeRendered::RunTest(const FString&)
{
	using namespace SpoutColorDecodeTests;
	SpoutTestExpectations::ExpectLegacyWrapDiagnostic(*this);
	UWorld* World = nullptr;
	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World()) { World = Context.World(); break; }
		}
	}
	if (!TestNotNull(TEXT("a world to draw with"), World)) return false;
	const FName Name = UniqueName(TEXT("Rendered"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(NewTarget(Width, Height, PF_A2B10G10R10));
	Upload(Source.Get());
	TestTrue(TEXT("send accepted"), Send(Name, Source.Get()));
	// The canvas draw renders the material's output with no lighting, tone mapping, exposure or post process; the
	// target's sRGB view encodes the linear result on write, as a display back buffer holds it.
	TStrongObjectPtr<UMaterial> Unlit;
	for (ESpoutColorSpace Mode : { ESpoutColorSpace::SRGB, ESpoutColorSpace::Auto })
	{
		const bool bDecoded = Mode == ESpoutColorSpace::SRGB;
		const FString What = bDecoded ? TEXT("sRGB") : TEXT("Auto");
		TStrongObjectPtr<UMaterialInstanceDynamic> Consumer;
		UTexture2D* Texture = nullptr;
		// The first receive gives the material its default texture; the plugin then binds each session's texture.
		UTexture2D* Probe = nullptr; UMaterialInstanceDynamic* NoMaterial = nullptr;
		if (!TestTrue(What + TEXT(": accepted"), ReceiveWithInput(Name, nullptr, Probe, NoMaterial, nullptr, Mode))) { Close(Name, Probe, NoMaterial); continue; }
		if (!Unlit.IsValid()) Unlit.Reset(MakeUnlitTextureMaterial(Probe));
		Consumer.Reset(UMaterialInstanceDynamic::Create(Unlit.Get(), GetTransientPackage()));
		const bool bBound = USpoutBPFunctionLibrary::SpoutReceiverWithMaterial(Name, Consumer.Get(), TEXT("SpoutTexture"), Texture, nullptr, Mode, false);
		FlushRenderingCommands();
		TestTrue(What + TEXT(": the plugin binds the texture to the caller's material"), bBound && Texture && Consumer->K2_GetTextureParameterValue(TEXT("SpoutTexture")) == Texture);
		TStrongObjectPtr<UTextureRenderTarget2D> Canvas(NewTarget(Width, Height, PF_B8G8R8A8, /*bForceLinearGamma*/ false));
		UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, Canvas.Get(), Consumer.Get());
		FlushRenderingCommands();
		const FTexels Shown = Read(Canvas.Get());
		int32 Misses = 0, WorstDiff = 0; FString First;
		for (int32 Y = 0; Y < Height && Shown.Bytes.Num() == Width * Height; ++Y)
		for (int32 X = 0; X < Width; ++X)
		{
			const FCode C = CodeAt(X, Y);
			const FColor& P = Shown.Bytes[Y * Width + X];
			// sRGB: the source's own display value. Auto reads the codes as linear light, so they show encoded again:
			// brighter. That is what sRGB exists for; Auto's result is checked here as well.
			auto Want = [bDecoded](uint32 Code) { return bDecoded ? FMath::RoundToInt(Code * 255.0 / 1023.0) : FMath::RoundToInt(255.0 * Encode(Code / 1023.0)); };
			const int32 Diff = FMath::Max3(FMath::Abs(int32(P.R) - Want(C.R)), FMath::Abs(int32(P.G) - Want(C.G)), FMath::Abs(int32(P.B) - Want(C.B)));
			WorstDiff = FMath::Max(WorstDiff, Diff);
			if (Diff > 1 && Misses++ == 0)
			{
				First = FString::Printf(TEXT("(%d,%d) codes %u,%u,%u shown %d,%d,%d, expected %d,%d,%d"), X, Y, C.R, C.G, C.B, P.R, P.G, P.B, Want(C.R), Want(C.G), Want(C.B));
			}
		}
		TestEqual(What + TEXT(": the drawn target has every texel"), Shown.Bytes.Num(), Width * Height);
		TestEqual(What + FString::Printf(TEXT(": every texel shown within 1 of the expected display value (largest difference %d) "), WorstDiff) + First, Misses, 0);
		// Texture is the session's one texture (Probe): one Close ends the session.
		USpoutBPFunctionLibrary::ReleaseReceiverMaterialBinding(Name, Consumer.Get(), TEXT("SpoutTexture"));
		Close(Name, Probe, NoMaterial);
	}
	USpoutBPFunctionLibrary::CloseSender(Name);
	FlushRenderingCommands();
	return true;
}
#endif
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutDecodeSession, "SpoutPlugin.ColorDecode.OneInterpretationPerSession", SPOUT_DECODE_FLAGS)
bool FSpoutDecodeSession::RunTest(const FString&)
{
	using namespace SpoutColorDecodeTests;
	SpoutTestExpectations::ExpectLegacyWrapDiagnostic(*this);
	AddExpectedMessage(TEXT("refused a receive asking for Color Space"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	const FName Name = UniqueName(TEXT("Session"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(NewTarget(Width, Height, PF_A2B10G10R10));
	Upload(Source.Get());
	TestTrue(TEXT("send accepted"), Send(Name, Source.Get()));
	FString First;
	UTexture2D* Texture = nullptr; UMaterialInstanceDynamic* Material = nullptr;
	TestTrue(TEXT("sRGB opens the session"), ReceiveWithInput(Name, nullptr, Texture, Material, nullptr, ESpoutColorSpace::SRGB));
	UTexture2D* Other = nullptr; UMaterialInstanceDynamic* OtherMaterial = nullptr;
	TestFalse(TEXT("Auto is refused while the session decodes"), ReceiveWithInput(Name, nullptr, Other, OtherMaterial, nullptr, ESpoutColorSpace::Auto));
	TestFalse(TEXT("Linear is refused while the session decodes"), ReceiveWithInput(Name, nullptr, Other, OtherMaterial, nullptr, ESpoutColorSpace::Linear));
	TestTrue(TEXT("sRGB is still accepted"), ReceiveWithInput(Name, nullptr, Texture, Material, nullptr, ESpoutColorSpace::SRGB));
	TestEqual(TEXT("and still decodes every texel ") + First, DecodeMisses(Read(Texture), First), 0);
	Close(Name, Texture, Material);
	TestTrue(TEXT("after Close, Auto opens a new session"), ReceiveWithInput(Name, nullptr, Texture, Material, nullptr, ESpoutColorSpace::Auto));
	TestEqual(TEXT("which keeps the codes ") + First, RawMisses(Read(Texture), First), 0);
	TestTrue(TEXT("Linear is the same interpretation of 10-bit and is accepted"), ReceiveWithInput(Name, nullptr, Texture, Material, nullptr, ESpoutColorSpace::Linear));
	TestFalse(TEXT("sRGB is refused while the session is undecoded"), ReceiveWithInput(Name, nullptr, Other, OtherMaterial, nullptr, ESpoutColorSpace::SRGB));
	TestEqual(TEXT("and the codes are unchanged ") + First, RawMisses(Read(Texture), First), 0);
	Close(Name, Texture, Material);
	USpoutBPFunctionLibrary::CloseSender(Name);
	FlushRenderingCommands();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutDecodeLifecycle, "SpoutPlugin.ColorDecode.ResizeRestartAndReopenKeepDecoding", SPOUT_DECODE_FLAGS)
bool FSpoutDecodeLifecycle::RunTest(const FString&)
{
	using namespace SpoutColorDecodeTests;
	SpoutTestExpectations::ExpectLegacyWrapDiagnostic(*this);
	FPinnedVariable Path(TEXT("r.Spout.GPUReceiver"));
	for (int32 GPU : { 1, 0 })
	{
		Path.Set(GPU);
		const FString What = GPU ? TEXT("GPU path") : TEXT("CPU readback");
		const FName Name = UniqueName(TEXT("Lifecycle"));
		TStrongObjectPtr<UTextureRenderTarget2D> Source(NewTarget(Width, Height, PF_A2B10G10R10));
		Upload(Source.Get());
		TestTrue(What + TEXT(": send accepted"), Send(Name, Source.Get()));
		FString First;
		UTexture2D* Texture = nullptr; UMaterialInstanceDynamic* Material = nullptr;
		TestTrue(What + TEXT(": decoded session opens"), ReceiveWithInput(Name, nullptr, Texture, Material, nullptr, ESpoutColorSpace::SRGB));
		TestEqual(What + TEXT(": first frame decoded ") + First, DecodeMisses(Read(Texture), First), 0);

		// Resize: the sender's size changes, the session stays open and decodes at the new size.
		Source->ResizeTarget(128, 5);
		FlushRenderingCommands();
		Upload(Source.Get());
		TestTrue(What + TEXT(": send after resize accepted"), Send(Name, Source.Get()));
		TestTrue(What + TEXT(": receive after resize accepted"), ReceiveWithInput(Name, nullptr, Texture, Material, nullptr, ESpoutColorSpace::SRGB));
		if (Texture)
		{
			TestTrue(What + TEXT(": the texture has the new size"), Texture->GetSizeX() == 128 && Texture->GetSizeY() == 5);
			TestEqual(What + TEXT(": decoded at the new size ") + First, DecodeMisses(Read(Texture), First), 0);
		}

		// The sender closes and starts again under the same name; the receiver is not closed.
		USpoutBPFunctionLibrary::CloseSender(Name);
		FlushRenderingCommands();
		Source->ResizeTarget(Width, Height);
		FlushRenderingCommands();
		Upload(Source.Get());
		TestTrue(What + TEXT(": the sender starts again"), Send(Name, Source.Get()));
		const TSharedPtr<FSpoutSharedSender> Entry = FSpoutSenderRegistry::Get().Find(Name, ESpoutType::Receiver);
		const uint64 Before = Entry ? Entry->ReceivedFrameNumber : 0;
		TestTrue(What + TEXT(": receive after the restart accepted"), ReceiveWithInput(Name, nullptr, Texture, Material, nullptr, ESpoutColorSpace::SRGB));
		TestTrue(What + TEXT(": a new frame was delivered"), Entry && Entry->ReceivedFrameNumber > Before);
		if (Texture) TestEqual(What + TEXT(": decoded after the restart ") + First, DecodeMisses(Read(Texture), First), 0);

		// The receiver closes and opens again.
		Close(Name, Texture, Material);
		TestTrue(What + TEXT(": reopened"), ReceiveWithInput(Name, nullptr, Texture, Material, nullptr, ESpoutColorSpace::SRGB));
		if (Texture) TestEqual(What + TEXT(": decoded after reopening ") + First, DecodeMisses(Read(Texture), First), 0);
		Close(Name, Texture, Material);
		USpoutBPFunctionLibrary::CloseSender(Name);
		FlushRenderingCommands();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutDecodeAsync, "SpoutPlugin.ColorDecode.AsyncRequestFallsBackAndDecodes", SPOUT_DECODE_FLAGS)
bool FSpoutDecodeAsync::RunTest(const FString&)
{
	using namespace SpoutColorDecodeTests;
	SpoutTestExpectations::ExpectLegacyWrapDiagnostic(*this);
	AddExpectedMessage(TEXT("was requested but is not used"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	const FName Name = UniqueName(TEXT("Async"));
	TStrongObjectPtr<UTextureRenderTarget2D> Source(NewTarget(Width, Height, PF_A2B10G10R10));
	Upload(Source.Get());
	TestTrue(TEXT("send accepted"), Send(Name, Source.Get()));
	FString First;
	UTexture2D* Texture = nullptr; UMaterialInstanceDynamic* Material = nullptr;
	TestTrue(TEXT("sRGB with Async Receive asked for is accepted"), ReceiveWithInput(Name, nullptr, Texture, Material, nullptr, ESpoutColorSpace::SRGB, /*bAsync*/ true));
	TestTrue(TEXT("a second call too"), ReceiveWithInput(Name, nullptr, Texture, Material, nullptr, ESpoutColorSpace::SRGB, /*bAsync*/ true));
	if (Texture) TestEqual(TEXT("the synchronous path decodes every texel ") + First, DecodeMisses(Read(Texture), First), 0);
	const FSpoutStreamStatus Status = USpoutBPFunctionLibrary::GetSpoutStreamStatus(Name, ESpoutType::Receiver);
	TestEqual(TEXT("the effective mode is synchronous"), static_cast<int32>(Status.EffectiveReceiveMode), static_cast<int32>(ESpoutReceiveMode::Synchronous));
	TestFalse(TEXT("and the status says why"), Status.ReceiveModeNote.IsEmpty());
	Close(Name, Texture, Material);
	USpoutBPFunctionLibrary::CloseSender(Name);
	FlushRenderingCommands();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpoutDecodeControls, "SpoutPlugin.ColorDecode.FloatRefusesSrgbAnd8BitIsUnchanged", SPOUT_DECODE_FLAGS)
bool FSpoutDecodeControls::RunTest(const FString&)
{
	using namespace SpoutColorDecodeTests;
	SpoutTestExpectations::ExpectLegacyWrapDiagnostic(*this);
	AddExpectedMessage(TEXT("sRGB decoding exists for the 8-bit and 10-bit formats; 16-bit float frames are linear"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	{
		const FName Name = UniqueName(TEXT("Float"));
		TStrongObjectPtr<UTextureRenderTarget2D> Source(NewTarget(64, 4, PF_FloatRGBA));
		TestTrue(TEXT("float send accepted"), Send(Name, Source.Get()));
		UTexture2D* Texture = nullptr; UMaterialInstanceDynamic* Material = nullptr;
		TestFalse(TEXT("sRGB on a 16-bit float sender is refused"), ReceiveWithInput(Name, nullptr, Texture, Material, nullptr, ESpoutColorSpace::SRGB));
		TestTrue(TEXT("Auto on it is accepted"), ReceiveWithInput(Name, nullptr, Texture, Material, nullptr, ESpoutColorSpace::Auto));
		if (Texture) TestTrue(TEXT("as linear 16-bit float"), Texture->GetPixelFormat() == PF_FloatRGBA && Texture->SRGB == 0);
		Close(Name, Texture, Material);
		USpoutBPFunctionLibrary::CloseSender(Name);
	}
	{
		const FName Name = UniqueName(TEXT("Eight"));
		TStrongObjectPtr<UTextureRenderTarget2D> Source(NewTarget(64, 4, PF_B8G8R8A8, /*bForceLinearGamma*/ false));
		TestTrue(TEXT("8-bit send accepted"), Send(Name, Source.Get()));
		UTexture2D* Texture = nullptr; UMaterialInstanceDynamic* Material = nullptr;
		TestTrue(TEXT("sRGB on an 8-bit sender is accepted"), ReceiveWithInput(Name, nullptr, Texture, Material, nullptr, ESpoutColorSpace::SRGB));
		if (Texture) TestTrue(TEXT("as before: the 8-bit texels with the sampler's sRGB decode"), Texture->GetPixelFormat() == PF_B8G8R8A8 && Texture->SRGB != 0);
		Close(Name, Texture, Material);
		USpoutBPFunctionLibrary::CloseSender(Name);
	}
	FlushRenderingCommands();
	return true;
}

namespace SpoutColorDecodeCost
{
	/**
	 * Spout.Dev.MeasureColorDecode [Width] [Height] [Passes] (development command). On the GPU, with
	 * timestamp queries around back-to-back work in one command list: Passes copies of a 10-bit texture into a 10-bit
	 * texture, bracketed as the shared-slot path brackets the copy that the decode replaces, then Passes decode passes of
	 * the same texture into a 16-bit float texture. Seven rounds; logs the per-pass median and range of both, and the
	 * render thread's recording time per pass. Measures the machine it runs on; no display latency is implied.
	 */
	void Measure(const TArray<FString>& Args)
	{
		const int32 W = Args.Num() > 0 ? FMath::Max(1, FCString::Atoi(*Args[0])) : 1920;
		const int32 H = Args.Num() > 1 ? FMath::Max(1, FCString::Atoi(*Args[1])) : 1080;
		const int32 Passes = Args.Num() > 2 ? FMath::Clamp(FCString::Atoi(*Args[2]), 1, 2000) : 200;
		constexpr int32 Rounds = 7;
		TArray<double> CopyUs, DecodeUs, CopyCpuUs, DecodeCpuUs;
		bool bOk = true;
		ENQUEUE_RENDER_COMMAND(SpoutMeasureColorDecode)([W, H, Passes, &CopyUs, &DecodeUs, &CopyCpuUs, &DecodeCpuUs, &bOk](FRHICommandListImmediate& Cmd)
		{
			auto Make = [&Cmd, W, H](const TCHAR* Name, EPixelFormat Format)
			{
				const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(Name, W, H, Format)
					.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::RenderTargetable).SetInitialState(ERHIAccess::SRVMask);
#if ENGINE_MINOR_VERSION >= 8
				return Cmd.CreateTexture(Desc);
#else
				return RHICreateTexture(Desc);
#endif
			};
			FTextureRHIRef Source = Make(TEXT("SpoutCostSource"), PF_A2B10G10R10);
			FTextureRHIRef Copy = Make(TEXT("SpoutCostCopy"), PF_A2B10G10R10);
			FTextureRHIRef Decoded = Make(TEXT("SpoutCostDecoded"), PF_FloatRGBA);
			if (!Source || !Copy || !Decoded) { bOk = false; return; }
			for (int32 Round = 0; Round < Rounds && bOk; ++Round)
			{
				FRenderQueryRHIRef Q0 = RHICreateRenderQuery(RQT_AbsoluteTime);
				FRenderQueryRHIRef Q1 = RHICreateRenderQuery(RQT_AbsoluteTime);
				FRenderQueryRHIRef Q2 = RHICreateRenderQuery(RQT_AbsoluteTime);
				Cmd.EndRenderQuery(Q0);
				const uint64 C0 = FPlatformTime::Cycles64();
				for (int32 I = 0; I < Passes; ++I)
				{
					Cmd.Transition(FRHITransitionInfo(Source, ERHIAccess::Unknown, ERHIAccess::CopySrc));
					Cmd.Transition(FRHITransitionInfo(Copy, ERHIAccess::Unknown, ERHIAccess::CopyDest));
					Cmd.CopyTexture(Source, Copy, FRHICopyTextureInfo());
					Cmd.Transition(FRHITransitionInfo(Source, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
					Cmd.Transition(FRHITransitionInfo(Copy, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
				}
				const uint64 C1 = FPlatformTime::Cycles64();
				Cmd.EndRenderQuery(Q1);
				for (int32 I = 0; I < Passes; ++I)
				{
					bOk &= SpoutColorDecode::DecodeSrgbToLinear(Cmd, Source, ERHIAccess::Unknown, Decoded, ERHIAccess::Unknown, ERHIAccess::SRVMask);
				}
				const uint64 C2 = FPlatformTime::Cycles64();
				Cmd.EndRenderQuery(Q2);
				Cmd.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
				uint64 T0 = 0, T1 = 0, T2 = 0;
				bOk &= RHIGetRenderQueryResult(Q0, T0, true) && RHIGetRenderQueryResult(Q1, T1, true) && RHIGetRenderQueryResult(Q2, T2, true);
				CopyUs.Add(double(T1 - T0) / Passes);
				DecodeUs.Add(double(T2 - T1) / Passes);
				CopyCpuUs.Add(FPlatformTime::ToMilliseconds64(C1 - C0) * 1000.0 / Passes);
				DecodeCpuUs.Add(FPlatformTime::ToMilliseconds64(C2 - C1) * 1000.0 / Passes);
			}
		});
		FlushRenderingCommands();
		auto Median = [](TArray<double> V) { V.Sort(); return V.Num() ? V[V.Num() / 2] : -1.0; };
		auto Low = [](const TArray<double>& V) { double M = V.Num() ? V[0] : -1.0; for (double X : V) M = FMath::Min(M, X); return M; };
		auto High = [](const TArray<double>& V) { double M = V.Num() ? V[0] : -1.0; for (double X : V) M = FMath::Max(M, X); return M; };
		UE_LOG(LogTemp, Display, TEXT("Spout colour decode cost: ok=%d size=%dx%d passes=%d rounds=%d gpu_us_per_pass copy10 median=%.2f [%.2f..%.2f] decode median=%.2f [%.2f..%.2f] added median=%.2f; render_thread_us_per_pass copy10 median=%.2f decode median=%.2f"),
			bOk ? 1 : 0, W, H, Passes, CopyUs.Num(), Median(CopyUs), Low(CopyUs), High(CopyUs), Median(DecodeUs), Low(DecodeUs), High(DecodeUs),
			Median(DecodeUs) - Median(CopyUs), Median(CopyCpuUs), Median(DecodeCpuUs));
	}

	FAutoConsoleCommand GSpoutMeasureColorDecode(
		TEXT("Spout.Dev.MeasureColorDecode"),
		TEXT("Development: GPU and render-thread time of the 10-bit sRGB decode pass against the copy it replaces. Arguments: [Width] [Height] [Passes]."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Measure));
}

#undef SPOUT_DECODE_FLAGS
#endif
