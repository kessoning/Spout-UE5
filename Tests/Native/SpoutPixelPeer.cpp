// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Separate-process pixel oracle for Unreal integration. No Unreal dependencies.
#include <windows.h>
#include <wrl/client.h>
#include "SpoutSdkSafety.h"
#include "SpoutGpuCompletion.h"
#include <vector>
#include <iostream>
#include <fstream>

using Microsoft::WRL::ComPtr;
static unsigned int Pixel(unsigned int X, unsigned int Y, unsigned int Frame)
{
	return (((X == 0 && Y == 0) ? 255u : ((X * 3 + Y * 7 + Frame) & 255)) << 24) | (((X * 17 + Frame) & 255) << 16)
		| (((Y * 29 + Frame) & 255) << 8) | ((X + Y + Frame * 13) & 255);
}
int main(int Argc, char** Argv)
{
	if (Argc < 4) return 2;
	const bool Sending = std::strcmp(Argv[1], "send") == 0;
	const DXGI_FORMAT ExpectedFormat = !Sending && Argc > 4 && std::strcmp(Argv[4], "srgb") == 0
		? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
	ComPtr<ID3D11Device> Device;
	ComPtr<ID3D11DeviceContext> Context;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
		D3D11_SDK_VERSION, &Device, nullptr, &Context))) return 3;
	FSpoutGpuCompletion Completion;
	if (!Completion.Prepare(Context.Get())) return 4;
	// Use the unmodified SDK access protocol as the external peer. Crash recovery
	// is exercised separately; this test compares complete frames across devices.
	spoutFrameCount FrameSync;
	if (!FrameSync.CreateAccessMutex(Argv[2])) return 5;
	FrameSync.EnableFrameCount(Argv[2]);
	FSpoutSafeSenderNames Names;
	ComPtr<ID3D11Texture2D> Texture, Staging;
	HANDLE ShareHandle = nullptr;
	HANDLE Stop = Sending ? OpenEventA(SYNCHRONIZE, FALSE, Argv[3]) : nullptr;
	HANDLE Ready = Sending && Argc > 4 ? OpenEventA(EVENT_MODIFY_STATE, FALSE, Argv[4]) : nullptr;
	if (Sending && (!Stop || !Ready)) return 6;
	unsigned int LastWidth = 0, LastHeight = 0, Received = 0, LastSequence = 999;
	bool Registered = false;
	int Result = 0;
	const ULONGLONG Deadline = GetTickCount64() + 30000;
	for (unsigned int Sequence = 1; GetTickCount64() < Deadline; ++Sequence)
	{
		if (Sending)
		{
			if (WaitForSingleObject(Stop, 0) == WAIT_OBJECT_0) break;
			const unsigned int W = (Sequence / 32) % 2 ? 64 : 32, H = W / 2;
			if (!Texture || W != LastWidth)
			{
				D3D11_TEXTURE2D_DESC Desc = {};
				Desc.Width = W; Desc.Height = H; Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				Desc.MipLevels = Desc.ArraySize = Desc.SampleDesc.Count = 1;
				Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; Desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
				ComPtr<ID3D11Texture2D> Replacement;
				if (FAILED(Device->CreateTexture2D(&Desc, nullptr, &Replacement))) { Result = 7; break; }
				ComPtr<IDXGIResource> Resource; Replacement.As(&Resource); Resource->GetSharedHandle(&ShareHandle);
				const bool Published = Registered
					? Names.UpdateSender(Argv[2], W, H, ShareHandle, Desc.Format)
					: Names.CreateSenderExact(Argv[2], W, H, ShareHandle, Desc.Format);
				if (!Published) { Result = 8; break; }
				Registered = true; Texture = Replacement; LastWidth = W; LastHeight = H;
			}
			if (!FrameSync.CheckTextureAccess(Texture.Get())) { Sleep(1); continue; }
			std::vector<unsigned int> Pixels(W * H);
			for (unsigned int Y = 0; Y < H; ++Y)
				for (unsigned int X = 0; X < W; ++X) Pixels[Y * W + X] = Pixel(X, Y, Sequence);
			Context->UpdateSubresource(Texture.Get(), 0, nullptr, Pixels.data(), W * 4, 0);
			const HRESULT Done = Completion.Wait(Context.Get());
			if (SUCCEEDED(Done)) FrameSync.SetNewFrame();
			FrameSync.AllowTextureAccess(Texture.Get());
			if (FAILED(Done)) { Result = 9; break; }
			SetEvent(Ready); Sleep(5);
		}
		else
		{
			unsigned int W = 0, H = 0; HANDLE Handle = nullptr; DWORD Format = 0;
			if (!Names.GetSenderInfo(Argv[2], W, H, Handle, Format)) { Sleep(2); continue; }
			if (!W || !H || W > 1024 || H > 1024 || Format != ExpectedFormat) { Result = 10; break; }
			if (!Texture || Handle != ShareHandle || W != LastWidth || H != LastHeight)
			{
				ComPtr<ID3D11Texture2D> Replacement;
				if (FAILED(Device->OpenSharedResource(Handle, IID_PPV_ARGS(&Replacement)))) { Sleep(2); continue; }
				D3D11_TEXTURE2D_DESC Desc = {}; Replacement->GetDesc(&Desc);
				if (Desc.Width != W || Desc.Height != H || Desc.MipLevels != 1 || Desc.ArraySize != 1 || Desc.SampleDesc.Count != 1) { Sleep(2); continue; }
				// Metadata must describe a view the receiving application can actually create.
				D3D11_SHADER_RESOURCE_VIEW_DESC ViewDesc = {};
				ViewDesc.Format = ExpectedFormat;
				ViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				ViewDesc.Texture2D.MipLevels = 1;
				ComPtr<ID3D11ShaderResourceView> View;
				if (FAILED(Device->CreateShaderResourceView(Replacement.Get(), &ViewDesc, &View))) { Result = 16; break; }
				Desc.Usage = D3D11_USAGE_STAGING; Desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				Desc.BindFlags = Desc.MiscFlags = 0;
				Staging.Reset();
				if (FAILED(Device->CreateTexture2D(&Desc, nullptr, &Staging))) { Result = 11; break; }
				Texture = Replacement; ShareHandle = Handle; LastWidth = W; LastHeight = H;
			}
			if (!FrameSync.CheckTextureAccess(Texture.Get())) { Sleep(1); continue; }
			Context->CopyResource(Staging.Get(), Texture.Get());
			const HRESULT Done = Completion.Wait(Context.Get());
			FrameSync.AllowTextureAccess(Texture.Get());
			if (FAILED(Done)) { Result = 12; break; }
			D3D11_MAPPED_SUBRESOURCE Map = {};
			if (FAILED(Context->Map(Staging.Get(), 0, D3D11_MAP_READ, 0, &Map))) { Result = 13; break; }
			const unsigned int Frame = (*static_cast<const unsigned int*>(Map.pData) >> 16) & 255;
			bool Match = true;
			for (unsigned int Y = 0; Y < H; ++Y)
			{
				const unsigned int* Row = reinterpret_cast<const unsigned int*>(static_cast<const char*>(Map.pData) + Y * Map.RowPitch);
				for (unsigned int X = 0; X < W; ++X) Match &= Row[X] == Pixel(X, Y, Frame);
			}
			Context->Unmap(Staging.Get(), 0);
			if (!Match) { Result = 14; break; }
			if (Frame != LastSequence) { ++Received; LastSequence = Frame; }
			if (Received >= 8) break;
			Sleep(2);
		}
	}
	if (Registered) Names.ReleaseSenderName(Argv[2]);
	if (Stop) CloseHandle(Stop);
	if (Ready) CloseHandle(Ready);
	if (!Sending)
	{
		if (Received < 8 && Result == 0) Result = 15;
		std::ofstream Out(Argv[3]);
		Out << "{\"exit\":" << Result << ",\"verifiedFrames\":" << Received << "}" << std::endl;
	}
	std::cout << "Spout pixel peer: result=" << Result << " verified=" << Received << std::endl;
	return Result;
}
