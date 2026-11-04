// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ScriptRuntime.h"

#include "lua/lauxlib.h"
#include "lua/lua.h"
#include "lua/lualib.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

// Note on the functions called from Lua below: Lua reports errors with longjmp (PCSX2 has no C++
// exceptions), which skips C++ destructors. So these functions only use plain values while they
// may raise a Lua error.

namespace
{
	constexpr size_t MEMORY_LIMIT = 32 * 1024 * 1024; // bytes of Lua memory a script may use
	constexpr int HOOK_STEP = 1000;                   // instructions between budget checks

	struct AllocState
	{
		size_t used = 0;
	};

	void* LimitedAlloc(void* ud, void* ptr, size_t osize, size_t nsize)
	{
		AllocState* st = static_cast<AllocState*>(ud);
		const size_t old_size = ptr ? osize : 0;
		if (nsize == 0)
		{
			st->used -= old_size;
			std::free(ptr);
			return nullptr;
		}
		if (nsize > old_size && st->used - old_size + nsize > MEMORY_LIMIT)
			return nullptr; // Lua turns this into a "not enough memory" error
		void* p = std::realloc(ptr, nsize);
		if (p)
			st->used = st->used - old_size + nsize;
		return p;
	}

	Mods::ScriptRuntime* Runtime(lua_State* L)
	{
		return *static_cast<Mods::ScriptRuntime**>(lua_getextraspace(L));
	}

	void BudgetHook(lua_State* L, lua_Debug*)
	{
		Mods::ScriptRuntime* rt = Runtime(L);
		rt->m_instructions_left -= HOOK_STEP;
		if (rt->m_instructions_left <= 0)
			luaL_error(L, "the script did too much work in one frame (stopped to keep the game running)");
	}

	// ---------------------------------------------------------------- mem.*
	int AddressError(lua_State* L, const char* what, uint32_t addr)
	{
		char hex[16];
		std::snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned>(addr));
		return luaL_error(L, "can't %s memory at %s", what, hex);
	}

	uint32_t CheckAddress(lua_State* L, int arg)
	{
		const lua_Integer a = luaL_checkinteger(L, arg);
		if (a < 0 || a > 0xFFFFFFFFll)
			luaL_argerror(L, arg, "address out of range");
		return static_cast<uint32_t>(a);
	}

	template <typename T>
	int MemRead(lua_State* L)
	{
		const uint32_t addr = CheckAddress(L, 1);
		T value;
		if (!Runtime(L)->Host()->ReadMemory(addr, &value, sizeof(T)))
			return AddressError(L, "read", addr);
		if constexpr (std::is_floating_point_v<T>)
			lua_pushnumber(L, static_cast<lua_Number>(value));
		else
			lua_pushinteger(L, static_cast<lua_Integer>(value));
		return 1;
	}

	template <typename T>
	int MemWrite(lua_State* L)
	{
		const uint32_t addr = CheckAddress(L, 1);
		T value;
		if constexpr (std::is_floating_point_v<T>)
			value = static_cast<T>(luaL_checknumber(L, 2));
		else
			value = static_cast<T>(luaL_checkinteger(L, 2)); // wraps like a store on the PS2
		if (!Runtime(L)->Host()->WriteMemory(addr, &value, sizeof(T)))
			return AddressError(L, "write", addr);
		return 0;
	}

	// ---------------------------------------------------------------- pad.*
	// Button mask as scripts see it: 1 = pressed. High byte = data[0] (select, L3, R3, start, up,
	// right, down, left), low byte = data[1] (L2, R2, L1, R1, triangle, circle, cross, square).
	struct ButtonInfo
	{
		const char* name;
		uint16_t mask;
		int pressure_index; // byte holding this button's pressure, or -1
	};
	constexpr ButtonInfo BUTTONS[] = {
		{"SELECT", 0x0100, -1}, {"L3", 0x0200, -1}, {"R3", 0x0400, -1}, {"START", 0x0800, -1},
		{"UP", 0x1000, 8}, {"RIGHT", 0x2000, 6}, {"DOWN", 0x4000, 9}, {"LEFT", 0x8000, 7},
		{"L2", 0x0001, 16}, {"R2", 0x0002, 17}, {"L1", 0x0004, 14}, {"R1", 0x0008, 15},
		{"TRIANGLE", 0x0010, 10}, {"CIRCLE", 0x0020, 11}, {"CROSS", 0x0040, 12}, {"SQUARE", 0x0080, 13},
	};

	Mods::PadBytes* CheckPad(lua_State* L, int arg)
	{
		Mods::ScriptRuntime* rt = Runtime(L);
		const lua_Integer p = luaL_checkinteger(L, arg);
		if (!rt->Pads() || p < 1 || p > rt->NumPads())
			luaL_argerror(L, arg, "no such player this frame");
		return &(*rt->Pads())[static_cast<size_t>(p - 1)];
	}

	uint16_t PressedMask(const Mods::PadBytes& d)
	{
		return static_cast<uint16_t>(((~d[0] & 0xFF) << 8) | (~d[1] & 0xFF));
	}

	int PadGet(lua_State* L)
	{
		const Mods::PadBytes& d = *CheckPad(L, 1);
		lua_createtable(L, 0, 5);
		lua_pushinteger(L, PressedMask(d));
		lua_setfield(L, -2, "buttons");
		lua_pushinteger(L, d[4]);
		lua_setfield(L, -2, "lx");
		lua_pushinteger(L, d[5]);
		lua_setfield(L, -2, "ly");
		lua_pushinteger(L, d[2]);
		lua_setfield(L, -2, "rx");
		lua_pushinteger(L, d[3]);
		lua_setfield(L, -2, "ry");
		return 1;
	}

	int PadPressed(lua_State* L)
	{
		const Mods::PadBytes& d = *CheckPad(L, 1);
		const lua_Integer mask = luaL_checkinteger(L, 2);
		lua_pushboolean(L, (PressedMask(d) & mask) == mask && mask != 0);
		return 1;
	}

	uint8_t CheckAxis(lua_State* L, int idx, const char* field)
	{
		const lua_Integer v = lua_tointeger(L, idx);
		if (v < 0 || v > 255)
			luaL_error(L, "pad.set: '%s' must be 0-255", field);
		return static_cast<uint8_t>(v);
	}

	// pad.set(player, {buttons = mask, lx = .., ly = .., rx = .., ry = ..}); missing fields are kept.
	int PadSet(lua_State* L)
	{
		Mods::PadBytes& d = *CheckPad(L, 1);
		luaL_checktype(L, 2, LUA_TTABLE);
		if (lua_getfield(L, 2, "buttons") != LUA_TNIL)
		{
			const uint16_t mask = static_cast<uint16_t>(luaL_checkinteger(L, -1));
			d[0] = static_cast<uint8_t>(~(mask >> 8));
			d[1] = static_cast<uint8_t>(~mask);
			for (const ButtonInfo& b : BUTTONS)
			{
				if (b.pressure_index >= 0)
					d[static_cast<size_t>(b.pressure_index)] = (mask & b.mask) ? 255 : 0;
			}
		}
		lua_pop(L, 1);
		static constexpr struct { const char* name; int index; } AXES[] = {{"lx", 4}, {"ly", 5}, {"rx", 2}, {"ry", 3}};
		for (const auto& axis : AXES)
		{
			if (lua_getfield(L, 2, axis.name) != LUA_TNIL)
				d[static_cast<size_t>(axis.index)] = CheckAxis(L, -1, axis.name);
			lua_pop(L, 1);
		}
		return 0;
	}

	// ---------------------------------------------------------------- n2.*
	int N2Frame(lua_State* L)
	{
		lua_pushinteger(L, Runtime(L)->Frame());
		return 1;
	}

	int N2Players(lua_State* L)
	{
		lua_pushinteger(L, Runtime(L)->NumPads());
		return 1;
	}

	int N2Log(lua_State* L)
	{
		const char* msg = luaL_tolstring(L, 1, nullptr);
		Runtime(L)->Host()->Log(msg);
		return 0;
	}

	int N2Notify(lua_State* L)
	{
		size_t len = 0;
		const char* msg = luaL_checklstring(L, 1, &len);
		char buf[201];
		const size_t n = len < 200 ? len : 200;
		std::memcpy(buf, msg, n);
		buf[n] = 0;
		Runtime(L)->Host()->Notify(buf);
		return 0;
	}

	void OpenSafeLibraries(lua_State* L)
	{
		static constexpr luaL_Reg LIBS[] = {
			{LUA_GNAME, luaopen_base}, {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
			{LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},
		};
		for (const luaL_Reg& lib : LIBS)
		{
			luaL_requiref(L, lib.name, lib.func, 1);
			lua_pop(L, 1);
		}

		static constexpr luaL_Reg MEM[] = {
			{"read8", MemRead<uint8_t>}, {"read16", MemRead<uint16_t>}, {"read32", MemRead<uint32_t>},
			{"reads8", MemRead<int8_t>}, {"reads16", MemRead<int16_t>}, {"reads32", MemRead<int32_t>},
			{"readf", MemRead<float>},
			{"write8", MemWrite<uint8_t>}, {"write16", MemWrite<uint16_t>}, {"write32", MemWrite<uint32_t>},
			{"writef", MemWrite<float>}, {nullptr, nullptr}};
		luaL_newlib(L, MEM);
		lua_setglobal(L, "mem");

		static constexpr luaL_Reg PAD[] = {{"get", PadGet}, {"set", PadSet}, {"pressed", PadPressed}, {nullptr, nullptr}};
		luaL_newlib(L, PAD);
		for (const ButtonInfo& b : BUTTONS)
		{
			lua_pushinteger(L, b.mask);
			lua_setfield(L, -2, b.name);
		}
		lua_setglobal(L, "pad");

		static constexpr luaL_Reg N2[] = {{"frame", N2Frame}, {"players", N2Players}, {"log", N2Log},
			{"notify", N2Notify}, {nullptr, nullptr}};
		luaL_newlib(L, N2);
		lua_setglobal(L, "n2");

		// Remove or tame anything that could reach outside the game or differ between PCs.
		static constexpr const char* SANDBOX = R"lua(
			dofile, loadfile = nil, nil
			print = function(...) local t = {} for i = 1, select('#', ...) do t[#t + 1] = tostring((select(i, ...))) end n2.log(table.concat(t, '\t')) end
			local raw_load = load
			load = function(chunk, name, mode, env) return raw_load(chunk, name, 't', env) end -- text only
			string.dump = nil
			local raw_seed = math.randomseed
			math.randomseed = function(a, b) raw_seed(a or 0, b or 0) end -- never seeded from the clock
			raw_seed(0x4E32, 0)
		)lua";
		if (luaL_dostring(L, SANDBOX) != LUA_OK)
			lua_pop(L, 1);
	}
} // namespace

namespace Mods
{
	ScriptRuntime::ScriptRuntime() = default;

	ScriptRuntime::~ScriptRuntime()
	{
		Unload();
	}

	void ScriptRuntime::Unload()
	{
		if (m_L)
		{
			void* ud = nullptr;
			lua_getallocf(m_L, &ud);
			lua_close(m_L);
			delete static_cast<AllocState*>(ud);
			m_L = nullptr;
		}
		m_host = nullptr;
		m_pads = nullptr;
		m_started = false;
	}

	bool ScriptRuntime::Load(const std::string& source, const std::string& name, ScriptHost* host, std::string* error)
	{
		Unload();
		AllocState* alloc = new AllocState();
		m_L = lua_newstate(LimitedAlloc, alloc);
		if (!m_L)
		{
			delete alloc;
			if (error)
				*error = "could not start the script engine";
			return false;
		}
		*static_cast<ScriptRuntime**>(lua_getextraspace(m_L)) = this;
		m_host = host;
		OpenSafeLibraries(m_L);
		lua_sethook(m_L, BudgetHook, LUA_MASKCOUNT, HOOK_STEP);

		m_instructions_left = INSTRUCTION_BUDGET;
		const std::string chunk_name = "=" + name;
		if (luaL_loadbufferx(m_L, source.data(), source.size(), chunk_name.c_str(), "t") != LUA_OK ||
			lua_pcall(m_L, 0, 0, 0) != LUA_OK)
		{
			if (error)
				*error = lua_tostring(m_L, -1) ? lua_tostring(m_L, -1) : "unknown script error";
			Unload();
			return false;
		}
		lua_getglobal(m_L, "on_frame");
		const bool has_frame = lua_isfunction(m_L, -1);
		lua_pop(m_L, 1);
		if (!has_frame)
		{
			if (error)
				*error = "the script has no on_frame function";
			Unload();
			return false;
		}
		return true;
	}

	bool ScriptRuntime::Call(const char* function, bool required, std::string* error)
	{
		lua_getglobal(m_L, function);
		if (!lua_isfunction(m_L, -1))
		{
			lua_pop(m_L, 1);
			if (required && error)
				*error = std::string("the script has no ") + function + " function";
			return !required;
		}
		int nargs = 0;
		if (std::strcmp(function, "on_frame") == 0)
		{
			lua_pushinteger(m_L, m_frame);
			nargs = 1;
		}
		if (lua_pcall(m_L, nargs, 0, 0) != LUA_OK)
		{
			if (error)
				*error = lua_tostring(m_L, -1) ? lua_tostring(m_L, -1) : "unknown script error";
			lua_pop(m_L, 1);
			return false;
		}
		return true;
	}

	bool ScriptRuntime::RunFrame(uint32_t frame, FramePads* pads, int num_pads, std::string* error)
	{
		if (!m_L)
			return false;
		m_frame = frame;
		m_pads = pads;
		m_num_pads = num_pads;
		m_instructions_left = INSTRUCTION_BUDGET;
		bool ok = true;
		if (!m_started)
		{
			m_started = true;
			ok = Call("on_start", false, error);
		}
		if (ok)
			ok = Call("on_frame", true, error);
		m_pads = nullptr;
		if (!ok)
			Unload();
		return ok;
	}
} // namespace Mods
