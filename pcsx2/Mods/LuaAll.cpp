// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Lua 5.4.7 (https://www.lua.org, MIT licence: see lua/LICENSE.txt), built into PCSX2 for
// N2Sphere game scripts. Only the core and the safe libraries are included: no file, OS,
// module-loading or debug library, so a script can only touch the running game.
//
// Built as C++ in one file. PCSX2 is compiled without C++ exceptions, so Lua reports errors
// with setjmp/longjmp.

#define LUA_USE_LONGJMP 1

// Every PC must run a script identically (online matches stay in sync only if they do):
//  - fixed seed for string hashing (Lua normally mixes in the time and memory addresses);
//  - '.' as the decimal point, whatever the PC's language settings.
#define luai_makeseed(L) (0x4E325370u)
#define lua_getlocaledecpoint() ('.')

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif

#include "lua/lapi.c"
#include "lua/lcode.c"
#include "lua/lctype.c"
#include "lua/ldebug.c"
#include "lua/ldo.c"
#include "lua/ldump.c"
#include "lua/lfunc.c"
#include "lua/lgc.c"
#include "lua/llex.c"
#include "lua/lmem.c"
#include "lua/lobject.c"
#include "lua/lopcodes.c"
#include "lua/lparser.c"
#include "lua/lstate.c"
#include "lua/lstring.c"
#include "lua/ltable.c"
#include "lua/ltm.c"
#include "lua/lundump.c"
#include "lua/lvm.c"
#include "lua/lzio.c"

#include "lua/lauxlib.c"
#include "lua/lbaselib.c"
#include "lua/lcorolib.c"
#include "lua/lmathlib.c"
#include "lua/lstrlib.c"
#include "lua/ltablib.c"
#include "lua/lutf8lib.c"

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
