// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

// Release smoke for the placeable Spout Sender / Spout Receiver actors in a packaged game (test-only, -SpoutActorsSmoke).
#pragma once

#include "CoreMinimal.h"

namespace SpoutHostTest
{
	class FActorsSmoke
	{
	public:
		static bool IsRequested();
		static void Start();
		static void Stop();
	};
}
