#pragma once

// =====================================================================
//  Generic, null-default extension seams for an optional out-of-tree
//  game module.
//
//  The open engine only FIRES these hooks. With every pointer null it
//  behaves as stock single-player -- nothing here reveals what fills
//  them. An external module (built separately) may fill them from its
//  own translation unit and register its own console commands / Lua
//  bindings behind them.
//
//  IMPORTANT (static-init): a filler must be compiled DIRECTLY into the
//  target executable, NOT buried in a static lib with no external
//  references -- otherwise the linker may drop its object and its static
//  initializer never runs.
// =====================================================================

struct lua_State;

namespace ALifeExt
{
using VoidFn = void (*)();
using ScriptRegFn = void (*)(lua_State*);

extern VoidFn g_on_new_game;          // fired once when a new game is created
extern ScriptRegFn g_script_register; // fired during ALife Lua export (register extra global funcs)
extern VoidFn g_register_console;     // fired at the end of console-command registration

inline void fire_on_new_game()
{
    if (g_on_new_game)
        g_on_new_game();
}
inline void fire_script_register(lua_State* L)
{
    if (g_script_register)
        g_script_register(L);
}
inline void fire_register_console()
{
    if (g_register_console)
        g_register_console();
}
} // namespace ALifeExt
