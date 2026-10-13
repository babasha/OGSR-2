// Switch-only: whole-process profiler and thread placement (ideas from the nfsmw-nx port, 02-10).
//
// Always on (cheap): every thread is registered through `-Wl,--wrap=threadCreate/threadClose` (pthreads go through
// threadCreate too), its creation is logged to stderr.log with priority, requested core and entry point, and the libnx
// calls that cost IPC are counted (fences, kickoffs, window queue, sleeps, data cache cleans).
//
// One-shot nx_prof.txt next to the NRO (read in x_ray.cpp, then deleted) arms the report: every 10 s, nx_prof.log gets
// the exact CPU time of each thread (InfoType_ThreadTickCount), its priority, preferred core and core mask, and the call
// counters. With the word "stacks" in the file a 0x2A thread on core 2 also samples the pc, lr and frame chain of the busy
// threads at 1 kHz: svcSetThreadActivity(Paused) -> svcGetThreadContext3 -> Runnable. Between pause and resume there is no
// malloc, no stdio and no lock -- the paused thread may hold them (the old main-thread sampler froze the game that way).
// With stacks on, every report also appends the busy threads' raw samples (CPU >= kRawCpu) to nx_prof_raw.bin, for
// inclusive per-function time offline: D:\switch_stage\tools\nxraw.py (07-10: the top-15 identical stacks cannot say
// what a 30 ms frame_move in a firefight is made of -- recursion in the ray walkers alone fills ten frames).
//
// One-shot nx_cores.txt sets where new threads run (they otherwise all inherit the process default core):
//   "spread"  -> each new thread that asked for the default core gets preferred core 1, 2, 1, 2... and mask 0x7;
//   "wide"    -> preferred core unchanged, mask widened to 0x7 (the kernel may move it to an idle core).
// A thread that later calls svcSetThreadCoreMask itself (render thread, core probes) keeps its own choice.
//
// Addresses are printed as img+0x..., i.e. the ELF address (switch.ld links .text at 0): symbolize with
// D:\switch_stage\tools\nx_prof_sym.sh <nx_prof.log> <xr_3da.elf>.
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <vector>

extern "C" {
Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz, int prio, int cpuid);
Result __real_threadClose(Thread* t);
Result __real_nvFenceWait(NvFence* f, s32 timeout_us);
Result __real_nvGpuChannelKickoff(NvGpuChannel* c);
Result __real_nwindowQueueBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence);
Result __real_nwindowDequeueBuffer(NWindow* nw, s32* out_slot, NvMultiFence* out_fence);
void __real_svcSleepThread(s64 nano);
void __real_armDCacheClean(void* addr, size_t size);
void xr_nx_overlay_init(void);                          // xr_nx_overlay.cpp
void xr_nx_overlay_present(uint32_t width, uint32_t height);
}

namespace
{
constexpr size_t kMaxThreads = 128;
constexpr size_t kFrames = 32; // 10 until 07-10: deep enough for the leaf, not for who called it
constexpr double kRawCpu = 15.0; // threads busier than this go to nx_prof_raw.bin
constexpr size_t kMaxSamples = 12000; // 10 s at 1 kHz, plus slack
constexpr u64 kReportSeconds = 10;

struct Slot
{
    std::atomic<u32> handle{0};
    u64 entry = 0;        // pthread start routine if known, else the threadCreate entry
    u64 creator[4] = {};  // return addresses at creation
    int prio = 0, cpuid = 0;
    // profiler thread only:
    u32 seen = 0;
    u64 last_ticks = 0;
    double cpu = 0.0;
};
Slot g_slots[kMaxThreads];

struct Sample
{
    u64 pc, lr;
    u64 frames[kFrames];
    u16 slot;
};
Sample* g_samples = nullptr;
size_t g_nsamples = 0;

enum : unsigned
{
    kFencePoll, // nvFenceWait(timeout 0)
    kFenceWait, // nvFenceWait(timeout != 0)
    kKickoff,
    kQueueBuf,
    kDequeueBuf,
    kSleep0,     // svcSleepThread(0)
    kSleepYield, // -1 / -2
    kSleepShort, // <= 1 ms
    kSleepLong,  // > 1 ms
    kDCacheClean,
    kCallCount
};
const char* const kCallNames[kCallCount] = {"fence poll", "fence wait", "kickoff", "queueBuffer", "dequeueBuffer",
                                            "sleep(0)",   "yield",      "sleep<=1ms", "sleep>1ms", "dcache clean"};
struct Calls
{
    std::atomic<u64> n{0}, ticks{0}, bytes{0};
};
Calls g_calls[kCallCount];

inline void note(unsigned id, u64 since, u64 bytes = 0)
{
    g_calls[id].n.fetch_add(1, std::memory_order_relaxed);
    g_calls[id].ticks.fetch_add(armGetSystemTick() - since, std::memory_order_relaxed);
    if (bytes)
        g_calls[id].bytes.fetch_add(bytes, std::memory_order_relaxed);
}

u64 g_text_lo = 0, g_text_hi = 0;
int g_core_policy = 0; // 0 keep, 1 spread, 2 wide
int g_rt_prio = 0x2D;  // nx_cores.txt "rtprio=0x3B": render thread priority (0 = keep libnx's 0x3B). 11-10: 0x2D by default,
                       // above the TTAPI workers now raised to 0x2E (xrCore.cpp g_ttapi_prio), below the main thread
std::atomic<u32> g_spread_next{0};
int g_armed = 0, g_stacks = 0;
Thread g_prof_thread;

__thread void* t_pending_fn; // set by __wrap_pthread_create around the real call

bool in_text(u64 a) { return a >= g_text_lo && a < g_text_hi; }

void text_range()
{
    if (g_text_hi)
        return;
    MemoryInfo mi;
    u32 pi = 0;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, reinterpret_cast<u64>(&text_range))))
    {
        g_text_lo = mi.addr;
        g_text_hi = mi.addr + mi.size;
    }
}

void print_addr(FILE* f, u64 a)
{
    if (in_text(a))
        fprintf(f, "img+0x%" PRIx64, a - g_text_lo);
    else
        fprintf(f, "0x%" PRIx64, a);
}

// The instruction before pc is `svc #n`: the thread is waiting in the kernel, not running.
bool after_svc(u64 pc)
{
    if (!in_text(pc) || pc < g_text_lo + 4)
        return false;
    const u32 insn = *reinterpret_cast<const u32*>(pc - 4);
    return (insn & 0xFFE0001Fu) == 0xD4000001u;
}

int register_thread(Handle h, u64 entry, const u64* creator, int prio, int cpuid)
{
    for (size_t i = 0; i < kMaxThreads; ++i)
    {
        u32 expected = 0;
        if (g_slots[i].handle.compare_exchange_strong(expected, h))
        {
            Slot& s = g_slots[i];
            s.entry = entry;
            for (int k = 0; k < 4; ++k)
                s.creator[k] = creator ? creator[k] : 0;
            s.prio = prio;
            s.cpuid = cpuid;
            return (int)i;
        }
    }
    return -1;
}

void walk_fp(u64 fp, u64 lo, u64 hi, u64* out, size_t n)
{
    for (size_t k = 0; k < n && fp >= lo && fp + 16 <= hi && (fp & 7) == 0; ++k)
    {
        out[k] = reinterpret_cast<const u64*>(fp)[1];
        const u64 next = reinterpret_cast<const u64*>(fp)[0];
        if (next <= fp)
            break;
        fp = next;
    }
}

struct Count
{
    u64 key;
    u32 n;
};
std::vector<Count> histogram(std::vector<u64>& v)
{
    std::sort(v.begin(), v.end());
    std::vector<Count> out;
    for (u64 a : v)
    {
        if (out.empty() || out.back().key != a)
            out.push_back({a, 1});
        else
            ++out.back().n;
    }
    std::sort(out.begin(), out.end(), [](const Count& a, const Count& b) { return a.n > b.n; });
    return out;
}

// nx_prof_raw.bin, one block per report: "NXPR" u32 version(1) u32 report u32 frames u32 records, then per record
// u32 thread id (low bits; bit 31 = the pc sits right after an svc) u32 pc u32 lr u32 frames[frames] -- addresses as
// offsets into the image (= ELF addresses), 0 = none or outside it. Samples arrive sorted by slot.
u32 img_off(u64 a) { return in_text(a) ? u32(a - g_text_lo) : 0u; }

void dump_raw(const std::vector<size_t>& order, int index)
{
    if (!g_stacks || !g_nsamples)
        return;
    std::vector<u32> busy_tid(kMaxThreads, 0);
    bool any = false;
    for (size_t i : order)
    {
        if (g_slots[i].cpu < kRawCpu)
            continue;
        u64 tid = 0;
        svcGetThreadId(&tid, g_slots[i].handle.load());
        busy_tid[i] = u32(tid) & 0x7FFFFFFFu;
        any = true;
    }
    if (!any)
        return;
    std::vector<u32> buf;
    buf.reserve(g_nsamples * (3 + kFrames));
    u32 records = 0;
    for (size_t k = 0; k < g_nsamples; ++k)
    {
        const Sample& s = g_samples[k];
        if (!busy_tid[s.slot])
            continue;
        buf.push_back(busy_tid[s.slot] | (after_svc(s.pc) ? 0x80000000u : 0u));
        buf.push_back(img_off(s.pc));
        buf.push_back(img_off(s.lr));
        for (size_t j = 0; j < kFrames; ++j)
            buf.push_back(img_off(s.frames[j]));
        ++records;
    }
    FILE* f = fopen("nx_prof_raw.bin", index <= 1 ? "wb" : "ab");
    if (!f)
        return;
    const u32 head[5] = {0x5250584Eu /* "NXPR" */, 1u, u32(index), u32(kFrames), records};
    fwrite(head, sizeof(head), 1, f);
    fwrite(buf.data(), sizeof(u32), buf.size(), f);
    fclose(f);
}

void report(u64 elapsed, u64 freq, u64 calls_last[kCallCount][3], int index)
{
    const double secs = double(elapsed) / double(freq);
    std::vector<size_t> order;
    for (size_t i = 0; i < kMaxThreads; ++i)
    {
        Slot& s = g_slots[i];
        const u32 h = s.handle.load();
        if (!h)
        {
            s.seen = 0;
            continue;
        }
        u64 ticks = 0;
        if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, UINT64_MAX)))
            continue;
        if (s.seen != h)
        {
            s.seen = h;
            s.last_ticks = ticks;
            s.cpu = 0.0;
        }
        else
        {
            s.cpu = double(ticks - s.last_ticks) * 100.0 / double(elapsed);
            s.last_ticks = ticks;
        }
        order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [](size_t a, size_t b) { return g_slots[a].cpu > g_slots[b].cpu; });

    FILE* f = fopen("nx_prof.log", "a");
    if (!f)
    {
        g_nsamples = 0;
        return;
    }
    double total = 0.0;
    for (size_t i : order)
        total += g_slots[i].cpu;

    // Horizon's real ceiling is the mapping limit, not "free memory" (UsedMemorySize counts the whole heap).
    u64 lim_used = 0, lim_max = 0, v = 0;
    if (R_SUCCEEDED(svcGetInfo(&v, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0)))
    {
        const Handle rl = (Handle)v;
        s64 cur = 0, top = 0;
        if (R_SUCCEEDED(svcGetResourceLimitCurrentValue(&cur, rl, LimitableResource_Memory)))
            lim_used = (u64)cur >> 20;
        if (R_SUCCEEDED(svcGetResourceLimitLimitValue(&top, rl, LimitableResource_Memory)))
            lim_max = (u64)top >> 20;
        svcCloseHandle(rl);
    }
    fprintf(f, "==== report %d | %.1f s | CPU total %.0f%% of 300%% | threads %zu | samples %zu | memory limit %" PRIu64
               "/%" PRIu64 " MB\n",
            index, secs, total, order.size(), g_nsamples, lim_used, lim_max);
    for (unsigned c = 0; c < kCallCount; ++c)
    {
        const u64 n = g_calls[c].n.load() - calls_last[c][0];
        const u64 t = g_calls[c].ticks.load() - calls_last[c][1];
        const u64 b = g_calls[c].bytes.load() - calls_last[c][2];
        calls_last[c][0] += n;
        calls_last[c][1] += t;
        calls_last[c][2] += b;
        if (n)
            fprintf(f, "   call %-14s %8.0f/s  %8.3f ms/s inside%s", kCallNames[c], double(n) / secs,
                    double(t) * 1000.0 / double(freq) / secs, b ? "" : "\n");
        if (n && b)
            fprintf(f, "  %8.2f MB/s\n", double(b) / 1048576.0 / secs);
    }

    // samples grouped by thread
    std::sort(g_samples, g_samples + g_nsamples, [](const Sample& a, const Sample& b) { return a.slot < b.slot; });
    for (size_t i : order)
    {
        const Slot& s = g_slots[i];
        const u32 h = s.handle.load();
        s32 prio = -1, pref = -9;
        u64 mask = 0, tid = 0;
        svcGetThreadPriority(&prio, h);
        svcGetThreadCoreMask(&pref, &mask, h);
        svcGetThreadId(&tid, h);
        fprintf(f, "-- thread %" PRIu64 " CPU %5.1f%% | prio 0x%X pref core %d mask 0x%" PRIx64 " | entry ", tid, s.cpu,
                (unsigned)prio, (int)pref, mask);
        print_addr(f, s.entry);
        fprintf(f, " | created by");
        for (int k = 0; k < 4 && s.creator[k]; ++k)
        {
            fputc(' ', f);
            print_addr(f, s.creator[k]);
        }
        fputc('\n', f);

        auto lo = std::lower_bound(g_samples, g_samples + g_nsamples, i,
                                   [](const Sample& a, size_t v) { return a.slot < v; });
        auto hi = std::upper_bound(g_samples, g_samples + g_nsamples, i,
                                   [](size_t v, const Sample& a) { return v < a.slot; });
        const size_t n = size_t(hi - lo);
        if (!n)
            continue;
        std::vector<u64> pcs, lrs;
        size_t insvc = 0;
        for (auto it = lo; it != hi; ++it)
        {
            if (after_svc(it->pc))
            {
                ++insvc;
                continue;
            }
            pcs.push_back(it->pc);
            lrs.push_back(it->lr);
        }
        fprintf(f, "   %zu samples, %.0f%% waiting in the kernel\n", n, double(insvc) * 100.0 / double(n));
        const bool busy = s.cpu >= 2.0;
        const auto ph = histogram(pcs);
        for (size_t k = 0; k < ph.size() && k < (busy ? 40u : 3u); ++k)
        {
            fprintf(f, "   %5.1f%%  pc ", double(ph[k].n) * 100.0 / double(pcs.size()));
            print_addr(f, ph[k].key);
            fputc('\n', f);
        }
        const auto lh = histogram(lrs);
        for (size_t k = 0; k < lh.size() && k < (busy ? 20u : 2u); ++k)
        {
            fprintf(f, "   %5.1f%%  lr ", double(lh[k].n) * 100.0 / double(lrs.size()));
            print_addr(f, lh[k].key);
            fputc('\n', f);
        }
        // whole stacks, waits included: they tell what a thread waits for
        std::vector<const Sample*> st;
        for (auto it = lo; it != hi; ++it)
            st.push_back(&*it);
        std::sort(st.begin(), st.end(),
                  [](const Sample* a, const Sample* b) { return memcmp(a->frames, b->frames, sizeof(a->frames)) < 0; });
        std::vector<std::pair<const Sample*, u32>> groups;
        for (const Sample* m : st)
        {
            if (groups.empty() || memcmp(groups.back().first->frames, m->frames, sizeof(m->frames)) != 0)
                groups.push_back({m, 1});
            else
                ++groups.back().second;
        }
        std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t k = 0; k < groups.size() && k < (busy ? 15u : 2u); ++k)
        {
            fprintf(f, "   %5.1f%%  stack", double(groups[k].second) * 100.0 / double(n));
            for (size_t j = 0; j < kFrames && groups[k].first->frames[j]; ++j)
            {
                fputs(j ? " <- " : " ", f);
                print_addr(f, groups[k].first->frames[j]);
            }
            fputc('\n', f);
        }
    }
    fputc('\n', f);
    fclose(f);
    dump_raw(order, index);
    // a mark in the game's own log: which report covers which part of a run
    fprintf(stderr, "[nx prof] report %d done (%zu samples)\n", index, g_nsamples);
    g_nsamples = 0;
}

void prof_main(void*)
{
    const u64 freq = armGetSystemTickFreq();
    u64 calls_last[kCallCount][3] = {};
    if (FILE* f = fopen("nx_prof.log", "w"))
    {
        fprintf(f, "nx_prof: image text 0x%" PRIx64 "..0x%" PRIx64 " (img+ = ELF address), stacks %s, tick %" PRIu64
                   " Hz\n\n",
                g_text_lo, g_text_hi, g_stacks ? "on (1 kHz)" : "off", freq);
        fclose(f);
    }
    u64 start = armGetSystemTick();
    report(1, freq, calls_last, 0); // sets the CPU baselines
    start = armGetSystemTick();
    size_t rr = 0;
    int index = 1;
    for (;;)
    {
        __real_svcSleepThread(g_stacks ? 1000000LL : 100000000LL);
        if (g_stacks && g_nsamples < kMaxSamples)
        {
            for (size_t tries = 0; tries < kMaxThreads; ++tries)
            {
                const size_t idx = rr;
                rr = (rr + 1) % kMaxThreads;
                Slot& s = g_slots[idx];
                const u32 h = s.handle.load();
                // skip idle threads after the first report: more samples where the CPU goes
                if (!h || h == g_prof_thread.handle || (index > 1 && s.cpu < 0.5))
                    continue;
                if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused)))
                    break;
                ThreadContext ctx;
                if (R_SUCCEEDED(svcGetThreadContext3(&ctx, h)))
                {
                    Sample& out = g_samples[g_nsamples++];
                    memset(&out, 0, sizeof(out));
                    out.pc = ctx.pc.x;
                    out.lr = ctx.lr;
                    out.slot = (u16)idx;
                    MemoryInfo smi;
                    u32 spi = 0;
                    if (R_SUCCEEDED(svcQueryMemory(&smi, &spi, ctx.sp)) && (smi.perm & Perm_R))
                        walk_fp(ctx.fp, smi.addr, smi.addr + smi.size, out.frames, kFrames);
                }
                svcSetThreadActivity(h, ThreadActivity_Runnable);
                break;
            }
        }
        const u64 now = armGetSystemTick();
        if (now - start >= kReportSeconds * freq)
        {
            report(now - start, freq, calls_last, index++);
            start = armGetSystemTick();
        }
    }
}

#ifdef NX_PGO_GEN
} // namespace
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
namespace
{
// The game is closed with HOME, never through exit(): libgcov's atexit dump would never run. Dump every 180 s
// instead; libgcov merges into the .gcda already on the SD, so reset after each dump keeps the totals right.
Thread g_pgo_thread;
void pgo_main(void*)
{
    mkdir("sdmc:/ogsr_pgo", 0777);
    for (int n = 1;; ++n)
    {
        __real_svcSleepThread(180LL * 1000000000LL);
        const u64 t0 = armGetSystemTick();
        __gcov_dump();
        __gcov_reset();
        fprintf(stderr, "[nx pgo] dump %d written in %.1f s\n", n,
                double(armGetSystemTick() - t0) / double(armGetSystemTickFreq()));
    }
}
#endif

bool read_one_shot(const char* name, char* buf, size_t cap)
{
    buf[0] = 0;
    FILE* t = fopen(name, "r");
    if (!t)
        return false;
    const size_t n = fread(buf, 1, cap - 1, t);
    buf[n] = 0;
    fclose(t);
    remove(name);
    return true;
}
} // namespace

extern "C" {

void xr_nx_prof_set_pending(void* fn) { t_pending_fn = fn; }

// For xrCore's ResolveCodeAddress (no dladdr on Horizon): a code address as an offset into the NRO's .text, which
// `addr2line -e ogsr_vNNN.elf` turns into a name -- the raw address changes with every launch.
int xr_nx_describe_addr(const void* addr, char* buf, unsigned size)
{
    text_range();
    const u64 a = reinterpret_cast<u64>(addr);
    return in_text(a) ? snprintf(buf, size, "img+0x%" PRIx64, a - g_text_lo) : snprintf(buf, size, "0x%" PRIx64, a);
}

// Called from main() in x_ray.cpp before anything else creates threads.
void xr_nx_prof_init(void)
{
    text_range();
    const u64 none[4] = {};
    register_thread(envGetMainThreadHandle(), 0, none, 0x2C, 0);

    char buf[128];
    read_one_shot("nx_cores.txt", buf, sizeof(buf));
    if (strstr(buf, "spread"))
        g_core_policy = 1;
    else if (strstr(buf, "wide"))
        g_core_policy = 2;
    if (const char* p = strstr(buf, "rtprio="))
    {
        const long v = strtol(p + 7, nullptr, 0);
        if (v >= 0x2C && v <= 0x3F)
            g_rt_prio = (int)v;
    }
    xr_nx_overlay_init();
    if (read_one_shot("nx_prof.txt", buf, sizeof(buf)))
        g_armed = 1;
    if (strstr(buf, "stacks"))
        g_stacks = 1;
    fprintf(stderr, "[nx prof] armed %d stacks %d core policy %s render thread prio %s0x%X\n", g_armed, g_stacks,
            g_core_policy == 1 ? "spread" : g_core_policy == 2 ? "wide" : "keep", g_rt_prio ? "" : "keep ",
            g_rt_prio ? (unsigned)g_rt_prio : 0x3Bu);
#ifdef NX_PGO_GEN
    // lowest priority, core 2: the dump writes ~2000 files to the SD
    if (R_SUCCEEDED(__real_threadCreate(&g_pgo_thread, pgo_main, nullptr, nullptr, 0x10000, 0x3F, 2)))
        threadStart(&g_pgo_thread);
    fprintf(stderr, "[nx pgo] instrumented build: .gcda dump to sdmc:/ogsr_pgo every 180 s\n");
#endif
    if (!g_armed)
        return;
    if (g_stacks)
        g_samples = static_cast<Sample*>(malloc(sizeof(Sample) * kMaxSamples));
    if (g_stacks && !g_samples)
        g_stacks = 0;
    // 0x2A: above the game threads so samples are on time; core 2 so it does not take core 0 from the main thread.
    if (R_SUCCEEDED(__real_threadCreate(&g_prof_thread, prof_main, nullptr, nullptr, 0x20000, 0x2A, 2)))
        threadStart(&g_prof_thread);
}

Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz, int prio,
                           int cpuid)
{
    const Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
    if (R_FAILED(rc) || !t)
        return rc;
    text_range();
    u64 creator[4] = {};
    MemoryInfo smi;
    u32 spi = 0;
    const u64 fp = reinterpret_cast<u64>(__builtin_frame_address(0));
    if (R_SUCCEEDED(svcQueryMemory(&smi, &spi, fp)))
        walk_fp(fp, smi.addr, smi.addr + smi.size, creator, 4);
    const u64 fn = t_pending_fn ? reinterpret_cast<u64>(t_pending_fn) : reinterpret_cast<u64>(entry);
    register_thread(t->handle, fn, creator, prio, cpuid);

    s32 pref = -9;
    u64 mask = 0;
    svcGetThreadCoreMask(&pref, &mask, t->handle);
    // pthreads: libnx sets (-1, process mask) after this returns; their policy runs in xr_nx_prof_on_thread_start
    if (t_pending_fn)
        ;
    else if (g_core_policy == 1 && cpuid == -2)
    {
        const s32 core = 1 + (s32)(g_spread_next.fetch_add(1) & 1);
        if (R_SUCCEEDED(svcSetThreadCoreMask(t->handle, core, 0x7)))
            pref = core, mask = 0x7;
    }
    else if (g_core_policy == 2)
    {
        if (R_SUCCEEDED(svcSetThreadCoreMask(t->handle, -3 /* IdealCoreNoUpdate */, 0x7)))
            mask = 0x7;
    }
    char where[24];
    if (in_text(fn))
        snprintf(where, sizeof(where), "img+0x%" PRIx64, fn - g_text_lo);
    else
        snprintf(where, sizeof(where), "0x%" PRIx64, fn);
    fprintf(stderr, "[nx thr] create prio 0x%X cpuid %d -> pref %d mask 0x%" PRIx64 " entry %s stack %zu KB\n",
            (unsigned)prio, cpuid, (int)pref, mask, where, stack_sz >> 10);
    return rc;
}

int xr_nx_prof_wants_start_hook(void) { return g_core_policy == 1; }

// First thing on a new pthread (trampoline in xr_nx_throw.cpp), after libnx has set its mask.
void xr_nx_prof_on_thread_start(void* fn)
{
    if (g_core_policy != 1)
        return;
    const s32 core = 1 + (s32)(g_spread_next.fetch_add(1) & 1);
    const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 0x7);
    const u64 a = reinterpret_cast<u64>(fn);
    fprintf(stderr, "[nx thr] start %s0x%" PRIx64 " -> pref core %d mask 0x7 rc=0x%X\n", in_text(a) ? "img+" : "",
            in_text(a) ? a - g_text_lo : a, (int)core, rc);
}

// device.cpp rt_main, right after it pins itself to core 1. libnx gives every pthread 0x3B, the same priority as the
// TTAPI workers that may run on core 1: equal priorities share a core in 10 ms quanta, so a woken render thread can
// wait a whole quantum (nfsmw-nx measured 7-17 ms for a 1 ms wait). A lower number preempts them.
void xr_nx_render_thread_started(void)
{
    s32 before = -1;
    svcGetThreadPriority(&before, CUR_THREAD_HANDLE);
    Result rc = 0;
    if (g_rt_prio)
        rc = svcSetThreadPriority(CUR_THREAD_HANDLE, g_rt_prio);
    s32 after = -1, pref = -9;
    u64 mask = 0;
    svcGetThreadPriority(&after, CUR_THREAD_HANDLE);
    svcGetThreadCoreMask(&pref, &mask, CUR_THREAD_HANDLE);
    fprintf(stderr, "[nx rt] render thread prio 0x%X -> 0x%X (rc=0x%X), pref core %d mask 0x%" PRIx64 "\n",
            (unsigned)before, (unsigned)after, rc, (int)pref, mask);
}

Result __wrap_threadClose(Thread* t)
{
    if (t)
    {
        for (auto& s : g_slots)
        {
            u32 expected = t->handle;
            if (s.handle.compare_exchange_strong(expected, 0))
                break;
        }
    }
    return __real_threadClose(t);
}

Result __wrap_nvFenceWait(NvFence* f, s32 timeout_us)
{
    const u64 t0 = armGetSystemTick();
    const Result rc = __real_nvFenceWait(f, timeout_us);
    note(timeout_us == 0 ? kFencePoll : kFenceWait, t0);
    return rc;
}

Result __wrap_nvGpuChannelKickoff(NvGpuChannel* c)
{
    const u64 t0 = armGetSystemTick();
    const Result rc = __real_nvGpuChannelKickoff(c);
    note(kKickoff, t0);
    return rc;
}

Result __wrap_nwindowQueueBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence)
{
    const u64 t0 = armGetSystemTick();
    const Result rc = __real_nwindowQueueBuffer(nw, slot, fence);
    note(kQueueBuf, t0);
    if (R_SUCCEEDED(rc) && nw)
        xr_nx_overlay_present(nw->width, nw->height); // xr_nx_overlay.cpp: FPS for Status Monitor
    return rc;
}

Result __wrap_nwindowDequeueBuffer(NWindow* nw, s32* out_slot, NvMultiFence* out_fence)
{
    const u64 t0 = armGetSystemTick();
    const Result rc = __real_nwindowDequeueBuffer(nw, out_slot, out_fence);
    note(kDequeueBuf, t0);
    return rc;
}

void __wrap_svcSleepThread(s64 nano)
{
    const u64 t0 = armGetSystemTick();
    __real_svcSleepThread(nano);
    note(nano == 0 ? kSleep0 : nano < 0 ? kSleepYield : nano <= 1000000 ? kSleepShort : kSleepLong, t0);
}

void __wrap_armDCacheClean(void* addr, size_t size)
{
    const u64 t0 = armGetSystemTick();
    __real_armDCacheClean(addr, size);
    note(kDCacheClean, t0, size);
}

} // extern "C"
