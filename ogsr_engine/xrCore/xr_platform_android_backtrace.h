// Android (bionic): <execinfo.h> backtrace() exists only from API 33, the build targets 29. The unwinder the
// NDK links anyway (libunwind behind <unwind.h>) walks the calling thread's stack the same way glibc's
// backtrace() does: entry 0 is this function's caller frame, as callers of backtrace() expect.
#pragma once

#include <unwind.h>
#include <stdint.h>

namespace xr_android_bt
{
struct State
{
    void** cur;
    void** end;
};
inline _Unwind_Reason_Code step(struct _Unwind_Context* ctx, void* arg)
{
    State* s = static_cast<State*>(arg);
    const uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc)
    {
        if (s->cur == s->end)
            return _URC_END_OF_STACK;
        *s->cur++ = reinterpret_cast<void*>(pc);
    }
    return _URC_NO_REASON;
}
} // namespace xr_android_bt

inline int backtrace(void** buffer, int size)
{
    if (size <= 0)
        return 0;
    xr_android_bt::State s{buffer, buffer + size};
    _Unwind_Backtrace(xr_android_bt::step, &s);
    return int(s.cur - buffer);
}
