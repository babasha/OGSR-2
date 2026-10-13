#include "StdAfx.h"
#include "sv_perf.h"

#include "../xr_3da/Stats.h"
#include "../xrCore/FTimer.h"
#include "Level.h"
#include "ai_space.h"
#include "alife_simulator.h"

namespace
{
// One accumulator per phase the engine already times. Summed per frame and
// divided at the end, so a single hitching frame cannot pass for the norm.
struct Accum
{
    double engine_total{0.0};
    double sheduler{0.0};
    double update_client{0.0};
    double physics{0.0};
    double ph_collision{0.0};
    double ph_core{0.0};
    double ai_think{0.0};
    double ai_range{0.0};
    double ai_path{0.0};
    double ai_node{0.0};
    double ai_vis{0.0};
    double animation{0.0};
    double render_total{0.0};

    double objects{0.0}; // mean count of objects the client update walked
    double active{0.0};

    u32 frames{0};
    u32 start_ms{0};
    float worst_frame{0.f};
};

Accum s_acc;
bool s_armed = false; // whether the engine's counters have been switched on

void reset_window()
{
    s_acc = Accum();
    s_acc.start_ms = Device.dwTimeGlobal;
}
} // namespace

void SVPerf::Reset()
{
    // Arming is what makes the CStatTimers actually record. It is left off
    // until somebody asks, so an unprofiled server pays nothing.
    if (!s_armed)
    {
        g_bForceStatGather = TRUE;
        s_armed = true;
    }
    reset_window();
}

void SVPerf::Sample()
{
    if (!s_armed || !Device.Statistic)
        return;

    CStats& S = *Device.Statistic;

    s_acc.engine_total += S.EngineTOTAL.result;
    s_acc.sheduler += S.Sheduler.result;
    s_acc.update_client += S.UpdateClient.result;
    s_acc.physics += S.Physics.result;
    s_acc.ph_collision += S.ph_collision.result;
    s_acc.ph_core += S.ph_core.result;
    s_acc.ai_think += S.AI_Think.result;
    s_acc.ai_range += S.AI_Range.result;
    s_acc.ai_path += S.AI_Path.result;
    s_acc.ai_node += S.AI_Node.result;
    s_acc.ai_vis += S.AI_Vis.result;
    s_acc.animation += S.Animation.result;
    s_acc.render_total += S.RenderTOTAL.result;

    s_acc.objects += double(S.UpdateClient_total);
    s_acc.active += double(S.UpdateClient_active);

    if (S.EngineTOTAL.result > s_acc.worst_frame)
        s_acc.worst_frame = S.EngineTOTAL.result;

    ++s_acc.frames;
}

void SVPerf::Dump()
{
    if (!s_armed)
    {
        // First call only arms and starts a window: there is nothing to
        // report yet, and reporting zeros would read as "nothing costs
        // anything" rather than "nothing was measured".
        Reset();
        Msg("~ [sv_perf] counters armed -- run for a few seconds, then sv_perf again for the breakdown");
        return;
    }

    if (!s_acc.frames)
    {
        Msg("~ [sv_perf] no frames sampled yet");
        return;
    }

    const float n = float(s_acc.frames);
    const u32 wall = Device.dwTimeGlobal - s_acc.start_ms;
    const float fps = wall ? (1000.f * n / float(wall)) : 0.f;
    const float mean_frame = wall ? (float(wall) / n) : 0.f;

    // The headline is the frame period, because that IS the rate at which
    // clients are told where anything is.
    Msg("~ [sv_perf] %u frames over %.1fs -> %.1f fps, mean frame %.1f ms, worst engine frame %.1f ms", s_acc.frames, float(wall) / 1000.f, fps, mean_frame, s_acc.worst_frame);

    Msg("~ [sv_perf]   engine total %.1f ms | sheduler %.1f | update_client %.1f | render %.1f", float(s_acc.engine_total / n), float(s_acc.sheduler / n),
        float(s_acc.update_client / n), float(s_acc.render_total / n));

    Msg("~ [sv_perf]   physics %.1f ms (collision %.1f, integrate %.1f) | animation %.1f", float(s_acc.physics / n), float(s_acc.ph_collision / n), float(s_acc.ph_core / n),
        float(s_acc.animation / n));

    Msg("~ [sv_perf]   ai think %.1f ms | range %.1f | path %.1f | node %.1f | vis %.1f", float(s_acc.ai_think / n), float(s_acc.ai_range / n), float(s_acc.ai_path / n),
        float(s_acc.ai_node / n), float(s_acc.ai_vis / n));

    // The counts matter as much as the times: 40 ms of AI over 900 objects
    // is a different problem from 40 ms over 12.
    u32 online = 0;
    if (g_pGameLevel)
        online = Level().Objects.o_count();
    Msg("~ [sv_perf]   %u objects online, client update walked %.0f (%.0f active)", online, float(s_acc.objects / n), float(s_acc.active / n));

    // Anything the phases above do not account for is the interesting
    // remainder -- it is where our own per-tick work would show up.
    const double accounted = s_acc.sheduler + s_acc.update_client + s_acc.physics + s_acc.render_total;
    Msg("~ [sv_perf]   unaccounted %.1f ms of %.1f engine (phases overlap, so treat as a hint not a total)", float((s_acc.engine_total - accounted) / n),
        float(s_acc.engine_total / n));

    reset_window();
}
