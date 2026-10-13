/* Horizon build: LuaJIT's sampling profiler is off (LJ_HASPROFILE 0: no SIGPROF). lj_dispatch.c then
 * renames its dispatch-table slot to lj_dispatch_ins, but the VM assembly that buildvm generates still
 * calls the real symbol from its profile hook. The hook is never armed without the profiler, so this
 * only has to exist. */
#include "lj_obj.h"
#include "lj_dispatch.h"

#if !LJ_HASPROFILE
void LJ_FASTCALL lj_dispatch_profile(lua_State *L, const BCIns *pc)
{
  (void)L; (void)pc;
}
#endif
