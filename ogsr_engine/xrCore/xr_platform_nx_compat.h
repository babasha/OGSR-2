// Horizon (newlib + libnx) gaps that the shim and a few engine files trip over. One place, all
// commented. Included from xr_platform_win32_shim.h right after <cctype>.
#pragma once
#include <ctype.h>
#include <pthread.h>
#include <sys/types.h>
#include <stdint.h>

// newlib's <ctype.h> #defines single-letter class masks (_U _L _N _S _P _C _X _B). The engine names
// parameters _P, _X, _R... (`set(const _vector3<T>& _P, T _R)`), and the macro turns them into octal
// numbers: "expected ',' or '...' before numeric constant". C++ uses the ctype FUNCTIONS, not those
// masks, so dropping the macros is safe (libstdc++'s <cctype> #undefs the function-like ones too).
#undef _U
#undef _L
#undef _N
#undef _S
#undef _P
#undef _C
#undef _X
#undef _B

// No gettid() in newlib. Only used as a per-thread key/label, so the pthread handle serves.
inline pid_t gettid() { return (pid_t)(uintptr_t)pthread_self(); }

// No pthread_{set,get}name_np: thread names are diagnostics (logs, profiler), nothing depends on them.
inline int pthread_setname_np(pthread_t, const char*) { return 0; }
inline int pthread_getname_np(pthread_t, char* buf, size_t len)
{
    if (buf && len) buf[0] = 0;
    return 0;
}

// Boot breadcrumbs. A NRO has no console and the engine's own log does not exist until the FS is up, so
// "where did it stop" needs an unbuffered line in stderr.log (redirected in main()). Cheap; remove when boot is solid.
#include <stdio.h>
#define XRNX_TRACE(...) do { fprintf(stderr, "[ogsr/switch] " __VA_ARGS__); fputc('\n', stderr); } while (0)
