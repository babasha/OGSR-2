#pragma once

// ⭐The combat AI, drawn into the world: roles over heads, where each man is
// going and what he holds, the houses as the siege reads them, and the points a
// choice was made from with the score each one got.
//
// WHY. Ten rounds of the siege on 06-10 were debugged from [tac] log lines --
// a coordinate, a bearing, a "blind" -- read back into a picture by hand. The
// mistakes that cost the most (an inner doorway taken for the front door, a
// post 20 m off behind the wrong fence) are obvious at a glance and invisible in
// a log. Step 2 of the combat AI plan: see it before changing it.
//
// It draws through the UI layer, like cl_zone_debug: the engine's debug renderer
// is a stub on this renderer and in Release (vk_DebugRender.cpp). Lines lie OVER
// the frame, not in it -- for this, a feature: a post behind a wall still shows.
//
//     ai_dbg_draw <mask>     0 off (the default; then it costs nothing)
//                            1 roles over heads   2 where they go, what they hold
//                            4 houses: openings and posts
//                            8 candidate points with their scores
//                           16 the threat map: where he may be (ai_threat_map.h)
//     ai_dbg_draw_range <m>  how far from the camera, default 60
//
// ⚠Producers test on() before building anything: off, a candidate costs one
// load and a branch. Everything here runs on the main thread, like the AI it
// draws (g_enemy_claims is not guarded either).
namespace AIDebugDraw
{
enum : u32
{
    eRoles = 1,
    eMoves = 2,
    eHouses = 4,
    eCandidates = 8,
    eThreat = 16,
};

extern int g_mask;
extern float g_range;

IC bool on(u32 what) { return (u32(g_mask) & what) != 0; }

// World-space primitives. ttl_ms 0: this frame only -- for the collectors that
// run at draw time; else kept that long, for things decided between frames.
void Line(const Fvector& a, const Fvector& b, u32 color, u32 ttl_ms = 0);
void Cross(const Fvector& p, float size, u32 color, u32 ttl_ms = 0);
void Ring(const Fvector& centre, float radius, u32 color, u32 ttl_ms = 0);
void Text(const Fvector& p, u32 color, u32 ttl_ms, LPCSTR fmt, ...);

// The points one choice was made from. A new `stamp` (any value that changes per
// choice: the level time it was made at will do) from the same owner and
// channel replaces his previous set. Lower score is better, as in both selectors
// that feed it; `verdict` is ' ' for a scored point, else the letter of what
// threw it out (see the legend in ai_debug_draw.cpp).
enum : u8
{
    eChannelCover, // the stock "best cover" (CCoverEvaluatorBest)
    eChannelPost, // a siege post in front of an opening
    // the position query (ai_position_query.h), by kind; '-' = never looked at with rays
    eChannelQueryCover,
    eChannelQueryWatch,
    eChannelQueryPost,
    eChannelQueryFlank,
    eChannelQueryDoor,
    eChannelQuerySweep,
    eChannelCount,
};
void Candidate(u16 owner, u8 channel, u32 stamp, const Fvector& p, float score, char verdict);

void Frame(); // from CLevel::OnFrame: outside the render pass
void Render(); // from CHUDManager::RenderUI, before Font().Render()
} // namespace AIDebugDraw
