// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#pragma once
#include <d3d11.h>
#include <wrl/client.h>

// Serialized by the caller's immediate-context lock. A CPU mutex may only be
// handed to a foreign device AFTER the GPU has stopped accessing its texture.
class FSpoutGpuCompletion
{
public:
	bool Prepare(ID3D11DeviceContext* Context)
	{
		if (Query) return true;
		if (!Context) return false;
		Microsoft::WRL::ComPtr<ID3D11Device> Device;
		Context->GetDevice(&Device);
		D3D11_QUERY_DESC Desc = { D3D11_QUERY_EVENT, 0 };
		LastResult = Device->CreateQuery(&Desc, &Query);
		return SUCCEEDED(LastResult);
	}
	HRESULT Wait(ID3D11DeviceContext* Context)
	{
		if (!Query || !Context) return E_UNEXPECTED;
		Context->End(Query.Get());
		Context->Flush();
		HRESULT Result;
		while ((Result = Context->GetData(Query.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
		{
			// Releasing access on a wall-clock timeout would expose a texture while
			// the GPU still uses it. Device removal is reported by GetData instead.
			SwitchToThread();
		}
		LastResult = Result;
		return Result;
	}
	HRESULT GetLastResult() const { return LastResult; }
	void Reset() { Query.Reset(); }
private:
	Microsoft::WRL::ComPtr<ID3D11Query> Query;
	HRESULT LastResult = S_OK;
};
