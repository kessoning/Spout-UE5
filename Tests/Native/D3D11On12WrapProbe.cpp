// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Isolated Windows-runtime diagnostic: no Unreal and no Spout code/library.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3d11on12.h>
#include <wrl/client.h>
#include <iostream>
#include <vector>
using Microsoft::WRL::ComPtr;
int main()
{
	ComPtr<ID3D12Debug> Debug;
	if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&Debug)))) return 1;
	Debug->EnableDebugLayer();
	ComPtr<ID3D12Device> Device;
	if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)))) return 2;
	D3D12_COMMAND_QUEUE_DESC QueueDesc = {};
	ComPtr<ID3D12CommandQueue> Queue;
	if (FAILED(Device->CreateCommandQueue(&QueueDesc, IID_PPV_ARGS(&Queue)))) return 3;
	IUnknown* Queues[] = {Queue.Get()};
	ComPtr<ID3D11Device> Device11;
	ComPtr<ID3D11DeviceContext> Context;
	if (FAILED(D3D11On12CreateDevice(Device.Get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, Queues, 1, 0, &Device11, &Context, nullptr))) return 4;
	ComPtr<ID3D11On12Device> On12; Device11.As(&On12);
	ComPtr<ID3D12InfoQueue> Info; Device.As(&Info);
	D3D12_HEAP_PROPERTIES Heap = {}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT; Heap.CreationNodeMask = Heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC Desc = {};
	Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; Desc.Width = Desc.Height = 32;
	Desc.DepthOrArraySize = Desc.MipLevels = Desc.SampleDesc.Count = 1;
	Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	ComPtr<ID3D12Resource> Texture;
	if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&Texture)))) return 5;
	D3D11_RESOURCE_FLAGS Flags = {}; Flags.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	Info->ClearStoredMessages();
	ComPtr<ID3D11Texture2D> Wrapped;
	HRESULT Result = On12->CreateWrappedResource(Texture.Get(), &Flags, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE, IID_PPV_ARGS(&Wrapped));
	std::cout << "CreateWrappedResource HRESULT=0x" << std::hex << Result << std::dec << " validTexture=" << !!Wrapped << std::endl;
	for (UINT64 I = 0; I < Info->GetNumStoredMessages(); ++I)
	{
		SIZE_T Size = 0; Info->GetMessage(I, nullptr, &Size); std::vector<char> Storage(Size);
		auto* Message = reinterpret_cast<D3D12_MESSAGE*>(Storage.data()); Info->GetMessage(I, Message, &Size);
		std::cout << "ID=" << Message->ID << " severity=" << Message->Severity << " " << Message->pDescription << std::endl;
	}
	return FAILED(Result) ? 6 : 0;
}
