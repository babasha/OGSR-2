#include "stdafx.h"

#include "lua_vm_watch.h"
#include "load_prof.h"
#include "xr_platform_time.h"
#include <chrono>
#include <algorithm>
#include "stacktrace_collector.h"

#include <atomic>
#include <mutex>
#include <set>
#include <string>

namespace lua_vm_watch
{
int enabled = 1;
int time_enabled = 0;   // консольный `lua_time` — хронометраж входов в VM
int wrapper_fastpath = 1;   // обход VM для непереопределённых виртуалок обёрток
int sample_enabled = 0;     // консольный `lua_sample` — сэмплирующий профайль по Lua-стеку

namespace
{
std::atomic<u64> g_fp_taken{0};
std::atomic<u64> g_fp_missed{0};
}

// ⚠relaxed и БЕЗ выравнивания по строкам кэша: это счётчик отчёта, а не рычаг.
// Шестнадцать воркеров бьются в две ячейки, но только пока `lua_fastpath` ведёт
// учёт -- см. гейт в вызывающем.
void wrapper_fastpath_note(bool taken)
{
    (taken ? g_fp_taken : g_fp_missed).fetch_add(1, std::memory_order_relaxed);
}
u64 wrapper_fastpath_taken() { return g_fp_taken.load(std::memory_order_relaxed); }
u64 wrapper_fastpath_missed() { return g_fp_missed.load(std::memory_order_relaxed); }

namespace
{
constexpr u32 kMaxThreads = 64;
constexpr u32 kMaxSitesPerThread = 16;
constexpr u32 kMaxConflictReports = 24; // a full stack trace each; the counter keeps running past this
constexpr u32 kMaxNearMissReports = 16;
constexpr u64 kNearMissReportGapMs = 5000;
constexpr u32 kMaxFirstSightTraces = 24;
constexpr u64 kConflictReportGapMs = 3000;

// ⚠These records are STATIC, not thread_local, on purpose: dump() reads them
// from the console thread long after a level-load worker has exited, and a
// thread_local record would be freed memory by then. The thread-local part is
// only the pointer and the nesting depth.
struct thread_rec
{
    u32 tid{};
    char name[64]{};
    u64 entries{};
    u64 ns{};        // ⭐время внутри VM на ЭТОМ потоке (только внешние входы)
    u64 ncalls{};    // сколько раз входил — VM одна, потоков два
    u64 conflicts{};
    u64 near_misses{};
    const char* sites[kMaxSitesPerThread]{};
    u32 site_count{};
};

thread_rec g_recs[kMaxThreads];
u32 g_rec_count{};
bool g_rec_overflow{};

struct tls_state
{
    thread_rec* rec{};
    u32 depth{};
    bool owns{};
    std::chrono::steady_clock::time_point t0{};   // начало ВНЕШНЕГО входа в VM
    u64 ns{};                                     // накоплено на этом потоке
    u64 calls{};                                  // внешних входов в VM
};
thread_local tls_state tls;

std::atomic<u32> g_owner{0}; // thread id currently inside the VM, 0 = free
std::atomic<const char*> g_owner_site{nullptr};
std::atomic<thread_rec*> g_owner_rec{nullptr};

std::mutex g_table_lock;
std::atomic<u64> g_conflicts{0};
std::atomic<u32> g_conflict_reports{0};
std::atomic<u64> g_last_report_ms{0};
std::atomic<u32> g_first_sight_traces{0};

// The frame's second-thread task, while it is in flight.
std::atomic<u32> g_worker_window{0};
std::atomic<u64> g_near_misses{0};
std::atomic<u32> g_near_miss_reports{0};
std::atomic<u64> g_last_near_miss_ms{0};

void capture_thread_name(char* out, size_t cap)
{
    out[0] = 0;

#ifdef _MSC_VER
    static HMODULE kernel = GetModuleHandle("kernel32.dll");
    using FuncGetThreadDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static auto get_desc = kernel ? (FuncGetThreadDescription)GetProcAddress(kernel, "GetThreadDescription") : nullptr;
    if (!get_desc)
        return;

    PWSTR wide{};
    if (FAILED(get_desc(GetCurrentThread(), &wide)) || !wide)
        return;

    WideCharToMultiByte(CP_ACP, 0, wide, -1, out, (int)cap, nullptr, nullptr);
    out[cap - 1] = 0;
    LocalFree(wide);
#else
    // Same /proc/self/task/<tid>/comm approach as cpu_budget.cpp's
    // thread_name() -- set by the kernel for every thread, no cooperation
    // needed from SetThreadDescription (which this codebase doesn't call
    // anyway; it relies on the OS default name from the thread's entry point).
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d/comm", (int)gettid());
    if (FILE* f = fopen(path, "r"))
    {
        if (fgets(out, (int)cap, f))
        {
            const size_t n = strlen(out);
            if (n && out[n - 1] == '\n')
                out[n - 1] = 0;
        }
        fclose(f);
    }
#endif
}

thread_rec* register_thread()
{
    const u32 tid = GetCurrentThreadId();

    std::lock_guard<std::mutex> lk(g_table_lock);
    if (g_rec_count >= kMaxThreads)
    {
        g_rec_overflow = true;
        return nullptr;
    }

    thread_rec* r = &g_recs[g_rec_count++];
    r->tid = tid;
    capture_thread_name(r->name, sizeof(r->name));
    if (!r->name[0])
        xr_strcpy(r->name, "unnamed");

    Msg("~[lua vm] thread %u [%s] is user #%u of the LuaJIT state", tid, r->name, g_rec_count);
    return r;
}

void report_conflict(thread_rec* mine, const char* site, u32 other_tid)
{
    const thread_rec* other = g_owner_rec.load(std::memory_order_relaxed);
    const char* other_site = g_owner_site.load(std::memory_order_relaxed);

    const u64 now = xrPlatform::TickCount64();
    if (g_conflict_reports.load(std::memory_order_relaxed) >= kMaxConflictReports)
        return;
    if (now - g_last_report_ms.load(std::memory_order_relaxed) < kConflictReportGapMs)
        return;
    g_last_report_ms.store(now, std::memory_order_relaxed);
    g_conflict_reports.fetch_add(1, std::memory_order_relaxed);

    Msg("!![lua vm] TWO THREADS IN ONE LuaJIT STATE (overlap #%llu)", g_conflicts.load(std::memory_order_relaxed));
    Msg("!![lua vm]   entering: thread %u [%s] via '%s'", mine ? mine->tid : GetCurrentThreadId(), mine ? mine->name : "?", site);
    Msg("!![lua vm]   already inside: thread %u [%s] via '%s'", other_tid, other ? other->name : "?", other_site ? other_site : "<just left>");

    // ⚠BuildStackTrace, NOT LogStackTrace. LogStackTrace runs the registered
    // crash handler, and this engine's crash handler is ScriptCrashHandler,
    // which walks the LUA stack (print_stack -> luaL_traceback). The diagnostic
    // would then do the exact thing it exists to catch -- enter the VM from the
    // thread that just lost the race -- and could take the process down with it.
    Log(BuildStackTrace("!![lua vm] stack of the thread that lost the race:\n"));
}

// Someone reached the VM while the second-thread task was in flight. They did
// not have to be inside Lua at the same instant for this to be the bug: the
// worker's task is a continuous stream of VM entries, and anything that enters
// alongside it is one scheduling accident away from landing in the middle of one.
void report_near_miss(thread_rec* mine, const char* site, u32 worker_tid)
{
    const u64 now = xrPlatform::TickCount64();
    if (g_near_miss_reports.load(std::memory_order_relaxed) >= kMaxNearMissReports)
        return;
    if (now - g_last_near_miss_ms.load(std::memory_order_relaxed) < kNearMissReportGapMs)
        return;
    g_last_near_miss_ms.store(now, std::memory_order_relaxed);
    g_near_miss_reports.fetch_add(1, std::memory_order_relaxed);

    Msg("!![lua vm] VM ENTERED WHILE THE SECOND-THREAD TASK IS IN FLIGHT (near miss #%llu)", g_near_misses.load(std::memory_order_relaxed));
    Msg("!![lua vm]   entering: thread %u [%s] via '%s'", mine ? mine->tid : GetCurrentThreadId(), mine ? mine->name : "?", site);
    Msg("!![lua vm]   second-thread task is running on thread %u", worker_tid);
    Log(BuildStackTrace("!![lua vm] stack of the thread entering alongside it:\n"));
}

void note_site(thread_rec* rec, const char* site)
{
    if (!rec)
        return;

    for (u32 i = 0; i < rec->site_count; ++i)
        if (rec->sites[i] == site)
            return;

    if (rec->site_count >= kMaxSitesPerThread)
        return;
    rec->sites[rec->site_count++] = site;

    Msg("~[lua vm] thread %u [%s] reaches the VM via '%s' (first time)", rec->tid, rec->name, site);

    if (enabled >= 2 && g_first_sight_traces.fetch_add(1, std::memory_order_relaxed) < kMaxFirstSightTraces)
        Log(BuildStackTrace("~[lua vm] entry path:\n"));
}
} // namespace

void enter(const char* site)
{
    tls_state& s = tls;
    const bool outermost = (s.depth++ == 0);
    // [sched prof] / [spawn prof]: the time inside the VM as one part, "lua"
    // (engine work the script calls back into is timed by its own scopes).
    if (outermost)
        LoadProf::Push("lua");
    // ⚠ДО проверки `enabled`: хронометраж не должен зависеть от того, включён ли
    // трекер гонок. Внутренние входы не считаем — время уже идёт у внешнего.
    if (outermost && time_enabled) {
        s.t0 = std::chrono::steady_clock::now();
        ++s.calls;
    }
    if (!outermost)
        return; // already inside on this thread: the outermost scope owns the decision

    if (!enabled)
    {
        s.owns = false;
        return;
    }

    if (!s.rec)
        s.rec = register_thread();
    if (s.rec)
        ++s.rec->entries;

    const u32 tid = GetCurrentThreadId();
    u32 other = 0;
    if (g_owner.compare_exchange_strong(other, tid, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        s.owns = true;
        g_owner_site.store(site, std::memory_order_relaxed);
        g_owner_rec.store(s.rec, std::memory_order_relaxed);
    }
    else
    {
        // `other` now holds whoever won. Observe and continue -- the point is to
        // learn the shape of the overlap, not to serialize it here.
        s.owns = false;
        if (s.rec)
            ++s.rec->conflicts;
        g_conflicts.fetch_add(1, std::memory_order_relaxed);
        report_conflict(s.rec, site, other);
    }

    // Separate question, and the one a quiet session can still answer: was the
    // frame's worker task running when we came in? A conflict is a near miss
    // that got unlucky; a near miss is the same race with better timing.
    if (const u32 worker = g_worker_window.load(std::memory_order_relaxed))
    {
        if (worker != tid)
        {
            if (s.rec)
                ++s.rec->near_misses;
            g_near_misses.fetch_add(1, std::memory_order_relaxed);
            report_near_miss(s.rec, site, worker);
        }
    }

    note_site(s.rec, site);
}

void leave() noexcept
{
    tls_state& s = tls;
    if (s.depth == 0)
        return; // unbalanced (watch switched on mid-call); nothing of ours to undo
    const bool outermost = (--s.depth == 0);
    if (outermost)
        LoadProf::Pop();
    if (outermost && time_enabled) {
        const u64 d = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - s.t0).count());
        s.ns += d;
        // ⭐И В ЗАПИСЬ ПОТОКА: VM одна, а входят в неё main и воркер ALife.
        // Сумма по вложенным pcall врёт (внешний включает внутренние), а это —
        // честное время на поток, без двойного счёта.
        if (s.rec) { s.rec->ns += d; ++s.rec->ncalls; }
    }
    if (!outermost)
        return;
    if (!s.owns)
        return;

    s.owns = false;
    g_owner_site.store(nullptr, std::memory_order_relaxed);
    g_owner_rec.store(nullptr, std::memory_order_relaxed);
    g_owner.store(0, std::memory_order_release);
}

u64 lua_time_ns() { return tls.ns; }

// Сколько РАЗ движок входил в VM на этом потоке (только внешние входы).
// Нужен, чтобы отличить "один дорогой скрипт" от "тысяча дешёвых вызовов":
// у NPC цена 0.42-0.55 мс за апдейт, и диагноз у этих двух случаев разный.
u64 lua_calls() { return tls.calls; }

// Единоличное владение: мы внутри VM, выиграли гонку за владение, и воркерная
// задача кадра сейчас не в полёте. Только в этот момент можно трогать
// диспетч-таблицу LuaJIT (установка/снятие хука) -- см. lua_sample в заголовке.
bool owns_vm()
{
    if (!tls.owns || tls.depth == 0)
        return false;
    const u32 worker = g_worker_window.load(std::memory_order_relaxed);
    return worker == 0 || worker == GetCurrentThreadId();
}

void worker_window_open() { g_worker_window.store(GetCurrentThreadId(), std::memory_order_relaxed); }

void worker_window_close() noexcept { g_worker_window.store(0, std::memory_order_relaxed); }

bool prof_armed() { return LoadProf::tls().armed != 0; }

void prof_push(const char* key)
{
    // LoadProf keeps names by pointer, so each distinct key is kept for good. The
    // set stays small (the functions a spawn or an NPC update reaches); past the cap
    // everything lands in one bucket rather than growing without end.
    static std::mutex mx;
    static std::set<std::string> names;
    const char* nm = "lua:(other)";
    {
        std::lock_guard<std::mutex> lk(mx);
        if (names.size() < 2048)
            nm = names.emplace(key).first->c_str();
        else if (const auto it = names.find(key); it != names.end())
            nm = it->c_str();
    }
    LoadProf::Push(nm);
}

void prof_pop() { LoadProf::Pop(); }

void dump()
{
    std::lock_guard<std::mutex> lk(g_table_lock);

    Msg("~[lua vm] watch=%d | threads that reached the VM: %u%s | overlaps: %llu (reported %u) | near misses: %llu (reported %u)", enabled, g_rec_count,
        g_rec_overflow ? " (TABLE FULL, some threads unlisted)" : "", g_conflicts.load(std::memory_order_relaxed), g_conflict_reports.load(std::memory_order_relaxed),
        g_near_misses.load(std::memory_order_relaxed), g_near_miss_reports.load(std::memory_order_relaxed));

    for (u32 i = 0; i < g_rec_count; ++i)
    {
        const thread_rec& r = g_recs[i];

        string512 sites{};
        for (u32 j = 0; j < r.site_count; ++j)
        {
            if (j)
                xr_strcat(sites, ", ");
            xr_strcat(sites, r.sites[j] ? r.sites[j] : "?");
        }

        Msg("~[lua vm]   thread %u [%s]: entries=%llu overlaps=%llu nearmiss=%llu via [%s]", r.tid, r.name, r.entries, r.conflicts, r.near_misses, sites);
    }

    if (const u32 cur = g_owner.load(std::memory_order_relaxed))
        Msg("~[lua vm]   the VM is held right now by thread %u", cur);
}
} // namespace lua_vm_watch

// --------------------------------------------------------------------------
// Пофункциональный профиль Lua (см. lua_vm_watch.h).
// ⚠Таблица фиксированная и без блокировок: пишет только тот поток, что вошёл в
// VM, а владелец VM в один момент один — это уже гарантирует трекер выше.
// Переполнение не растёт, а копится в "…overflow": лучше потерять хвост, чем
// аллоцировать на горячем пути замера.
// --------------------------------------------------------------------------
namespace lua_vm_watch
{
namespace
{
constexpr u32 kProfMax = 384;
constexpr u32 kProfFrames = 3;   // ra[0] всегда, ra[1..2] только под `lua_time 2`
struct ProfRec
{
    char  key[96]{};
    void* ra[kProfFrames]{};
    double ms{};
    u64   n{};
};
ProfRec  g_prof[kProfMax];
u32      g_prof_n = 0;
double   g_prof_overflow_ms = 0.0;
u64      g_prof_overflow_n = 0;
} // namespace

void lua_prof_add(const char* key, double ms, void* const* ra)
{
    if (!key || !key[0]) return;
    void* ra0 = ra ? ra[0] : nullptr;
    for (u32 i = 0; i < g_prof_n; ++i)
        if (g_prof[i].ra[0] == ra0 && 0 == xr_strcmp(g_prof[i].key, key)) { g_prof[i].ms += ms; ++g_prof[i].n; return; }
    if (g_prof_n >= kProfMax) { g_prof_overflow_ms += ms; ++g_prof_overflow_n; return; }
    ProfRec& r = g_prof[g_prof_n++];
    xr_strcpy(r.key, sizeof(r.key), key);
    for (u32 f = 0; f < kProfFrames; ++f) r.ra[f] = ra ? ra[f] : nullptr;
    r.ms = ms; r.n = 1;
}

void lua_prof_dump()
{
    if (!g_prof_n) return;
    u32 idx[kProfMax];
    for (u32 i = 0; i < g_prof_n; ++i) idx[i] = i;
    std::sort(idx, idx + g_prof_n, [](u32 a, u32 b) { return g_prof[a].ms > g_prof[b].ms; });
    double tot = 0.0; u64 totN = 0;
    for (u32 i = 0; i < g_prof_n; ++i) { tot += g_prof[i].ms; totN += g_prof[i].n; }
    for (u32 i = 0; i < g_rec_count; ++i) {
        thread_rec& r = g_recs[i];
        if (!r.ncalls) continue;
        Msg("[LuaProf] thread %-22s %8.2f ms | %8llu entries | %5.2f us/entry",
            r.name[0] ? r.name : "?", double(r.ns) / 1e6, (unsigned long long)r.ncalls,
            r.ncalls ? double(r.ns) / 1000.0 / double(r.ncalls) : 0.0);
        r.ns = 0; r.ncalls = 0;
    }
    if (const u64 fp = g_fp_taken.load(std::memory_order_relaxed) + g_fp_missed.load(std::memory_order_relaxed))
        Msg("[LuaProf] wrapper fastpath=%d: %llu calls skipped the VM, %llu really overridden (%.0f%% skipped)",
            wrapper_fastpath, (unsigned long long)g_fp_taken.load(std::memory_order_relaxed),
            (unsigned long long)g_fp_missed.load(std::memory_order_relaxed),
            100.0 * double(g_fp_taken.load(std::memory_order_relaxed)) / double(fp));
    g_fp_taken.store(0, std::memory_order_relaxed); g_fp_missed.store(0, std::memory_order_relaxed);
    Msg("[LuaProf] total %.2f ms over %llu calls, %u distinct (overflow %.2f ms / %llu)",
        tot, (unsigned long long)totN, g_prof_n, g_prof_overflow_ms, (unsigned long long)g_prof_overflow_n);
    const u32 n = g_prof_n < 20 ? g_prof_n : 20;
    for (u32 i = 0; i < n; ++i) {
        const ProfRec& r = g_prof[idx[i]];
        // ⚠Разрешение символа — ЗДЕСЬ, а не на горячем пути: SymGetSymFromAddr
        // стоит десятки микросекунд, и двадцать таких раз в пять секунд ничего
        // не значат, а двести тысяч раз в секунду сожгли бы кадр целиком.
        Msg("[LuaProf]   %-40s %8.2f ms | %7llu calls | %6.1f us/call | from %s",
            r.key, r.ms, (unsigned long long)r.n, r.n ? r.ms * 1000.0 / double(r.n) : 0.0,
            ResolveCodeAddress(r.ra[0]).c_str());
        for (u32 f = 1; f < kProfFrames; ++f)
            if (r.ra[f])
                Msg("[LuaProf]       %*s        ^-- %s", 40, "", ResolveCodeAddress(r.ra[f]).c_str());
    }
    for (u32 i = 0; i < g_prof_n; ++i) { g_prof[i].ms = 0.0; g_prof[i].n = 0; }
    g_prof_n = 0; g_prof_overflow_ms = 0.0; g_prof_overflow_n = 0;
}

// --------------------------------------------------------------------------
// Перехват долгих входов в VM на отрезке (см. lua_vm_watch.h).
// --------------------------------------------------------------------------
namespace
{
constexpr u32 kCapMax = 6;
struct CapRec { char key[96]; float ms; int depth; };
struct CapState
{
    bool on = false;
    int depth = 0;
    u32 n = 0;
    CapRec e[kCapMax];
};
thread_local CapState t_cap;
} // namespace

void capture_begin()
{
    t_cap.on = true;
    t_cap.depth = 0;
    t_cap.n = 0;
}

bool capture_active() { return t_cap.on; }
int capture_enter() { return t_cap.depth++; }
void capture_leave() { --t_cap.depth; }

void capture_note(const char* key, double ms, int depth)
{
    // Держим самые долгие: при переполнении вытесняется самая короткая запись.
    u32 slot = t_cap.n;
    if (slot >= kCapMax)
    {
        slot = 0;
        for (u32 i = 1; i < kCapMax; ++i)
            if (t_cap.e[i].ms < t_cap.e[slot].ms)
                slot = i;
        if (t_cap.e[slot].ms >= float(ms))
            return;
    }
    else
        ++t_cap.n;
    CapRec& r = t_cap.e[slot];
    xr_strcpy(r.key, sizeof(r.key), key ? key : "?");
    r.ms = float(ms);
    r.depth = depth;
}

void capture_end(char* buf, unsigned size)
{
    t_cap.on = false;
    if (!buf || !size)
        return;
    buf[0] = 0;
    CapRec* e[kCapMax];
    for (u32 i = 0; i < t_cap.n; ++i)
        e[i] = &t_cap.e[i];
    std::sort(e, e + t_cap.n, [](const CapRec* a, const CapRec* b) { return a->ms > b->ms; });
    int at = 0;
    for (u32 i = 0; i < t_cap.n && at >= 0 && unsigned(at) < size; ++i)
        at += snprintf(buf + at, size - at, "%s%s %.2f%s", i ? "; " : "", e[i]->key, e[i]->ms, e[i]->depth ? " (nested)" : "");
}

// --------------------------------------------------------------------------
// ⭐СЭМПЛИРУЮЩИЙ ПРОФАЙЛЬ LUA (см. lua_vm_watch.h).
//
// Две таблицы с ОДНОГО сэмпла: по функциям (кто) и по строкам (что внутри).
// ⚠Под замком, в отличие от pcall-профиля: хук срабатывает на ЛЮБОМ потоке,
// зашедшем в VM, и «владелец один» здесь не гарантия — шестнадцать воркеров
// ALife ходят в ту же VM. Замок стоит раз в N тысяч инструкций, это не горячий
// путь; а порванная таблица сделала бы отчёт бессмысленным.
// --------------------------------------------------------------------------
namespace
{
constexpr u32 kSampMax = 192;
struct SampRec { char key[96]{}; u64 n{}; };
SampRec  g_sampFunc[kSampMax];
SampRec  g_sampLine[kSampMax];
u32      g_sampFuncN = 0, g_sampLineN = 0;
u64      g_sampTotal = 0, g_sampLost = 0;
std::mutex g_sampLock;

void samp_bump(SampRec* tab, u32& n, const char* key)
{
    if (!key || !key[0]) return;
    for (u32 i = 0; i < n; ++i)
        if (0 == xr_strcmp(tab[i].key, key)) { ++tab[i].n; return; }
    if (n >= kSampMax) { ++g_sampLost; return; }
    xr_strcpy(tab[n].key, sizeof(tab[n].key), key);
    tab[n].n = 1;
    ++n;
}

void samp_print(const char* title, SampRec* tab, u32 n, u64 total)
{
    if (!n) return;
    u32 idx[kSampMax];
    for (u32 i = 0; i < n; ++i) idx[i] = i;
    std::sort(idx, idx + n, [tab](u32 a, u32 b) { return tab[a].n > tab[b].n; });
    const u32 show = n < 14 ? n : 14;
    for (u32 i = 0; i < show; ++i)
        Msg("[LuaSamp]   %-6s %-56s %6.2f%%  (%llu)", i ? "" : title,
            tab[idx[i]].key, 100.0 * double(tab[idx[i]].n) / double(total),
            (unsigned long long)tab[idx[i]].n);
}
} // namespace

void lua_sample_add(const char* func, const char* line)
{
    std::lock_guard<std::mutex> lk(g_sampLock);
    ++g_sampTotal;
    samp_bump(g_sampFunc, g_sampFuncN, func);
    samp_bump(g_sampLine, g_sampLineN, line);
}

void lua_sample_dump()
{
    std::lock_guard<std::mutex> lk(g_sampLock);
    if (!g_sampTotal) return;
    Msg("[LuaSamp] %llu samples (every %d k VM instructions), %u distinct functions, %llu lost to table overflow",
        (unsigned long long)g_sampTotal, sample_enabled, g_sampFuncN, (unsigned long long)g_sampLost);
    samp_print("FUNC:", g_sampFunc, g_sampFuncN, g_sampTotal);
    samp_print("LINE:", g_sampLine, g_sampLineN, g_sampTotal);
    for (u32 i = 0; i < g_sampFuncN; ++i) g_sampFunc[i].n = 0;
    for (u32 i = 0; i < g_sampLineN; ++i) g_sampLine[i].n = 0;
    g_sampFuncN = 0; g_sampLineN = 0; g_sampTotal = 0; g_sampLost = 0;
}

} // namespace lua_vm_watch
