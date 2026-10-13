#include "stdafx.h"

#include "cpuid.h"
#include "xr_platform_seh.h"
#ifdef _MSC_VER
#include <powerbase.h>

#pragma comment(lib, "PowrProf.lib")
#include <VersionHelpers.h>

typedef struct _PROCESSOR_POWER_INFORMATION
{
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} PROCESSOR_POWER_INFORMATION, *PPROCESSOR_POWER_INFORMATION;
#else
#include <cstdio>
#include <pthread.h>
#endif

// Initialized on startup
XRCORE_API Fmatrix Fidentity;

CRandom& _GetRandom()
{
    thread_local static CRandom _Random{};
    return _Random;
}

namespace CPU
{
_processor_info ID;
#ifdef _MSC_VER
u64 QPC()
{
    u64 _dest;
    QueryPerformanceCounter(PLARGE_INTEGER(&_dest));
    return _dest;
}

u64 QPCFreq()
{
    static const u64 freq = [] {
        u64 f = 0;
        QueryPerformanceFrequency(PLARGE_INTEGER(&f));
        return f ? f : 1;
    }();
    return freq;
}
#else
// ⭐Тот же трюк, что и QueryThreadCycleTime в xr_platform_win32_shim.h:
// clock_gettime(CLOCK_MONOTONIC) уже даёт наносекунды напрямую, поэтому
// "частота" -- точная константа 1e9, а не калибровка TSC как на Windows.
u64 QPC()
{
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

u64 QPCFreq() { return 1000000000ull; }
#endif

u64 QPC_us()
{
    // Split the divide so a long-uptime machine cannot overflow: at a 10 MHz
    // counter, ticks * 1000000 passes 2^64 after about three weeks of uptime,
    // and a wrapped timestamp would read as a negative latency.
    const u64 f = QPCFreq();
    const u64 t = QPC();
    return (t / f) * 1000000ull + ((t % f) * 1000000ull) / f;
}
} // namespace CPU

void _initialize_cpu()
{
#ifdef __SWITCH__
    XRNX_TRACE("_initialize_cpu: enter (vendor=%s brand=%s)", CPU::ID.vendor, CPU::ID.brand);
#endif
    Msg("* Detected CPU: %s [%s], F%d/M%d/S%d", CPU::ID.brand, CPU::ID.vendor, CPU::ID.family, CPU::ID.model, CPU::ID.stepping);
#ifdef __SWITCH__
    XRNX_TRACE("_initialize_cpu: first Msg returned");
#endif

    // WTF???
    // if (strstr(Core.Params, "-x86"))
    //	CPU::ID.clearFeatures();

    string256 features;
#if defined(__aarch64__)
    strcpy_s(features, sizeof(features), "NEON/FP (AArch64 baseline)");
#else
    strcpy_s(features, sizeof(features), "RDTSC");
#endif
    if (CPU::ID.hasMMX())
        strcat(features, ", MMX");
    if (CPU::ID.has3DNOWExt())
        strcat(features, ", 3DNowExt!");
    if (CPU::ID.has3DNOW())
        strcat(features, ", 3DNow!");
    if (CPU::ID.hasSSE())
        strcat(features, ", SSE");
    if (CPU::ID.hasSSE2())
        strcat(features, ", SSE2");
    if (CPU::ID.hasSSE3())
        strcat(features, ", SSE3");
    if (CPU::ID.hasMWAIT())
        strcat(features, ", MONITOR/MWAIT");
    if (CPU::ID.hasSSSE3())
        strcat(features, ", SSSE3");
    if (CPU::ID.hasSSE41())
        strcat(features, ", SSE4.1");
    if (CPU::ID.hasSSE42())
        strcat(features, ", SSE4.2");
    if (CPU::ID.hasSSE4a())
        strcat(features, ", SSE4a");
    if (CPU::ID.hasAVX())
        strcat(features, ", AVX");
    if (CPU::ID.hasAVX2())
        strcat(features, ", AVX2");

    Msg("* CPU features: %s", features);
    Msg("* CPU cores: [%u], threads: [%u]", CPU::ID.coresCount, CPU::ID.threadCount);
#ifdef __SWITCH__
    XRNX_TRACE("_initialize_cpu: features+cores logged");
#endif

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    R_ASSERT(CPU::ID.hasSSE2());
#endif

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    const size_t cpusCount = sysInfo.dwNumberOfProcessors;
#ifdef __SWITCH__
    XRNX_TRACE("_initialize_cpu: GetSystemInfo ok, cpus=%zu", cpusCount);
#endif

#ifdef _MSC_VER
    xr_vector<PROCESSOR_POWER_INFORMATION> cpusInfo(cpusCount);
    CallNtPowerInformation(ProcessorInformation, nullptr, 0, cpusInfo.data(), ULONG(sizeof(PROCESSOR_POWER_INFORMATION) * cpusCount));

    for (size_t i = 0; i < cpusInfo.size(); i++)
    {
        const PROCESSOR_POWER_INFORMATION& cpuInfo = cpusInfo[i];
        Msg("* CPU%zu current freq: %lu MHz, max freq: %lu MHz", i, cpuInfo.CurrentMhz, cpuInfo.MaxMhz);
    }
#else
    // ⭐CallNtPowerInformation's per-CPU current/max MHz has a real POSIX
    // source in cpufreq (not always present -- containers/WSL/some VMs have
    // no cpufreq driver, so this reads what's there and says "n/a" for the
    // rest rather than pretending a number). Current freq: /proc/cpuinfo's
    // "cpu MHz" (always present, one line per logical CPU, in listed order).
    // Max freq: cpufreq's cpuinfo_max_freq (kHz) when the sysfs node exists.
    if (FILE* f = fopen("/proc/cpuinfo", "r"))
    {
        char line[256];
        size_t cpuIdx = 0;
        while (fgets(line, sizeof(line), f) && cpuIdx < cpusCount)
        {
            double curMhz;
            if (sscanf(line, "cpu MHz : %lf", &curMhz) != 1)
                continue;

            char maxPath[128];
            snprintf(maxPath, sizeof(maxPath), "/sys/devices/system/cpu/cpu%zu/cpufreq/cpuinfo_max_freq", cpuIdx);
            long maxKhz = 0;
            if (FILE* mf = fopen(maxPath, "r"))
            {
                if (fscanf(mf, "%ld", &maxKhz) != 1)
                    maxKhz = 0;
                fclose(mf);
            }

            if (maxKhz > 0)
                Msg("* CPU%zu current freq: %lu MHz, max freq: %lu MHz", cpuIdx, (unsigned long)curMhz, (unsigned long)(maxKhz / 1000));
            else
                Msg("* CPU%zu current freq: %lu MHz, max freq: n/a (no cpufreq)", cpuIdx, (unsigned long)curMhz);
            cpuIdx++;
        }
        fclose(f);
    }
#endif

#ifdef __SWITCH__
    XRNX_TRACE("_initialize_cpu: cpuinfo loop done, Fidentity/pvInitializeStatics...");
#endif
    Fidentity.identity(); // Identity matrix
    pvInitializeStatics(); // Lookup table for compressed normals
#ifdef __SWITCH__
    XRNX_TRACE("_initialize_cpu: done");
#endif
}

#ifdef _MSC_VER
#pragma pack(push, 8)
typedef struct
{
    DWORD dwType; // Must be 0x1000.
    LPCSTR szName; // Pointer to name (in user addr space).
    DWORD dwThreadID; // Thread ID (-1=caller thread).
    DWORD dwFlags; // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)
#endif

static void set_thread_name(HANDLE ThreadHandle, const char* threadName)
{
#ifdef TRACY_ENABLE
    tracy::SetThreadName(threadName);
#endif

#ifdef _MSC_VER
    if (IsWindows10OrGreater())
    {
        static HMODULE KernelLib = GetModuleHandle("kernel32.dll");
        using FuncSetThreadDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        static auto pSetThreadDescription = (FuncSetThreadDescription)GetProcAddress(KernelLib, "SetThreadDescription");

        if (pSetThreadDescription)
        {
            wchar_t buf[64]{};
            mbstowcs(buf, threadName, std::size(buf));

            pSetThreadDescription(ThreadHandle, buf);

            return;
        }
        else if (!IsDebuggerPresent())
            return;
    }
    else if (!IsDebuggerPresent())
        return;

    THREADNAME_INFO info;
    info.dwType = 0x1000;
    info.szName = threadName;
    info.dwThreadID = GetThreadId(ThreadHandle);
    info.dwFlags = 0;

    XR_SEH_TRY
    {
        constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;
        RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    XR_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {}
#else
    // ⭐SetThreadDescription's real POSIX counterpart: glibc's
    // pthread_setname_np targets ANY thread of the calling process (not
    // just self), exactly like SetThreadDescription(ThreadHandle, ...)
    // above -- a real port, not a self-only approximation. 16 bytes
    // including the NUL is a hard glibc limit (TASK_COMM_LEN), unlike
    // Windows' unbounded description -- truncated, not rejected.
    //
    // ThreadHandle is either our tagged current-thread pseudo-handle
    // (xr_thread_handle, from GetCurrentThread()) or a raw pthread_t
    // round-tripped through a HANDLE-sized slot by the std::thread&
    // overload below -- resolve which one we got.
    const pthread_t target = xr_is_thread_handle(ThreadHandle) ? pthread_self() : (pthread_t)(intptr_t)ThreadHandle;

    char buf[16];
    strncpy(buf, threadName, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    pthread_setname_np(target, buf);
#endif
}

void set_current_thread_name(const char* threadName) { set_thread_name(GetCurrentThread(), threadName); }

void set_thread_name(const char* threadName, std::thread& thread) { set_thread_name(reinterpret_cast<HANDLE>(thread.native_handle()), threadName); }
