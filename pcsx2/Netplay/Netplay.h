// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// Online play for 2 to 4 players (delay-based lockstep), driven by an external launcher.
//
// Activated only when the environment variable PCSX2_NETPLAY is set when PCSX2 starts, e.g.
//   PCSX2_NETPLAY=port=50123;player=1;delay=3[;players=4;pads=0.0,0.1,0.2,0.3]
// where `port` is the launcher's localhost UDP bridge, `player` is this player's number,
// `delay` is the input delay in frames, `players` is 2-4 and `pads` the console port.slot for each
// player (slots 1-3 are Multitap slots; the launcher enables the Multitap in the settings).
// Without that variable none of this code does anything.
//
// While active:
//  - The local player's physical controller (bound to Pad 1) feeds a private "shadow" pad
//    instead of the console's pads.
//  - Every frame, all players' inputs are exchanged and written into their console ports.
namespace Netplay
{
	// True while an online match is running.
	bool IsActive();

	// Called from Pad::SetControllerState. Returns true if netplay consumed the input.
	bool HandleControllerState(u32 controller, u32 bind, float value);

	// Called once per frame from VMManager::Internal::PollInputOnCPUThread,
	// after host input has been polled.
	void OnFrame();

	// Called when the virtual machine shuts down.
	void Shutdown();
} // namespace Netplay
