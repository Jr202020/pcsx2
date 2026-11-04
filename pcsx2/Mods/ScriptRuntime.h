// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Runs one N2Sphere game script (Lua) once per emulated frame.
//
// This file has no PCSX2 dependencies on purpose, so it can be unit tested on its own.
// The emulator side supplies memory access and messages through ScriptHost.
//
// What a script can do (see docs/mod-scripts.md in the N2Sphere repository):
//   - read and write the game's main memory (mem.read32, mem.write16, ...);
//   - read and change every player's controller input for the current frame (pad.get, pad.set);
//   - show a short message on screen (n2.notify) or write to the log (n2.log).
// It defines on_frame(frame) (called every frame) and optionally on_start() (called once).
//
// Determinism: in an online match every player's PC runs the same script on the same frames with
// the same inputs, so the results must be identical everywhere. The runtime therefore removes
// anything that could differ between PCs (clock, files, OS, random seeds) and limits how much
// work a script may do per frame, counted in Lua instructions (identical on every PC).

#include <array>
#include <cstdint>
#include <string>

struct lua_State;

namespace Mods
{
	// Same 18-byte controller layout as Netplay::InputData / PadData.
	using PadBytes = std::array<uint8_t, 18>;
	constexpr int MAX_PADS = 4;
	using FramePads = std::array<PadBytes, MAX_PADS>;

	class ScriptHost
	{
	public:
		virtual ~ScriptHost() = default;
		// Copy `size` bytes of game memory at `addr`. Return false if the address isn't valid.
		virtual bool ReadMemory(uint32_t addr, void* out, uint32_t size) = 0;
		virtual bool WriteMemory(uint32_t addr, const void* in, uint32_t size) = 0;
		virtual void Log(const char* message) = 0;
		virtual void Notify(const char* message) = 0;
	};

	class ScriptRuntime
	{
	public:
		// Lua instructions a script may run in one frame before it is stopped.
		static constexpr int INSTRUCTION_BUDGET = 5'000'000;

		ScriptRuntime();
		~ScriptRuntime();
		ScriptRuntime(const ScriptRuntime&) = delete;
		ScriptRuntime& operator=(const ScriptRuntime&) = delete;

		// Compiles the script (text only, never precompiled bytecode) and runs its top level.
		bool Load(const std::string& source, const std::string& name, ScriptHost* host, std::string* error);
		void Unload();
		bool IsLoaded() const { return m_L != nullptr; }

		// Runs on_start() (first call only) and on_frame(frame). `pads` holds this frame's controller
		// input for `num_pads` players and may be changed by the script. On error the script is
		// unloaded and false is returned.
		bool RunFrame(uint32_t frame, FramePads* pads, int num_pads, std::string* error);

		// Used by the Lua bindings.
		ScriptHost* Host() const { return m_host; }
		FramePads* Pads() const { return m_pads; }
		int NumPads() const { return m_num_pads; }
		uint32_t Frame() const { return m_frame; }
		int m_instructions_left = 0;

	private:
		bool Call(const char* function, bool required, std::string* error);

		lua_State* m_L = nullptr;
		ScriptHost* m_host = nullptr;
		FramePads* m_pads = nullptr;
		int m_num_pads = 0;
		uint32_t m_frame = 0;
		bool m_started = false;
	};
} // namespace Mods
