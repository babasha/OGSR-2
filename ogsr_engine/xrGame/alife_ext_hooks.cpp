#include "StdAfx.h"
#include "alife_ext_hooks.h"

// Definitions of the generic A-Life extension seams. All null in the open
// engine (stock single-player). An external out-of-tree module assigns them
// from its own static initializer -- see alife_ext_hooks.h for the linkage
// caveat.
namespace ALifeExt
{
VoidFn g_on_new_game = nullptr;
ScriptRegFn g_script_register = nullptr;
VoidFn g_register_console = nullptr;
} // namespace ALifeExt
