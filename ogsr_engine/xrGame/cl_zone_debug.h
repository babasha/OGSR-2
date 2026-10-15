#pragma once

// Draws the TRUE volume of nearby zones, because the rendered one lies.
//
// ⭐WHY. An anomaly's collision shape and its visual effect are unrelated
// quantities. sv_anomaly_spawn can place a zone of radius 15 whose particle
// effect looks about 3 m across, and zharka_static at rest is nearly invisible
// even then -- so an operator placing anomalies is laying invisible traps, and
// on 01-08-2026 one of them killed the player twice in one session. The second
// time he knew the centre coordinates, knew the radius was 6, had been warned
// not to come within 7 m, and still died at 4.77 m from the centre: the volume
// simply cannot be read off the screen.
//
// ⚠It draws through the UI layer (IUIRender line lists, the same path the
// crosshair uses) and NOT through the engine's debug renderer, which draws
// nothing on this renderer -- Layers/xrRenderVulkan/vk_DebugRender.cpp is a
// stub whose add_lines() is empty, so CSpaceRestrictor::OnRender has always
// been drawing into the void here. ImGui is not an option either: the engine's
// ImGui_Backend_* hooks are stubs too, and the renderer's own ImGui frame is
// gated behind r_profiler>=2 / -vk_editor and opens and closes entirely inside
// one function, so there is nowhere for the game to submit into it.
//
// Going through the UI keeps every line of this in xrGame -- nothing in
// Layers/ has to change, which matters because that tree belongs to the public
// renderer fork.
//
//     cl_zone_debug 0   off
//                   1   outlines
//                   2   outlines + labels (section, radius, distance)
//     cl_zone_debug_range <m>   how far to look, default 100
namespace ClZoneDebug
{
// From CLevel::OnFrame, OUTSIDE the render pass. Creating the UI shader from
// inside rendering left it with a null texture descriptor ("SetShader:
// descriptor=NULL, using white fallback" in the client log), so it is built
// here instead, where a texture is allowed to resolve.
void Frame();

// From CHUDManager::RenderUI, i.e. during rendering with the UI set up.
void Render();

// cl_zone_debug_dump: the numbers behind what is (or is not) on screen -- how
// many zones were picked up, what each one's radius / power / effective radius
// is, how many outline points and hatch segments were built for it, and whether
// the downward rays are finding ground at all.
void Dump();

// Which zone the editor has selected, so the overlay can pick it out. 0xffff for
// none. Kept here rather than in the editor because the overlay is what draws.
void SetSelected(u16 id);

// A flat ring with an optional grid fill at a given height -- the editor's ghost
// of a zone it is about to place. Flat rather than ground-traced on purpose: the
// aim point moves every frame and tracing a ring per frame is a raycast storm,
// while the aim point itself is already on the ground.
// ⚠Valid only while the UI is rendering (it drives IUIRender directly).
void DrawFlatDisc(const Fvector& centre, float radius, u32 edge_color, u32 fill_color, float grid_step);

// One world-space segment, near-plane clipped, for anything else that needs to
// draw into the world through the UI layer -- the furniture editor's ghost box
// and facing arrow, for one.
// ⚠Valid only while the UI is rendering (it drives IUIRender directly).
void DrawLine(const Fvector& a, const Fvector& b, u32 color);

// A batch of DrawLine segments for an overlay that draws many of them -- the
// combat AI's (ai_debug_draw.cpp). DrawLine only pushes vertices; these open and
// flush the primitive around them, with room for `segments` segments.
// ⚠Valid only while the UI is rendering.
void BeginLines(u32 segments);
void EndLines();

// Where a world point lands on screen, in pixels; false behind the eye.
bool ToScreen(const Fvector& world, float& sx, float& sy);

// The line shader, built outside the render pass (see Frame) for a caller that
// draws while cl_zone_debug itself is off.
void EnsureShaders();
} // namespace ClZoneDebug
