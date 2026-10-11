// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
#pragma once

// Native-only adapter: it uses no Unreal types, only Windows and the bundled SDK.
// Keep the bundled headers/library/DLL together: these SDK classes expose a C++ ABI.
#include "SpoutDX/SpoutSenderNames.h"
#include "SpoutDX/SpoutFrameCount.h"
#include <cstring>

// The bundled SpoutSharedMemory::Lock also returns null after WAIT_ABANDONED
// without releasing Windows ownership. Acquire its actual named mutex first,
// then let the SDK take a balanced recursive acquisition on the successful path.
class FSpoutSafeMemoryLock
{
public:
	explicit FSpoutSafeMemoryLock(SpoutSharedMemory& InMap) : Map(InMap)
	{
		const char* Name = Map.Name();
		if (!Name) return;
		const size_t Length = std::strlen(Name);
		char MutexName[512] = {};
		if (Length + sizeof("_mutex") > sizeof(MutexName)) return;
		std::memcpy(MutexName, Name, Length);
		std::memcpy(MutexName + Length, "_mutex", sizeof("_mutex"));
		Mutex = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, MutexName);
		if (!Mutex) return;
		const DWORD Result = WaitForSingleObject(Mutex, 0);
		if (Result == WAIT_ABANDONED)
		{
			ReleaseMutex(Mutex);
			return;
		}
		if (Result != WAIT_OBJECT_0) return;
		bOwned = true;
		Buffer = Map.Lock();
	}
	~FSpoutSafeMemoryLock()
	{
		if (Buffer) Map.Unlock();
		if (bOwned) ReleaseMutex(Mutex);
		if (Mutex) CloseHandle(Mutex);
	}
	FSpoutSafeMemoryLock(const FSpoutSafeMemoryLock&) = delete;
	FSpoutSafeMemoryLock& operator=(const FSpoutSafeMemoryLock&) = delete;
	char* Get() const { return Buffer; }
private:
	SpoutSharedMemory& Map;
	HANDLE Mutex = nullptr;
	char* Buffer = nullptr;
	bool bOwned = false;
};

class FSpoutSafeSenderNames : public spoutSenderNames
{
public:
	bool CreateSenderExact(const char* Name, unsigned int Width, unsigned int Height, HANDLE Handle, DWORD Format)
	{
		if (!Name || !Name[0] || std::strlen(Name) >= SpoutMaxSenderNameLen || !Width || !Height || !Handle)
			return false;
		if (!CreateSenderSet())
			return false;
		FSpoutSafeMemoryLock Lock(m_senderNames);
		char* Buffer = Lock.Get();
		if (!Buffer)
			return false;

		// The SDK's CreateSender ignores registration failure and may rewrite Name.
		// Hold the SAME cross-process map lock as foreign SDK senders across checking,
		// metadata creation and registration. A process-local preflight is insufficient.
		const int Capacity = (m_senderNames.Size() / SpoutMaxSenderNameLen < m_MaxSenders)
			? m_senderNames.Size() / SpoutMaxSenderNameLen : m_MaxSenders;
		// Never ask the DLL to populate a caller-owned STL container: its CRT
		// allocations cannot be freed by Unreal's module operator delete. The
		// SDK registry is a bounded array of 256-byte, zero-terminated name slots.
		const size_t NameLength = std::strlen(Name);
		int Used = 0;
		for (int Index = 0; Index < Capacity; ++Index)
		{
			const char* Slot = Buffer + Index * SpoutMaxSenderNameLen;
			if (!Slot[0]) continue;
			const char* End = static_cast<const char*>(std::memchr(Slot, 0, SpoutMaxSenderNameLen));
			if (!End) return false;
			++Used;
			if (static_cast<size_t>(End - Slot) == NameLength && std::memcmp(Slot, Name, NameLength) == 0)
				return false;
		}
		if (Capacity <= 0 || Used >= Capacity)
			return false;
		char WritableName[SpoutMaxSenderNameLen] = {};
		std::memcpy(WritableName, Name, std::strlen(Name) + 1);
		if (!UpdateSender(Name, Width, Height, Handle, Format) || !RegisterSenderName(WritableName, false))
		{
			// No other sender could acquire this name while the map lock was held.
			ReleaseSenderName(Name);
			return false;
		}
		return true;
	}
};

class FSpoutSafeFrameCount : public spoutFrameCount
{
public:
	/** Upper bound on one access attempt's wait. Callers clamp too; this keeps INFINITE out. */
	static constexpr DWORD MaxAccessWaitMs = 10;

	// Waits at most TimeoutMs (clamped to MaxAccessWaitMs) for a peer to release the texture;
	// zero only tries. The wait ends as soon as access is granted. Windows scheduling can make
	// the elapsed time exceed TimeoutMs.
	bool CheckTextureAccessSafe(ID3D11Texture2D* Texture, DWORD TimeoutMs)
	{
		if (TimeoutMs > MaxAccessWaitMs) TimeoutMs = MaxAccessWaitMs;
		if (Texture && IsKeyedMutex(Texture))
		{
			IDXGIKeyedMutex* Keyed = nullptr;
			if (FAILED(Texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&Keyed))) || !Keyed) return false;
			// Spout uses key zero for both acquire and release. WAIT_TIMEOUT is a
			// nonnegative HRESULT, so SUCCEEDED would incorrectly grant ownership.
			// The bound is spent polling zero-timeout acquires rather than passed to AcquireSync:
			// on the plugin's D3D11-on-12 device (which shares the engine's graphics queue) a timed
			// keyed acquire can deliver fewer frames than zero-timeout attempts while a peer holds
			// the texture for long periods.
			HRESULT Result = Keyed->AcquireSync(0, 0);
			if (Result == static_cast<HRESULT>(WAIT_TIMEOUT) && TimeoutMs > 0)
			{
				LARGE_INTEGER Frequency, Now, Deadline;
				QueryPerformanceFrequency(&Frequency);
				QueryPerformanceCounter(&Now);
				Deadline.QuadPart = Now.QuadPart + Frequency.QuadPart * static_cast<LONGLONG>(TimeoutMs) / 1000;
				const LONGLONG PollInterval = Frequency.QuadPart / 20000; // about 50 microseconds
				while (Result == static_cast<HRESULT>(WAIT_TIMEOUT) && Now.QuadPart < Deadline.QuadPart)
				{
					const LONGLONG NextPoll = Now.QuadPart + PollInterval;
					SwitchToThread();
					do { YieldProcessor(); QueryPerformanceCounter(&Now); } while (Now.QuadPart < NextPoll && Now.QuadPart < Deadline.QuadPart);
					Result = Keyed->AcquireSync(0, 0);
					QueryPerformanceCounter(&Now);
				}
			}
			Keyed->Release();
			return Result == S_OK;
		}
		if (!m_hAccessMutex)
			return false;
		const DWORD Result = WaitForSingleObject(m_hAccessMutex, TimeoutMs);
		if (Result == WAIT_ABANDONED)
		{
			// Windows granted ownership, unlike a timeout. Discard this inconsistent
			// frame, but release on this thread so the next attempt can recover.
			ReleaseMutex(m_hAccessMutex);
			return false;
		}
		return Result == WAIT_OBJECT_0;
	}
};
