// Horizon (newlib + libnx): no <execinfo.h> backtrace() and no dladdr(). Stack traces are diagnostics
// only (crash logs, `lua_time`, the allocator-error report), so for now capture yields ZERO frames and
// callers print what they have. A frame-pointer walk over the aarch64 chain would fill this in, but
// needs -fno-omit-frame-pointer everywhere and a bounds check against the thread's stack.
#pragma once

inline int backtrace(void** /*buffer*/, int /*size*/) { return 0; }
