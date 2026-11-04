// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "LockstepSession.h"

// Note: (std::min)/(std::max) are written in brackets on purpose. If <windows.h> was already
// included (e.g. through the precompiled header) its min/max macros can't break them.
#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX // windows.h's min/max macros would break std::min/std::max
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t BAD_SOCKET = INVALID_SOCKET;
static void CloseSocket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t BAD_SOCKET = -1;
static void CloseSocket(socket_t s) { close(s); }
#endif

namespace
{
	// Packet: 'N' 'P' <version> <type> <sender> <num_players> ...
	constexpr uint8_t MAGIC0 = 'N';
	constexpr uint8_t MAGIC1 = 'P';
	constexpr uint8_t VERSION = 2; // 2 = up to 4 players (version 1 was 2 players only)
	constexpr uint8_t TYPE_INPUT = 1;
	constexpr uint8_t TYPE_QUIT = 3;

	// TYPE_INPUT layout:
	//   [0..3]   magic, version, type
	//   [4]      sender (0-based player number)
	//   [5]      number of players in the match
	//   [6..21]  4 x ack: sender has every frame of player j below acks[j]
	//   [22..25] hash_frame (0xFFFFFFFF = none)
	//   [26..29] hash
	//   [30..33] first frame number in this packet
	//   [34]     count
	//   [35..]   count * 18 bytes of input
	// TYPE_QUIT layout: [0..5] only.
	constexpr size_t HEADER = 6;
	constexpr size_t INPUT_HEADER = 35;
	constexpr size_t MAX_FRAMES_PER_PACKET = 64; // 35 + 64*18 = 1187 bytes, below typical MTU
	constexpr uint32_t NO_HASH = 0xFFFFFFFFu;

	void PutU32(uint8_t* p, uint32_t v)
	{
		p[0] = static_cast<uint8_t>(v);
		p[1] = static_cast<uint8_t>(v >> 8);
		p[2] = static_cast<uint8_t>(v >> 16);
		p[3] = static_cast<uint8_t>(v >> 24);
	}

	uint32_t GetU32(const uint8_t* p)
	{
		return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
			   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
	}

	int64_t NowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
			.count();
	}

	bool InitSockets()
	{
#ifdef _WIN32
		static bool s_done = false;
		if (!s_done)
		{
			WSADATA wsa;
			if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
				return false;
			s_done = true;
		}
#endif
		return true;
	}
} // namespace

namespace Netplay
{
	InputData NeutralInput()
	{
		InputData d{};
		d[0] = 0xFF; // no buttons pressed (active low)
		d[1] = 0xFF;
		d[2] = d[3] = d[4] = d[5] = 127; // sticks centred
		return d;
	}

	LockstepSession::LockstepSession() = default;

	LockstepSession::~LockstepSession()
	{
		Stop();
	}

	bool LockstepSession::Start(const Options& opts, std::string* error)
	{
		Stop();
		m_opts = opts;
		m_opts.input_delay = std::clamp(m_opts.input_delay, 1, 15);
		if (m_opts.num_players < 2 || m_opts.num_players > MAX_PLAYERS)
		{
			if (error)
				*error = "number of players must be 2 to 4";
			return false;
		}
		if (m_opts.local_player < 0 || m_opts.local_player >= m_opts.num_players)
		{
			if (error)
				*error = "local player out of range";
			return false;
		}
		if (!InitSockets())
		{
			if (error)
				*error = "could not initialise sockets";
			return false;
		}

		socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s == BAD_SOCKET)
		{
			if (error)
				*error = "could not create socket";
			return false;
		}

		sockaddr_in local{};
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		local.sin_port = 0;
		sockaddr_in bridge{};
		bridge.sin_family = AF_INET;
		bridge.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bridge.sin_port = htons(m_opts.bridge_port);

		if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 ||
			connect(s, reinterpret_cast<sockaddr*>(&bridge), sizeof(bridge)) != 0)
		{
			CloseSocket(s);
			if (error)
				*error = "could not connect to the launcher bridge";
			return false;
		}

#ifdef _WIN32
		u_long nonblocking = 1;
		ioctlsocket(s, FIONBIO, &nonblocking);
		// Stop Windows reporting "connection reset" on UDP when the bridge isn't listening yet.
		BOOL report = FALSE;
		DWORD bytes = 0;
		WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &report, sizeof(report), nullptr, 0, &bytes, nullptr, nullptr);
#else
		fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif

		m_socket = static_cast<intptr_t>(s);
		m_running = true;
		m_frame = 0;
		m_local.clear();
		m_local_hashes.clear();
		for (int p = 0; p < MAX_PLAYERS; p++)
		{
			m_remote[p].clear();
			m_remote_hashes[p].clear();
			m_remote_next[p] = 0;
			m_remote_ack[p] = 0;
		}
		m_quit_player = -1;
		m_got_any_packet = false;
		m_desync = false;
		m_desync_frame = 0;
		m_stall_frames = 0;

		// The first `delay` frames have no real input yet: everyone treats them as neutral.
		for (int i = 0; i < m_opts.input_delay; i++)
			m_local[static_cast<uint32_t>(i)] = NeutralInput();
		m_local_next = static_cast<uint32_t>(m_opts.input_delay);

		SendInputs();
		return true;
	}

	void LockstepSession::Stop()
	{
		if (m_socket != -1)
		{
			// Leave gracefully: give the others up to half a second to receive every input we've
			// sent, so they can all run the same frames before they see that we left.
			const int64_t until = NowMs() + 500;
			while (m_running && m_quit_player < 0 && !m_local.empty() && MinAck() < m_local_next && NowMs() < until)
			{
				SendInputs();
				ReceiveAll(10);
			}
			const uint8_t quit[HEADER] = {MAGIC0, MAGIC1, VERSION, TYPE_QUIT,
				static_cast<uint8_t>(m_opts.local_player), static_cast<uint8_t>(m_opts.num_players)};
			for (int i = 0; i < 3; i++)
				SendRaw(quit, sizeof(quit));
			CloseSocket(static_cast<socket_t>(m_socket));
			m_socket = -1;
		}
		m_running = false;
	}

	bool LockstepSession::HaveAllInputs(uint32_t frame) const
	{
		for (int p = 0; p < m_opts.num_players; p++)
		{
			if (p != m_opts.local_player && m_remote[p].find(frame) == m_remote[p].end())
				return false;
		}
		return true;
	}

	int LockstepSession::LaggingPlayer() const
	{
		for (int p = 0; p < m_opts.num_players; p++)
		{
			if (p != m_opts.local_player && m_remote[p].find(m_frame) == m_remote[p].end())
				return p;
		}
		return -1;
	}

	uint32_t LockstepSession::MinAck() const
	{
		uint32_t ack = UINT32_MAX;
		for (int p = 0; p < m_opts.num_players; p++)
		{
			if (p != m_opts.local_player)
				ack = (std::min)(ack, m_remote_ack[p]);
		}
		return ack;
	}

	LockstepSession::Result LockstepSession::AdvanceFrame(const InputData& local,
		FrameInputs* out_inputs, const std::function<bool()>& keep_waiting)
	{
		if (!m_running)
			return Result::Disconnected;

		m_local[m_local_next++] = local;
		SendInputs();
		ReceiveAll(0);

		if (!HaveAllInputs(m_frame))
		{
			m_stall_frames++;
			const int64_t start = NowMs();
			int64_t last_send = start;
			int64_t last_pump = start;
			while (!HaveAllInputs(m_frame))
			{
				if (m_quit_player >= 0)
					return Result::Disconnected;

				const int timeout = m_got_any_packet ? m_opts.frame_timeout_ms : m_opts.first_frame_timeout_ms;
				const int64_t now = NowMs();
				if (now - start > timeout)
					return Result::TimedOut;
				if (now - last_send >= m_opts.resend_interval_ms)
				{
					SendInputs();
					last_send = now;
				}
				if (keep_waiting && now - last_pump >= 16)
				{
					last_pump = now;
					if (!keep_waiting())
						return Result::Aborted;
				}
				ReceiveAll(2);
			}
		}
		// (If someone quit but their input for this frame already arrived, the frame still runs:
		// every remaining player runs exactly the same frames before noticing the quit.)
		for (int p = 0; p < MAX_PLAYERS; p++)
			(*out_inputs)[p] = NeutralInput();
		for (int p = 0; p < m_opts.num_players; p++)
		{
			if (p == m_opts.local_player)
				(*out_inputs)[p] = m_local[m_frame];
			else
			{
				(*out_inputs)[p] = m_remote[p][m_frame];
				m_remote[p].erase(m_remote[p].begin(), m_remote[p].upper_bound(m_frame));
			}
		}

		// Forget local inputs every other player already has.
		const uint32_t keep_local_from = (std::min)(MinAck(), m_frame + 1);
		m_local.erase(m_local.begin(), m_local.lower_bound(keep_local_from));

		m_frame++;
		return Result::Ok;
	}

	void LockstepSession::ReportChecksum(uint32_t frame, uint32_t hash)
	{
		m_local_hashes[frame] = hash;
		while (m_local_hashes.size() > 16)
			m_local_hashes.erase(m_local_hashes.begin());
		CheckChecksums();
	}

	void LockstepSession::CheckChecksums()
	{
		if (m_desync)
			return;
		for (const auto& [frame, hash] : m_local_hashes)
		{
			for (int p = 0; p < m_opts.num_players; p++)
			{
				if (p == m_opts.local_player)
					continue;
				const auto it = m_remote_hashes[p].find(frame);
				if (it != m_remote_hashes[p].end() && it->second != hash)
				{
					m_desync = true;
					m_desync_frame = frame;
					return;
				}
			}
		}
	}

	void LockstepSession::SendInputs()
	{
		if (m_local.empty())
			return;
		uint8_t buf[INPUT_HEADER + MAX_FRAMES_PER_PACKET * 18];
		buf[0] = MAGIC0;
		buf[1] = MAGIC1;
		buf[2] = VERSION;
		buf[3] = TYPE_INPUT;
		buf[4] = static_cast<uint8_t>(m_opts.local_player);
		buf[5] = static_cast<uint8_t>(m_opts.num_players);
		for (int p = 0; p < MAX_PLAYERS; p++)
			PutU32(buf + 6 + p * 4, p == m_opts.local_player ? 0 : m_remote_next[p]);
		if (m_local_hashes.empty())
		{
			PutU32(buf + 22, NO_HASH);
			PutU32(buf + 26, 0);
		}
		else
		{
			const auto& latest = *m_local_hashes.rbegin();
			PutU32(buf + 22, latest.first);
			PutU32(buf + 26, latest.second);
		}

		// Everyone gets the same packet, starting from the oldest frame anyone still lacks.
		const uint32_t first = (std::max)(MinAck(), m_local.begin()->first);
		uint32_t count = 0;
		for (auto it = m_local.lower_bound(first); it != m_local.end() && count < MAX_FRAMES_PER_PACKET; ++it)
		{
			if (it->first != first + count)
				break; // should never happen: local inputs are contiguous
			std::memcpy(buf + INPUT_HEADER + count * 18, it->second.data(), 18);
			count++;
		}
		PutU32(buf + 30, first);
		buf[34] = static_cast<uint8_t>(count);
		SendRaw(buf, INPUT_HEADER + count * 18);
	}

	void LockstepSession::SendRaw(const uint8_t* data, size_t len)
	{
		if (m_socket == -1)
			return;
		send(static_cast<socket_t>(m_socket), reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
	}

	void LockstepSession::ReceiveAll(int timeout_ms)
	{
		if (m_socket == -1)
			return;
		const socket_t s = static_cast<socket_t>(m_socket);
		if (timeout_ms > 0)
		{
			fd_set set;
			FD_ZERO(&set);
			FD_SET(s, &set);
			timeval tv;
			tv.tv_sec = 0;
			tv.tv_usec = timeout_ms * 1000;
			select(static_cast<int>(s + 1), &set, nullptr, nullptr, &tv);
		}
		uint8_t buf[2048];
		for (;;)
		{
			const auto n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
			if (n <= 0)
				break; // would block, or a transient error (ignored)
			HandlePacket(buf, static_cast<size_t>(n));
		}
	}

	void LockstepSession::HandlePacket(const uint8_t* data, size_t len)
	{
		if (len < HEADER || data[0] != MAGIC0 || data[1] != MAGIC1 || data[2] != VERSION)
			return;
		const int sender = data[4];
		if (data[5] != m_opts.num_players || sender >= m_opts.num_players || sender == m_opts.local_player)
			return; // not from this match

		if (data[3] == TYPE_QUIT)
		{
			if (m_quit_player < 0)
				m_quit_player = sender;
			return;
		}
		if (data[3] != TYPE_INPUT || len < INPUT_HEADER)
			return;

		const uint32_t count = data[34];
		if (len < INPUT_HEADER + count * 18)
			return;
		m_got_any_packet = true;

		m_remote_ack[sender] = (std::max)(m_remote_ack[sender], GetU32(data + 6 + m_opts.local_player * 4));

		const uint32_t hash_frame = GetU32(data + 22);
		if (hash_frame != NO_HASH)
		{
			auto& hashes = m_remote_hashes[sender];
			hashes[hash_frame] = GetU32(data + 26);
			while (hashes.size() > 16)
				hashes.erase(hashes.begin());
			CheckChecksums();
		}

		auto& remote = m_remote[sender];
		uint32_t& next = m_remote_next[sender];
		const uint32_t first = GetU32(data + 30);
		for (uint32_t i = 0; i < count; i++)
		{
			const uint32_t f = first + i;
			if (f < next)
				continue; // already have it
			InputData in;
			std::memcpy(in.data(), data + INPUT_HEADER + i * 18, 18);
			remote.emplace(f, in);
		}
		while (remote.find(next) != remote.end())
			next++;
	}
} // namespace Netplay
