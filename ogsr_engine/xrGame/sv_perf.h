#pragma once

// =====================================================================
//  Where a server frame goes.
//
//  The server's tick IS its frame: xrServer::Update runs once per
//  Ed_RenderFrame, and everything a client learns about the world -- every
//  entity position, every puppet step -- ships from there. So the frame
//  rate is the update rate, and on a shooter that is not a background
//  detail: at one frame a second a client interpolating over 120 ms has
//  nothing to interpolate towards, and every NPC teleports.
//
//  Measured, not assumed: the headless server does ~150 fps with a level
//  loaded and nobody connected, and fell to roughly 1 fps once a client
//  joined and the world around it came online. That is a factor of a
//  hundred and it has to be attributed before anything is "optimised".
//
//  The engine already times its own phases (CStats, gated behind
//  g_bForceStatGather). Nothing here measures anything new -- it turns
//  those counters on, averages them over a window of real frames, and
//  prints the breakdown, because a single frame's numbers on a server
//  that hitches are noise.
//
//  Console:
//    sv_perf          print the breakdown since the last print, and reset
//    sv_perf_reset    start a fresh window without printing
// =====================================================================

namespace SVPerf
{
// Called once per server tick from xrServer::Update. Cheap and inert until
// somebody asks for a report -- the engine's own timers stay off until then,
// so this costs nothing on a server nobody is profiling.
void Sample();

// sv_perf
void Dump();

// sv_perf_reset
void Reset();
} // namespace SVPerf
