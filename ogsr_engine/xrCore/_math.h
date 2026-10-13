#pragma once

#include <thread> // std::thread -- на MSVC приходит транзитивно, явно нужен на Linux

// __rdtsc -- на MSVC приходит транзитивно через <intrin.h> (windows.h тянет
// его сам); на GCC/Clang нужен явный <x86intrin.h> (тот же символ, x86-only
// интринсик, работает одинаково). На ARM64 (Switch) __rdtsc нет вообще --
// CPU::GetCLK() ниже читает CNTPCT_EL0 (физический счётчик, 19.2 МГц на Tegra X1; см. ниже, почему не CNTVCT).
#if !defined(_MSC_VER) && (defined(__x86_64__) || defined(__i386__))
#include <x86intrin.h>
#endif

struct _processor_info;

namespace CPU
{
XRCORE_API extern _processor_info ID;
XRCORE_API extern u64 QPC();
// Ticks per second of the counter QPC() reads. Constant for the life of the
// machine, so two processes that both call QPC() are reading ONE clock with
// one epoch (system boot) -- which is the only clock a client and a server on
// the same box can compare timestamps on. Level().timeServer() cannot be used
// for that: it is this process's own uptime (NET_Client.h), so the same
// instant reads differently in each exe.
XRCORE_API u64 QPCFreq();
// Microseconds since boot, on that shared clock.
XRCORE_API u64 QPC_us();

// Monotonic tick counter, NOT a cycle count off x86: callers only difference two reads.
#if defined(__aarch64__)
inline u64 GetCLK()
{
    u64 v;
#ifdef __SWITCH__
    // CNTPCT_EL0, not CNTVCT_EL0: Horizon lets user mode read the PHYSICAL counter (libnx's armGetSystemTick does
    // exactly this, 19.2 MHz) and the virtual one stops the process dead -- found on hardware 30-09, the log
    // ended at the first CPU::GetCLK() with no crash report.
    __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(v));
#else
    // Linux / Android: the other way round -- the kernel opens only the VIRTUAL counter to EL0; CNTPCT_EL0 is
    // SIGILL (Android 10-10: the process died before its first log line, exit-info "SIGNALED status=4").
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
#endif
    return v;
}
#else
inline u64 GetCLK() { return __rdtsc(); }
#endif
} // namespace CPU

XRCORE_API void _initialize_cpu();
XRCORE_API void set_current_thread_name(const char* threadName);
XRCORE_API void set_thread_name(const char* threadName, std::thread& thread);
