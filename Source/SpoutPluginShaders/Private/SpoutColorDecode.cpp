// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#include "SpoutColorDecode.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "PixelShaderUtils.h"
#include "RHICommandList.h"
#include "ShaderParameterStruct.h"

namespace
{
	class FSpoutSrgbDecodePS final : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSpoutSrgbDecodePS);
		SHADER_USE_PARAMETER_STRUCT(FSpoutSrgbDecodePS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D, SourceTexture)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FSpoutSrgbDecodePS, "/Plugin/SpoutPlugin/Private/SpoutColorDecode.usf", "MainPS", SF_Pixel);
}

bool SpoutColorDecode::DecodeSrgbToLinear(FRHICommandList& RHICmdList,
	FRHITexture* Source, ERHIAccess SourceBefore,
	FRHITexture* Destination, ERHIAccess DestinationBefore, ERHIAccess DestinationAfter)
{
	check(IsInRenderingThread());
	if (!Source || !Destination)
	{
		return false;
	}
	const FIntVector SourceSize = Source->GetSizeXYZ();
	const FIntVector DestinationSize = Destination->GetSizeXYZ();
	if (SourceSize.X != DestinationSize.X || SourceSize.Y != DestinationSize.Y || DestinationSize.X <= 0 || DestinationSize.Y <= 0)
	{
		return false;
	}
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	if (!ShaderMap)
	{
		return false;
	}
	TShaderMapRef<FSpoutSrgbDecodePS> PixelShader(ShaderMap);
	if (!PixelShader.IsValid())
	{
		return false;
	}

	RHICmdList.Transition({
		FRHITransitionInfo(Source, SourceBefore, ERHIAccess::SRVGraphics),
		FRHITransitionInfo(Destination, DestinationBefore, ERHIAccess::RTV) });
	// Every destination texel is written, so nothing is loaded.
	FRHIRenderPassInfo PassInfo(Destination, ERenderTargetActions::DontLoad_Store);
	RHICmdList.BeginRenderPass(PassInfo, TEXT("SpoutSrgbDecode"));
	FSpoutSrgbDecodePS::FParameters Parameters;
	Parameters.SourceTexture = Source;
	FPixelShaderUtils::DrawFullscreenPixelShader(RHICmdList, ShaderMap, PixelShader, Parameters,
		FIntRect(0, 0, DestinationSize.X, DestinationSize.Y));
	RHICmdList.EndRenderPass();
	RHICmdList.Transition({
		FRHITransitionInfo(Source, ERHIAccess::SRVGraphics, ERHIAccess::SRVMask),
		FRHITransitionInfo(Destination, ERHIAccess::RTV, DestinationAfter) });
	return true;
}
