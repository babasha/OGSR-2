#include "StdAfx.h"
#include "goap_stats.h"
#include <chrono>

namespace GoapStats
{
int enabled = 0;
int use_cache = 0;

unsigned g_solves = 0;
unsigned g_replans = 0;
unsigned g_eval_actual = 0;
unsigned g_eval_search = 0;
unsigned g_hits = 0;

namespace
{
u32 s_last_ms = 0;

// ---- goap_stats 2: время по условиям/действиям (см. goap_stats.h) ----
struct ProfRec
{
    const char* cpp{}; // C++-класс (typeid) / имя условия
    const char* lua{}; // Lua-класс скриптовой обёртки или nullptr
    u64 self_ns{};
    u64 incl_ns{};
    u32 calls{};
};
struct ProfKeyHash
{
    size_t operator()(const std::pair<u64, u64>& k) const noexcept { return size_t(k.first * 0x9E3779B97F4A7C15ull ^ k.second); }
};
// ключ: (kind<<32 | id, указатель на Lua-класс либо C++-имя)
xr_unordered_map<std::pair<u64, u64>, ProfRec, ProfKeyHash> s_prof;
u64 s_child[64];
const char* s_label[64];
ProfKind s_kind[64];
int s_depth = 0;
u32 s_frames0 = 0;

void ProfReport(double sec)
{
    if (s_prof.empty())
        return;
    static const char* kinds[PK_COUNT] = {"eval", "exec", "init", "solve"};
    double tot[PK_COUNT] = {};
    u32 cnt[PK_COUNT] = {};
    using Entry = std::pair<const std::pair<u64, u64>, ProfRec>;
    xr_vector<const Entry*> v;
    v.reserve(s_prof.size());
    for (const auto& kv : s_prof)
    {
        const u32 k = u32(kv.first.first >> 32);
        tot[k] += double(kv.second.self_ns) / 1e6;
        cnt[k] += kv.second.calls;
        v.push_back(&kv);
    }
    const u32 frames = Device.dwFrame - s_frames0;
    const double f = frames ? double(frames) : 1.0;
    Msg("[GOAP prof] over %.1f s, %u frames: self ms/frame eval %.3f (%u/s) | exec %.3f (%u/s) | init/fin %.3f | solve(C++) %.3f (%u/s)", sec, frames,
        tot[PK_EVAL] / f, u32(cnt[PK_EVAL] / sec), tot[PK_EXEC] / f, u32(cnt[PK_EXEC] / sec), tot[PK_INIT] / f, tot[PK_SOLVE] / f, u32(cnt[PK_SOLVE] / sec));
    std::sort(v.begin(), v.end(), [](const Entry* a, const Entry* b) { return a->second.self_ns > b->second.self_ns; });
    const size_t n = v.size() < 30 ? v.size() : 30;
    for (size_t i = 0; i < n; ++i)
    {
        const ProfRec& r = v[i]->second;
        const u32 k = u32(v[i]->first.first >> 32);
        const u32 id = u32(v[i]->first.first);
        Msg("[GOAP prof]  %-5s id %-6u %-34s %-40s self %.3f ms/f incl %.3f | %5.0f/s | %.1f us/call", kinds[k], id, r.lua ? r.lua : "-", r.cpp ? r.cpp : "?",
            double(r.self_ns) / 1e6 / f, double(r.incl_ns) / 1e6 / f, double(r.calls) / sec, r.calls ? double(r.self_ns) / 1e3 / double(r.calls) : 0.0);
    }
    s_prof.clear();
    s_frames0 = Device.dwFrame;
}
}

u64 ProfNowNs() { return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

// ⚠Main thread only: stalkers' update_object_handler runs as a second-thread task and the tables are not shared-safe.
static bool prof_thread() { return Device.OnMainThread(); }

void ProfEnter(ProfKind kind)
{
    if (!prof_thread())
        return;
    if (s_depth < 64)
    {
        s_child[s_depth] = 0;
        s_label[s_depth] = nullptr;
        s_kind[s_depth] = kind;
    }
    ++s_depth;
}

void SetLabel(ProfKind kind, const char* lua_class)
{
    if (!prof_thread())
        return;
    if (s_depth > 0 && s_depth <= 64 && s_kind[s_depth - 1] == kind)
        s_label[s_depth - 1] = lua_class;
}

void ProfLeave(ProfKind kind, u32 id, const char* cpp_name, u64 dt_ns)
{
    if (!prof_thread())
        return;
    if (s_depth <= 0)
        return;
    --s_depth;
    if (s_depth >= 64)
        return;
    const u64 child = s_child[s_depth];
    const char* lua = s_label[s_depth];
    if (s_depth > 0)
        s_child[s_depth - 1] += dt_ns;
    const u64 key2 = u64(reinterpret_cast<uintptr_t>(lua ? lua : cpp_name));
    ProfRec& r = s_prof[{(u64(kind) << 32) | id, key2}];
    r.cpp = cpp_name;
    r.lua = lua;
    r.self_ns += dt_ns > child ? dt_ns - child : 0;
    r.incl_ns += dt_ns;
    ++r.calls;
}

void Tick()
{
    if (!Device.OnMainThread()) // solve() also runs in the second-thread object handler task
        return;
    // ⚠По РЕАЛЬНЫМ часам (dwTimeContinual): игровые встают на паузе и
    // масштабируются time_factor, и тогда отчёт не выйдет никогда.
    const u32 now = Device.dwTimeContinual;
    if (s_last_ms == 0 || now > s_last_ms + 30000) // первый вызов или прибор долго был выключен
    {
        s_last_ms = now;
        s_frames0 = Device.dwFrame;
        s_prof.clear();
        g_solves = g_replans = g_eval_actual = g_eval_search = g_hits = 0;
        return;
    }
    if (now < s_last_ms + 5000)
        return;

    const double sec = double(now - s_last_ms) / 1000.0;
    s_last_ms = now;

    const unsigned total = g_eval_actual + g_eval_search;
    Msg("[GOAP] over %.1f s: solve %u (replan %u = %.0f%%) | evaluate() %u = actual %u + search %u | redundant %u (%.0f%% of search, %.0f%% of all) | cache %s",
        sec, g_solves, g_replans, g_solves ? 100.0 * double(g_replans) / double(g_solves) : 0.0, total, g_eval_actual, g_eval_search, g_hits,
        g_eval_search ? 100.0 * double(g_hits) / double(g_eval_search) : 0.0, total ? 100.0 * double(g_hits) / double(total) : 0.0, use_cache ? "ON" : "off");

    g_solves = g_replans = g_eval_actual = g_eval_search = g_hits = 0;

    if (enabled >= 2)
        ProfReport(sec);
    else
        s_prof.clear();
}
}
