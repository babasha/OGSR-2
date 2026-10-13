#pragma once

// ⭐The frame-rate route: the same places, looked at the same way, every run -- so a change to the renderer or the
// simulation is measured against the last build instead of against "it felt smoother" (07-10, the Switch: "stable 60").
//
//     perf_route list                     every smart terrain of this level: name and position (pick the stops from it)
//     perf_route [dwell=16] [settle=4] <stop> <stop> ...
//         A stop is a smart terrain's name or x,y,z. At each: the player (immortal for the run) is put on the AI map
//         vertex of the stop, left `settle` s for the place to come online (spawns, streaming -- reported apart as
//         "arrival"), then turned once round in `dwell` s while every frame's time is taken. One line per stop:
//             [perf] stop 2/5 esc_lager | frames 960 avg 16.7 p50 16.6 p95 18.4 p99 24.0 max 31.2 ms | >20ms 12 >33ms 0 | arrival max 140 ms
//         and "[perf] stop i begin/end" markers, so the renderer's own lines between them ([nx frame], [VK Perf]) can be
//         read per stop: D:\switch_stage\tools\perfroute.py <log>.
//     perf_route ... a=<cmds> b=<cmds> <stop> ...
//         A/B at every stop: one turn with the console commands of A, then one with B (1.5 s apart), so the two
//         settings are compared at the same place within seconds of each other -- not across runs, whose weather and
//         time of day differ. <cmds> = "cmd:arg;cmd:arg" (':' stands for the space). The run ends on A.
//         Segments are named <stop>#A / <stop>#B; the totals are "ALL STOPS #A" / "#B".
//     perf_route walk=<m/s> [settle=4] <stop> <stop> ...
//         On the move (08-10): after `settle` s at the first stop the player travels in a straight line to the next one,
//         and on, at <m/s> (6 = a run), facing the way, height off the AI map (through a house it keeps the last one).
//         One line per leg: "[perf] leg 1/3 a->b | frames ...". What a standing turn never shows: the far sun map
//         redrawn every 40 m, streaming, NPCs coming online.
//     perf_route stop                     ends the run now
namespace PerfRoute
{
void Command(LPCSTR args);
void Frame(); // from CLevel::OnFrame
} // namespace PerfRoute
