#include "StdAfx.h"
#include "npc_perf.h"
#include "../xrCore/lua_vm_watch.h"

namespace NpcPerf
{
int enabled = 0;

namespace
{
const char* s_names[S_COUNT] = {
    "stalker/object_handler", "stalker/anim_callbacks", "stalker/agent_manager", "stalker/vision_dispatch", "stalker/process_enemies",
    "stalker/memory_update",
    "  mem/visual",           "  mem/sound",             "  mem/hit",             "  mem/scan_objects",
    "    scan/ITERATIONS",     "    scan/danger_add",     "    scan/smart_casts",  "    scan/item_add",
    "      useful/base",        "      useful/ai_locations", "      useful/access_pos",
    "      useful/access_vtx",  "      useful/inv_item",     "      useful/level_graph",
    "  mem/enemy",
    "  mem/item",             "  mem/danger",
    "stalker/anomaly_det",     "stalker/inherited",     "stalker/THINK",           "stalker/feel_touch",
    "stalker/net_update",     "stalker/inventory_owner", "stalker/physics",
    "actor/hud_items",        "actor/inventory",         "actor/cam_update",      "actor/controls",          "actor/inherited",
    "any/skeleton_cform",     "any/feel_touch_addons",   "any/script_binder",
};

double s_ms[S_COUNT] = {};
double s_lua[S_COUNT] = {};
u32 s_hits[S_COUNT] = {};

u32 s_last_ms = 0;
u32 s_last_frame = 0;

struct BinderCls
{
    const char* cls;
    double ms;
    u32 n;
};
BinderCls s_bind[64];
int s_nbind = 0;

void report()
{
    const u32 frames = Device.dwFrame - s_last_frame;
    if (!frames)
        return;
    const double f = double(frames);

    double tot = 0.0, totLua = 0.0;
    for (int i = 0; i < S_COUNT; ++i)
    {
        tot += s_ms[i];
        totLua += s_lua[i];
    }
    Msg("[NPCp] over %u frames: %.3f ms/frame accounted (of it Lua %.3f = %.0f%%)", frames, tot / f, totLua / f, tot > 0.0 ? 100.0 * totLua / tot : 0.0);

    // Порядок вывода -- по убыванию цены, иначе таблица из 21 строки нечитаема.
    int order[S_COUNT];
    for (int i = 0; i < S_COUNT; ++i)
        order[i] = i;
    std::sort(order, order + S_COUNT, [](int a, int b) { return s_ms[a] > s_ms[b]; });

    for (int k = 0; k < S_COUNT; ++k)
    {
        const int i = order[k];
        if (s_ms[i] < 0.0005 * f && !s_hits[i])
            continue;
        Msg("[NPCp]   %-26s %6.3f ms/f | Lua %6.3f (%3.0f%%) | %6.1f calls/f | %7.1f us/call", s_names[i], s_ms[i] / f, s_lua[i] / f,
            s_ms[i] > 0.0 ? 100.0 * s_lua[i] / s_ms[i] : 0.0, double(s_hits[i]) / f, s_hits[i] ? s_ms[i] * 1000.0 / double(s_hits[i]) : 0.0);
    }

    std::sort(s_bind, s_bind + s_nbind, [](const BinderCls& a, const BinderCls& b) { return a.ms > b.ms; });
    for (int i = 0; i < s_nbind && i < 10; ++i)
        Msg("[NPCp]     binder %-34s %6.3f ms/f | %5.2f calls/f | %6.1f us/call", s_bind[i].cls, s_bind[i].ms / f, double(s_bind[i].n) / f,
            s_bind[i].n ? s_bind[i].ms * 1000.0 / double(s_bind[i].n) : 0.0);
    s_nbind = 0;

    for (int i = 0; i < S_COUNT; ++i)
    {
        s_ms[i] = 0.0;
        s_lua[i] = 0.0;
        s_hits[i] = 0;
    }
}
} // namespace

void add_binder(const char* cls, double ms)
{
    int i = 0;
    while (i < s_nbind && s_bind[i].cls != cls)
        ++i;
    if (i == s_nbind)
    {
        if (s_nbind >= 64)
            return;
        s_bind[s_nbind++] = {cls, 0.0, 0};
    }
    s_bind[i].ms += ms;
    ++s_bind[i].n;
}

void add(int slot, double ms, double lua_ms)
{
    s_ms[slot] += ms;
    s_lua[slot] += lua_ms;
    ++s_hits[slot];

    const u32 now = Device.dwTimeContinual;
    if (s_last_ms == 0)
    {
        s_last_ms = now;
        s_last_frame = Device.dwFrame;
        return;
    }
    if (now >= s_last_ms + 5000)
    {
        report();
        s_last_ms = now;
        s_last_frame = Device.dwFrame;
    }
}

const char* slot_name(int slot)
{
    if (slot < 0 || slot >= S_COUNT)
        return "?";
    const char* p = s_names[slot];
    while (*p == ' ') // the table indents children for the [NPCp] report
        ++p;
    return p;
}

Scope::Scope(int slot) : s(slot), on(enabled != 0), lua0(0), lp(slot_name(slot))
{
    if (!on)
        return;
    lua0 = lua_vm_watch::lua_time_ns();
    t0 = std::chrono::steady_clock::now();
}

Scope::~Scope() { stop(); }

void Scope::stop()
{
    if (!on)
        return;
    on = false;
    const double ms = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) / 1e6;
    const double lua = double(lua_vm_watch::lua_time_ns() - lua0) / 1e6;
    add(s, ms, lua);
}
}
