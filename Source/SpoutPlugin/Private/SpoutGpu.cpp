// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

#include "SpoutGpu.h"

#include "SpoutD3DContext.h"
#include "SpoutModule.h"
#include "DynamicRHI.h"
#include "RHI.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <dxgi1_4.h>
#include "Windows/HideWindowsPlatformTypes.h"

namespace
{
	struct FCachedLocation
	{
		void* Handle = nullptr;
		SpoutGpu::FSenderGpu Result;
		double NextRetry = 0.0;
		// This texture on another GPU has been reported (TakeOtherGpuReport).
		bool bReported = false;
	};

	/** Another hardware adapter, with a device made on it to try handles that Unreal's GPU cannot open. */
	struct FOtherAdapter
	{
		FString Name;
		TRefCountPtr<ID3D11Device> Device;
	};

	// Game thread only.
	TMap<FName, FCachedLocation> GLocations;
	TArray<FOtherAdapter> GOtherAdapters;
	bool bOtherAdaptersListed = false;

	// A texture that opens nowhere (a sender closing or still starting) is tried again at most this often.
	constexpr double UnknownRetrySeconds = 1.0;

	bool UnrealAdapter(LUID& OutLuid, FString& OutName)
	{
		if (!GDynamicRHI || GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::D3D12)
		{
			return false;
		}
		ID3D12Device* Device12 = static_cast<ID3D12Device*>(GDynamicRHI->RHIGetNativeDevice());
		if (!Device12)
		{
			return false;
		}
		OutLuid = Device12->GetAdapterLuid();
		TRefCountPtr<IDXGIFactory4> Factory;
		TRefCountPtr<IDXGIAdapter1> Adapter;
		DXGI_ADAPTER_DESC1 Desc = {};
		if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(Factory.GetInitReference())))
			&& SUCCEEDED(Factory->EnumAdapterByLuid(OutLuid, IID_PPV_ARGS(Adapter.GetInitReference())))
			&& SUCCEEDED(Adapter->GetDesc1(&Desc)))
		{
			OutName = Desc.Description;
		}
		return true;
	}

	bool Opens(ID3D11Device* Device, void* Handle)
	{
		TRefCountPtr<ID3D11Texture2D> Texture;
		return Device && SUCCEEDED(Device->OpenSharedResource(static_cast<HANDLE>(Handle), IID_PPV_ARGS(Texture.GetInitReference()))) && Texture.IsValid();
	}

	/** Makes a device on every other hardware adapter, once: only reached when a texture does not open on Unreal's GPU. */
	void ListOtherAdapters(const LUID& UnrealLuid)
	{
		if (bOtherAdaptersListed)
		{
			return;
		}
		bOtherAdaptersListed = true;
		TRefCountPtr<IDXGIFactory1> Factory;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(Factory.GetInitReference()))))
		{
			return;
		}
		for (UINT Index = 0;; ++Index)
		{
			TRefCountPtr<IDXGIAdapter1> Adapter;
			if (Factory->EnumAdapters1(Index, Adapter.GetInitReference()) == DXGI_ERROR_NOT_FOUND)
			{
				break;
			}
			DXGI_ADAPTER_DESC1 Desc = {};
			if (!Adapter.IsValid() || FAILED(Adapter->GetDesc1(&Desc)) || (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0
				|| (Desc.AdapterLuid.LowPart == UnrealLuid.LowPart && Desc.AdapterLuid.HighPart == UnrealLuid.HighPart))
			{
				continue;
			}
			FOtherAdapter Other;
			Other.Name = Desc.Description;
			const D3D_FEATURE_LEVEL FeatureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
			if (SUCCEEDED(D3D11CreateDevice(Adapter.GetReference(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, FeatureLevels, UE_ARRAY_COUNT(FeatureLevels),
				D3D11_SDK_VERSION, Other.Device.GetInitReference(), nullptr, nullptr)) && Other.Device.IsValid())
			{
				GOtherAdapters.Add(MoveTemp(Other));
			}
		}
	}

	SpoutGpu::FSenderGpu Probe(void* Handle)
	{
		SpoutGpu::FSenderGpu Result;
		ID3D11Device* UnrealDevice = FSpoutD3DContext::Get().GetD3D11Device();
		LUID UnrealLuid = {};
		FString UnrealName;
		if (!UnrealDevice || !UnrealAdapter(UnrealLuid, UnrealName))
		{
			return Result;
		}
		if (Opens(UnrealDevice, Handle))
		{
			Result.Where = ESpoutSenderGpu::SameAsUnreal;
			Result.AdapterName = UnrealName;
			return Result;
		}
		ListOtherAdapters(UnrealLuid);
		for (const FOtherAdapter& Other : GOtherAdapters)
		{
			if (Opens(Other.Device.GetReference(), Handle))
			{
				Result.Where = ESpoutSenderGpu::Other;
				Result.AdapterName = Other.Name;
				return Result;
			}
		}
		return Result;
	}
}

namespace SpoutGpu
{
	FSenderGpu Locate(FName SenderName, void* SharedHandle)
	{
		check(IsInGameThread());
		if (!SharedHandle || SenderName.IsNone())
		{
			return FSenderGpu();
		}
		FCachedLocation& Entry = GLocations.FindOrAdd(SenderName);
		const double Now = FPlatformTime::Seconds();
		if (Entry.Handle == SharedHandle && (Entry.Result.Where != ESpoutSenderGpu::Unknown || Now < Entry.NextRetry))
		{
			return Entry.Result;
		}
		if (Entry.Handle != SharedHandle)
		{
			Entry.bReported = false;
		}
		Entry.Handle = SharedHandle;
		Entry.Result = Probe(SharedHandle);
		Entry.NextRetry = Now + UnknownRetrySeconds;
		return Entry.Result;
	}

	bool TakeOtherGpuReport(FName SenderName)
	{
		check(IsInGameThread());
		FCachedLocation* Entry = GLocations.Find(SenderName);
		if (!Entry || Entry->Result.Where != ESpoutSenderGpu::Other || Entry->bReported)
		{
			return false;
		}
		Entry->bReported = true;
		return true;
	}

	FString UnrealAdapterName()
	{
		LUID Luid = {};
		FString Name;
		return UnrealAdapter(Luid, Name) ? Name : FString();
	}

	void Reset()
	{
		check(IsInGameThread());
		GLocations.Reset();
		GOtherAdapters.Reset();
		bOtherAdaptersListed = false;
	}
}
