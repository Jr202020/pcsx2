// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Delay-based lockstep netplay for 2 to 4 players.
//
// This file has no PCSX2 dependencies on purpose, so it can be unit tested on its own.
//
// How it works:
//  - Every emulated frame, each side captures its local controller state and schedules it
//    for frame (current + delay). It sends that input to every other player.
//  - A frame only runs once every player's input for it is known. All emulators therefore
//    run exactly the same inputs on exactly the same frames, and stay in sync as long as
//    emulation is deterministic.
//  - Packets carry every input the other players haven't acknowledged yet, so lost UDP
//    packets are recovered automatically.
//  - Packets don't go straight to the other players: they go to the launcher on localhost
//    (the "bridge"), which forwards them to everyone else in the match (directly, or through
//    the server relay).
//
// Needs proper testing with real games across real networks.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace Netplay
{
	// Matches the 18-byte layout of PadData(port, slot, std::array<u8, 18>).
	using InputData = std::array<uint8_t, 18>;

	constexpr int MAX_PLAYERS = 4;
	using FrameInputs = std::array<InputData, MAX_PLAYERS>;

	InputData NeutralInput();

	class LockstepSession
	{
	public:
		enum class Result
		{
			Ok,
			Disconnected, // another player quit (see QuitPlayer())
			TimedOut,     // no input from another player for too long (see LaggingPlayer())
			Aborted,      // keep_waiting callback asked us to stop (e.g. emulator shutting down)
		};

		struct Options
		{
			uint16_t bridge_port = 0;     // launcher's localhost UDP port
			int num_players = 2;          // 2..4
			int local_player = 0;         // 0 = player 1 ... num_players-1
			int input_delay = 3;          // frames
			int first_frame_timeout_ms = 120000; // allows for the others still booting
			int frame_timeout_ms = 15000;
			int resend_interval_ms = 30;
		};

		LockstepSession();
		~LockstepSession();
		LockstepSession(const LockstepSession&) = delete;
		LockstepSession& operator=(const LockstepSession&) = delete;

		bool Start(const Options& opts, std::string* error);
		void Stop(); // tells the other players we quit

		bool IsRunning() const { return m_running; }
		uint32_t CurrentFrame() const { return m_frame; }
		int LocalPlayer() const { return m_opts.local_player; }
		int NumPlayers() const { return m_opts.num_players; }
		int InputDelay() const { return m_opts.input_delay; }

		// Call once per emulated frame with the local controller state. On Ok, out_inputs[i]
		// holds player i+1's input for this frame (for i < NumPlayers()).
		// keep_waiting is called repeatedly while stalled for others; return false to abort.
		Result AdvanceFrame(const InputData& local, FrameInputs* out_inputs,
			const std::function<bool()>& keep_waiting);

		// Desync detection: report a hash of emulator state at a frame. If any other player's
		// hash for the same frame differs, DesyncDetected() becomes true.
		void ReportChecksum(uint32_t frame, uint32_t hash);
		bool DesyncDetected() const { return m_desync; }
		uint32_t DesyncFrame() const { return m_desync_frame; }

		// Which player (0-based) quit / is being waited for, or -1.
		int QuitPlayer() const { return m_quit_player; }
		int LaggingPlayer() const;

		// Stats for display.
		uint32_t StallCount() const { return m_stall_frames; }

	private:
		bool HaveAllInputs(uint32_t frame) const;
		uint32_t MinAck() const;
		void SendInputs();
		void SendRaw(const uint8_t* data, size_t len);
		void ReceiveAll(int timeout_ms);
		void HandlePacket(const uint8_t* data, size_t len);
		void CheckChecksums();

		Options m_opts;
		bool m_running = false;
		intptr_t m_socket = -1;

		uint32_t m_frame = 0;                       // next frame to run
		std::map<uint32_t, InputData> m_local;      // frame -> local input (unacked + pending)
		uint32_t m_local_next = 0;                  // next frame we'll schedule local input for

		// Per other player (indexed by player number; the local slot is unused).
		std::array<std::map<uint32_t, InputData>, MAX_PLAYERS> m_remote{}; // frame -> their input
		std::array<uint32_t, MAX_PLAYERS> m_remote_next{}; // next contiguous frame we need from them
		std::array<uint32_t, MAX_PLAYERS> m_remote_ack{};  // they have all our frames below this
		std::array<std::map<uint32_t, uint32_t>, MAX_PLAYERS> m_remote_hashes{};

		int m_quit_player = -1;
		bool m_got_any_packet = false;

		std::map<uint32_t, uint32_t> m_local_hashes;
		bool m_desync = false;
		uint32_t m_desync_frame = 0;

		uint32_t m_stall_frames = 0;
	};
} // namespace Netplay
