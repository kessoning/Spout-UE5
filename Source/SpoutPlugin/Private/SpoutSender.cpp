/**
 * Implements the Spout sender flow by copying UE D3D12 textures into shared DX11 textures
 * via D3D11-on-12, coordinating with the sender registry and D3D context.
 */
#include "SpoutSender.h"
#include "SpoutD3DContext.h"
#include "SpoutD3DUtils.h"
#include "SpoutSenderRegistry.h"
#include "SpoutModule.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "RenderResource.h"
#include "RenderingThread.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "Widgets/SWindow.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture.h"
#include "TextureResource.h"
#include "RenderResource.h"
#include "Engine/TextureRenderTarget.h"

// Defines ENGINE_MAJOR_VERSION / ENGINE_MINOR_VERSION. Required because these are
// not guaranteed to be defined transitively in all targets (e.g. UnrealGame builds).
#include "Runtime/Launch/Resources/Version.h"

// UE 5.8 changed FSlateRenderer::OnBackBufferReadyToPresent() to pass an
// ISlateViewportProvider& (from which the back buffer is fetched) instead of a
// const FTextureRHIRef&. Guard on engine version so 5.3-5.7 keep the old signature.
#define SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER \
	((ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8))

#if SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER
#include "Slate/SlateViewportProvider.h"
#endif

#include <cstring>

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3dcompiler.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

using Microsoft::WRL::ComPtr;

namespace
{
	DXGI_FORMAT SanitizeDxgiFormat(DXGI_FORMAT InFormat)
	{
		switch (InFormat)
		{
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default:
			return InFormat;
		}
	}

	struct FSpoutSendSource
	{
		// RHI texture to read from; resolved on the game thread before enqueueing.
		FTextureRHIRef SourceRHI;
		// Used to restore access state after copy.
		bool bIsViewport = false;
	};

	bool BuildSendSource(ESpoutSendTextureFrom SendTextureFrom, UTextureRenderTarget2D* TextureRenderTarget2D, FSpoutSendSource& OutSource)
	{
		if (SendTextureFrom == ESpoutSendTextureFrom::TextureRenderTarget2D)
		{
			if (!TextureRenderTarget2D)
			{
				return false;
			}

			FTextureRenderTargetResource* RTResource = TextureRenderTarget2D->GameThread_GetRenderTargetResource();
			if (!RTResource)
			{
				return false;
			}

			// Resolve the RHI texture on the game thread where the UObject is guaranteed alive,
			// so the render-thread lambda captures a ref-counted handle instead of a raw pointer.
			OutSource.SourceRHI = RTResource->GetRenderTargetTexture();
			if (!OutSource.SourceRHI.IsValid())
			{
				return false;
			}

			OutSource.bIsViewport = false;
			return true;
		}

		return false;
	}

	void EnsureSharedSenderTexture(FSpoutSharedSender& Sender, uint32 Width, uint32 Height, DXGI_FORMAT Format, FSpoutD3DContext& Context);
	ComPtr<ID3D11Texture2D> GetWrappedResource(FSpoutSharedSender& Sender, ID3D12Resource* NativeResource);

	struct FSpoutGammaConstants
	{
		float Gamma = 1.0f;
		float Padding[3] = {};
	};

	class FSpoutGammaPipeline
	{
	public:
		static FSpoutGammaPipeline& Get()
		{
			static FSpoutGammaPipeline Instance;
			return Instance;
		}

		bool Initialize(ID3D11Device* InDevice)
		{
			if (bInitialized && InDevice == CachedDevice)
			{
				return true;
			}

			CachedDevice = InDevice;
			VertexShader.Reset();
			PixelShader.Reset();
			SamplerState.Reset();
			RasterizerState.Reset();
			BlendState.Reset();
			DepthStencilState.Reset();
			GammaBuffer.Reset();
			bInitialized = false;

			if (!InDevice)
			{
				return false;
			}

			static const char* ShaderSource = R"(
cbuffer GammaCB : register(b0)
{
	float Gamma;
	float3 Padding;
};

Texture2D InputTex : register(t0);
SamplerState InputSampler : register(s0);

struct VSOut
{
	float4 Pos : SV_Position;
	float2 UV : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
	VSOut o;
	float2 pos = float2((id == 2) ? 3.0f : -1.0f, (id == 1) ? -3.0f : 1.0f);
	o.Pos = float4(pos, 0.0f, 1.0f);
	o.UV = float2((pos.x + 1.0f) * 0.5f, 1.0f - (pos.y + 1.0f) * 0.5f);
	return o;
}

float4 PSMain(VSOut i) : SV_Target
{
	float4 c = InputTex.Sample(InputSampler, i.UV);
	c.rgb = pow(saturate(c.rgb), Gamma);
	return c;
}
)";

			UINT CompileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if UE_BUILD_DEBUG
			CompileFlags |= D3DCOMPILE_DEBUG;
#endif

			ComPtr<ID3DBlob> VSBlob;
			ComPtr<ID3DBlob> PSBlob;
			ComPtr<ID3DBlob> ErrorBlob;

			HRESULT hr = D3DCompile(
				ShaderSource,
				strlen(ShaderSource),
				nullptr,
				nullptr,
				nullptr,
				"VSMain",
				"vs_5_0",
				CompileFlags,
				0,
				VSBlob.GetAddressOf(),
				ErrorBlob.GetAddressOf()
			);

			if (FAILED(hr) || !VSBlob)
			{
				UE_LOG(LogSpoutPlugin, Error, TEXT("Failed to compile Spout gamma VS (hr=0x%08x)"), hr);
				return false;
			}

			ErrorBlob.Reset();
			hr = D3DCompile(
				ShaderSource,
				strlen(ShaderSource),
				nullptr,
				nullptr,
				nullptr,
				"PSMain",
				"ps_5_0",
				CompileFlags,
				0,
				PSBlob.GetAddressOf(),
				ErrorBlob.GetAddressOf()
			);

			if (FAILED(hr) || !PSBlob)
			{
				UE_LOG(LogSpoutPlugin, Error, TEXT("Failed to compile Spout gamma PS (hr=0x%08x)"), hr);
				return false;
			}

			hr = InDevice->CreateVertexShader(VSBlob->GetBufferPointer(), VSBlob->GetBufferSize(), nullptr, VertexShader.GetAddressOf());
			if (FAILED(hr))
			{
				UE_LOG(LogSpoutPlugin, Error, TEXT("Failed to create Spout gamma VS (hr=0x%08x)"), hr);
				return false;
			}

			hr = InDevice->CreatePixelShader(PSBlob->GetBufferPointer(), PSBlob->GetBufferSize(), nullptr, PixelShader.GetAddressOf());
			if (FAILED(hr))
			{
				UE_LOG(LogSpoutPlugin, Error, TEXT("Failed to create Spout gamma PS (hr=0x%08x)"), hr);
				return false;
			}

			D3D11_SAMPLER_DESC SamplerDesc = {};
			SamplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			SamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			SamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			SamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			SamplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			hr = InDevice->CreateSamplerState(&SamplerDesc, SamplerState.GetAddressOf());
			if (FAILED(hr))
			{
				UE_LOG(LogSpoutPlugin, Error, TEXT("Failed to create Spout gamma sampler (hr=0x%08x)"), hr);
				return false;
			}

			D3D11_RASTERIZER_DESC RasterDesc = {};
			RasterDesc.FillMode = D3D11_FILL_SOLID;
			RasterDesc.CullMode = D3D11_CULL_NONE;
			RasterDesc.ScissorEnable = false;
			RasterDesc.DepthClipEnable = true;
			hr = InDevice->CreateRasterizerState(&RasterDesc, RasterizerState.GetAddressOf());
			if (FAILED(hr))
			{
				UE_LOG(LogSpoutPlugin, Error, TEXT("Failed to create Spout gamma rasterizer (hr=0x%08x)"), hr);
				return false;
			}

			D3D11_BLEND_DESC BlendDesc = {};
			BlendDesc.RenderTarget[0].BlendEnable = false;
			BlendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			hr = InDevice->CreateBlendState(&BlendDesc, BlendState.GetAddressOf());
			if (FAILED(hr))
			{
				UE_LOG(LogSpoutPlugin, Error, TEXT("Failed to create Spout gamma blend state (hr=0x%08x)"), hr);
				return false;
			}

			D3D11_DEPTH_STENCIL_DESC DepthDesc = {};
			DepthDesc.DepthEnable = false;
			DepthDesc.StencilEnable = false;
			hr = InDevice->CreateDepthStencilState(&DepthDesc, DepthStencilState.GetAddressOf());
			if (FAILED(hr))
			{
				UE_LOG(LogSpoutPlugin, Error, TEXT("Failed to create Spout gamma depth state (hr=0x%08x)"), hr);
				return false;
			}

			D3D11_BUFFER_DESC BufferDesc = {};
			BufferDesc.ByteWidth = sizeof(FSpoutGammaConstants);
			BufferDesc.Usage = D3D11_USAGE_DEFAULT;
			BufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			BufferDesc.CPUAccessFlags = 0;
			hr = InDevice->CreateBuffer(&BufferDesc, nullptr, GammaBuffer.GetAddressOf());
			if (FAILED(hr))
			{
				UE_LOG(LogSpoutPlugin, Error, TEXT("Failed to create Spout gamma constant buffer (hr=0x%08x)"), hr);
				return false;
			}

			bInitialized = true;
			return true;
		}

		bool Apply(
			ID3D11DeviceContext* ImmediateContext,
			ID3D11ShaderResourceView* InputSRV,
			ID3D11RenderTargetView* OutputRTV,
			float Gamma,
			uint32 Width,
			uint32 Height)
		{
			if (!bInitialized || !ImmediateContext || !InputSRV || !OutputRTV)
			{
				return false;
			}

			const FSpoutGammaConstants Constants = { Gamma, {0.0f, 0.0f, 0.0f} };
			ImmediateContext->UpdateSubresource(GammaBuffer.Get(), 0, nullptr, &Constants, 0, 0);

			D3D11_VIEWPORT Viewport = {};
			Viewport.TopLeftX = 0.0f;
			Viewport.TopLeftY = 0.0f;
			Viewport.Width = static_cast<float>(Width);
			Viewport.Height = static_cast<float>(Height);
			Viewport.MinDepth = 0.0f;
			Viewport.MaxDepth = 1.0f;
			ImmediateContext->RSSetViewports(1, &Viewport);
			ImmediateContext->RSSetState(RasterizerState.Get());

			ID3D11RenderTargetView* RenderTargets[1] = { OutputRTV };
			ImmediateContext->OMSetRenderTargets(1, RenderTargets, nullptr);
			const float BlendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			ImmediateContext->OMSetBlendState(BlendState.Get(), BlendFactor, 0xFFFFFFFF);
			ImmediateContext->OMSetDepthStencilState(DepthStencilState.Get(), 0);
			ImmediateContext->IASetInputLayout(nullptr);
			ImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			ImmediateContext->VSSetShader(VertexShader.Get(), nullptr, 0);
			ImmediateContext->PSSetShader(PixelShader.Get(), nullptr, 0);

			ID3D11Buffer* ConstantBuffers[1] = { GammaBuffer.Get() };
			ImmediateContext->PSSetConstantBuffers(0, 1, ConstantBuffers);

			ID3D11SamplerState* Samplers[1] = { SamplerState.Get() };
			ImmediateContext->PSSetSamplers(0, 1, Samplers);

			ID3D11ShaderResourceView* SRVs[1] = { InputSRV };
			ImmediateContext->PSSetShaderResources(0, 1, SRVs);

			ImmediateContext->Draw(3, 0);

			ID3D11ShaderResourceView* NullSRVs[1] = { nullptr };
			ImmediateContext->PSSetShaderResources(0, 1, NullSRVs);

			ID3D11RenderTargetView* NullRTVs[1] = { nullptr };
			ImmediateContext->OMSetRenderTargets(1, NullRTVs, nullptr);

			return true;
		}

	private:
		ID3D11Device* CachedDevice = nullptr;
		bool bInitialized = false;
		ComPtr<ID3D11VertexShader> VertexShader;
		ComPtr<ID3D11PixelShader> PixelShader;
		ComPtr<ID3D11SamplerState> SamplerState;
		ComPtr<ID3D11RasterizerState> RasterizerState;
		ComPtr<ID3D11BlendState> BlendState;
		ComPtr<ID3D11DepthStencilState> DepthStencilState;
		ComPtr<ID3D11Buffer> GammaBuffer;
	};

// Gamma helpers are kept for potential future use; currently unused.
float SanitizeGamma(float Gamma)
{
	if (Gamma <= 0.0f)
	{
		return 1.0f;
	}

	return FMath::Clamp(Gamma, 0.01f, 10.0f);
}

bool EnsureGammaResources(FSpoutSharedSender& Sender, uint32 Width, uint32 Height, DXGI_FORMAT Format, FSpoutD3DContext& Context, ComPtr<ID3D11Texture2D>& OutTexture, ComPtr<ID3D11RenderTargetView>& OutRTV)
{
	FScopeLock SenderLock(&Sender.ResourceMutex);
	bool bNeedsCreate = false;

	if (!Sender.GammaTexture || !Sender.GammaRTV)
	{
		bNeedsCreate = true;
	}
	else
	{
		D3D11_TEXTURE2D_DESC Desc;
		Sender.GammaTexture->GetDesc(&Desc);
		if (Desc.Width != Width || Desc.Height != Height || Desc.Format != Format)
		{
			bNeedsCreate = true;
		}
	}

	if (bNeedsCreate)
	{
		Sender.GammaTexture.Reset();
		Sender.GammaRTV.Reset();

		spoutDirectX* SpoutDX = Context.GetSpoutDirectX();
		ID3D11Device* Device = Context.GetD3D11Device();
		if (!SpoutDX || !Device)
		{
			return false;
		}

		ComPtr<ID3D11Texture2D> NewTexture;
		const bool bCreated = SpoutDX->CreateDX11Texture(
			Device,
			Width,
			Height,
			Format,
			D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
			0,
			NewTexture.GetAddressOf());

		if (!bCreated || !NewTexture)
		{
			return false;
		}

		ComPtr<ID3D11RenderTargetView> NewRTV;
		HRESULT hr = Device->CreateRenderTargetView(NewTexture.Get(), nullptr, NewRTV.GetAddressOf());
		if (FAILED(hr))
		{
			return false;
		}

		Sender.GammaTexture = MoveTemp(NewTexture);
		Sender.GammaRTV = MoveTemp(NewRTV);
	}

	OutTexture = Sender.GammaTexture;
	OutRTV = Sender.GammaRTV;
	return OutTexture && OutRTV;
}

	void SendTextureOnRenderThread(const FName SpoutName, const FTextureRHIRef& SourceRHI, bool bIsViewport, FRHICommandListImmediate& RHICmdList)
	{
		if (!SourceRHI.IsValid())
		{
			return;
		}

		FSpoutD3DContext& Context = FSpoutD3DContext::Get();
		if (!Context.IsSpoutAvailable())
		{
			return;
		}

		// D3D11 immediate context is not thread-safe; guard its usage.
		FScopeLock D3DLock(&Context.GetD3D11ContextMutex());
		if (!Context.GetImmediateContext() || !Context.GetD3D11On12Device())
		{
			return;
		}

		// UE D3D12 textures expose their native resource for interop.
		ID3D12Resource* NativeRes = static_cast<ID3D12Resource*>(SourceRHI->GetNativeResource());
		if (!NativeRes)
		{
			return;
		}

		// Transition for copy and flush so the wrapped D3D11 context can see up-to-date data.
		// Viewport backbuffer comes in Present state; render targets come in SRVMask after scene render.
		const ERHIAccess FromState = bIsViewport ? ERHIAccess::Present : ERHIAccess::SRVMask;
		RHICmdList.Transition(FRHITransitionInfo(SourceRHI, FromState, ERHIAccess::CopySrc));
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);

		D3D12_RESOURCE_DESC Desc = NativeRes->GetDesc();

		DXGI_FORMAT Format = SanitizeDxgiFormat(Desc.Format);
		if (Format == DXGI_FORMAT_UNKNOWN)
		{
			// Fall back to a common format if the backbuffer format cannot be used.
			Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		}
		TSharedPtr<FSpoutSharedSender> Sender = FSpoutSenderRegistry::Get().FindOrAdd(SpoutName, ESpoutType::Sender);
		if (!Sender)
		{
			return;
		}

		EnsureSharedSenderTexture(*Sender, static_cast<uint32>(Desc.Width), static_cast<uint32>(Desc.Height), Format, Context);

		ComPtr<ID3D11Texture2D> SharedTexture;
		{
			FScopeLock SenderLock(&Sender->ResourceMutex);
			SharedTexture = Sender->SharedTexture;
		}

		ComPtr<ID3D11Texture2D> WrappedResource = SharedTexture ? GetWrappedResource(*Sender, NativeRes) : nullptr;
		if (SharedTexture && WrappedResource)
		{
			FScopedD3D11On12Acquire Acquire(WrappedResource.Get());
			if (Acquire.IsValid())
			{
				Context.GetImmediateContext()->CopyResource(SharedTexture.Get(), WrappedResource.Get());
			}
		}

		// Restore the source access state after the copy.
		RHICmdList.Transition(FRHITransitionInfo(
			SourceRHI,
			ERHIAccess::CopySrc,
			bIsViewport ? ERHIAccess::Present : ERHIAccess::SRVGraphics
		));
	}

	class FSpoutViewportSender
	{
	public:
		static FSpoutViewportSender& Get()
		{
			static FSpoutViewportSender Instance;
			return Instance;
		}

		bool QueueSend(const FName& SpoutName)
		{
			if (SpoutName.IsNone())
			{
				return false;
			}

			if (!RegisterIfNeeded())
			{
				return false;
			}

			FScopeLock Lock(&PendingMutex);
			PendingSenders.Add(SpoutName);
			return true;
		}

		void Shutdown()
		{
			if (bRegistered && FSlateApplication::IsInitialized())
			{
				if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
				{
					Renderer->OnBackBufferReadyToPresent().Remove(BackBufferHandle);
				}
			}

			bRegistered = false;
			BackBufferHandle.Reset();

			FScopeLock Lock(&PendingMutex);
			PendingSenders.Reset();
		}

	private:
		bool RegisterIfNeeded()
		{
			if (bRegistered)
			{
				return true;
			}

			if (!GEngine || !GEngine->GameViewport)
			{
				return false;
			}

			if (!FSlateApplication::IsInitialized())
			{
				return false;
			}

			FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer();
			if (!Renderer)
			{
				return false;
			}

			TSharedPtr<SWindow> Window = GEngine->GameViewport->GetWindow();
			if (!Window.IsValid())
			{
				return false;
			}

			TargetWindow = Window;

			BackBufferHandle =
			Renderer->OnBackBufferReadyToPresent().AddLambda(
#if SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER
				[this](SWindow& SlateWindow, ISlateViewportProvider& ViewportProvider)
#else
				[this](SWindow& SlateWindow, const FTextureRHIRef& IncomingBackBuffer)
#endif
				{
#if SPOUT_BACKBUFFER_USES_VIEWPORT_PROVIDER
					// UE 5.8+: the back buffer is obtained from the viewport provider.
					const FTextureRHIRef BackBuffer = ViewportProvider.GetBackBufferResource();
#else
					const FTextureRHIRef& BackBuffer = IncomingBackBuffer;
#endif
					if (!BackBuffer.IsValid())
					{
						return;
					}

					TSharedPtr<SWindow> ViewportWindow =
						(GEngine && GEngine->GameViewport)
						? GEngine->GameViewport->GetWindow()
						: nullptr;

					if (!ViewportWindow.IsValid() || ViewportWindow.Get() != &SlateWindow)
					{
						return;
					}

					TArray<FName> NamesToSend;
					{
						FScopeLock Lock(&PendingMutex);
						NamesToSend = PendingSenders.Array();
						PendingSenders.Reset();
					}

					if (NamesToSend.Num() == 0)
					{
						return;
					}

					const FTextureRHIRef BackBufferCopy = BackBuffer;

					ENQUEUE_RENDER_COMMAND(SpoutViewportSend)(
						[NamesToSend = MoveTemp(NamesToSend), BackBufferCopy](FRHICommandListImmediate& RHICmdList)
						{
							for (const FName& Name : NamesToSend)
							{
								SendTextureOnRenderThread(
									Name,
									BackBufferCopy,
									true,
									RHICmdList
								);
							}
						}
					);
				}
			);

			bRegistered = BackBufferHandle.IsValid();
			return bRegistered;
		}

		FCriticalSection PendingMutex;
		TSet<FName> PendingSenders;
		TWeakPtr<SWindow> TargetWindow;
		FDelegateHandle BackBufferHandle;
		bool bRegistered = false;
	};

	void EnsureSharedSenderTexture(FSpoutSharedSender& Sender, uint32 Width, uint32 Height, DXGI_FORMAT Format, FSpoutD3DContext& Context)
	{
		// Sender state can be accessed on the render thread and by BP calls.
		FScopeLock SenderLock(&Sender.ResourceMutex);
		if (Sender.SharedTexture && Sender.Width == Width && Sender.Height == Height)
		{
			return;
		}

		Sender.SharedTexture.Reset();
		Sender.SharedHandle = nullptr;
		Sender.CachedWrappedResource.Reset();
		Sender.CachedNativeResource = nullptr;
		Sender.GammaTexture.Reset();
		Sender.GammaRTV.Reset();
		Sender.Width = Width;
		Sender.Height = Height;

		spoutDirectX* SpoutDX = Context.GetSpoutDirectX();
		if (!SpoutDX)
		{
			return;
		}

		// Spout creates a DX11 shared texture and returns a shared handle.
		const bool bCreated = SpoutDX->CreateSharedDX11Texture(
			Context.GetD3D11Device(),
			Width,
			Height,
			Format,
			Sender.SharedTexture.GetAddressOf(),
			Sender.SharedHandle
		);

		if (bCreated && Sender.SharedTexture && Sender.SharedHandle)
		{
			spoutSenderNames* SenderNames = Context.GetSenderNames();
			if (SenderNames)
			{
				// Register or update this sender in Spout's global registry.
				FScopeLock SpoutLock(&Context.GetSpoutMutex());
				if (!Sender.bRegistered)
				{
					Sender.bRegistered = SenderNames->CreateSender(
						TCHAR_TO_UTF8(*Sender.Name.ToString()),
						Sender.Width,
						Sender.Height,
						Sender.SharedHandle,
						Format
					);
				}
				else
				{
					SenderNames->UpdateSender(
						TCHAR_TO_UTF8(*Sender.Name.ToString()),
						Sender.Width,
						Sender.Height,
						Sender.SharedHandle,
						Format
					);
				}
			}
		}
	}

	ComPtr<ID3D11Texture2D> GetWrappedResource(FSpoutSharedSender& Sender, ID3D12Resource* NativeResource)
	{
		// Cache the wrapped resource per native D3D12 texture to avoid re-wrapping every frame.
		FScopeLock SenderLock(&Sender.ResourceMutex);
		if (Sender.CachedNativeResource != NativeResource || !Sender.CachedWrappedResource)
		{
			Sender.CachedWrappedResource = WrapD3D12Resource(NativeResource, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Sender.CachedNativeResource = NativeResource;
		}

		return Sender.CachedWrappedResource;
	}
}

bool FSpoutSender::Send(FName SpoutName, ESpoutSendTextureFrom SendTextureFrom, UTextureRenderTarget2D* TextureRenderTarget2D)
{
	FSpoutD3DContext& Context = FSpoutD3DContext::Get();
	Context.InitializeIfNeeded();

	if (!Context.IsSpoutAvailable())
	{
		return false;
	}

	if (SendTextureFrom == ESpoutSendTextureFrom::GameViewport)
	{
		if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
		{
			return false;
		}

		// Queue the request; capture happens when the backbuffer is ready to present.
		return FSpoutViewportSender::Get().QueueSend(SpoutName);
	}

	FSpoutSendSource SendSource;
	if (!BuildSendSource(SendTextureFrom, TextureRenderTarget2D, SendSource))
	{
		return false;
	}

	// The copy must run on the render thread to safely touch RHI resources.
	ENQUEUE_RENDER_COMMAND(SpoutSenderCmd)(
		[SpoutName, SendSource](FRHICommandListImmediate& RHICmdList)
		{
			if (!SendSource.SourceRHI.IsValid())
			{
				return;
			}

			SendTextureOnRenderThread(SpoutName, SendSource.SourceRHI, SendSource.bIsViewport, RHICmdList);
		});

	return true;
}

void FSpoutSender::Close(FName SpoutName)
{
	// Registry is keyed by (Name, Type); Close must scope to the Sender entry only,
	// leaving any Receiver entry with the same FName untouched.
	if (FSpoutSenderRegistry::Get().Find(SpoutName, ESpoutType::Sender))
	{
		FSpoutSenderRegistry::Get().Remove(SpoutName, ESpoutType::Sender);
		UE_LOG(LogSpoutPlugin, Log, TEXT("Closed Spout Sender: %s"), *SpoutName.ToString());
	}
}

void FSpoutSender::Shutdown()
{
	FSpoutViewportSender::Get().Shutdown();
}
