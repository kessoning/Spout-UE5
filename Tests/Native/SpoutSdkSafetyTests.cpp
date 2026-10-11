// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "SpoutSdkSafety.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;
static int Failures = 0;
static void Check(bool Value, const char* Message)
{
	std::cout << (Value ? "PASS " : "FAIL ") << Message << std::endl;
	Failures += !Value;
}
static double MsSince(std::chrono::steady_clock::time_point Begin)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Begin).count();
}
// Times Count attempts with the given wait against a texture a live peer holds. Returns the
// per-attempt durations; AllRefused reports whether ownership was never granted.
static std::vector<double> TimeHeldAttempts(FSpoutSafeFrameCount& Access, ID3D11Texture2D* Texture, DWORD TimeoutMs, int Count, bool& AllRefused)
{
	std::vector<double> Waits;
	AllRefused = true;
	for (int Attempt = 0; Attempt < Count; ++Attempt)
	{
		const auto Begin = std::chrono::steady_clock::now();
		const bool Granted = Access.CheckTextureAccessSafe(Texture, TimeoutMs);
		Waits.push_back(MsSince(Begin));
		if (Granted) { AllRefused = false; Access.AllowTextureAccess(Texture); }
	}
	std::sort(Waits.begin(), Waits.end());
	return Waits;
}
static void PrintWaits(const char* Label, const std::vector<double>& Waits)
{
	if (Waits.empty()) return;
	std::cout << Label << " attempts=" << Waits.size() << " minMs=" << Waits.front()
		<< " medianMs=" << Waits[Waits.size() / 2] << " maxMs=" << Waits.back() << std::endl;
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
	if ((Argc == 4 || Argc == 5) && std::strcmp(Argv[1], "hold-and-die") == 0)
	{
		spoutFrameCount Peer;
		if (!Peer.CreateAccessMutex(Argv[2]) || !Peer.CheckAccess()) return 2;
		HANDLE Ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, Argv[3]);
		if (!Ready) return 3;
		SetEvent(Ready);
		// Optional: keep holding for a few milliseconds first, so the crash lands while the
		// parent's bounded attempt is already waiting.
		if (Argc == 5) Sleep(static_cast<DWORD>(std::strtoul(Argv[4], nullptr, 10)));
		// Deliberately bypass SDK destructors to model a foreign peer crash.
		TerminateProcess(GetCurrentProcess(), 0);
		return 4;
	}
	if (Argc == 4 && std::strcmp(Argv[1], "hold-keyed-and-die") == 0)
	{
		ComPtr<ID3D11Device> PeerDevice;
		if (FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&PeerDevice,nullptr,nullptr))) return 20;
		ComPtr<ID3D11Texture2D> PeerTexture;
		const auto Handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_strtoui64(Argv[2],nullptr,10)));
		if (FAILED(PeerDevice->OpenSharedResource(Handle,IID_PPV_ARGS(&PeerTexture)))) return 21;
		ComPtr<IDXGIKeyedMutex> Keyed;
		if (FAILED(PeerTexture.As(&Keyed)) || Keyed->AcquireSync(0, 1000) != S_OK) return 22;
		HANDLE Ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, Argv[3]);
		if (!Ready) return 23;
		SetEvent(Ready);
		TerminateProcess(GetCurrentProcess(), 0);
		return 24;
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
			const char* Kind = Keyed ? "keyed" : "named";
			if (Ready)
			{
				// Zero wait (r.Spout.AccessWaitMs=0): only tries.
				const auto Begin = std::chrono::steady_clock::now();
				bool AllRefused = true;
				for (int Attempt=0; Attempt<100; ++Attempt)
				{
					if (Access.CheckTextureAccessSafe(HeldTexture.Get(), 0)) { AllRefused=false; Access.AllowTextureAccess(HeldTexture.Get()); }
				}
				const auto Milliseconds = MsSince(Begin);
				std::cout << Kind << " 100 held attempts ms=" << Milliseconds << std::endl;
				Check(AllRefused, "busy peer never grants ownership or gets unlocked");
				Check(Milliseconds < 100.0, "100 held access attempts with a zero wait are nonblocking");

				// Bounded wait (the 2 ms default): every attempt times out while the peer holds on,
				// and each one ends near its bound. Measured at this process's default timer
				// resolution and again at 1 ms, which Unreal requests at startup.
				bool BoundedRefused = true;
				const std::vector<double> Default = TimeHeldAttempts(Access, HeldTexture.Get(), 2, 20, BoundedRefused);
				PrintWaits(Keyed ? "keyed 2ms default-timer" : "named 2ms default-timer", Default);
				bool FineRefused = true;
				timeBeginPeriod(1);
				const std::vector<double> Fine = TimeHeldAttempts(Access, HeldTexture.Get(), 2, 20, FineRefused);
				timeEndPeriod(1);
				PrintWaits(Keyed ? "keyed 2ms 1ms-timer" : "named 2ms 1ms-timer", Fine);
				Check(BoundedRefused && FineRefused, "busy peer: bounded attempts time out without granting ownership");
				Check(Default[Default.size() / 2] >= 0.25 && Fine[Fine.size() / 2] >= 0.25, "bounded attempts actually wait for the peer");
				Check(Default.back() < 100.0 && Fine.back() < 100.0, "bounded attempts end within 100 ms each");

				// No caller can ask for an unbounded wait: INFINITE is clamped to the maximum.
				const auto InfiniteBegin = std::chrono::steady_clock::now();
				const bool InfiniteGranted = Access.CheckTextureAccessSafe(HeldTexture.Get(), INFINITE);
				const double InfiniteMs = MsSince(InfiniteBegin);
				if (InfiniteGranted) Access.AllowTextureAccess(HeldTexture.Get());
				std::cout << Kind << " INFINITE request ms=" << InfiniteMs << std::endl;
				Check(!InfiniteGranted && InfiniteMs < 200.0, "an INFINITE request is clamped to the bounded maximum");

				// Reference only: how long the SDK's own check waits on a busy peer, which is what
				// other Spout applications use.
				const auto SdkBegin = std::chrono::steady_clock::now();
				const bool SdkGranted = Access.CheckTextureAccess(HeldTexture.Get());
				std::cout << Kind << " SDK CheckTextureAccess on a busy peer ms=" << MsSince(SdkBegin) << " granted=" << SdkGranted << std::endl;
				if (SdkGranted) Access.AllowTextureAccess(HeldTexture.Get());
			}
			// A peer that lets go while a bounded attempt is waiting grants that attempt: a short
			// hold does not cost the frame.
			SetEvent(ReleaseEvent);
			const auto ReleaseBegin = std::chrono::steady_clock::now();
			const bool GrantedInWait = Ready && Access.CheckTextureAccessSafe(HeldTexture.Get(), FSpoutSafeFrameCount::MaxAccessWaitMs);
			const double ReleaseMs = MsSince(ReleaseBegin);
			std::cout << Kind << " release during a 10 ms attempt ms=" << ReleaseMs << " granted=" << GrantedInWait << std::endl;
			if (GrantedInWait) Access.AllowTextureAccess(HeldTexture.Get());
			if (Ready) Check(GrantedInWait, "a peer that releases during the wait grants access within the bound");
			if (WaitForSingleObject(Child.hProcess,5000) != WAIT_OBJECT_0) { TerminateProcess(Child.hProcess,25); Check(false,"owned test peer exited"); }
			DWORD Exit=1; GetExitCodeProcess(Child.hProcess,&Exit); Check(Exit==0,"foreign SDK releases cleanly");
			const bool Acquired = Access.CheckTextureAccessSafe(HeldTexture.Get(), 0);
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
	HANDLE Ready = nullptr;
	char Exe[MAX_PATH] = {}; GetModuleFileNameA(nullptr, Exe, MAX_PATH);
	std::string Command;
	STARTUPINFOA Startup = {}; Startup.cb = sizeof(Startup); PROCESS_INFORMATION Process = {};
	// Once with a zero wait and once with the 2 ms default: abandonment is handled the same way.
	for (DWORD TimeoutMs : {DWORD(0), DWORD(2)})
	{
		const std::string Suffix = " (wait " + std::to_string(TimeoutMs) + " ms)";
		Ready = CreateEventA(nullptr, TRUE, FALSE, EventName.c_str()); if (Ready) ResetEvent(Ready);
		Command = "\"" + std::string(Exe) + "\" hold-and-die " + MutexName + " " + EventName;
		Process = {};
		const bool Started = CreateProcessA(nullptr, &Command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
			nullptr, nullptr, &Startup, &Process) != FALSE;
		Check(Started, ("separate crash peer launched" + Suffix).c_str());
		if (Started)
		{
			Check(WaitForSingleObject(Ready, 10000) == WAIT_OBJECT_0, ("peer owned mutex before crash" + Suffix).c_str());
			Check(WaitForSingleObject(Process.hProcess, 10000) == WAIT_OBJECT_0, ("peer exited while owning mutex" + Suffix).c_str());
			Check(!Safe.CheckTextureAccessSafe(nullptr, TimeoutMs), ("abandoned frame discarded" + Suffix).c_str());
			bool OtherAcquired = false;
			std::thread Other([&] {
				spoutFrameCount Peer; Peer.CreateAccessMutex(MutexName.c_str());
				OtherAcquired = Peer.CheckAccess(); if (OtherAcquired) Peer.AllowAccess();
			}); Other.join();
			Check(OtherAcquired, ("abandoned ownership released for another thread" + Suffix).c_str());
			Check(Safe.CheckTextureAccessSafe(nullptr, TimeoutMs), ("next frame can recover" + Suffix).c_str());
			Safe.AllowAccess();
			CloseHandle(Process.hThread); CloseHandle(Process.hProcess);
		}
		CloseHandle(Ready);
	}
	// A peer that crashes while a bounded attempt is already waiting: no attempt outlives its
	// bound, and access recovers within a few attempts whether the crash landed inside the wait
	// (WAIT_ABANDONED, released at once) or just after it (the next attempt sees it).
	{
		Ready = CreateEventA(nullptr, TRUE, FALSE, EventName.c_str()); if (Ready) ResetEvent(Ready);
		Command = "\"" + std::string(Exe) + "\" hold-and-die " + MutexName + " " + EventName + " 2";
		Process = {};
		const bool Started = CreateProcessA(nullptr, &Command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
			nullptr, nullptr, &Startup, &Process) != FALSE;
		Check(Started, "crash-during-wait peer launched");
		if (Started && WaitForSingleObject(Ready, 10000) == WAIT_OBJECT_0)
		{
			double LongestMs = 0.0;
			int Attempts = 0;
			bool Recovered = false;
			while (!Recovered && Attempts < 5)
			{
				const auto Begin = std::chrono::steady_clock::now();
				Recovered = Safe.CheckTextureAccessSafe(nullptr, FSpoutSafeFrameCount::MaxAccessWaitMs);
				LongestMs = (std::max)(LongestMs, MsSince(Begin));
				++Attempts;
				if (!Recovered) WaitForSingleObject(Process.hProcess, 1000);
			}
			std::cout << "crash during a 10 ms attempt: attempts=" << Attempts << " longestMs=" << LongestMs << std::endl;
			Check(Recovered && Attempts >= 2, "a peer crash during a bounded wait is discarded, then access recovers");
			Check(LongestMs < 200.0, "no attempt outlives its bound when the peer crashes");
			if (Recovered) Safe.AllowAccess();
		}
		if (Started) { WaitForSingleObject(Process.hProcess, 5000); CloseHandle(Process.hThread); CloseHandle(Process.hProcess); }
		CloseHandle(Ready);
	}
	// Keyed texture whose holder crashes: attempts stay bounded. Recovery of an abandoned keyed
	// mutex is the D3D runtime's decision (it may keep reporting WAIT_ABANDONED until the sender
	// recreates the texture), so it is reported rather than asserted.
	{
		D3D11_TEXTURE2D_DESC KeyedDesc = Desc;
		KeyedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
		ComPtr<ID3D11Texture2D> KeyedTexture;
		Check(SUCCEEDED(Device->CreateTexture2D(&KeyedDesc, nullptr, &KeyedTexture)), "keyed crash texture created");
		ComPtr<IDXGIResource> KeyedResource;
		HANDLE KeyedHandle = nullptr;
		if (KeyedTexture && SUCCEEDED(KeyedTexture.As(&KeyedResource))) KeyedResource->GetSharedHandle(&KeyedHandle);
		Ready = CreateEventA(nullptr, TRUE, FALSE, EventName.c_str()); if (Ready) ResetEvent(Ready);
		Command = "\"" + std::string(Exe) + "\" hold-keyed-and-die " + std::to_string(reinterpret_cast<uintptr_t>(KeyedHandle)) + " " + EventName;
		Process = {};
		const bool Started = KeyedHandle && CreateProcessA(nullptr, &Command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
			nullptr, nullptr, &Startup, &Process) != FALSE;
		Check(Started, "keyed crash peer launched");
		if (Started)
		{
			Check(WaitForSingleObject(Ready, 10000) == WAIT_OBJECT_0, "keyed peer owned the texture before crash");
			Check(WaitForSingleObject(Process.hProcess, 10000) == WAIT_OBJECT_0, "keyed peer exited while owning the texture");
			double LongestMs = 0.0;
			int GrantedAt = -1;
			for (int Attempt = 0; Attempt < 5; ++Attempt)
			{
				const auto Begin = std::chrono::steady_clock::now();
				const bool Granted = Safe.CheckTextureAccessSafe(KeyedTexture.Get(), 2);
				LongestMs = (std::max)(LongestMs, MsSince(Begin));
				if (Granted) { Safe.AllowTextureAccess(KeyedTexture.Get()); if (GrantedAt < 0) GrantedAt = Attempt; }
			}
			std::cout << "keyed crash: firstGrantedAttempt=" << GrantedAt << " longestMs=" << LongestMs << std::endl;
			Check(LongestMs < 200.0, "keyed attempts stay bounded after the holder crashed");
			CloseHandle(Process.hThread); CloseHandle(Process.hProcess);
		}
		CloseHandle(Ready);
	}
	// A PRIVATE map exercises the registry lock adapter without abandoning or
	// corrupting the machine-wide sender registry used by other applications.
	const std::string MapName = Prefix + "private_map";
	SpoutSharedMemory Map;
	Check(Map.Create(MapName.c_str(), 256) != SPOUT_CREATE_FAILED, "private SDK map created");
	Ready = CreateEventA(nullptr, TRUE, FALSE, EventName.c_str()); if (Ready) ResetEvent(Ready);
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
