// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "SpoutSdkSafety.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

using Microsoft::WRL::ComPtr;
static int Failures = 0;
static void Check(bool Value, const char* Message)
{
	std::cout << (Value ? "PASS " : "FAIL ") << Message << std::endl;
	Failures += !Value;
}
struct FCapacityTestNames : FSpoutSafeSenderNames
{
	void LimitTo(int Count) { m_MaxSenders = Count; }
};
int main(int Argc, char** Argv)
{
	if (Argc == 6 && std::strcmp(Argv[1], "hold-live") == 0)
	{
		ComPtr<ID3D11Device> PeerDevice;
		if (FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&PeerDevice,nullptr,nullptr))) return 20;
		ComPtr<ID3D11Texture2D> PeerTexture;
		const auto Handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_strtoui64(Argv[3],nullptr,10)));
		if (FAILED(PeerDevice->OpenSharedResource(Handle,IID_PPV_ARGS(&PeerTexture)))) return 21;
		spoutFrameCount Peer;
		if (!Peer.CreateAccessMutex(Argv[2]) || !Peer.CheckTextureAccess(PeerTexture.Get())) return 22;
		HANDLE Ready = OpenEventA(EVENT_MODIFY_STATE,FALSE,Argv[4]);
		HANDLE Release = OpenEventA(SYNCHRONIZE,FALSE,Argv[5]);
		if (!Ready || !Release) return 23;
		SetEvent(Ready);
		const bool Released = WaitForSingleObject(Release,10000) == WAIT_OBJECT_0;
		Peer.AllowTextureAccess(PeerTexture.Get());
		CloseHandle(Ready); CloseHandle(Release);
		return Released ? 0 : 24;
	}
	if (Argc == 4 && std::strcmp(Argv[1], "hold-map-and-die") == 0)
	{
		SpoutSharedMemory Map;
		if (!Map.Open(Argv[2]) || !Map.Lock()) return 2;
		HANDLE Ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, Argv[3]);
		if (!Ready) return 3;
		SetEvent(Ready);
		TerminateProcess(GetCurrentProcess(), 0);
		return 4;
	}
	if (Argc == 4 && std::strcmp(Argv[1], "hold-and-die") == 0)
	{
		spoutFrameCount Peer;
		if (!Peer.CreateAccessMutex(Argv[2]) || !Peer.CheckAccess()) return 2;
		HANDLE Ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, Argv[3]);
		if (!Ready) return 3;
		SetEvent(Ready);
		// Deliberately bypass SDK destructors to model a foreign peer crash.
		TerminateProcess(GetCurrentProcess(), 0);
		return 4;
	}
	const std::string Prefix = "SpoutP0_" + std::to_string(GetCurrentProcessId()) + "_";
	ComPtr<ID3D11Device> Device;
	ComPtr<ID3D11DeviceContext> Context;
	Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
		D3D11_SDK_VERSION, &Device, nullptr, &Context)), "hardware D3D11 device");
	if (!Device) return 1;
	D3D11_TEXTURE2D_DESC Desc = {};
	Desc.Width = Desc.Height = 8; Desc.MipLevels = Desc.ArraySize = Desc.SampleDesc.Count = 1;
	Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	Desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
	ComPtr<ID3D11Texture2D> Texture;
	Check(SUCCEEDED(Device->CreateTexture2D(&Desc, nullptr, &Texture)), "real shared texture");
	if (!Texture) return 1;
	ComPtr<IDXGIResource> Resource;
	Texture.As(&Resource);
	HANDLE Shared = nullptr; Resource->GetSharedHandle(&Shared);
	for (bool Keyed : {false,true})
	{
		D3D11_TEXTURE2D_DESC HeldDesc = Desc;
		HeldDesc.MiscFlags = Keyed ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : D3D11_RESOURCE_MISC_SHARED;
		ComPtr<ID3D11Texture2D> HeldTexture;
		Check(SUCCEEDED(Device->CreateTexture2D(&HeldDesc,nullptr,&HeldTexture)), "contention texture created");
		if (!HeldTexture) continue;
		ComPtr<IDXGIResource> HeldResource; HeldTexture.As(&HeldResource);
		HANDLE HeldHandle = nullptr; HeldResource->GetSharedHandle(&HeldHandle);
		const std::string Stream = Prefix + (Keyed ? "keyed" : "named");
		const std::string ReadyName = Stream + "_ready", ReleaseName = Stream + "_release";
		HANDLE ReadyEvent = CreateEventA(nullptr,TRUE,FALSE,ReadyName.c_str());
		HANDLE ReleaseEvent = CreateEventA(nullptr,TRUE,FALSE,ReleaseName.c_str());
		char Exe[MAX_PATH] = {}; GetModuleFileNameA(nullptr,Exe,MAX_PATH);
		std::string Command = "\"" + std::string(Exe) + "\" hold-live " + Stream + " " + std::to_string(reinterpret_cast<uintptr_t>(HeldHandle)) + " " + ReadyName + " " + ReleaseName;
		STARTUPINFOA Startup = {}; Startup.cb=sizeof(Startup); PROCESS_INFORMATION Child = {};
		const bool Started = CreateProcessA(nullptr,Command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&Startup,&Child) != FALSE;
		Check(Started, "live peer started");
		if (Started)
		{
			const bool Ready = WaitForSingleObject(ReadyEvent,5000) == WAIT_OBJECT_0;
			Check(Ready, "foreign SDK peer holds texture");
			FSpoutSafeFrameCount Access; Access.CreateAccessMutex(Stream.c_str());
			if (Ready)
			{
				const auto Begin = std::chrono::steady_clock::now();
				bool AllRefused = true;
				for (int Attempt=0; Attempt<100; ++Attempt)
				{
					if (Access.CheckTextureAccessSafe(HeldTexture.Get())) { AllRefused=false; Access.AllowTextureAccess(HeldTexture.Get()); }
				}
				const auto Milliseconds = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-Begin).count();
				std::cout << (Keyed ? "keyed" : "named") << " 100 held attempts ms=" << Milliseconds << std::endl;
				Check(AllRefused, "busy peer never grants ownership or gets unlocked");
				Check(Milliseconds < 100.0, "100 held access attempts are nonblocking");
			}
			SetEvent(ReleaseEvent);
			if (WaitForSingleObject(Child.hProcess,5000) != WAIT_OBJECT_0) { TerminateProcess(Child.hProcess,25); Check(false,"owned test peer exited"); }
			DWORD Exit=1; GetExitCodeProcess(Child.hProcess,&Exit); Check(Exit==0,"foreign SDK releases cleanly");
			const bool Acquired = Access.CheckTextureAccessSafe(HeldTexture.Get());
			Check(Acquired,"access resumes after peer release");
			if (Acquired) Access.AllowTextureAccess(HeldTexture.Get());
			CloseHandle(Child.hThread); CloseHandle(Child.hProcess);
		}
		CloseHandle(ReadyEvent); CloseHandle(ReleaseEvent);
	}
	FSpoutSafeSenderNames Names;
	spoutSenderNames Foreign;
	for (size_t Length : {size_t(31), size_t(63), size_t(127), size_t(255)})
	{
		std::string Name = Prefix + std::string(Length - Prefix.size(), 'x');
		char Writable[SpoutMaxSenderNameLen] = {};
		std::memcpy(Writable, Name.c_str(), Name.size() + 1);
		Check(Foreign.CreateSender(Writable, 8, 8, Shared, Desc.Format), "foreign sender created");
		unsigned int BeforeW = 0, BeforeH = 0; HANDLE BeforeHandle = nullptr; DWORD BeforeFormat = 0;
		Check(Foreign.GetSenderInfo(Name.c_str(), BeforeW, BeforeH, BeforeHandle, BeforeFormat), "foreign metadata readable before collision");
		Check(!Names.CreateSenderExact(Name.c_str(), 16, 16, Shared, Desc.Format), "collision rejected without rewrite");
		unsigned int W = 0, H = 0; HANDLE Handle = nullptr; DWORD Format = 0;
		Check(Foreign.GetSenderInfo(Name.c_str(), W, H, Handle, Format) && W == BeforeW && H == BeforeH && Handle == BeforeHandle && Format == BeforeFormat,
			"foreign advertisement preserved");
		Check(!Foreign.FindSenderName((Name + "_1").c_str()), "no suffixed registration leaked");
		Foreign.ReleaseSenderName(Writable);
	}
	const std::string Name = Prefix + "owned";
	Check(Names.CreateSenderExact(Name.c_str(), 8, 8, Shared, Desc.Format), "exact name created");
	{
		FCapacityTestNames Full;
		Full.LimitTo(Names.GetSenderCount());
		Check(!Full.CreateSenderExact((Prefix + "full").c_str(), 8, 8, Shared, Desc.Format), "capacity refusal is false");
		Check(!Names.FindSenderName((Prefix + "full").c_str()), "capacity failure publishes nothing");
	}
	Names.ReleaseSenderName(Name.c_str());
	Check(!Names.FindSenderName(Name.c_str()), "owned sender removed");
	const std::string MutexName = Prefix + "abandoned";
	FSpoutSafeFrameCount Safe;
	Check(Safe.CreateAccessMutex(MutexName.c_str()), "access mutex created");
	const std::string EventName = Prefix + "ready";
	HANDLE Ready = CreateEventA(nullptr, TRUE, FALSE, EventName.c_str());
	char Exe[MAX_PATH] = {}; GetModuleFileNameA(nullptr, Exe, MAX_PATH);
	std::string Command = "\"" + std::string(Exe) + "\" hold-and-die " + MutexName + " " + EventName;
	STARTUPINFOA Startup = {}; Startup.cb = sizeof(Startup); PROCESS_INFORMATION Process = {};
	const bool Started = CreateProcessA(nullptr, &Command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
		nullptr, nullptr, &Startup, &Process) != FALSE;
	Check(Started, "separate crash peer launched");
	if (Started)
	{
		Check(WaitForSingleObject(Ready, 10000) == WAIT_OBJECT_0, "peer owned mutex before crash");
		Check(WaitForSingleObject(Process.hProcess, 10000) == WAIT_OBJECT_0, "peer exited while owning mutex");
		Check(!Safe.CheckTextureAccessSafe(nullptr), "abandoned frame discarded");
		bool OtherAcquired = false;
		std::thread Other([&] {
			spoutFrameCount Peer; Peer.CreateAccessMutex(MutexName.c_str());
			OtherAcquired = Peer.CheckAccess(); if (OtherAcquired) Peer.AllowAccess();
		}); Other.join();
		Check(OtherAcquired, "abandoned ownership released for another thread");
		Check(Safe.CheckTextureAccessSafe(nullptr), "next frame can recover");
		Safe.AllowAccess();
		CloseHandle(Process.hThread); CloseHandle(Process.hProcess);
	}
	CloseHandle(Ready);
	// A PRIVATE map exercises the registry lock adapter without abandoning or
	// corrupting the machine-wide sender registry used by other applications.
	const std::string MapName = Prefix + "private_map";
	SpoutSharedMemory Map;
	Check(Map.Create(MapName.c_str(), 256) != SPOUT_CREATE_FAILED, "private SDK map created");
	Ready = CreateEventA(nullptr, TRUE, FALSE, EventName.c_str());
	Command = "\"" + std::string(Exe) + "\" hold-map-and-die " + MapName + " " + EventName;
	Process = {};
	const bool MapPeerStarted = CreateProcessA(nullptr, &Command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
		nullptr, nullptr, &Startup, &Process) != FALSE;
	Check(MapPeerStarted, "private map crash peer launched");
	if (MapPeerStarted)
	{
		Check(WaitForSingleObject(Ready, 10000) == WAIT_OBJECT_0, "peer held private SDK map");
		Check(WaitForSingleObject(Process.hProcess, 10000) == WAIT_OBJECT_0, "private map peer exited");
		{ FSpoutSafeMemoryLock Lock(Map); Check(!Lock.Get(), "abandoned SDK map transaction discarded"); }
		bool OtherAcquired = false;
		std::thread Other([&] {
			SpoutSharedMemory Peer;
			OtherAcquired = Peer.Open(MapName.c_str()) && Peer.Lock();
			if (OtherAcquired) Peer.Unlock();
		}); Other.join();
		Check(OtherAcquired, "abandoned SDK map ownership released for another thread");
		{ FSpoutSafeMemoryLock Lock(Map); Check(Lock.Get() != nullptr, "next SDK map transaction recovers"); }
		CloseHandle(Process.hThread); CloseHandle(Process.hProcess);
	}
	CloseHandle(Ready);
	std::cout << "Failures: " << Failures << std::endl;
	return Failures ? 1 : 0;
}
