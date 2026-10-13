/*
** LuaJIT -- a Just-In-Time Compiler for Lua. https://luajit.org/
**
** Copyright (C) 2005-2021 Mike Pall. All rights reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining
** a copy of this software and associated documentation files (the
** "Software"), to deal in the Software without restriction, including
** without limitation the rights to use, copy, modify, merge, publish,
** distribute, sublicense, and/or sell copies of the Software, and to
** permit persons to whom the Software is furnished to do so, subject to
** the following conditions:
**
** The above copyright notice and this permission notice shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
** EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
** CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
** SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
** [ MIT license: https://www.opensource.org/licenses/mit-license.php ]
*/

#ifndef _LUAJIT_H
#define _LUAJIT_H

#include "lua.h"

#define LUAJIT_VERSION		"LuaJIT 2.1.0-beta3"
#define LUAJIT_VERSION_NUM	20100  /* Version 2.1.0 = 02.01.00. */
#define LUAJIT_VERSION_SYM	luaJIT_version_2_1_0_beta3
#define LUAJIT_COPYRIGHT	"Copyright (C) 2005-2021 Mike Pall"
#define LUAJIT_URL		"https://luajit.org/"

/* Modes for luaJIT_setmode. */
#define LUAJIT_MODE_MASK	0x00ff

enum {
  LUAJIT_MODE_ENGINE,		/* Set mode for whole JIT engine. */
  LUAJIT_MODE_DEBUG,		/* Set debug mode (idx = level). */

  LUAJIT_MODE_FUNC,		/* Change mode for a function. */
  LUAJIT_MODE_ALLFUNC,		/* Recurse into subroutine protos. */
  LUAJIT_MODE_ALLSUBFUNC,	/* Change only the subroutines. */

  LUAJIT_MODE_TRACE,		/* Flush a compiled trace. */

  LUAJIT_MODE_WRAPCFUNC = 0x10,	/* Set wrapper mode for C function calls. */

  LUAJIT_MODE_MAX
};

/* Flags or'ed in to the mode. */
#define LUAJIT_MODE_OFF		0x0000	/* Turn feature off. */
#define LUAJIT_MODE_ON		0x0100	/* Turn feature on. */
#define LUAJIT_MODE_FLUSH	0x0200	/* Flush JIT-compiled code. */

/* LuaJIT public C API. */

/* Control the JIT engine. */
LUA_API int luaJIT_setmode(lua_State *L, int idx, int mode);

/* Low-overhead profiling API. */
typedef void (*luaJIT_profile_callback)(void *data, lua_State *L,
					int samples, int vmstate);
LUA_API void luaJIT_profile_start(lua_State *L, const char *mode,
				  luaJIT_profile_callback cb, void *data);
LUA_API void luaJIT_profile_stop(lua_State *L);
LUA_API const char *luaJIT_profile_dumpstack(lua_State *L, const char *fmt,
					     int depth, size_t *len);

/* OGSR: per-GC-state wall time (us) since the last call, then reset; see lj_gc.c. */
LUA_API int luaJIT_gcprof(lua_State *L, int enable, double *sum_us,
			  double *max_us, unsigned *n, size_t *maxcost);

/* OGSR: lua_newuserdata for a userdata that never needs its __gc run: kept
** off the finalizer-candidate list, so the GC's atomic phase never walks it. */
LUA_API void *luaJIT_newuserdata_nofinalizer(lua_State *L, size_t size);

/* OGSR: LUA_GCSTEP that stops after max_us (debt carries over); see lj_api.c. */
LUA_API int luaJIT_gcstep_capped(lua_State *L, int kb, int max_us);
LUA_API size_t luaJIT_gcdebt(lua_State *L);

/* OGSR: the last GC cycle -- atomic phase by part, marking time, heap; returns
** a counter bumped by every atomic phase. See lj_gc.c. */
LUA_API unsigned luaJIT_gcprof_cycle(double *atomic_us, size_t *atomic_sz,
				     unsigned *grayagain_n, double *mark_us,
				     unsigned *mark_steps, size_t *heap,
				     double *strresize_us);

/* OGSR: the atomic phase's walk of the finalizer-candidate userdata list; see
** gc_udmove in lj_gc.c. */
LUA_API void luaJIT_gc_udmove(int on);
LUA_API void luaJIT_gc_udsettle(int on);
LUA_API void luaJIT_gc_udlist_stats(unsigned *visited, unsigned *moved, double *walk_us,
				    unsigned *settled, unsigned *prewalk_visits);
LUA_API unsigned luaJIT_gc_udlist_walk(lua_State *L,
				       void (*cb)(void *ctx, void *payload, const void *mt, int pending),
				       void *ctx);

/* Enforce (dynamic) linker error for version mismatches. Call from main. */
LUA_API void LUAJIT_VERSION_SYM(void);

#endif
