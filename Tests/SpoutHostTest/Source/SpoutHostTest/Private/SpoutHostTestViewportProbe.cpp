// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#include "SpoutHostTestViewportProbe.h"

#ifndef SPOUT_HOSTTEST_HAS_SDK
#define SPOUT_HOSTTEST_HAS_SDK 0
#endif

#if PLATFORM_WINDOWS && SPOUT_HOSTTEST_HAS_SDK
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include <d3d11.h>
#include <wrl/client.h>
#include "SpoutDX/SpoutSenderNames.h"
#include "SpoutDX/SpoutDirectX.h"
#include "SpoutDX/SpoutFrameCount.h"
#include "Windows/HideWindowsPlatformTypes.h"
#ifdef GetNextSibling
#undef GetNextSibling
#endif
#endif

namespace SpoutHostTest
{
#if PLATFORM_WINDOWS && SPOUT_HOSTTEST_HAS_SDK

	struct FViewportSenderProbe::FImpl
	{
		// The probe's OWN SDK observers. Deliberately not the plugin's instances: independence is
		// the whole point. Multiple instances across (or within) processes is the SDK's normal
		// operating mode - every Spout application holds its own.
		spoutSenderNames SenderNames;
		spoutDirectX SpoutDX;
		spoutFrameCount FrameCount;

		ID3D11Device* Device = nullptr;
		ID3D11DeviceContext* Context = nullptr;
		ID3D11Texture2D* SharedTexture = nullptr;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> PixelStaging;
		Microsoft::WRL::ComPtr<ID3D11Query> PixelDone;
		FString AccessName;
		HANDLE PixelShareHandle = nullptr;

		bool bFrameObservationOpen = false;

		static void CopyName(const FString& SenderName, char (&OutName)[256])
		{
			const auto Converted = StringCast<ANSICHAR>(*SenderName);
			FCStringAnsi::Strncpy(OutName, Converted.Get(), 256);
			OutName[255] = '\0';
		}
	};

	FViewportSenderProbe::FViewportSenderProbe() = default;

	FViewportSenderProbe::~FViewportSenderProbe()
	{
		Close();
	}

	bool FViewportSenderProbe::Initialize(FString& OutError)
	{
		if (Impl.IsValid())
		{
			return true;
		}

		TUniquePtr<FImpl> NewImpl = MakeUnique<FImpl>();

		// A plain hardware D3D11 device on the default adapter - the same kind of device an
		// external DX11 receiver application would create. NOT the plugin's D3D11-on-12 device:
		// opening the share handle on an unrelated device is what proves the texture is genuinely
		// shared rather than merely present in the sender's own device.
		const D3D_FEATURE_LEVEL RequestedLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		D3D_FEATURE_LEVEL ObtainedLevel = D3D_FEATURE_LEVEL_11_0;
		const HRESULT DeviceResult = D3D11CreateDevice(
			/*pAdapter*/ nullptr,
			D3D_DRIVER_TYPE_HARDWARE,
			/*Software*/ nullptr,
			/*Flags*/ 0,
			RequestedLevels,
			UE_ARRAY_COUNT(RequestedLevels),
			D3D11_SDK_VERSION,
			&NewImpl->Device,
			&ObtainedLevel,
			&NewImpl->Context);

		if (FAILED(DeviceResult) || NewImpl->Device == nullptr)
		{
			OutError = FString::Printf(
				TEXT("the probe could not create its own D3D11 device (HRESULT 0x%08X)"),
				static_cast<uint32>(DeviceResult));
			return false;
		}

		Impl = MoveTemp(NewImpl);
		return true;
	}

	void FViewportSenderProbe::Close()
	{
		if (!Impl.IsValid())
		{
			return;
		}

		EndFrameObservation();
		ReleaseSharedTexture();

		if (Impl->Context != nullptr)
		{
			Impl->Context->Release();
			Impl->Context = nullptr;
		}
		if (Impl->Device != nullptr)
		{
			Impl->Device->Release();
			Impl->Device = nullptr;
		}

		Impl.Reset();
	}

	bool FViewportSenderProbe::IsFrameCountingAvailable() const
	{
		return Impl.IsValid() && Impl->FrameCount.IsFrameCountEnabled();
	}

	bool FViewportSenderProbe::IsSenderAdvertised(const FString& SenderName) const
	{
		if (!Impl.IsValid())
		{
			return false;
		}

		char Name[256];
		FImpl::CopyName(SenderName, Name);
		return Impl->SenderNames.FindSenderName(Name);
	}

	bool FViewportSenderProbe::GetSenderDescription(const FString& SenderName,
		uint32& OutWidth, uint32& OutHeight, uint32& OutDxgiFormat) const
	{
		OutWidth = 0;
		OutHeight = 0;
		OutDxgiFormat = 0;

		if (!Impl.IsValid())
		{
			return false;
		}

		char Name[256];
		FImpl::CopyName(SenderName, Name);

		unsigned int Width = 0;
		unsigned int Height = 0;
		HANDLE ShareHandle = nullptr;
		DWORD Format = 0;
		if (!Impl->SenderNames.GetSenderInfo(Name, Width, Height, ShareHandle, Format))
		{
			return false;
		}

		OutWidth = static_cast<uint32>(Width);
		OutHeight = static_cast<uint32>(Height);
		OutDxgiFormat = static_cast<uint32>(Format);
		return true;
	}

	bool FViewportSenderProbe::OpenSharedTexture(const FString& SenderName,
		uint32& OutWidth, uint32& OutHeight, uint32& OutDxgiFormat, FString& OutError)
	{
		OutWidth = 0;
		OutHeight = 0;
		OutDxgiFormat = 0;

		if (!Impl.IsValid())
		{
			OutError = TEXT("the probe is not initialized");
			return false;
		}

		char Name[256];
		FImpl::CopyName(SenderName, Name);

		unsigned int Width = 0;
		unsigned int Height = 0;
		HANDLE ShareHandle = nullptr;
		DWORD Format = 0;
		if (!Impl->SenderNames.GetSenderInfo(Name, Width, Height, ShareHandle, Format))
		{
			OutError = TEXT("the sender is not in the shared registry");
			return false;
		}
		if (ShareHandle == nullptr)
		{
			OutError = TEXT("the registry entry carries no share handle");
			return false;
		}

		ReleaseSharedTexture();

		if (!Impl->SpoutDX.OpenDX11shareHandle(Impl->Device, &Impl->SharedTexture, ShareHandle)
			|| Impl->SharedTexture == nullptr)
		{
			Impl->SharedTexture = nullptr;
			OutError = TEXT("OpenDX11shareHandle failed on the probe's device");
			return false;
		}

		// Report what the OPENED resource says about itself, not what the registry claimed. The
		// two agreeing is part of the evidence.
		D3D11_TEXTURE2D_DESC Desc = {};
		Impl->SharedTexture->GetDesc(&Desc);
		OutWidth = static_cast<uint32>(Desc.Width);
		OutHeight = static_cast<uint32>(Desc.Height);
		OutDxgiFormat = static_cast<uint32>(Desc.Format);
		return true;
	}

	void FViewportSenderProbe::ReleaseSharedTexture()
	{
		if (Impl.IsValid() && Impl->SharedTexture != nullptr)
		{
			Impl->SharedTexture->Release();
			Impl->SharedTexture = nullptr;
		}
	}

	void FViewportSenderProbe::BeginFrameObservation(const FString& SenderName)
	{
		if (!Impl.IsValid() || Impl->bFrameObservationOpen)
		{
			return;
		}

		char Name[256];
		FImpl::CopyName(SenderName, Name);
		Impl->FrameCount.EnableFrameCount(Name);
		Impl->bFrameObservationOpen = true;
	}

	bool FViewportSenderProbe::PollNewFrame()
	{
		if (!Impl.IsValid() || !Impl->bFrameObservationOpen)
		{
			return false;
		}
		return Impl->FrameCount.GetNewFrame();
	}

	bool FViewportSenderProbe::ReadFirstPixel(const FString& SenderName, FColor& Pixel)
	{
		if (!Impl) return false;
		char Name[256]; FImpl::CopyName(SenderName, Name);
		unsigned int W=0,H=0; HANDLE Handle=nullptr; DWORD Format=0;
		if (!Impl->SenderNames.GetSenderInfo(Name,W,H,Handle,Format) || !Handle || Format!=DXGI_FORMAT_B8G8R8A8_UNORM) return false;
		if (!Impl->SharedTexture || Handle!=Impl->PixelShareHandle)
		{
			uint32 TW=0,TH=0,TF=0; FString Error;
			if (!OpenSharedTexture(SenderName,TW,TH,TF,Error) || TW!=W || TH!=H || TF!=Format) return false;
			Impl->PixelShareHandle=Handle;
		}
		if (Impl->AccessName!=SenderName)
		{
			Impl->FrameCount.CloseAccessMutex();
			if (!Impl->FrameCount.CreateAccessMutex(Name)) return false;
			Impl->AccessName=SenderName;
		}
		if (!Impl->PixelStaging)
		{
			D3D11_TEXTURE2D_DESC Desc={}; Desc.Width=Desc.Height=Desc.MipLevels=Desc.ArraySize=Desc.SampleDesc.Count=1;
			Desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; Desc.Usage=D3D11_USAGE_STAGING; Desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
			if (FAILED(Impl->Device->CreateTexture2D(&Desc,nullptr,&Impl->PixelStaging))) return false;
			D3D11_QUERY_DESC Query={D3D11_QUERY_EVENT,0};
			if (FAILED(Impl->Device->CreateQuery(&Query,&Impl->PixelDone))) { Impl->PixelStaging.Reset(); return false; }
		}
		if (!Impl->FrameCount.CheckTextureAccess(Impl->SharedTexture)) return false;
		D3D11_BOX Box={0,0,0,1,1,1};
		Impl->Context->CopySubresourceRegion(Impl->PixelStaging.Get(),0,0,0,0,Impl->SharedTexture,0,&Box);
		Impl->Context->End(Impl->PixelDone.Get()); Impl->Context->Flush();
		HRESULT Done=S_FALSE;
		while ((Done=Impl->Context->GetData(Impl->PixelDone.Get(),nullptr,0,0))==S_FALSE) SwitchToThread();
		Impl->FrameCount.AllowTextureAccess(Impl->SharedTexture);
		if (FAILED(Done)) return false;
		D3D11_MAPPED_SUBRESOURCE Map={};
		if (FAILED(Impl->Context->Map(Impl->PixelStaging.Get(),0,D3D11_MAP_READ,0,&Map))) return false;
		FMemory::Memcpy(&Pixel,Map.pData,sizeof(Pixel));
		Impl->Context->Unmap(Impl->PixelStaging.Get(),0);
		return true;
	}

	long FViewportSenderProbe::GetSenderFrameCount() const
	{
		if (!Impl.IsValid() || !Impl->bFrameObservationOpen)
		{
			return 0;
		}
		return Impl->FrameCount.GetSenderFrame();
	}

	void FViewportSenderProbe::EndFrameObservation()
	{
		if (Impl.IsValid() && Impl->bFrameObservationOpen)
		{
			Impl->FrameCount.CleanupFrameCount();
			Impl->bFrameObservationOpen = false;
		}
	}

#else // !(PLATFORM_WINDOWS && SPOUT_HOSTTEST_HAS_SDK)

	// Built without the bundled SDK headers (an unstaged layout). Every operation fails loudly so
	// the scenario reports missing evidence instead of skipping into a pass.
	struct FViewportSenderProbe::FImpl {};

	FViewportSenderProbe::FViewportSenderProbe() = default;
	FViewportSenderProbe::~FViewportSenderProbe() = default;

	bool FViewportSenderProbe::Initialize(FString& OutError)
	{
		OutError = TEXT("this host was compiled without the bundled Spout SDK headers, so no external observation is possible");
		return false;
	}

	void FViewportSenderProbe::Close() {}
	bool FViewportSenderProbe::IsFrameCountingAvailable() const { return false; }
	bool FViewportSenderProbe::IsSenderAdvertised(const FString&) const { return false; }
	bool FViewportSenderProbe::GetSenderDescription(const FString&, uint32&, uint32&, uint32&) const { return false; }
	bool FViewportSenderProbe::OpenSharedTexture(const FString&, uint32&, uint32&, uint32&, FString& OutError)
	{
		OutError = TEXT("no SDK in this build");
		return false;
	}
	void FViewportSenderProbe::ReleaseSharedTexture() {}
	void FViewportSenderProbe::BeginFrameObservation(const FString&) {}
	bool FViewportSenderProbe::PollNewFrame() { return false; }
	bool FViewportSenderProbe::ReadFirstPixel(const FString&, FColor&) { return false; }
	long FViewportSenderProbe::GetSenderFrameCount() const { return 0; }
	void FViewportSenderProbe::EndFrameObservation() {}

#endif
}
