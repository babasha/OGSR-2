#include "stdafx.h"
#ifdef _MSC_VER
#include <psapi.h>
#else
#include <cstdio>
#endif
#include "xrsharedmem.h"

// Horizon: mimalloc's OS layer needs mmap, which newlib/libnx do not have. newlib's malloc (over the
// libnx heap) carries the engine until a Horizon-specific mimalloc prim (svcMapPhysicalMemory/virtmem) exists.
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SWITCH__)
#define USE_MIMALLOC
#endif

#ifdef USE_MIMALLOC
#include "mimalloc-override.h"
// xrSimpodin: перегрузка операторов new/delete будет действовать только внутри того модуля движка, в котором они перегружены.
// Если движок разбит на модули и операторы перегружены в xrCore.dll, то в других модулях будут использоваться стандартные операторы,
// и если создать объект через new в xrCore, а delete сделать в xrGame - будет ошибка, т.к. объект создали кастомным аллокатором, а удалить пытаемся системным.
// Здесь два варианта решения проблемы: или перегружать операторы в каждом модуле, что не очень рационально,
// или перегружать их только в случае, если движок собирается в один exe файл. Второй вариант мне кажется более рациональным.
// Android: the engine is libmain.so next to libc++_shared.so, which keeps its own new/delete (scudo).
// A string grown inside libc++ then freed a mimalloc block through scudo -> "Pointer tag was truncated" abort.
#ifndef __ANDROID__
#include "mimalloc-new-delete.h"
#endif
#pragma comment(lib, "mimalloc")
#endif

xrMemory Memory;

#ifdef USE_MIMALLOC
// mimalloc reports double frees, corrupted free lists and overflowed blocks on
// stderr -- which a windowed build drops on the floor. Route it into the game
// log, and print the stack of the thread that caused it: the reported address
// alone has never named an owner. Output arrives in fragments (prefix, then
// text), so assemble whole lines before judging them.
static std::mutex g_mi_out_lock;
static char g_mi_line[1024];
static size_t g_mi_len = 0;

static void mi_cdecl xr_mi_output(const char* msg, void* /*arg*/)
{
    if (!msg || !msg[0])
        return;

    std::lock_guard<std::mutex> keep(g_mi_out_lock);
    for (const char* c = msg; *c; ++c)
    {
        if (*c != '\n' && g_mi_len + 1 < sizeof(g_mi_line))
        {
            g_mi_line[g_mi_len++] = *c;
            continue;
        }
        g_mi_line[g_mi_len] = 0;
        if (g_mi_len)
        {
            const bool bad = strstr(g_mi_line, "mimalloc: error") || strstr(g_mi_line, "mimalloc: warning");
            Msg("%s[mimalloc] %s", bad ? "!!" : "* ", g_mi_line);
            if (bad)
                LogStackTrace("!!the allocator was called from:\n", false);
        }
        g_mi_len = 0;
    }
}
#endif


void xrMemory::_initialize()
{
#ifdef USE_MIMALLOC
    mi_register_output(xr_mi_output, nullptr);
#endif

    SProcessMemInfo memCounters;
    GetProcessMemInfo(memCounters);

    u64 disableMemoryTotalMb = 32000ull;
    if (char* str = strstr(Core.Params, "-smem_disable_limit "))
        sscanf(str + 20, "%llu", &disableMemoryTotalMb);
    bool disableMemoryPool = memCounters.TotalPhysicalMemory > (disableMemoryTotalMb * (1024ull * 1024ull));

    if (disableMemoryPool)
    {
        Msg("--[%s] memory pool disabled due to available memory limit: [%u MB]", __FUNCTION__, disableMemoryTotalMb);
    }

    g_pStringContainer = xr_new<str_container>();
    g_pSharedMemoryContainer = xr_new<smem_container>(disableMemoryPool);
}

void xrMemory::_destroy()
{
    xr_delete(g_pSharedMemoryContainer);
    xr_delete(g_pStringContainer);
}

void xrMemory::mem_compact()
{
#if !defined(USE_MIMALLOC) && defined(_MSC_VER) // _heapmin/HeapCompact are MSVC CRT / Win32 only
/*
Следующая команда, в целом, не нужна.
Современные аллокаторы достаточно грамотно и когда нужно возвращают память операционной системе.
Эта строчка нужна, скорее всего, в определённых ситуациях, вроде использования файлов отображаемых в память,
которые требуют большие свободные области памяти.
*/
    _heapmin(); //-V530
    HeapCompact(GetProcessHeap(), 0);
#endif

    if (g_pStringContainer)
        g_pStringContainer->clean();
    if (g_pSharedMemoryContainer)
        g_pSharedMemoryContainer->clean();
}

void* xrMemory::mem_alloc(size_t size)
{
    void* ptr = malloc(size);

    return ptr;
}

void xrMemory::mem_free(void* P)
{
    free(P);
}

void* xrMemory::mem_realloc(void* P, size_t size)
{
    void* ptr = realloc(P, size);

    return ptr;
}

u32 xrMemory::mem_usage(u32* pBlocksUsed, u32* pBlocksFree) { return u32(mem_usage_impl(pBlocksUsed, pBlocksFree)); }

#ifdef _MSC_VER
static void FillProcessMemInfo(SProcessMemInfo& minfo)
{
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);

    minfo.TotalPhysicalMemory = mem.ullTotalPhys;
    minfo.FreePhysicalMemory = mem.ullAvailPhys;

    minfo.TotalPageFile = mem.ullTotalPageFile - mem.ullTotalPhys;

    if (minfo.TotalPageFile > 0l)
    {
        // эта херня погоду показывает на самом деле. надо найти способ как получить свободный размер файла подкачки
        minfo.FreePageFile = mem.ullAvailPageFile > mem.ullAvailPhys ? mem.ullAvailPageFile - mem.ullAvailPhys : mem.ullAvailPageFile;
    }

    minfo.MemoryLoad = mem.dwMemoryLoad;

    PROCESS_MEMORY_COUNTERS pc;
    std::memset(&pc, 0, sizeof(PROCESS_MEMORY_COUNTERS));
    pc.cb = sizeof(pc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pc, sizeof(pc)))
    {
        minfo.WorkingSetSize = pc.WorkingSetSize;
        minfo.PeakWorkingSetSize = pc.PeakWorkingSetSize;

        minfo.PagefileUsage = pc.PagefileUsage;
        minfo.PeakPagefileUsage = pc.PeakPagefileUsage;
    }
}
#else
// POSIX аналог. Системная память -- /proc/meminfo (значения в кБ), процессные
// счётчики -- /proc/self/status. WorkingSetSize/PagefileUsage не имеют точных
// эквивалентов на Linux: VmRSS/VmHWM (резидентная память) -- ближайший аналог
// WorkingSetSize, VmSize/VmPeak (вся виртуальная память процесса, не только
// страничный файл) -- ближайший аналог PagefileUsage. TotalPageFile/FreePageFile
// читаются из своп-полей meminfo, семантически это то же самое (место за
// пределами RAM, куда система может выгружать страницы).
static u64 ReadMeminfoField(const char* buf, const char* key)
{
    const char* p = strstr(buf, key);
    if (!p)
        return 0;
    p += strlen(key);
    while (*p == ' ' || *p == ':')
        ++p;
    return (u64)strtoull(p, nullptr, 10) * 1024ull; // kB -> bytes
}

static u64 ReadStatusField(const char* buf, const char* key)
{
    return ReadMeminfoField(buf, key);
}

static void FillProcessMemInfo(SProcessMemInfo& minfo)
{
    char buf[8192];

    if (FILE* f = fopen("/proc/meminfo", "r"))
    {
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);

        const u64 memTotal = ReadMeminfoField(buf, "MemTotal:");
        const u64 memAvail = ReadMeminfoField(buf, "MemAvailable:");
        const u64 swapTotal = ReadMeminfoField(buf, "SwapTotal:");
        const u64 swapFree = ReadMeminfoField(buf, "SwapFree:");

        minfo.TotalPhysicalMemory = memTotal;
        minfo.FreePhysicalMemory = memAvail;
        minfo.TotalPageFile = swapTotal;
        minfo.FreePageFile = swapFree;
        minfo.MemoryLoad = memTotal ? (u32)(((memTotal - memAvail) * 100ull) / memTotal) : 0;
    }

    if (FILE* f = fopen("/proc/self/status", "r"))
    {
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);

        minfo.WorkingSetSize = ReadStatusField(buf, "VmRSS:");
        minfo.PeakWorkingSetSize = ReadStatusField(buf, "VmHWM:");
        minfo.PagefileUsage = ReadStatusField(buf, "VmSize:");
        minfo.PeakPagefileUsage = ReadStatusField(buf, "VmPeak:");
    }
}
#endif

void GetProcessMemInfo(SProcessMemInfo& minfo)
{
    std::memset(&minfo, 0, sizeof(SProcessMemInfo));

    FillProcessMemInfo(minfo);

#ifdef USE_MIMALLOC
    Log("####################[+MIMALLOC+]####################");
    mi_stats_print_out(
        [](const char* msg, void*) {
            std::string str{msg};
            xr_string_utils::rtrim(str);
            Log(str);
        },
        nullptr);
    Log("####################[-MIMALLOC-]####################");
#endif
}

#ifndef _MSC_VER
// MSVC CRT debug-heap walk (_heapwalk/_HEAPINFO) has no POSIX equivalent --
// glibc/mimalloc don't expose per-block enumeration of the live heap. This is
// opt-in diagnostics behind "-memory_usage" (never called by default), so an
// honest "unsupported here" stub is correct, not a functional gap that blocks
// anything real.
size_t mem_usage_impl(u32* pBlocksUsed, u32* pBlocksFree)
{
    if (pBlocksFree)
        *pBlocksFree = 0;
    if (pBlocksUsed)
        *pBlocksUsed = 0;
    return 0;
}
#else
size_t mem_usage_impl(u32* pBlocksUsed, u32* pBlocksFree)
{
    static bool no_memory_usage = !strstr(Core.Params, "-memory_usage");
    if (no_memory_usage)
        return 0;

    auto heap_handle = reinterpret_cast<HANDLE>(_get_heap_handle());
    if (!HeapLock(heap_handle))
        return 0;

    _HEAPINFO hinfo;
    int heapstatus;
    hinfo._pentry = nullptr;
    size_t total = 0;
    u32 blocks_free = 0;
    u32 blocks_used = 0;
    while ((heapstatus = _heapwalk(&hinfo)) == _HEAPOK)
    {
        if (hinfo._useflag == _USEDENTRY)
        {
            total += hinfo._size;
            blocks_used += 1;
        }
        else
        {
            blocks_free += 1;
        }
    }
    if (pBlocksFree)
        *pBlocksFree = 1024 * blocks_free;
    if (pBlocksUsed)
        *pBlocksUsed = 1024 * blocks_used;

    HeapUnlock(heap_handle);

    switch (heapstatus)
    {
    case _HEAPEMPTY: break;
    case _HEAPEND: break;
    case _HEAPBADPTR: Msg("!![%s] bad pointer to heap", __FUNCTION__); break;
    case _HEAPBADBEGIN: Msg("!![%s] bad start of heap", __FUNCTION__); break;
    case _HEAPBADNODE: Msg("!![%s] bad node in heap", __FUNCTION__); break;
    }
    return total;
}
#endif