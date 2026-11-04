// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Netplay/Netplay.h"
#include "Netplay/LockstepSession.h"

#include "Host.h"
#include "IconsFontAwesome.h"
#include "Memory.h"
#include "Mods/Mods.h"
#include "Recording/PadData.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace Netplay
{
	namespace
	{
		constexpr u32 CHECKSUM_INTERVAL = 300; // frames (~5 seconds)
		constexpr int FRAME_TIMEOUT_MS = 20000; // give up if the opponent sends nothing for this long

		enum class State
		{
			Unchecked,  // haven't looked at the environment yet
			Disabled,   // not requested, or ended
			Active,
		};

		State s_state = State::Unchecked;
		LockstepSession s_session;
		// Console controller (port, slot) for each player. Players 3-4 sit in Multitap slots.
		std::array<std::pair<u32, u32>, MAX_PLAYERS> s_pad_slots = {{{0, 0}, {1, 0}, {0, 2}, {0, 3}}};
		std::unique_ptr<PadDualshock2> s_shadow_pad; // local physical controller lands here
		bool s_desync_warned = false;

		void EndMatch(std::string_view reason)
		{
			s_session.Stop();
			s_state = State::Disabled;
			s_shadow_pad.reset();
			Console.WriteLn(Color_StrongYellow, fmt::format("Netplay: {}", reason));
			Host::AddIconOSDMessage("NetplayStatus", ICON_FA_PLUG, std::string(reason), Host::OSD_ERROR_DURATION);
		}

		// Parses "port=50123;player=1;delay=3;players=4;pads=0.0,0.1,0.2,0.3".
		// players: 2 (default) to 4. pads: console port.slot for players 1..N; slots 1-3 are
		// Multitap slots B-D. Default: ports 1 and 2 for two players, Multitap on port 1 for more.
		bool ParseEnvironment(LockstepSession::Options* opts)
		{
			const char* env = std::getenv("PCSX2_NETPLAY");
			if (!env || !*env)
				return false;

			int port = 0, player = 0, delay = 3, players = 2;
			std::string_view pads;
			for (const std::string_view part : StringUtil::SplitString(env, ';'))
			{
				const size_t eq = part.find('=');
				if (eq == std::string_view::npos)
					continue;
				const std::string_view key = part.substr(0, eq);
				const std::string_view text = part.substr(eq + 1);
				const int value = StringUtil::FromChars<int>(text).value_or(-1);
				if (key == "port")
					port = value;
				else if (key == "player")
					player = value;
				else if (key == "delay")
					delay = value;
				else if (key == "players")
					players = value;
				else if (key == "pads")
					pads = text;
			}
			if (port <= 0 || port > 65535 || players < 2 || players > MAX_PLAYERS || player < 1 || player > players)
			{
				Console.Error(fmt::format("Netplay: invalid PCSX2_NETPLAY value '{}'", env));
				return false;
			}

			s_pad_slots = (players == 2) ? std::array<std::pair<u32, u32>, MAX_PLAYERS>{{{0, 0}, {1, 0}, {0, 2}, {0, 3}}} :
										   std::array<std::pair<u32, u32>, MAX_PLAYERS>{{{0, 0}, {0, 1}, {0, 2}, {0, 3}}};
			if (!pads.empty())
			{
				int i = 0;
				for (const std::string_view entry : StringUtil::SplitString(pads, ','))
				{
					const size_t dot = entry.find('.');
					if (i >= MAX_PLAYERS || dot == std::string_view::npos)
						break;
					const int p = StringUtil::FromChars<int>(entry.substr(0, dot)).value_or(-1);
					const int sl = StringUtil::FromChars<int>(entry.substr(dot + 1)).value_or(-1);
					if (p < 0 || p > 1 || sl < 0 || sl > 3)
					{
						Console.Error(fmt::format("Netplay: invalid pads entry '{}'", entry));
						return false;
					}
					s_pad_slots[i++] = {static_cast<u32>(p), static_cast<u32>(sl)};
				}
			}

			opts->bridge_port = static_cast<u16>(port);
			opts->num_players = players;
			opts->local_player = player - 1;
			opts->input_delay = delay;
			return true;
		}

		void EnsureInitialized()
		{
			if (s_state != State::Unchecked)
				return;
			s_state = State::Disabled;

			LockstepSession::Options opts;
			if (!ParseEnvironment(&opts))
				return;
			opts.frame_timeout_ms = FRAME_TIMEOUT_MS;

			std::string error;
			if (!s_session.Start(opts, &error))
			{
				Console.Error(fmt::format("Netplay: failed to start: {}", error));
				return;
			}

			s_shadow_pad = std::make_unique<PadDualshock2>(0, 0);
			s_shadow_pad->SetAxisScale(Pad::DEFAULT_STICK_DEADZONE, Pad::DEFAULT_STICK_SCALE);
			s_shadow_pad->SetDiagonalScaleCorrection(Pad::DEFAULT_USE_DIAGONAL_SCALE_CORRECTION);
			s_shadow_pad->SetButtonDeadzone(Pad::DEFAULT_BUTTON_DEADZONE);
			s_desync_warned = false;
			s_state = State::Active;

			Console.WriteLn(Color_StrongGreen, fmt::format("Netplay: started as player {} of {} with {} frame(s) input delay",
												   opts.local_player + 1, opts.num_players, opts.input_delay));
			Host::AddIconOSDMessage("NetplayStatus", ICON_FA_WIFI,
				fmt::format("Online match: you are Player {}. Waiting for the other {}...", opts.local_player + 1,
					opts.num_players == 2 ? "player" : "players"),
				Host::OSD_INFO_DURATION);
		}

		// Same byte layout PadData(port, slot, std::array<u8, 18>) expects.
		InputData CaptureShadowPad()
		{
			using In = PadDualshock2::Inputs;
			const PadDualshock2& pad = *s_shadow_pad;
			const u32 buttons = pad.GetButtons();
			const auto [lx, ly] = pad.GetRawLeftAnalog();
			const auto [rx, ry] = pad.GetRawRightAnalog();
			InputData d;
			d[0] = static_cast<u8>((buttons & 0xFF00) >> 8);
			d[1] = static_cast<u8>(buttons & 0xFF);
			d[2] = rx;
			d[3] = ry;
			d[4] = lx;
			d[5] = ly;
			d[6] = pad.GetRawInput(In::PAD_RIGHT);
			d[7] = pad.GetRawInput(In::PAD_LEFT);
			d[8] = pad.GetRawInput(In::PAD_UP);
			d[9] = pad.GetRawInput(In::PAD_DOWN);
			d[10] = pad.GetRawInput(In::PAD_TRIANGLE);
			d[11] = pad.GetRawInput(In::PAD_CIRCLE);
			d[12] = pad.GetRawInput(In::PAD_CROSS);
			d[13] = pad.GetRawInput(In::PAD_SQUARE);
			d[14] = pad.GetRawInput(In::PAD_L1);
			d[15] = pad.GetRawInput(In::PAD_R1);
			d[16] = pad.GetRawInput(In::PAD_L2);
			d[17] = pad.GetRawInput(In::PAD_R2);
			return d;
		}

		// Cheap fingerprint of main RAM (every 64th word). Identical on both sides if in sync.
		u32 HashMainRam()
		{
			u32 hash = 2166136261u;
			const u32* ram = reinterpret_cast<const u32*>(eeMem->Main);
			for (u32 i = 0; i < Ps2MemSize::MainRam / 4; i += 16)
				hash = (hash ^ ram[i]) * 16777619u;
			return hash;
		}
	} // namespace

	bool IsActive()
	{
		return s_state == State::Active;
	}

	bool HandleControllerState(u32 controller, u32 bind, float value)
	{
		EnsureInitialized();
		if (s_state != State::Active)
			return false;
		// Physical input never reaches the console's pads directly during a match.
		if (controller == 0)
			s_shadow_pad->Set(bind, value);
		return true;
	}

	void OnFrame()
	{
		EnsureInitialized();
		if (s_state != State::Active)
			return;

		const u32 frame = s_session.CurrentFrame();
		if (frame > 0 && frame % CHECKSUM_INTERVAL == 0)
			s_session.ReportChecksum(frame, HashMainRam());

		// While waiting for the opponent's input (they lag, crashed, or closed the game), keep the
		// window responsive and show how long until the match is given up, instead of a silent freeze.
		const auto stall_start = std::chrono::steady_clock::now();
		int shown_seconds = -1;
		bool showed_wait = false;
		FrameInputs inputs;
		const LockstepSession::Result result = s_session.AdvanceFrame(CaptureShadowPad(), &inputs,
			[&stall_start, &shown_seconds, &showed_wait, frame]() {
				Host::PumpMessagesOnCPUThread();
				const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - stall_start).count();
				const int seconds = static_cast<int>(waited / 1000);
				if (seconds >= 1 && seconds != shown_seconds)
				{
					shown_seconds = seconds;
					showed_wait = true;
					const int lagging = s_session.LaggingPlayer();
					const std::string who = (s_session.NumPlayers() == 2 || lagging < 0) ?
						std::string("Opponent") : fmt::format("Player {}", lagging + 1);
					const std::string message = (frame == 0) ?
						std::string("Waiting for the other players' games to start...") :
						fmt::format("{} not responding... ending the match in {}s", who,
							(std::max)(0, FRAME_TIMEOUT_MS / 1000 - seconds));
					Host::AddIconOSDMessage("NetplayWaiting", ICON_FA_WIFI, message, 1.5f);
				}
				return VMManager::GetState() != VMState::Stopping;
			});
		if (showed_wait)
			Host::RemoveKeyedOSDMessage("NetplayWaiting");

		switch (result)
		{
			case LockstepSession::Result::Ok:
				break;
			case LockstepSession::Result::Disconnected:
				EndMatch(s_session.NumPlayers() == 2 ? std::string("Online match ended: your opponent left.") :
						  fmt::format("Online match ended: Player {} left.", s_session.QuitPlayer() + 1));
				return;
			case LockstepSession::Result::TimedOut:
				EndMatch(s_session.NumPlayers() == 2 ? std::string("Online match ended: lost connection to your opponent.") :
						  fmt::format("Online match ended: lost connection to Player {}.", s_session.LaggingPlayer() + 1));
				return;
			case LockstepSession::Result::Aborted:
				EndMatch("Online match ended.");
				return;
		}

		if (frame == static_cast<u32>(s_session.InputDelay()))
		{
			Host::AddIconOSDMessage("NetplayStatus", ICON_FA_WIFI,
				fmt::format("Connected! You are Player {}.", s_session.LocalPlayer() + 1), Host::OSD_QUICK_DURATION);
		}

		// A game script (co-op mod) may change the inputs. Every player's PC runs it on the same frame
		// with the same inputs, so the results stay identical.
		Mods::ProcessFrameInputs(frame, inputs, s_session.NumPlayers());

		for (int p = 0; p < s_session.NumPlayers(); p++)
			PadData(s_pad_slots[p].first, s_pad_slots[p].second, inputs[p]).OverrideActualController();

		if (s_session.DesyncDetected() && !s_desync_warned)
		{
			s_desync_warned = true;
			Console.Error(fmt::format("Netplay: desync detected at frame {}", s_session.DesyncFrame()));
			Host::AddIconOSDMessage("NetplayDesync", ICON_FA_TRIANGLE_EXCLAMATION,
				s_session.NumPlayers() == 2 ? "The two games are out of sync. Please restart the match." :
											  "The games are out of sync. Please restart the match.",
				Host::OSD_WARNING_DURATION);
		}
	}

	void Shutdown()
	{
		if (s_state == State::Active)
		{
			s_session.Stop();
			s_shadow_pad.reset();
		}
		// A launcher starts a fresh PCSX2 process per match, so netplay is not re-armed.
		if (s_state != State::Unchecked)
			s_state = State::Disabled;
	}
} // namespace Netplay
