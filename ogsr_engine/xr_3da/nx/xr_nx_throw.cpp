// Switch-only: log every C++ throw (type + call chain) to stderr.log, via `-Wl,--wrap=__cxa_throw`.
// The NRO has no debugger; an exception that ends in std::terminate otherwise leaves only its what() text.
// Addresses are module-relative once you subtract (self - <nm of __wrap___cxa_throw>): the line prints `self`.
#include <cstdio>
#include <typeinfo>
#include <unwind.h>
#include <pthread.h>
#include <new>
#include <thread>
#include <system_error>

extern "C" void __real___cxa_throw(void*, void*, void (*)(void*));

namespace
{
struct Trace
{
    int n;
};

_Unwind_Reason_Code on_frame(_Unwind_Context* ctx, void* arg)
{
    Trace* t = static_cast<Trace*>(arg);
    if (t->n++ >= 20)
        return _URC_END_OF_STACK;
    if (t->n > 1) // skip this function's own frame
        fprintf(stderr, "  #%d %p\n", t->n - 2, reinterpret_cast<void*>(_Unwind_GetIP(ctx)));
    return _URC_NO_REASON;
}

int g_throws;
} // namespace

extern "C" [[noreturn]] void __wrap___cxa_throw(void* ex, void* ti, void (*dest)(void*))
{
    if (g_throws++ < 200)
    {
        fprintf(stderr, "[throw] #%d type=%s self=%p\n", g_throws, static_cast<std::type_info*>(ti)->name(), reinterpret_cast<void*>(&__wrap___cxa_throw));
        Trace t{0};
        _Unwind_Backtrace(on_frame, &t);
    }
    __real___cxa_throw(ex, ti, dest);
    __builtin_unreachable();
}

// The caller's chain, same format as [throw] (subtract self = &__wrap___cxa_throw's nm address).
// For one-off diagnostics that need "who called this" -- e.g. [model late].
extern "C" void xr_nx_backtrace(const char* tag)
{
    fprintf(stderr, "[bt] %s self=%p\n", tag, reinterpret_cast<void*>(&__wrap___cxa_throw));
    Trace t{0};
    _Unwind_Backtrace(on_frame, &t);
}

// Answers "can this build start threads at all, and what does it say when it cannot" before the engine tries.
extern "C" void xr_nx_thread_selftest()
{
    pthread_t t;
    const int rc = pthread_create(&t, nullptr, [](void*) -> void* { return nullptr; }, nullptr);
    fprintf(stderr, "[ogsr/switch] selftest: pthread_create rc=%d\n", rc);
    if (rc == 0)
        pthread_join(t, nullptr);
    try
    {
        std::thread th([] {});
        fprintf(stderr, "[ogsr/switch] selftest: std::thread constructed, joinable=%d\n", (int)th.joinable());
        if (th.joinable())
            th.join();
        fprintf(stderr, "[ogsr/switch] selftest: std::thread ok\n");
    }
    catch (const std::system_error& e)
    {
        fprintf(stderr, "[ogsr/switch] selftest: std::thread threw system_error code=%d %s\n", e.code().value(), e.what());
    }
    catch (...)
    {
        fprintf(stderr, "[ogsr/switch] selftest: std::thread threw something else\n");
    }
    try
    {
        std::thread th([] {});
        th.detach();
        fprintf(stderr, "[ogsr/switch] selftest: std::thread detach ok\n");
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "[ogsr/switch] selftest: detach threw: %s\n", e.what());
    }
    catch (...)
    {
        fprintf(stderr, "[ogsr/switch] selftest: detach threw something else\n");
    }
}

// devkitA64's libstdc++ throws system_error out of std::thread::detach() (measured 30-09: create+join work,
// detach throws and the process dies -- the engine detaches its watchdog/worker threads). Replace it with the
// plain pthread call; std::thread is one pthread_t (checked below), so its storage is that handle.
static_assert(sizeof(std::thread) == sizeof(pthread_t), "std::thread is not a bare pthread_t here");

extern "C" void __wrap__ZNSt6thread6detachEv(void* self)
{
    pthread_t* h = static_cast<pthread_t*>(self);
    static int s_n;
    if (*h == pthread_t())
    {
        if (s_n++ < 4)
            fprintf(stderr, "[detach] thread not joinable -- ignored\n");
        return;
    }
    const int rc = pthread_detach(*h);
    if (s_n++ < 4)
        fprintf(stderr, "[detach] pthread_detach rc=%d\n", rc);
    *h = pthread_t(); // the std::thread is no longer joinable, as after a real detach
}

// --- worker stack size ------------------------------------------------------------------------------------------
// Loading a save crashed in dxQuickStepper (ODE builds its work arrays with alloca) on a worker thread: Data Abort in
// memset right next to the thread's stack, i.e. the default pthread stack of this libnx is too small for a physics
// island of a real save (30-09, crash report 01737592849). Every pthread now gets at least 2 MB.
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" void xr_nx_prof_set_pending(void* fn); // xr_nx_prof.cpp: names the thread its threadCreate registers
extern "C" int xr_nx_prof_wants_start_hook(void);  // nx_cores.txt asks for a placement policy
extern "C" void xr_nx_prof_on_thread_start(void* fn); // applies it on the new thread itself
namespace
{
// libnx's pthread_create widens the core mask AFTER threadCreate returns, so a placement policy has to run on the new
// thread, before its own code: this trampoline does that and then calls the real start routine.
struct StartHook
{
    void* (*fn)(void*);
    void* arg;
};
void* start_hook(void* p)
{
    const StartHook h = *static_cast<StartHook*>(p);
    delete static_cast<StartHook*>(p);
    xr_nx_prof_on_thread_start(reinterpret_cast<void*>(h.fn));
    return h.fn(h.arg);
}
} // namespace
extern "C" int __wrap_pthread_create(pthread_t* t, const pthread_attr_t* a, void* (*fn)(void*), void* arg)
{
    struct PendingFn
    {
        explicit PendingFn(void* f) { xr_nx_prof_set_pending(f); }
        ~PendingFn() { xr_nx_prof_set_pending(nullptr); }
    } pending(reinterpret_cast<void*>(fn));
    if (xr_nx_prof_wants_start_hook())
    {
        if (StartHook* h = new (std::nothrow) StartHook{fn, arg})
        {
            pthread_attr_t na;
            size_t sz = 0;
            if (a)
            {
                na = *a;
                pthread_attr_getstacksize(&na, &sz);
            }
            else
                pthread_attr_init(&na);
            if (sz < 2u * 1024u * 1024u)
                pthread_attr_setstacksize(&na, 2u * 1024u * 1024u);
            const int rc = __real_pthread_create(t, &na, start_hook, h);
            if (rc != 0)
                delete h;
            return rc;
        }
    }
    constexpr size_t kMinStack = 2u * 1024u * 1024u;
    pthread_attr_t na;
    size_t sz = 0;
    if (a)
    {
        na = *a;
        pthread_attr_getstacksize(&na, &sz);
    }
    else
        pthread_attr_init(&na);
    if (sz < kMinStack)
        pthread_attr_setstacksize(&na, kMinStack);
    return __real_pthread_create(t, &na, fn, arg);
}

// --- who asks for the big blocks? -------------------------------------------------------------------------------
// Horizon has one 3.1 GB pool shared by CPU heap and (through NVK) GPU buffers. Every request >= 32 MB is logged
// with its call chain (`-Wl,--wrap=malloc` & co.), first 80 only. Guarded against recursion: fprintf may allocate.
#include <cstdlib>
#include <cstring>
extern "C" {
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void* __real_memalign(size_t, size_t);
void* __real_aligned_alloc(size_t, size_t);
int __real_posix_memalign(void**, size_t, size_t);
void __real_free(void*);
// libnx (svc.s); declared here so this file does not pull <switch.h> in
unsigned svcGetCurrentProcessorNumber(void);
unsigned svcGetThreadPriority(int* priority, unsigned handle);
extern int g_xr_malloc_slow_us; // [malloc slow] threshold, us (console nx_malloc_slow_us)
}

extern "C" {
// bytes and counts by request size: <4K, <64K, <256K, <1M, <2M, >=2M
unsigned long long g_xr_bucket_bytes[6];
unsigned long long g_xr_bucket_count[6];
}

namespace
{
inline void count_alloc(size_t n)
{
    const int b = n < (4u << 10) ? 0 : n < (64u << 10) ? 1 : n < (256u << 10) ? 2 : n < (1u << 20) ? 3 : n < (2u << 20) ? 4 : 5;
    // Statistics only: relaxed load + store instead of an atomic add. The A57 has no LSE, so fetch_add is an
    // LDXR/STXR retry loop on a line every allocating thread shares (nfsmw-nx: 83k fetch_add per frame = 1.1 ms).
    // Two threads allocating at once may lose a count; nothing depends on it being exact.
    __atomic_store_n(&g_xr_bucket_bytes[b], __atomic_load_n(&g_xr_bucket_bytes[b], __ATOMIC_RELAXED) + n, __ATOMIC_RELAXED);
    __atomic_store_n(&g_xr_bucket_count[b], __atomic_load_n(&g_xr_bucket_count[b], __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
}
constexpr size_t kBigAlloc = 4u << 20;
__thread int t_in_hook;
int g_big_count;
size_t g_big_total;

_Unwind_Reason_Code big_frame(_Unwind_Context* ctx, void* arg)
{
    int* n = static_cast<int*>(arg);
    if ((*n)++ >= 16) // 7 ended inside r_open -- never reached who asked for the file
        return _URC_END_OF_STACK;
    if (*n > 2) // skip the unwinder callback and the wrapper itself
        fprintf(stderr, "  #%d %p\n", *n - 3, reinterpret_cast<void*>(_Unwind_GetIP(ctx)));
    return _URC_NO_REASON;
}

void note_big(const char* what, size_t n, size_t align, void* self)
{
    count_alloc(n);
    if (n < kBigAlloc || t_in_hook || g_big_count >= 400)
        return;
    t_in_hook = 1;
    ++g_big_count;
    g_big_total += n;
    fprintf(stderr, "[big] %s size=%zu KB align=%zu total=%zu MB self=%p\n", what, n >> 10, align, g_big_total >> 20, self);
    int depth = 0;
    _Unwind_Backtrace(big_frame, &depth);
    t_in_hook = 0;
}

// --- [malloc slow]: which allocator calls take milliseconds -----------------------------------------------------
// 📏11-10 (au7, a fight): the main thread (frame_move 99 ms) and the second thread (ALife 243 ms) stalled in the SAME
// frames, next to an 8 MB memalign and two HUD models loading in the background; Lua GC steps of 10 freed objects
// took 4.6-12 ms, one finalizer 10 ms. Everything on the Switch shares newlib's malloc and its one lock (no mimalloc:
// no mmap). This times every call (two cntpct reads, ~40 cycles) and prints the slow ones -- op, size, ms, core,
// priority, call chain -- so one run says whether the allocator is the common cause. g_xr_malloc_slow_us 0 = off.
inline unsigned long long ticks()
{
    unsigned long long t;
    asm volatile("mrs %0, cntpct_el0" : "=r"(t)); // 19.2 MHz
    return t;
}
int g_slow_printed;

void note_slow(const char* what, size_t n, unsigned long long t0, void* self)
{
    const unsigned long long dt = ticks() - t0;
    // 19.2 ticks per us = 96 / 5
    if (g_xr_malloc_slow_us <= 0 || dt * 5ull < static_cast<unsigned long long>(g_xr_malloc_slow_us) * 96ull)
        return;
    if (t_in_hook || g_slow_printed >= 300)
        return;
    t_in_hook = 1;
    ++g_slow_printed;
    int prio = -1;
    svcGetThreadPriority(&prio, 0xFFFF8000u); // CUR_THREAD_HANDLE
    fprintf(stderr, "[malloc slow] %s %zu B took %.2f ms | core %u prio 0x%X | #%d self=%p\n", what, n, double(dt) / 19200.0,
            svcGetCurrentProcessorNumber(), prio, g_slow_printed, self);
    int depth = 0;
    _Unwind_Backtrace(big_frame, &depth);
    t_in_hook = 0;
}
} // namespace

extern "C" {
int g_xr_malloc_slow_us = 5000; // 2000 for a hunt: each printed line carries a call chain

void* __wrap_malloc(size_t n)
{
    note_big("malloc", n, 0, reinterpret_cast<void*>(&__wrap_malloc));
    const unsigned long long t0 = ticks();
    void* r = __real_malloc(n);
    note_slow("malloc", n, t0, reinterpret_cast<void*>(&__wrap_malloc));
    return r;
}
void* __wrap_calloc(size_t a, size_t b)
{
    note_big("calloc", a * b, 0, reinterpret_cast<void*>(&__wrap_malloc));
    const unsigned long long t0 = ticks();
    void* r = __real_calloc(a, b);
    note_slow("calloc", a * b, t0, reinterpret_cast<void*>(&__wrap_malloc));
    return r;
}
void* __wrap_realloc(void* p, size_t n)
{
    note_big("realloc", n, 0, reinterpret_cast<void*>(&__wrap_malloc));
    const unsigned long long t0 = ticks();
    void* r = __real_realloc(p, n);
    note_slow(n ? "realloc" : "realloc(free)", n, t0, reinterpret_cast<void*>(&__wrap_malloc));
    return r;
}
void* __wrap_memalign(size_t al, size_t n)
{
    note_big("memalign", n, al, reinterpret_cast<void*>(&__wrap_malloc));
    const unsigned long long t0 = ticks();
    void* r = __real_memalign(al, n);
    note_slow("memalign", n, t0, reinterpret_cast<void*>(&__wrap_malloc));
    return r;
}
void* __wrap_aligned_alloc(size_t al, size_t n)
{
    note_big("aligned_alloc", n, al, reinterpret_cast<void*>(&__wrap_malloc));
    const unsigned long long t0 = ticks();
    void* r = __real_aligned_alloc(al, n);
    note_slow("aligned_alloc", n, t0, reinterpret_cast<void*>(&__wrap_malloc));
    return r;
}
int __wrap_posix_memalign(void** out, size_t al, size_t n)
{
    note_big("posix_memalign", n, al, reinterpret_cast<void*>(&__wrap_malloc));
    const unsigned long long t0 = ticks();
    const int r = __real_posix_memalign(out, al, n);
    note_slow("posix_memalign", n, t0, reinterpret_cast<void*>(&__wrap_malloc));
    return r;
}
void __wrap_free(void* p)
{
    const unsigned long long t0 = ticks();
    __real_free(p);
    note_slow("free", 0, t0, reinterpret_cast<void*>(&__wrap_malloc));
}
}

// --- vkCmdDraw*IndirectCount on a GPU that does not have it ---------------------------------------------------------
// NVK exposes drawIndirectCount only from Turing (nvk_physical_device.c: `cls_eng3d >= TURING_A`); the Switch's GM20B
// is Maxwell, and calling the function anyway wedges the GPU (30-09). The renderer's GPU-driven passes call it directly,
// so the linker swaps it (`-Wl,--wrap=`) for the plain multi-draw.
//
// First version drew ALL maxDrawCount commands (every group's whole region: ~830k per pass on Kordon) and a world frame
// took 8-10 s. The count lives in memory the CPU can read (Horizon RAM is unified and every indirect buffer is mapped and
// registered here by CVulkanBuffer::Create), so the wrapper reads the LAST count the GPU wrote and draws only that many:
//   n = 0 if highWater == 0 else min(maxDrawCount, 1.5 * highWater + 2); highWater = max(count now, previous decayed ~3 % per call).
// ⚠The value read is one to two frames old (the GPU writes this frame's count after this command is recorded), hence the
// margin and the slow decay: when the view turns, a group can be short of commands for a frame or two (pop-in), never
// beyond maxDrawCount, never out of bounds. The first frame draws nothing (counts are zero until the cull has run once).
#include <vulkan/vulkan_core.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
struct IndirectMap
{
    volatile uint8_t* ptr;
    uint64_t size;
    std::vector<uint32_t> high; // per 4-byte slot
};
std::mutex g_indirect_mtx;
} // namespace
namespace
{
std::unordered_map<uint64_t, IndirectMap> g_indirect;
uint64_t g_indirect_clamped_calls, g_indirect_full_calls, g_indirect_drawn, g_indirect_max;
// Bumped (under the mutex) on every register/unregister. The recording thread keeps its last lookup and only takes the
// mutex when the map changed: one pass draws ~100 groups from the same count buffer, ~1100 calls a frame, and each
// used to lock + hash. unordered_map nodes do not move on rehash, so a cached element stays valid until an erase,
// which bumps the generation. (Commands are recorded by one thread at a time; the stats are approximate anyway.)
std::atomic<uint32_t> g_indirect_gen{1};
struct IndirectLookup
{
    uint64_t buffer = 0;
    uint32_t gen = 0;
    IndirectMap* map = nullptr;
};
thread_local IndirectLookup t_indirect_last;

uint32_t effective_count(VkBuffer countBuffer, VkDeviceSize countOffset, uint32_t maxDraw)
{
    const uint64_t key = reinterpret_cast<uint64_t>(countBuffer);
    IndirectLookup& c = t_indirect_last;
    const uint32_t gen = g_indirect_gen.load(std::memory_order_acquire);
    if (c.buffer != key || c.gen != gen)
    {
        std::lock_guard<std::mutex> lk(g_indirect_mtx);
        auto it = g_indirect.find(key);
        c.buffer = key;
        c.gen = g_indirect_gen.load(std::memory_order_relaxed);
        c.map = it == g_indirect.end() ? nullptr : &it->second;
    }
    if (!c.map || countOffset + 4 > c.map->size)
    {
        ++g_indirect_full_calls;
        g_indirect_max += maxDraw;
        g_indirect_drawn += maxDraw;
        return maxDraw;
    }
    IndirectMap& m = *c.map;
    uint32_t cur = *reinterpret_cast<volatile const uint32_t*>(m.ptr + countOffset);
    if (cur > maxDraw)
        cur = maxDraw;
    uint32_t& h = m.high[size_t(countOffset >> 2)];
    const uint32_t decayed = h - (h >> 5);
    h = cur > decayed ? cur : decayed;
    // An empty group draws NOTHING (a zero-size indirect command still costs tens of microseconds on Maxwell -- 175k of
    // them were a 7 s frame). A group with survivors gets +50 % headroom for what the stale count misses.
    const uint64_t want = h ? uint64_t(h) + (h >> 1) + 2 : 0;
    const uint32_t n = want < maxDraw ? uint32_t(want) : maxDraw;
    ++g_indirect_clamped_calls;
    g_indirect_max += maxDraw;
    g_indirect_drawn += n;
    return n;
}
} // namespace

extern "C" {
// Called from CVulkanBuffer (Switch only): the buffer's persistent mapping, for the wrappers below.
void xr_nx_indirect_register(uint64_t buffer, void* ptr, uint64_t size)
{
    std::lock_guard<std::mutex> lk(g_indirect_mtx);
    IndirectMap& m = g_indirect[buffer];
    m.ptr = static_cast<volatile uint8_t*>(ptr);
    m.size = size;
    m.high.assign(size_t(size >> 2) + 1, 0u);
    g_indirect_gen.fetch_add(1, std::memory_order_release);
}
int xr_nx_indirect_unregister(uint64_t buffer)
{
    std::lock_guard<std::mutex> lk(g_indirect_mtx);
    const int n = int(g_indirect.erase(buffer));
    g_indirect_gen.fetch_add(1, std::memory_order_release);
    return n;
}
// Totals for the sampler line: calls with a real count, calls without, commands drawn vs. the maximum asked for.
void xr_nx_indirect_stats(uint64_t out[4])
{
    std::lock_guard<std::mutex> lk(g_indirect_mtx);
    out[0] = g_indirect_clamped_calls;
    out[1] = g_indirect_full_calls;
    out[2] = g_indirect_drawn;
    out[3] = g_indirect_max;
}

// 01-10 (v91): our NVK now implements *IndirectCount on GM20B (nvk_mme_draw_indirect_count_fermi): the GPU-written count
// reaches the macro and commands past it are consumed, not drawn. n stays the CPU-side upper bound (how many commands are
// pushed to the front end); the stale tail no longer costs raster (-nx_bisect measured ~6 ms of 23.5). -nx_mmecount0 = the
// old plain multi-draw of n commands, for A/B.
void __real_vkCmdDrawIndexedIndirectCount(VkCommandBuffer, VkBuffer, VkDeviceSize, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
void __real_vkCmdDrawIndirectCount(VkCommandBuffer, VkBuffer, VkDeviceSize, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
int g_xr_nx_mme_count = 1;

void __wrap_vkCmdDrawIndexedIndirectCount(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer,
                                          VkDeviceSize countOffset, uint32_t maxDrawCount, uint32_t stride)
{
    const uint32_t n = maxDrawCount ? effective_count(countBuffer, countOffset, maxDrawCount) : 0;
    if (!n)
        return;
    if (g_xr_nx_mme_count)
        __real_vkCmdDrawIndexedIndirectCount(cmd, buffer, offset, countBuffer, countOffset, n, stride);
    else
        vkCmdDrawIndexedIndirect(cmd, buffer, offset, n, stride);
}

void __wrap_vkCmdDrawIndirectCount(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer,
                                   VkDeviceSize countOffset, uint32_t maxDrawCount, uint32_t stride)
{
    const uint32_t n = maxDrawCount ? effective_count(countBuffer, countOffset, maxDrawCount) : 0;
    if (!n)
        return;
    if (g_xr_nx_mme_count)
        __real_vkCmdDrawIndirectCount(cmd, buffer, offset, countBuffer, countOffset, n, stride);
    else
        vkCmdDrawIndirect(cmd, buffer, offset, n, stride);
}

// nfs35: transfer / barrier audit. Every copy, fill, update, blit, clear and query reset the engine records is (in NVK on
// GM20B) work on another engine class of the same channel, and every barrier a wait-for-idle; the nfsmw-nx port measured
// copy-engine use at +1.12 ms. Counted per kind; for the transfer kinds the call site (return address) too, so the log
// can name who does it: addr2line on the ELF at (offset + address of xr_nx_xfer_take).
enum { XF_COPYBUF, XF_FILL, XF_UPDATE, XF_IMG2BUF, XF_BUF2IMG, XF_COPYIMG, XF_BLIT, XF_CLEAR, XF_QRESET, XF_BARRIER,
       XF_BARRIER2, XF_DISPATCH, XF_N };
static std::atomic<uint32_t> g_xfer[XF_N];
static std::mutex g_xfer_mtx;
static uintptr_t g_xfer_ra[128];
static uint32_t  g_xfer_rc[128];
static uint8_t   g_xfer_rk[128];
static void xfer_note(int kind, void* ra)
{
    g_xfer[kind].fetch_add(1, std::memory_order_relaxed);
    if (kind >= XF_BARRIER) return;
    std::lock_guard<std::mutex> lk(g_xfer_mtx);
    const uintptr_t a = (uintptr_t)ra;
    uint32_t h = (uint32_t)((a >> 2) * 2654435761u) & 127u;
    for (int p = 0; p < 128; ++p, h = (h + 1) & 127u) {
        if (g_xfer_ra[h] == a) { ++g_xfer_rc[h]; return; }
        if (!g_xfer_ra[h]) { g_xfer_ra[h] = a; g_xfer_rc[h] = 1; g_xfer_rk[h] = (uint8_t)kind; return; }
    }
}
// Take (and reset) the tallies: counts per kind, the top-n call sites by count (offset to this function, kind, count).
void xr_nx_xfer_take(uint32_t cnt[XF_N], int64_t off[16], uint32_t rcnt[16], uint32_t rkind[16])
{
    for (int k = 0; k < XF_N; ++k) cnt[k] = g_xfer[k].exchange(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_xfer_mtx);
    for (int t = 0; t < 16; ++t) {
        int best = -1;
        for (int i = 0; i < 128; ++i)
            if (g_xfer_ra[i] && g_xfer_rc[i] && (best < 0 || g_xfer_rc[i] > g_xfer_rc[best])) best = i;
        if (best < 0) { off[t] = 0; rcnt[t] = 0; rkind[t] = 0; continue; }
        off[t] = (int64_t)g_xfer_ra[best] - (int64_t)(uintptr_t)&xr_nx_xfer_take;
        rcnt[t] = g_xfer_rc[best]; rkind[t] = g_xfer_rk[best];
        g_xfer_rc[best] = 0;   // taken
    }
    for (int i = 0; i < 128; ++i) { g_xfer_ra[i] = 0; g_xfer_rc[i] = 0; }
}
void __real_vkCmdCopyBuffer(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy*);
void __wrap_vkCmdCopyBuffer(VkCommandBuffer c, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r)
{ xfer_note(XF_COPYBUF, __builtin_return_address(0)); __real_vkCmdCopyBuffer(c, s, d, n, r); }
void __real_vkCmdFillBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, uint32_t);
void __wrap_vkCmdFillBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize s, uint32_t d)
{ xfer_note(XF_FILL, __builtin_return_address(0)); __real_vkCmdFillBuffer(c, b, o, s, d); }
void __real_vkCmdUpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void*);
void __wrap_vkCmdUpdateBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize s, const void* d)
{ xfer_note(XF_UPDATE, __builtin_return_address(0)); __real_vkCmdUpdateBuffer(c, b, o, s, d); }
void __real_vkCmdCopyImageToBuffer(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy*);
void __wrap_vkCmdCopyImageToBuffer(VkCommandBuffer c, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy* r)
{ xfer_note(XF_IMG2BUF, __builtin_return_address(0)); __real_vkCmdCopyImageToBuffer(c, i, l, b, n, r); }
void __real_vkCmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy*);
void __wrap_vkCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy* r)
{ xfer_note(XF_BUF2IMG, __builtin_return_address(0)); __real_vkCmdCopyBufferToImage(c, b, i, l, n, r); }
void __real_vkCmdCopyImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy*);
void __wrap_vkCmdCopyImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy* r)
{ xfer_note(XF_COPYIMG, __builtin_return_address(0)); __real_vkCmdCopyImage(c, s, sl, d, dl, n, r); }
void __real_vkCmdBlitImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit*, VkFilter);
void __wrap_vkCmdBlitImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit* r, VkFilter f)
{ xfer_note(XF_BLIT, __builtin_return_address(0)); __real_vkCmdBlitImage(c, s, sl, d, dl, n, r, f); }
void __real_vkCmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*);
void __wrap_vkCmdClearColorImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearColorValue* v, uint32_t n, const VkImageSubresourceRange* r)
{ xfer_note(XF_CLEAR, __builtin_return_address(0)); __real_vkCmdClearColorImage(c, i, l, v, n, r); }
void __real_vkCmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t);
void __wrap_vkCmdResetQueryPool(VkCommandBuffer c, VkQueryPool q, uint32_t f, uint32_t n)
{ xfer_note(XF_QRESET, __builtin_return_address(0)); __real_vkCmdResetQueryPool(c, q, f, n); }
void __real_vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t,
                                 const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*);
void __wrap_vkCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm,
                                 const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni, const VkImageMemoryBarrier* i)
{ xfer_note(XF_BARRIER, nullptr); __real_vkCmdPipelineBarrier(c, s, d, f, nm, m, nb, b, ni, i); }
void __real_vkCmdPipelineBarrier2(VkCommandBuffer, const VkDependencyInfo*);
void __wrap_vkCmdPipelineBarrier2(VkCommandBuffer c, const VkDependencyInfo* d)
{ xfer_note(XF_BARRIER2, nullptr); __real_vkCmdPipelineBarrier2(c, d); }
void __real_vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
void __wrap_vkCmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z)
{ xfer_note(XF_DISPATCH, nullptr); __real_vkCmdDispatch(c, x, y, z); }

// --- [vk mem slow]: GPU memory allocations that take milliseconds ---------------------------------------------------
// 📏11-10 (early_gc2_new2): rt_wait stalls of 180-590 ms in fights, the render thread idle, next to an 8 MB memalign
// whose chain is VMA CreateBlock -> vkAllocateMemory -> nvk -> nouveau_horizon_memory_create. The memalign itself is
// fast ([malloc slow] silent), so: how long is the whole vkAllocateMemory (nvmap create + GPU map), and on which thread
// -- VMA holds the memory type's block-vector mutex across it, so every other allocation of that type waits.
VkResult __real_vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*);
void __real_vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*);
int g_xr_vkmem_slow_us = 3000;
static int g_vkmem_printed;
static void vkmem_note(const char* what, unsigned long long size, unsigned long long t0)
{
    const unsigned long long dt = ticks() - t0;
    if (g_xr_vkmem_slow_us <= 0 || dt * 5ull < static_cast<unsigned long long>(g_xr_vkmem_slow_us) * 96ull || g_vkmem_printed >= 200)
        return;
    ++g_vkmem_printed;
    int prio = -1;
    svcGetThreadPriority(&prio, 0xFFFF8000u);
    fprintf(stderr, "[vk mem slow] %s %llu KB took %.2f ms | core %u prio 0x%X | #%d\n", what, size >> 10, double(dt) / 19200.0,
            svcGetCurrentProcessorNumber(), prio, g_vkmem_printed);
}
VkResult __wrap_vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* cb, VkDeviceMemory* out)
{
    const unsigned long long t0 = ticks();
    const VkResult r = __real_vkAllocateMemory(d, ai, cb, out);
    vkmem_note("vkAllocateMemory", ai ? ai->allocationSize : 0, t0);
    return r;
}
void __wrap_vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks* cb)
{
    const unsigned long long t0 = ticks();
    __real_vkFreeMemory(d, m, cb);
    vkmem_note("vkFreeMemory", 0, t0);
}
}
