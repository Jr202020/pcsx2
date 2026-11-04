// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "Mods/ScriptRuntime.h"

// N2Sphere game scripts: one Lua script per game that runs every frame and can read/write the
// game's memory and the players' controller input (used for co-op mods, for example adding a
// second player to a single-player game).
//
// Activated only when the launcher sets these environment variables when it starts PCSX2:
//   PCSX2_N2MOD=<path to the game's .lua script>
//   PCSX2_N2MOD_SERIAL=<game serial the script was written for, e.g. SLUS-21269>
// The launcher only passes scripts it has checked the signature of (official N2Sphere scripts).
// Without those variables none of this code does anything.
//
// Online, the script runs inside the lockstep frame (same frame, same inputs on every PC); offline
// it runs once per frame on the local controllers in ports 1 and 2 (couch co-op on one PC).
namespace Mods
{
	// True while a script is loaded and running.
	bool IsActive();

	// Called by netplay every frame, after all players' inputs are known and before they are
	// written to the console's controllers. The script may change them.
	void ProcessFrameInputs(u32 frame, FramePads& pads, int num_players);

	// Called once per frame from VMManager::Internal::PollInputOnCPUThread. Does nothing during
	// an online match (netplay calls ProcessFrameInputs instead).
	void OnFrameOffline();

	// Called when the virtual machine shuts down.
	void Shutdown();
} // namespace Mods
