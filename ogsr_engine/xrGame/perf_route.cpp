#include "StdAfx.h"
#include "perf_route.h"

#include "../xr_3da/XR_IOConsole.h"
#include "Level.h"
#include "Actor.h"
#include "Actor_Flags.h"
#include "ai_space.h"
#include "level_graph.h"
#include "game_graph.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"

namespace
{
struct SStop
{
    shared_str name;
    Fvector pos{};
};

constexpr float SWITCH_SETTLE = 1.5f; // s after the A->B switch: new pipelines, the first frames of the other setting

struct SRun
{
    bool active{};
    bool god_was{};
    xr_vector<SStop> stops;
    xr_string cmd[2]; // A and B: console commands, ';'-separated (empty B = no A/B)
    u32 cur{}, half{};
    float dwell{16.f}, settle{4.f};
    float walk{}; // m/s: > 0 = travel stop -> next stop on a straight line instead of turning on each (no A/B)
    u64 seg_t0{}; // us: start of the current segment (one stop, one setting)
    u64 last_t{};
    float arrival_max{};
    xr_vector<float> ft; // ms, the measured part of the current segment
    xr_vector<float> all[2]; // ms, every stop, per setting
} s_run;

u64 now_us() { return CPU::QPC_us(); }
bool ab() { return !s_run.cmd[1].empty(); }

bool on_this_level(const CSE_ALifeDynamicObject* o)
{
    return ai().game_graph().valid_vertex_id(o->m_tGraphID) && ai().game_graph().vertex(o->m_tGraphID)->level_id() == ai().level_graph().level_id();
}

// The AI map vertex at (or nearest to) the spot: a place the player can stand.
Fvector ground(const Fvector& p)
{
    u32 v = ai().level_graph().vertex_id(p);
    if (!ai().level_graph().valid_vertex_id(v))
        v = ai().level_graph().nearest_vertex_id(p);
    return ai().level_graph().valid_vertex_id(v) ? ai().level_graph().vertex_position(v) : p;
}

void list_zones()
{
    xr_vector<std::pair<xr_string, Fvector>> z;
    for (const auto& [id, o] : ai().alife().objects().objects())
        if (smart_cast<CSE_ALifeSmartZone*>(o) && on_this_level(o))
            z.emplace_back(o->name_replace(), o->o_Position);
    std::sort(z.begin(), z.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [name, p] : z)
        Msg("[perf] zone %s %.1f,%.1f,%.1f", name.c_str(), VPUSH(p));
    Msg("[perf] %u smart terrains on %s", (u32)z.size(), Level().name().c_str());
}

bool find_stop(LPCSTR tok, SStop& out)
{
    Fvector p;
    if (3 == sscanf(tok, "%f,%f,%f", &p.x, &p.y, &p.z))
    {
        out = {tok, ground(p)};
        return true;
    }
    for (const auto& [id, o] : ai().alife().objects().objects())
        if (on_this_level(o) && !xr_strcmp(o->name_replace(), tok))
        {
            out = {tok, ground(o->o_Position)};
            return true;
        }
    return false;
}

// "r_shadow_lite:1;r_sun_cad_npc:2" -> two console commands
void run_cmds(const xr_string& cmds)
{
    xr_string c;
    for (size_t i = 0; i <= cmds.size(); ++i)
    {
        const char ch = i < cmds.size() ? cmds[i] : ';';
        if (ch == ';')
        {
            if (!c.empty())
            {
                Msg("[perf] > %s", c.c_str());
                Console->Execute(c.c_str());
            }
            c.clear();
        }
        else
            c += ch == ':' ? ' ' : ch;
    }
}

float pct(xr_vector<float>& v, float q)
{
    if (v.empty())
        return 0.f;
    const size_t i = std::min(v.size() - 1, size_t(q * float(v.size() - 1) + .5f));
    std::nth_element(v.begin(), v.begin() + i, v.end());
    return v[i];
}

void report(LPCSTR what, xr_vector<float> v, float arrival)
{
    if (v.empty())
        return;
    double sum = 0;
    u32 over20 = 0, over33 = 0;
    float mx = 0.f;
    for (float f : v)
    {
        sum += f;
        over20 += f > 20.f;
        over33 += f > 33.4f;
        mx = std::max(mx, f);
    }
    const float avg = float(sum / v.size());
    Msg("[perf] %s | frames %u avg %.2f fps %.1f p50 %.2f p95 %.2f p99 %.2f max %.1f ms | >20ms %u >33ms %u | arrival max %.0f ms", what, (u32)v.size(), avg,
        1000.f / avg, pct(v, .5f), pct(v, .95f), pct(v, .99f), mx, over20, over33, arrival);
}

void seg_name(string256& out)
{
    if (s_run.walk > 0.f && s_run.cur + 1 < s_run.stops.size())
        xr_sprintf(out, "%s->%s", s_run.stops[s_run.cur].name.c_str(), s_run.stops[s_run.cur + 1].name.c_str());
    else
        xr_sprintf(out, "%s%s", s_run.stops[s_run.cur].name.c_str(), ab() ? (s_run.half ? "#B" : "#A") : "");
}

// walk: the point `d` metres along the leg cur -> cur+1, on the AI map, and the heading of the leg (MoveActor's NewDir).
bool walk_pose(float d, Fvector& pos, Fvector& hpb)
{
    const Fvector& a = s_run.stops[s_run.cur].pos;
    const Fvector& b = s_run.stops[s_run.cur + 1].pos;
    Fvector dir;
    dir.sub(b, a);
    dir.y = 0.f;
    const float len = dir.magnitude();
    if (len < 0.1f || d >= len)
        return false;
    dir.div(len);
    static float s_last_y = a.y;
    if (d <= 0.f)
        s_last_y = a.y;
    pos.mad(a, dir, d);
    pos.y = s_last_y;
    // Only the cheap lookup per frame: nearest_vertex_id (ground()'s fallback) would show up in the very frames measured.
    // Off the map (through a house) the height stays where it was.
    const u32 v = ai().level_graph().vertex_id(pos);
    if (ai().level_graph().valid_vertex_id(v))
        s_last_y = pos.y = ai().level_graph().vertex_position(v).y;
    float h, p;
    dir.getHP(h, p);
    hpb.set(0.f, -h, 0.f);
    return true;
}

void finish(LPCSTR why)
{
    if (!s_run.active)
        return;
    report(ab() ? "ALL STOPS #A" : "ALL STOPS", s_run.all[0], 0.f);
    if (ab())
        report("ALL STOPS #B", s_run.all[1], 0.f);
    if (ab())
        run_cmds(s_run.cmd[0]); // leave the game on A, the setting the run started from
    Msg("[perf] route end (%s)", why);
    psActorFlags.set(AF_GODMODE, s_run.god_was);
    s_run = {};
}

void begin_segment()
{
    const SStop& s = s_run.stops[s_run.cur];
    if (ab())
        run_cmds(s_run.cmd[s_run.half]);
    if (CActor* a = Actor())
        a->MoveActor(s.pos, Fvector{0.f, 0.f, 0.f});
    s_run.seg_t0 = s_run.last_t = now_us();
    s_run.arrival_max = 0.f;
    s_run.ft.clear();
    string256 n;
    seg_name(n);
    Msg("[perf] stop %u/%u begin %s at %.1f,%.1f,%.1f", s_run.cur + 1, (u32)s_run.stops.size(), n, VPUSH(s.pos));
}
} // namespace

void PerfRoute::Command(LPCSTR args)
{
    if (!g_pGameLevel || !Actor() || !ai().get_alife())
    {
        Msg("! [perf] needs a single-player game with the player in it");
        return;
    }
    string4096 buf;
    xr_strcpy(buf, args ? args : "");
    xr_vector<xr_string> tok;
    for (char* t = strtok(buf, " \t"); t; t = strtok(nullptr, " \t"))
        tok.emplace_back(t);

    if (!tok.empty() && tok[0] == "list")
        return list_zones();
    if (!tok.empty() && tok[0] == "stop")
        return finish("stopped");

    finish("restarted");
    for (const auto& t : tok)
    {
        if (!strncmp(t.c_str(), "dwell=", 6))
            s_run.dwell = std::max(2.f, (float)atof(t.c_str() + 6));
        else if (!strncmp(t.c_str(), "settle=", 7))
            s_run.settle = std::max(0.f, (float)atof(t.c_str() + 7));
        else if (!strncmp(t.c_str(), "walk=", 5))
            s_run.walk = std::max(0.f, (float)atof(t.c_str() + 5));
        else if (!strncmp(t.c_str(), "a=", 2))
            s_run.cmd[0] = t.c_str() + 2;
        else if (!strncmp(t.c_str(), "b=", 2))
            s_run.cmd[1] = t.c_str() + 2;
        else if (SStop s; find_stop(t.c_str(), s))
            s_run.stops.push_back(s);
        else
            Msg("! [perf] no smart terrain or x,y,z '%s' on %s -- skipped", t.c_str(), Level().name().c_str());
    }
    if (s_run.stops.empty() || (s_run.cmd[1].empty() != s_run.cmd[0].empty()) || (s_run.walk > 0.f && (s_run.stops.size() < 2 || ab())))
    {
        Msg("! [perf] usage: perf_route list | perf_route [dwell=16] [settle=4] [a=cmd:arg;cmd:arg b=cmd:arg] <name|x,y,z> ... | "
            "perf_route walk=<m/s> [settle=4] <stop> <stop> ... (2+ stops, no a=/b=)");
        s_run = {};
        return;
    }
    s_run.active = true;
    s_run.god_was = !!psActorFlags.test(AF_GODMODE);
    psActorFlags.set(AF_GODMODE, TRUE);
    Msg("[perf] route begin: %u stops on %s, dwell %.0f s, settle %.0f s%s%s%s%s", (u32)s_run.stops.size(), Level().name().c_str(), s_run.dwell, s_run.settle,
        ab() ? " | A: " : "", s_run.cmd[0].c_str(), ab() ? " | B: " : "", s_run.cmd[1].c_str());
    begin_segment();
}

void PerfRoute::Frame()
{
    if (!s_run.active)
        return;
    CActor* a = Actor();
    if (!a || Device.Paused())
        return finish("no player or paused");

    const u64 t = now_us();
    const float ms = float(t - s_run.last_t) / 1000.f;
    s_run.last_t = t;
    const float since = float(t - s_run.seg_t0) / 1e6f;
    const SStop& s = s_run.stops[s_run.cur];
    const bool walking = s_run.walk > 0.f;
    // walk: only the first leg waits for the place to come online; the next ones carry on from where the last ended
    const float settle = s_run.half ? SWITCH_SETTLE : (walking && s_run.cur > 0 ? 0.f : s_run.settle);

    if (since < settle)
    {
        s_run.arrival_max = std::max(s_run.arrival_max, ms);
        Fvector pos, hpb{};
        if (walking && walk_pose(0.f, pos, hpb))
            a->MoveActor(s.pos, hpb);   // already facing the way it will go
        else
            a->MoveActor(s.pos, Fvector{0.f, 0.f, 0.f});
        return;
    }
    if (walking)
    {
        Fvector pos, hpb;
        if (walk_pose((since - settle) * s_run.walk, pos, hpb))
        {
            if (since - settle > 0.05f)
            {
                s_run.ft.push_back(ms);
                s_run.all[0].push_back(ms);
            }
            // 09-10: without the collision correction -- the straight line goes through houses and fences, and the
            // correction pushing the actor out of 10-20 m of geometry was 7-16 ms "CLevel" frames that a player never has
            a->MoveActor(pos, hpb, false);
            return;
        }
        string256 n, what;
        seg_name(n);
        xr_sprintf(what, "leg %u/%u %s", s_run.cur + 1, (u32)s_run.stops.size() - 1, n);
        report(what, s_run.ft, s_run.arrival_max);
        Msg("[perf] leg %u/%u end", s_run.cur + 1, (u32)s_run.stops.size() - 1);
        if (++s_run.cur + 1 >= s_run.stops.size())
            return finish("done");
        return begin_segment();
    }
    const float turn = (since - settle) / s_run.dwell;
    if (turn < 1.f)
    {
        if (since - settle > 0.05f) // the first frame of the turn still carries the settle phase
        {
            s_run.ft.push_back(ms);
            s_run.all[s_run.half].push_back(ms);
        }
        a->MoveActor(s.pos, Fvector{0.f, -PI_MUL_2 * turn, 0.f});
        return;
    }
    string256 n, what;
    seg_name(n);
    xr_sprintf(what, "stop %u/%u %s", s_run.cur + 1, (u32)s_run.stops.size(), n);
    report(what, s_run.ft, s_run.arrival_max);
    Msg("[perf] stop %u/%u end", s_run.cur + 1, (u32)s_run.stops.size());
    if (ab() && !s_run.half)
        s_run.half = 1;
    else
    {
        s_run.half = 0;
        if (++s_run.cur >= s_run.stops.size())
            return finish("done");
    }
    begin_segment();
}
