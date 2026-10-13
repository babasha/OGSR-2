#include "stdafx.h"
#include "cpuid.h"
#include "xr_platform_time.h"
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cstdio>
#include <set>
#include <utility>

#if defined(__x86_64__) || defined(__i386__)
// Настоящий (не через <cpuid.h>, чей макрос `__cpuid(level,a,b,c,d)` берёт
// 5 аргументов и молча ломает существующие 2-аргументные вызовы ниже) порт
// MSVC-интринсика: та же инструкция `cpuid`, тот же формат вывода int[4].
inline void __cpuid(int cpuInfo[4], int function_id)
{
    __asm__ __volatile__("cpuid" : "=a"(cpuInfo[0]), "=b"(cpuInfo[1]), "=c"(cpuInfo[2]), "=d"(cpuInfo[3]) : "a"(function_id), "c"(0));
}
#define XR_CPU_HAS_CPUID 1
#endif
#endif
#ifdef _MSC_VER
#define XR_CPU_HAS_CPUID 1
#endif

#ifdef _MSC_VER
decltype(auto) countSetBits(ULONG_PTR bitMask)
{
    DWORD LSHIFT = sizeof(ULONG_PTR) * 8 - 1;
    DWORD bitSetCount = 0;
    auto bitTest = static_cast<ULONG_PTR>(1) << LSHIFT;
    DWORD i;

    for (i = 0; i <= LSHIFT; ++i)
    {
        bitSetCount += ((bitMask & bitTest) ? 1 : 0);
        bitTest /= 2;
    }

    return bitSetCount;
}
#endif

_processor_info::_processor_info()
{
#ifndef XR_CPU_HAS_CPUID
    // ARM64 has no CPUID: identification lives in MIDR_EL1 / HWCAP, which user space reads
    // through the kernel (/proc/cpuinfo). Every x86 feature bit is simply absent; NEON/FP are
    // architectural baseline on AArch64, so nothing needs to be detected to use them.
    strcpy_s(vendor, sizeof(vendor), "ARM");
    strcpy_s(brand, sizeof(brand), "aarch64");
    family = 8; // ARMv8-A
    model = 0;
    stepping = 0;
    clearFeatures();
    if (FILE* f = fopen("/proc/cpuinfo", "r"))
    {
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            unsigned part = 0;
            if (sscanf(line, "CPU part : %x", &part) == 1) // e.g. 0xd07 = Cortex-A57
            {
                model = (int)part;
                break;
            }
        }
        fclose(f);
    }
#else
    int cpinfo[4];
    // detect cpu vendor
    __cpuid(cpinfo, 0);
    memcpy(vendor, &(cpinfo[1]), sizeof(int));
    memcpy(vendor + sizeof(int), &(cpinfo[3]), sizeof(int));
    memcpy(vendor + 2 * sizeof(int), &(cpinfo[2]), sizeof(int));

    // detect cpu model
    __cpuid(cpinfo, 0x80000002);
    memcpy(brand, cpinfo, sizeof(cpinfo));
    __cpuid(cpinfo, 0x80000003);
    memcpy(brand + sizeof(cpinfo), cpinfo, sizeof(cpinfo));
    __cpuid(cpinfo, 0x80000004);
    memcpy(brand + 2 * sizeof(cpinfo), cpinfo, sizeof(cpinfo));

    // detect cpu main features
    __cpuid(cpinfo, 1);
    stepping = cpinfo[0] & 0xf;
    model = (u8)((cpinfo[0] >> 4) & 0xf) | ((u8)((cpinfo[0] >> 16) & 0xf) << 4);
    family = (u8)((cpinfo[0] >> 8) & 0xf) | ((u8)((cpinfo[0] >> 20) & 0xff) << 4);
    m_f1_ECX = cpinfo[2];
    m_f1_EDX = cpinfo[3];

    __cpuid(cpinfo, 7);
    m_f7_EBX = cpinfo[1];
    m_f7_ECX = cpinfo[2];

    // and check 3DNow! support
    __cpuid(cpinfo, 0x80000001);
    m_f81_ECX = cpinfo[2];
    m_f81_EDX = cpinfo[3];
#endif // XR_CPU_HAS_CPUID

#ifdef _MSC_VER
    // Calculate available processors
    DWORD returnedLength = 0;
    DWORD byteOffset = 0;
    GetLogicalProcessorInformation(nullptr, &returnedLength);

    auto buffer = std::make_unique<u8[]>(returnedLength);
    auto ptr = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(buffer.get());
    GetLogicalProcessorInformation(ptr, &returnedLength);

    auto processorCoreCount = 0u;
    auto logicalProcessorCount = 0u;

    while (byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnedLength)
    {
        switch (ptr->Relationship)
        {
        case RelationProcessorCore:
            processorCoreCount++;

            // A hyperthreaded core supplies more than one logical processor.
            logicalProcessorCount += countSetBits(ptr->ProcessorMask);
            break;

        default: break;
        }

        byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        ptr++;
    }

    // All logical processors
    coresCount = processorCoreCount;
    threadCount = logicalProcessorCount;

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    m_dwNumberOfProcessors = sysInfo.dwNumberOfProcessors;
    fUsage = std::make_unique<float[]>(m_dwNumberOfProcessors);
    m_idleTime = std::make_unique<LARGE_INTEGER[]>(m_dwNumberOfProcessors);
    perfomanceInfo = std::make_unique<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION[]>(m_dwNumberOfProcessors);
#else
    // ⭐GetLogicalProcessorInformation's "core vs. logical processor" split
    // has a real POSIX source: /proc/cpuinfo's (physical id, core id) pairs
    // are exactly that distinction (a hyperthreaded core reports the SAME
    // pair for each of its logical siblings). sysconf() only gives the
    // logical count, so this parses the real thing rather than collapsing
    // coresCount into threadCount.
    threadCount = (unsigned int)sysconf(_SC_NPROCESSORS_ONLN);
    coresCount = threadCount;

    if (FILE* f = fopen("/proc/cpuinfo", "r"))
    {
        std::set<std::pair<long, long>> uniqueCores;
        long physId = 0, coreId = -1;
        bool haveCoreId = false;
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            long v;
            if (sscanf(line, "physical id : %ld", &v) == 1)
                physId = v;
            else if (sscanf(line, "core id : %ld", &v) == 1)
            {
                coreId = v;
                haveCoreId = true;
            }
            else if (line[0] == '\n' && haveCoreId) // blank line ends one "processor" block
            {
                uniqueCores.emplace(physId, coreId);
                haveCoreId = false;
            }
        }
        if (haveCoreId)
            uniqueCores.emplace(physId, coreId);
        fclose(f);
        if (!uniqueCores.empty())
            coresCount = (unsigned int)uniqueCores.size();
    }

    m_dwNumberOfProcessors = threadCount;
    fUsage = std::make_unique<float[]>(m_dwNumberOfProcessors);
    m_prevCoreIdle = std::make_unique<unsigned long long[]>(m_dwNumberOfProcessors);
    m_prevCoreTotal = std::make_unique<unsigned long long[]>(m_dwNumberOfProcessors);
#endif
}

#ifdef _MSC_VER
unsigned long long SubtractTimes(const FILETIME one, const FILETIME two)
{
    LARGE_INTEGER a, b;
    a.LowPart = one.dwLowDateTime;
    a.HighPart = one.dwHighDateTime;

    b.LowPart = two.dwLowDateTime;
    b.HighPart = two.dwHighDateTime;

    return a.QuadPart - b.QuadPart;
}
#endif

#ifdef _MSC_VER
bool _processor_info::getCPULoad(double& val)
{
    FILETIME sysIdle, sysKernel, sysUser;
    // sysKernel include IdleTime
    if (GetSystemTimes(&sysIdle, &sysKernel, &sysUser) == 0) // GetSystemTimes func FAILED return value is zero;
        return false;

    if (prevSysIdle.dwLowDateTime != 0 && prevSysIdle.dwHighDateTime != 0)
    {
        DWORDLONG sysIdleDiff, sysKernelDiff, sysUserDiff;
        sysIdleDiff = SubtractTimes(sysIdle, prevSysIdle);
        sysKernelDiff = SubtractTimes(sysKernel, prevSysKernel);
        sysUserDiff = SubtractTimes(sysUser, prevSysUser);

        DWORDLONG sysTotal = sysKernelDiff + sysUserDiff;
        DWORDLONG kernelTotal = sysKernelDiff - sysIdleDiff; // kernelTime - IdleTime = kernelTime, because sysKernel include IdleTime

        if (sysTotal > 0) // sometimes kernelTime > idleTime
            val = (double)(((kernelTotal + sysUserDiff) * 100.0) / sysTotal);
    }

    prevSysIdle = sysIdle;
    prevSysKernel = sysKernel;
    prevSysUser = sysUser;

    return true;
}

void _processor_info::MTCPULoad()
{
    using NTQUERYSYSTEMINFORMATION = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    static auto m_pNtQuerySystemInformation = (NTQUERYSYSTEMINFORMATION)GetProcAddress(GetModuleHandle("ntdll.dll"), "NtQuerySystemInformation");

    if (!NT_SUCCESS(m_pNtQuerySystemInformation(SystemProcessorPerformanceInformation, perfomanceInfo.get(),
                                                sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION) * (ULONG)m_dwNumberOfProcessors, nullptr)))
        Msg("!![%s] Can't get NtQuerySystemInformation", __FUNCTION__);

    DWORD dwTickCount = xrPlatform::TickCount();
    if (!m_dwCount)
        m_dwCount = dwTickCount;

    for (DWORD i = 0; i < m_dwNumberOfProcessors; i++)
    {
        auto* cpuPerfInfo = &perfomanceInfo[i];
        cpuPerfInfo->KernelTime.QuadPart -= cpuPerfInfo->IdleTime.QuadPart;

        fUsage[i] = 100.0f - 0.01f * (cpuPerfInfo->IdleTime.QuadPart - m_idleTime[i].QuadPart) / ((dwTickCount - m_dwCount));
        if (fUsage[i] < 0.0f)
        {
            fUsage[i] = 0.0f;
        }
        if (fUsage[i] > 100.0f)
        {
            fUsage[i] = 100.0f;
        }

        m_idleTime[i] = cpuPerfInfo->IdleTime;
    }

    m_dwCount = dwTickCount;
}
#else
// ⭐/proc/stat's "cpu" line carries the exact same idle-vs-total split as
// GetSystemTimes, just in jiffies instead of 100ns FILETIME units, and
// without a FILETIME's "all-zero means never sampled" trick -- so a
// separate `bool` tracks the first call instead of a sentinel value.
namespace
{
bool read_proc_stat_line(FILE* f, unsigned long long& idle, unsigned long long& total)
{
    char line[512];
    if (!fgets(line, sizeof(line), f))
        return false;
    char tag[16];
    unsigned long long user, nice, system_, idleT, iowait = 0, irq = 0, softirq = 0, steal = 0;
    const int n = sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu", tag, &user, &nice, &system_, &idleT, &iowait, &irq, &softirq, &steal);
    if (n < 5)
        return false;
    idle = idleT + iowait;
    total = user + nice + system_ + idleT + iowait + irq + softirq + steal;
    return true;
}
} // namespace

bool _processor_info::getCPULoad(double& val)
{
    FILE* f = fopen("/proc/stat", "r");
    if (!f)
        return false;

    unsigned long long idle = 0, total = 0;
    const bool ok = read_proc_stat_line(f, idle, total);
    fclose(f);
    if (!ok)
        return false;

    if (m_prevGlobalTotal != 0)
    {
        const unsigned long long idleDiff = idle - m_prevGlobalIdle;
        const unsigned long long totalDiff = total - m_prevGlobalTotal;
        if (totalDiff > 0)
            val = 100.0 * (double)(totalDiff - idleDiff) / (double)totalDiff;
    }

    m_prevGlobalIdle = idle;
    m_prevGlobalTotal = total;
    return true;
}

void _processor_info::MTCPULoad()
{
    FILE* f = fopen("/proc/stat", "r");
    if (!f)
        return;

    // First line is the aggregate "cpu" -- skip it, MTCPULoad wants the
    // per-processor "cpuN" lines that follow, same split as getCPULoad above.
    char discard[512];
    if (!fgets(discard, sizeof(discard), f))
    {
        fclose(f);
        return;
    }

    for (DWORD i = 0; i < m_dwNumberOfProcessors; i++)
    {
        unsigned long long idle = 0, total = 0;
        if (!read_proc_stat_line(f, idle, total))
            break;

        if (m_prevCoreTotal[i] != 0)
        {
            const unsigned long long idleDiff = idle - m_prevCoreIdle[i];
            const unsigned long long totalDiff = total - m_prevCoreTotal[i];
            fUsage[i] = totalDiff > 0 ? 100.0f * (float)(totalDiff - idleDiff) / (float)totalDiff : 0.0f;
            if (fUsage[i] < 0.0f)
                fUsage[i] = 0.0f;
            if (fUsage[i] > 100.0f)
                fUsage[i] = 100.0f;
        }

        m_prevCoreIdle[i] = idle;
        m_prevCoreTotal[i] = total;
    }

    fclose(f);
}
#endif
