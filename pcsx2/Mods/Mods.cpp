// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Mods/Mods.h"

#include "Host.h"
#include "IconsFontAwesome.h"
#include "Memory.h"
#include "Netplay/Netplay.h"
#include "Recording/PadData.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

namespace Mods
{
	namespace
	{
		enum class State
		{
			Unchecked, // haven't looked at the environment yet
			Disabled,  // not requested, failed, or stopped after an error
			Active,
		};

		State s_state = State::Unchecked;
		ScriptRuntime s_runtime;
		u32 s_offline_frame = 0;

		// The game's main memory (32 MB). Addresses may use any of the PS2's mirrors of it
		// (0x0..., 0x2... uncached, 0x3... uncached accelerated); other areas aren't scriptable.
		class EmuHost final : public ScriptHost
		{
		public:
			bool ReadMemory(uint32_t addr, void* out, uint32_t size) override
			{
				const std::optional<u32> offset = MainRamOffset(addr, size);
				if (!offset)
					return false;
				std::memcpy(out, eeMem->Main + *offset, size);
				return true;
			}
			bool WriteMemory(uint32_t addr, const void* in, uint32_t size) override
			{
				const std::optional<u32> offset = MainRamOffset(addr, size);
				if (!offset)
					return false;
				std::memcpy(eeMem->Main + *offset, in, size);
				return true;
			}
			void Log(const char* message) override { Console.WriteLn(Color_StrongCyan, fmt::format("Game script: {}", message)); }
			void Notify(const char* message) override
			{
				Host::AddIconOSDMessage("N2ModNotify", ICON_FA_GAMEPAD, message, Host::OSD_INFO_DURATION);
			}

		private:
			static std::optional<u32> MainRamOffset(u32 addr, u32 size)
			{
				const u32 segment = addr & 0xF0000000u;
				if (segment != 0x00000000u && segment != 0x20000000u && segment != 0x30000000u)
					return std::nullopt;
				const u32 offset = addr & 0x0FFFFFFFu;
				if (offset >= Ps2MemSize::MainRam || size > Ps2MemSize::MainRam - offset)
					return std::nullopt;
				return offset;
			}
		};
		EmuHost s_host;

		void Stop(const std::string& reason)
		{
			s_runtime.Unload();
			s_state = State::Disabled;
			Console.Error(fmt::format("Game script stopped: {}", reason));
			Host::AddIconOSDMessage("N2ModStatus", ICON_FA_TRIANGLE_EXCLAMATION,
				fmt::format("The game script stopped: {}", reason), Host::OSD_ERROR_DURATION);
		}

		void EnsureLoaded()
		{
			if (s_state != State::Unchecked)
				return;
			s_state = State::Disabled;

			const char* path = std::getenv("PCSX2_N2MOD");
			const char* serial = std::getenv("PCSX2_N2MOD_SERIAL");
			if (!path || !*path)
				return;

			// A script is written for one exact game; never run it on anything else.
			const std::string running = VMManager::GetDiscSerial();
			if (!serial || running != serial)
			{
				Console.Error(fmt::format("Game script not started: it is for {}, but {} is running.",
					serial ? serial : "(unknown)", running.empty() ? "no disc" : running));
				return;
			}

			const std::optional<std::string> source = FileSystem::ReadFileToString(path);
			if (!source.has_value())
			{
				Console.Error(fmt::format("Game script not started: can't read {}", path));
				return;
			}

			std::string error;
			if (!s_runtime.Load(*source, Path::GetFileTitle(path).empty() ? "script" : std::string(Path::GetFileTitle(path)),
					&s_host, &error))
			{
				Stop(error);
				return;
			}
			s_offline_frame = 0;
			s_state = State::Active;
			Console.WriteLn(Color_StrongGreen, fmt::format("Game script started for {}.", running));
		}

		void Run(u32 frame, FramePads& pads, int num_players)
		{
			std::string error;
			if (!s_runtime.RunFrame(frame, &pads, num_players, &error))
				Stop(error);
		}

		PadBytes ReadPad(int port, int slot)
		{
			const PadData d(port, slot);
			PadBytes b;
			b[0] = d.m_compactPressFlagsGroupOne;
			b[1] = d.m_compactPressFlagsGroupTwo;
			b[2] = std::get<0>(d.m_rightAnalog);
			b[3] = std::get<1>(d.m_rightAnalog);
			b[4] = std::get<0>(d.m_leftAnalog);
			b[5] = std::get<1>(d.m_leftAnalog);
			b[6] = std::get<1>(d.m_right);
			b[7] = std::get<1>(d.m_left);
			b[8] = std::get<1>(d.m_up);
			b[9] = std::get<1>(d.m_down);
			b[10] = std::get<1>(d.m_triangle);
			b[11] = std::get<1>(d.m_circle);
			b[12] = std::get<1>(d.m_cross);
			b[13] = std::get<1>(d.m_square);
			b[14] = std::get<1>(d.m_l1);
			b[15] = std::get<1>(d.m_r1);
			b[16] = std::get<1>(d.m_l2);
			b[17] = std::get<1>(d.m_r2);
			return b;
		}
	} // namespace

	bool IsActive()
	{
		return s_state == State::Active;
	}

	void ProcessFrameInputs(u32 frame, FramePads& pads, int num_players)
	{
		EnsureLoaded();
		if (s_state == State::Active)
			Run(frame, pads, num_players);
	}

	void OnFrameOffline()
	{
		if (Netplay::IsActive())
			return;
		EnsureLoaded();
		if (s_state != State::Active)
			return;

		// Couch co-op on one PC: the controllers in ports 1 and 2.
		FramePads pads;
		for (PadBytes& p : pads)
		{
			p.fill(0);
			p[0] = p[1] = 0xFF;
			p[2] = p[3] = p[4] = p[5] = PadData::ANALOG_VECTOR_NEUTRAL;
		}
		pads[0] = ReadPad(0, 0);
		pads[1] = ReadPad(1, 0);
		const FramePads before = pads;
		Run(s_offline_frame++, pads, 2);
		if (s_state != State::Active)
			return;
		// Only write back controllers the script changed; the others keep their live input.
		for (int i = 0; i < 2; i++)
		{
			if (pads[i] != before[i])
				PadData(i, 0, pads[i]).OverrideActualController();
		}
	}

	void Shutdown()
	{
		s_runtime.Unload();
		// A launcher starts a fresh PCSX2 process per session, so scripts are not re-armed.
		if (s_state != State::Unchecked)
			s_state = State::Disabled;
	}
} // namespace Mods
