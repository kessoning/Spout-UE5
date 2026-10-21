// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#include "SpoutReceivedTexture.h"
#include "SpoutDebugCapture.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "TextureResource.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Runtime/Launch/Resources/Version.h"

namespace
{
	class FSpoutReceivedTextureResource final : public FTextureResource
	{
	public:
		explicit FSpoutReceivedTextureResource(UTexture2D& Owner)
			: Width(Owner.GetSizeX()), Height(Owner.GetSizeY()), Format(Owner.GetPixelFormat()), bSRGB(Owner.SRGB != 0)
		{
			if (const auto* Data = Cast<USpoutTextureReservation>(Owner.GetAssetUserDataOfClass(USpoutTextureReservation::StaticClass()))) Reservation = Data->Reservation;
			auto AddressMode = [](TextureAddress Address)
			{
				return Address == TA_Wrap ? AM_Wrap : (Address == TA_Mirror ? AM_Mirror : AM_Clamp);
			};
			Sampler = FSamplerStateInitializerRHI(Owner.Filter == TF_Nearest ? SF_Point : SF_Bilinear,
				AddressMode(Owner.AddressX), AddressMode(Owner.AddressY), AM_Wrap);
		}
		virtual uint32 GetSizeX() const override { return Width; }
		virtual uint32 GetSizeY() const override { return Height; }
		virtual void InitRHI(FRHICommandListBase& RHICmdList) override
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Spout_ReceivedTextureInitRHI);
			// Shader-only UTexture2D allocations use a fixed SRV state in D3D12RHI.
			// RenderTargetable opts into tracking so external writes can be bracketed.
			ETextureCreateFlags Flags = ETextureCreateFlags::ShaderResource | ETextureCreateFlags::RenderTargetable;
			if (bSRGB) Flags |= ETextureCreateFlags::SRGB;
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("SpoutReceivedTexture"), Width, Height, Format)
				.SetFlags(Flags).SetClearValue(FClearValueBinding(FLinearColor::Transparent)).SetInitialState(ERHIAccess::RTV);
#if ENGINE_MINOR_VERSION >= 8
			TextureRHI = RHICmdList.CreateTexture(Desc);
#else
			TextureRHI = RHICreateTexture(Desc);
#endif
			SamplerStateRHI = RHICreateSamplerState(Sampler);
			DeferredPassSamplerStateRHI = SamplerStateRHI;
#if ENGINE_MINOR_VERSION >= 4
			RHICmdList.UpdateTextureReference(TextureReferenceRHI, TextureRHI);
#else
			RHIUpdateTextureReference(TextureReferenceRHI, TextureRHI);
#endif
			// UTexture resource initialization is enqueued on the immediate render list.
			FRHICommandListImmediate& Immediate = static_cast<FRHICommandListImmediate&>(RHICmdList);
			Immediate.BeginRenderPass(FRHIRenderPassInfo(TextureRHI, ERenderTargetActions::Clear_Store), TEXT("SpoutClearReceivedTexture"));
			Immediate.EndRenderPass();
			SPOUT_D3D_DIAG_TRANSITION("receivedtexture.init.rtv-to-srv", TextureRHI.GetReference(), ERHIAccess::RTV, ERHIAccess::SRVMask);
			Immediate.Transition(FRHITransitionInfo(TextureRHI, ERHIAccess::RTV, ERHIAccess::SRVMask));
		}
		virtual void ReleaseRHI() override
		{
#if ENGINE_MINOR_VERSION >= 7
			RHIClearTextureReference(TextureReferenceRHI);
#else
			RHIUpdateTextureReference(TextureReferenceRHI, nullptr);
#endif
			FTextureResource::ReleaseRHI();
		}
	private:
		uint32 Width, Height;
		EPixelFormat Format;
		bool bSRGB;
		FSamplerStateInitializerRHI Sampler;
		TSharedPtr<FSpoutResourceReservation, ESPMode::ThreadSafe> Reservation;
	};
}

void EnsureSpoutReceivedTextureResource(UTexture2D& Texture)
{
	check(IsInGameThread());
	// Only called for textures owned by this plugin. An ordinary FTexture2DResource
	// means a caller rebuilt the resource through UTexture2D::UpdateResource.
	if (Texture.GetResource() && !Texture.GetResource()->GetTexture2DResource()) return;
	Texture.ReleaseResource();
	FTextureResource* Resource = new FSpoutReceivedTextureResource(Texture);
	Texture.SetResource(Resource);
	FTextureReference* Reference = &Texture.TextureReference;
	ENQUEUE_RENDER_COMMAND(SpoutInitReceivedResource)([Resource, Reference](FRHICommandListImmediate& RHICmdList)
	{
		Resource->SetTextureReference(Reference->TextureReferenceRHI);
		Resource->InitResource(RHICmdList);
	});
}
