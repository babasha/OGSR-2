#include "StdAfx.h"
#include "cl_zone_debug.h"

#include "Level.h"
#include "Actor.h"
#include "space_restrictor.h"
#include "CustomZone.h"
#include "HUDManager.h"
#include "ui_defs.h"
#include "../Include/xrRender/UIRender.h"
#include "../xr_3da/xr_collide_form.h"
#include "../xr_3da/GameFont.h"

int g_cl_zone_debug = 0;
float g_cl_zone_debug_range = 100.f;
float g_cl_zone_debug_hatch = 0.6f; // metres between grid lines at power 1.0; 0 = no fill
// Which shader candidate draws the lines. 2 = "hud\cursor" + "ui\cursor", the
// only one of the four that came out coloured on screen (measured 01-08-2026;
// see the note on the rig below for why this cannot be decided from code).
int g_cl_zone_debug_shader = 2;
float g_cl_zone_debug_width = 2.5f; // line thickness in pixels -- see push_quad

namespace
{
constexpr int RING_SEGMENTS = 64; // the outline is the thing people look at
constexpr float HATCH_STEP = 1.5f; // how often a hatch line samples the ground
constexpr float GROUND_LIFT = 0.06f; // keeps lines off the surface they trace
constexpr float GROUND_PROBE = 80.f; // how far down to look for ground

// Zones outlined at once, and of those, how many get the interior hatch. The UI
// vertex ring is a fixed budget and wrapping it mid-frame corrupts other UI
// (vk_UIRender says so out loud), so both are bounded rather than trusted to the
// range cvar.
constexpr int MAX_ZONES = 8;
constexpr int MAX_HATCHED = 4;

constexpr float NEAR_W = 0.05f;

struct ClipVertex
{
    float x, y, w;
};

// A hatch or outline segment, already traced onto the ground, carrying the
// strength at its own position so the colour can vary along it.
struct Seg
{
    Fvector a, b;
    float power; // 0..1, relative
};

// Ground tracing is far too expensive to redo every frame, and it does not need
// to be: terrain does not move and a zone only changes when somebody resizes it.
// Keyed by object id, rebuilt when the centre or radius no longer matches.
struct Footprint
{
    Fvector centre{};
    float radius{};
    float eff_radius{};
    xr_vector<Fvector> outline; // ground-following, closed
    xr_vector<Fvector> outline_eff; // the part that actually hurts
    xr_vector<Seg> hatch;
    bool hatched{};
};

xr_map<u16, Footprint> s_cache;

// ⚠⚠NOT a file-scope object. ui_shader is FactoryPtr<IUIShader>, and
// FactoryPtr's constructor calls CreateObject() -- it asks the render factory
// for an object the moment it is constructed. At namespace scope that runs
// during static initialisation of the module, long before any renderer exists,
// and the headless server (which links this same code) died instantly with no
// log at all, not having got as far as opening one.
//
// ⚠And it is built from Frame(), not from Render(): created inside the render
// pass the texture never resolved, and the renderer said so --
// "SetShader: descriptor=NULL, using white fallback".
// ⭐The calibration rig, and why it exists.
//
// The UI multiplies vertex colour by the shader's texture, and when a UI shader
// resolves no texture this renderer substitutes s_WhiteTexture -- which, despite
// the name and despite the log saying "using white fallback", is 0xFF1A2018,
// a dark olive-grey chosen on purpose so that MISSING menu backgrounds do not
// glare white (see the comment at vk_UIPipeline.cpp). Multiply a colour by that
// and you get near-black, which is exactly what the first version of this
// overlay drew.
//
// So the colour of these lines depends entirely on which shader/texture pair
// actually resolves, and that cannot be settled by reading code -- it needs one
// look at the screen. cl_zone_debug 3 draws one bar per candidate, labelled, all
// in the same pure yellow. Whichever bar IS yellow is the pair to use.
struct ShaderCandidate
{
    const char* label;
    const char* shader;
    const char* texture; // nullptr = textureless form
    float u, v;
    ui_shader* ptr;
};

ShaderCandidate s_candidates[] = {
    {"A hud/crosshair", "hud\\crosshair", nullptr, 0.f, 0.f, nullptr},
    {"B hud/default + actor_menu_back", "hud\\default", "ui\\ui_actor_menu_back", 0.5f, 0.5f, nullptr},
    {"C hud/cursor + ui/cursor", "hud\\cursor", "ui\\cursor", 0.5f, 0.5f, nullptr},
    {"D bullet_tracer + fx_tracer", "effects\\bullet_tracer", "fx\\fx_tracer", 0.5f, 0.5f, nullptr},
};
constexpr int CANDIDATE_COUNT = sizeof(s_candidates) / sizeof(s_candidates[0]);

// Which candidate the outline actually draws with.
int& g_pick = g_cl_zone_debug_shader;

void ensure_shaders()
{
    for (auto& c : s_candidates)
    {
        if (c.ptr)
            continue;
        c.ptr = xr_new<ui_shader>();
        if (c.texture)
            (*c.ptr)->create(c.shader, c.texture);
        else
            (*c.ptr)->create(c.shader);
    }
}

ui_shader& shader()
{
    const int i = (g_pick >= 0 && g_pick < CANDIDATE_COUNT) ? g_pick : 0;
    return *s_candidates[i].ptr;
}

float shader_u() { return s_candidates[(g_pick >= 0 && g_pick < CANDIDATE_COUNT) ? g_pick : 0].u; }
float shader_v() { return s_candidates[(g_pick >= 0 && g_pick < CANDIDATE_COUNT) ? g_pick : 0].v; }

ClipVertex to_clip(const Fvector& world)
{
    const Fmatrix& M = Device.mFullTransform;
    ClipVertex c;
    c.x = world.x * M._11 + world.y * M._21 + world.z * M._31 + M._41;
    c.y = world.x * M._12 + world.y * M._22 + world.z * M._32 + M._42;
    c.w = world.x * M._14 + world.y * M._24 + world.z * M._34 + M._44;
    return c;
}

void to_screen(const ClipVertex& c, float& sx, float& sy)
{
    const float inv = 1.f / c.w;
    sx = (c.x * inv * 0.5f + 0.5f) * float(Device.dwWidth);
    sy = (0.5f - c.y * inv * 0.5f) * float(Device.dwHeight);
}

// ⭐Why these are quads and not lines.
//
// The Vulkan UI pipeline hardcodes rasterizer.lineWidth = 1.0 and declares no
// VK_DYNAMIC_STATE_LINE_WIDTH, so a LINE_LIST is one pixel wide and there is no
// knob. Making it configurable would mean touching the renderer AND depending on
// the `wideLines` device feature, which Vulkan does not guarantee. Expanding the
// segment into a screen-space quad costs four more vertices, works on every
// device, gives exact pixel widths -- and keeps all of this in xrGame.
void push_quad(float x0, float y0, float x1, float y1, u32 color)
{
    float dx = x1 - x0, dy = y1 - y0;
    const float len = _sqrt(dx * dx + dy * dy);
    if (len < 0.01f)
        return;
    const float hw = g_cl_zone_debug_width * 0.5f;
    const float px = -(dy / len) * hw, py = (dx / len) * hw;

    UIRender->PushPoint(x0 - px, y0 - py, 0.f, color, shader_u(), shader_v());
    UIRender->PushPoint(x0 + px, y0 + py, 0.f, color, shader_u(), shader_v());
    UIRender->PushPoint(x1 + px, y1 + py, 0.f, color, shader_u(), shader_v());

    UIRender->PushPoint(x0 - px, y0 - py, 0.f, color, shader_u(), shader_v());
    UIRender->PushPoint(x1 + px, y1 + py, 0.f, color, shader_u(), shader_v());
    UIRender->PushPoint(x1 - px, y1 - py, 0.f, color, shader_u(), shader_v());
}

// One world-space segment, near-plane clipped, pushed as a screen-space quad.
void line(const Fvector& a, const Fvector& b, u32 color)
{
    ClipVertex ca = to_clip(a);
    ClipVertex cb = to_clip(b);

    if (ca.w < NEAR_W && cb.w < NEAR_W)
        return; // wholly behind the eye

    if (ca.w < NEAR_W || cb.w < NEAR_W)
    {
        // Interpolate the offending end up to the near plane. Linear in clip
        // space is exact: all three components are linear in the world-space
        // parameter. Standing INSIDE a zone -- the case this exists for -- puts
        // most of the outline behind the camera, so dropping those segments
        // instead of cutting them would break the picture exactly when it
        // matters most.
        const float t = (NEAR_W - ca.w) / (cb.w - ca.w);
        const ClipVertex mid{ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t, NEAR_W};
        if (ca.w < NEAR_W)
            ca = mid;
        else
            cb = mid;
    }

    float x0, y0, x1, y1;
    to_screen(ca, x0, y0);
    to_screen(cb, x1, y1);
    push_quad(x0, y0, x1, y1, color);
}

void polyline(const xr_vector<Fvector>& pts, u32 color, bool closed)
{
    if (pts.size() < 2)
        return;
    for (size_t i = 1; i < pts.size(); ++i)
        line(pts[i - 1], pts[i], color);
    if (closed)
        line(pts.back(), pts.front(), color);
}

// Straight down onto the world. Static geometry only: the point is where the
// ground is, not where a passing dog is.
u32 s_rays_total = 0, s_rays_hit = 0; // for the dump: is the ground even being found?

bool ground_at(float x, float z, float y_from, float& out_y)
{
    ++s_rays_total;
    collide::rq_result RQ;
    Fvector from, dir;
    from.set(x, y_from, z);
    dir.set(0.f, -1.f, 0.f);
    if (Level().ObjectSpace.RayPick(from, dir, GROUND_PROBE, collide::rqtStatic, RQ, nullptr))
    {
        ++s_rays_hit;
        out_y = y_from - RQ.range + GROUND_LIFT;
        return true;
    }
    return false;
}

void trace_ring(const Fvector& centre, float radius, float y_from, xr_vector<Fvector>& out)
{
    out.clear();
    if (radius <= EPS_L)
        return;
    out.reserve(RING_SEGMENTS);
    for (int i = 0; i < RING_SEGMENTS; ++i)
    {
        const float a = (PI_MUL_2 * i) / RING_SEGMENTS;
        const float x = centre.x + radius * _cos(a);
        const float z = centre.z + radius * _sin(a);
        float y;
        if (!ground_at(x, z, y_from, y))
            y = centre.y; // no ground under it: draw at the zone's own height
        Fvector p;
        p.set(x, y, z);
        out.push_back(p);
    }
}

// Parallel lines across the disc, traced onto the ground, spaced by the zone's
// strength: a fiercer anomaly gets a denser weave. Each segment also carries the
// relative power at its own position, so the hatch fades out towards the rim the
// same way the damage does.
void build_hatch(CCustomZone* zone, const Fvector& centre, float radius, float y_from, xr_vector<Seg>& out)
{
    out.clear();
    if (g_cl_zone_debug_hatch <= EPS_L || radius <= EPS_L)
        return;

    // ⚠Spacing, measured rather than guessed. The first version divided the base
    // by the zone's power, and for zone_zharka_static_weak (power 0.50) that came
    // out at 2 m -- eight parallel lines across a 16 m disc, seen from 22 m at a
    // grazing angle. The dump said "hatch=96", the screen said nothing: it was
    // being drawn and was simply unreadable, and the weak zone ended up SPARSER
    // than a strong one, which is backwards.
    //
    // So the power only modulates within a band that stays legible, and the fill
    // is a GRID rather than parallel lines -- one set of lines always runs across
    // the view no matter where you stand.
    const float power = zone ? zone->GetMaxPower() : 1.f;
    float spacing = g_cl_zone_debug_hatch / _max(0.5f, power);
    clamp(spacing, 0.4f, 1.6f);

    // A cap that matters: a big zone with a tight spacing is thousands of rays.
    if (iFloor((2.f * radius) / spacing) > 32)
        spacing = (2.f * radius) / 32.f;

    // axis 0: lines of constant Z, running along X. axis 1: the other way.
    for (int axis = 0; axis < 2; ++axis)
    {
        for (float off = -radius + spacing * 0.5f; off < radius; off += spacing)
        {
            const float half = _sqrt(_max(0.f, radius * radius - off * off));
            if (half < 0.2f)
                continue;

            Fvector prev;
            bool have_prev = false;
            for (float t = -half; t <= half + EPS_L; t += HATCH_STEP)
            {
                const float along = _min(t, half);
                const float x = (axis == 0) ? centre.x + along : centre.x + off;
                const float z = (axis == 0) ? centre.z + off : centre.z + along;
                float y;
                if (!ground_at(x, z, y_from, y))
                    y = centre.y;
                Fvector p;
                p.set(x, y, z);
                if (have_prev)
                {
                    Fvector mid;
                    mid.add(prev, p);
                    mid.mul(0.5f);
                    const float d = _sqrt((mid.x - centre.x) * (mid.x - centre.x) + (mid.z - centre.z) * (mid.z - centre.z));
                    const float rel = zone ? zone->RelativePower(d) : (1.f - d / radius);
                    out.push_back({prev, p, rel});
                }
                prev = p;
                have_prev = true;
            }
        }
    }
}

const Footprint& footprint(CSpaceRestrictor* SR, CCustomZone* zone, u16 id, const Fvector& centre, float radius, bool want_hatch)
{
    Footprint& fp = s_cache[id];

    const float eff = zone ? zone->effective_radius() : radius;
    const bool stale = !fsimilar(fp.radius, radius) || !fsimilar(fp.eff_radius, eff) || centre.distance_to(fp.centre) > 0.05f || (want_hatch && !fp.hatched);
    if (!stale)
        return fp;

    const float y_from = centre.y + radius + 5.f;

    fp.centre = centre;
    fp.radius = radius;
    fp.eff_radius = eff;
    trace_ring(centre, radius, y_from, fp.outline);

    // The inner ring is only worth drawing when the zone actually acts over a
    // smaller area than its collision shape -- which is the usual case, and
    // exactly the distinction an operator cannot see.
    if (eff > 0.2f && !fsimilar(eff, radius, 0.05f))
        trace_ring(centre, eff, y_from, fp.outline_eff);
    else
        fp.outline_eff.clear();

    if (want_hatch)
    {
        build_hatch(zone, centre, radius, y_from, fp.hatch);
        fp.hatched = true;
    }
    else
    {
        fp.hatch.clear();
        fp.hatched = false;
    }
    return fp;
}

struct Candidate
{
    CSpaceRestrictor* sr;
    CCustomZone* zone;
    float dist;
    bool live;
};
} // namespace

namespace
{
u16 s_selected = 0xffff;
}

void ClZoneDebug::SetSelected(u16 id) { s_selected = id; }

// ⭐The clipped world-space segment, published.
//
// The furniture editor draws boxes and a facing arrow rather than rings, and
// every one of them needs the same near-plane clipping this file already got
// right -- an operator crouched next to the crate they are placing has half the
// ghost behind the eye. Re-deriving that in a second file is how the two
// pictures start disagreeing about what is on screen.
void ClZoneDebug::DrawLine(const Fvector& a, const Fvector& b, u32 color)
{
    if (!UIRender)
        return;
    ensure_shaders();
    line(a, b, color);
}

void ClZoneDebug::BeginLines(u32 segments)
{
    if (!UIRender)
        return;
    ensure_shaders();
    UIRender->StartPrimitive(segments * 6 + 6, IUIRender::ptTriList, UI()->m_currentPointType);
}

void ClZoneDebug::EndLines()
{
    if (!UIRender)
        return;
    UIRender->SetShader(*shader());
    UIRender->FlushPrimitive();
}

bool ClZoneDebug::ToScreen(const Fvector& world, float& sx, float& sy)
{
    const ClipVertex c = to_clip(world);
    if (c.w < NEAR_W)
        return false;
    to_screen(c, sx, sy);
    return true;
}

void ClZoneDebug::EnsureShaders()
{
    if (UIRender)
        ensure_shaders();
}

void ClZoneDebug::DrawFlatDisc(const Fvector& centre, float radius, u32 edge_color, u32 fill_color, float grid_step)
{
    if (!UIRender || radius <= EPS_L)
        return;
    ensure_shaders();

    int grid_lines = 0;
    if (grid_step > EPS_L)
        grid_lines = iFloor((2.f * radius) / grid_step) * 2;

    UIRender->StartPrimitive(u32(RING_SEGMENTS + grid_lines + 8) * 6 + 32, IUIRender::ptTriList, UI()->m_currentPointType);

    if (grid_step > EPS_L)
    {
        for (int axis = 0; axis < 2; ++axis)
            for (float off = -radius + grid_step * 0.5f; off < radius; off += grid_step)
            {
                const float half = _sqrt(_max(0.f, radius * radius - off * off));
                if (half < 0.2f)
                    continue;
                Fvector a, b;
                if (axis == 0)
                {
                    a.set(centre.x - half, centre.y, centre.z + off);
                    b.set(centre.x + half, centre.y, centre.z + off);
                }
                else
                {
                    a.set(centre.x + off, centre.y, centre.z - half);
                    b.set(centre.x + off, centre.y, centre.z + half);
                }
                line(a, b, fill_color);
            }
    }

    Fvector prev;
    for (int i = 0; i <= RING_SEGMENTS; ++i)
    {
        const float ang = (PI_MUL_2 * i) / RING_SEGMENTS;
        Fvector p;
        p.set(centre.x + radius * _cos(ang), centre.y, centre.z + radius * _sin(ang));
        if (i)
            line(prev, p, edge_color);
        prev = p;
    }

    UIRender->SetShader(*shader());
    UIRender->FlushPrimitive();
}

void ClZoneDebug::Dump()
{
    Msg("~ [zone_dbg] mode=%d range=%.0f hatch=%.2f shader=%d | ground rays %u/%u hit", g_cl_zone_debug, g_cl_zone_debug_range, g_cl_zone_debug_hatch, g_cl_zone_debug_shader,
        s_rays_hit, s_rays_total);

    if (!g_pGameLevel || !Level().game_configured)
    {
        Msg("~ [zone_dbg] no level");
        return;
    }

    const Fvector eye = Device.vCameraPosition;
    u32 seen = 0;
    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CObject* O = Level().Objects.o_get_by_iterator(i);
        if (!O || O->getDestroy())
            continue;
        CSpaceRestrictor* SR = smart_cast<CSpaceRestrictor*>(O);
        if (!SR || !SR->CFORM())
            continue;

        Fvector centre;
        SR->Center(centre);
        const float dist = centre.distance_to(eye);
        if (dist > g_cl_zone_debug_range)
            continue;

        CCustomZone* zone = smart_cast<CCustomZone*>(O);
        auto it = s_cache.find(u16(SR->ID()));
        const Footprint* fp = (it != s_cache.end()) ? &it->second : nullptr;

        Msg("~ [zone_dbg]   id=%u '%s' %.0fm live=%d r=%.2f maxpow=%.2f eff_r=%.2f shapes=%u | outline=%u eff=%u hatch=%u hatched=%d", SR->ID(), O->cNameSect().c_str(), dist,
            (zone && zone->IsEnabled()) ? 1 : 0, SR->Radius(), zone ? zone->GetMaxPower() : -1.f, zone ? zone->effective_radius() : -1.f,
            u32(((CCF_Shape*)SR->CFORM())->Shapes().size()), fp ? u32(fp->outline.size()) : 0, fp ? u32(fp->outline_eff.size()) : 0, fp ? u32(fp->hatch.size()) : 0,
            fp ? (fp->hatched ? 1 : 0) : -1);
        ++seen;
    }
    Msg("~ [zone_dbg] %u zone(s) in range, %u cached footprint(s)", seen, u32(s_cache.size()));
}

void ClZoneDebug::Frame()
{
    // Outside the render pass on purpose: a UI shader created from inside
    // rendering came back with a null texture descriptor.
    if (g_cl_zone_debug && g_pGameLevel && Level().game_configured)
        ensure_shaders();
}

// One labelled bar per shader candidate, all in the same pure yellow. See the
// note on the rig above: which of them is actually yellow on screen is a fact
// only a pair of eyes can supply.
static void draw_calibration()
{
    const u32 YELLOW = color_rgba(255, 232, 70, 255);
    CGameFont* F = HUD().Font().pFontSmall;

    for (int i = 0; i < CANDIDATE_COUNT; ++i)
    {
        const float y = 120.f + i * 40.f;
        UIRender->StartPrimitive(64, IUIRender::ptTriList, UI()->m_currentPointType);
        for (int k = 0; k < 6; ++k) // six stacked lines so it reads as a bar
        {
            UIRender->PushPoint(40.f, y + k, 0.f, YELLOW, s_candidates[i].u, s_candidates[i].v);
            UIRender->PushPoint(240.f, y + k, 0.f, YELLOW, s_candidates[i].u, s_candidates[i].v);
        }
        UIRender->SetShader(**s_candidates[i].ptr);
        UIRender->FlushPrimitive();

        if (F)
        {
            F->SetAligment(CGameFont::alLeft);
            F->SetColor(color_rgba(255, 255, 255, 255));
            F->Out(250.f, y - 2.f, "%s%s", s_candidates[i].label, (i == g_pick) ? "   <= in use" : "");
        }
    }
    if (F)
    {
        F->SetColor(color_rgba(255, 255, 255, 255));
        F->Out(40.f, 96.f, "cl_zone_debug_shader <0..%d> -- pick the bar that is YELLOW", CANDIDATE_COUNT - 1);
    }
}

void ClZoneDebug::Render()
{
    if (!g_cl_zone_debug)
        return;
    if (!g_pGameLevel || !Level().game_configured)
        return;
    if (!UIRender)
        return;
    ensure_shaders();

    if (g_cl_zone_debug >= 3)
        draw_calibration();

    const Fvector eye = Device.vCameraPosition;

    // Nearest first, then capped: with the cap applied to an unsorted sweep the
    // zone that gets dropped could be the one at your feet.
    xr_vector<Candidate> found;
    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CObject* O = Level().Objects.o_get_by_iterator(i);
        if (!O || O->getDestroy())
            continue;

        CSpaceRestrictor* SR = smart_cast<CSpaceRestrictor*>(O);
        if (!SR || !SR->CFORM())
            continue;

        Fvector centre;
        SR->Center(centre);
        const float dist = centre.distance_to(eye);
        if (dist > g_cl_zone_debug_range)
            continue;

        CCustomZone* zone = smart_cast<CCustomZone*>(O);
        found.push_back({SR, zone, dist, zone && zone->IsEnabled()});
    }
    if (found.empty())
        return;

    std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) { return a.dist < b.dist; });
    if (found.size() > MAX_ZONES)
        found.resize(MAX_ZONES);

    int hatched = 0;
    for (const auto& cand : found)
    {
        CSpaceRestrictor* SR = cand.sr;
        Fvector centre;
        SR->Center(centre);
        const float radius = SR->Radius();

        const bool want_hatch = cand.live && hatched < MAX_HATCHED && g_cl_zone_debug_hatch > EPS_L;
        const Footprint& fp = footprint(SR, cand.zone, u16(SR->ID()), centre, radius, want_hatch);
        if (want_hatch)
            ++hatched;

        // The editor's selection reads as white, so it is unmistakable against
        // the yellow of everything else.
        const bool picked = (u16(SR->ID()) == s_selected);
        const u32 col_edge = picked ? color_rgba(255, 255, 255, 255) : (cand.live ? color_rgba(255, 232, 70, 255) : color_rgba(170, 170, 170, 130));
        const u32 col_eff = cand.live ? color_rgba(255, 170, 30, 255) : color_rgba(140, 140, 140, 110);
        const u32 col_vol = cand.live ? color_rgba(255, 90, 60, 90) : color_rgba(120, 120, 120, 60);

        // Six vertices per segment now that each one is a quad, not a line.
        u32 budget = u32(fp.outline.size() + fp.outline_eff.size() + fp.hatch.size()) * 6 + 64;
        if (g_cl_zone_debug >= 2)
            budget += RING_SEGMENTS * 3 * 6 + 32;
        UIRender->StartPrimitive(budget, IUIRender::ptTriList, UI()->m_currentPointType);

        // Hatching first, so the outline sits on top of it. The alpha floor is
        // deliberately high: the rim of a zone still hurts, and a fill that
        // fades to nothing there reads as "the zone ends before it does".
        for (const auto& s : fp.hatch)
        {
            const u32 a = u32(110.f + 145.f * _min(1.f, _max(0.f, s.power)));
            line(s.a, s.b, color_rgba(255, 64, 40, a));
        }

        polyline(fp.outline_eff, col_eff, true);
        polyline(fp.outline, col_edge, true);

        // The volume itself, kept faint and only in the verbose mode: it answers
        // "how high does this reach", which the ground print cannot.
        if (g_cl_zone_debug >= 2)
        {
            auto& shapes = ((CCF_Shape*)SR->CFORM())->Shapes();
            for (auto& sh : shapes)
            {
                if (sh.type != 0)
                    continue;
                Fvector wc;
                SR->XFORM().transform_tiny(wc, sh.data.sphere.P);
                const float r = sh.data.sphere.R;
                Fvector prev_a, prev_b, prev_c;
                for (int i = 0; i <= RING_SEGMENTS; ++i)
                {
                    const float ang = (PI_MUL_2 * i) / RING_SEGMENTS;
                    const float cs = _cos(ang), sn = _sin(ang);
                    Fvector pa, pb, pc;
                    pa.set(wc.x + r * cs, wc.y, wc.z + r * sn);
                    pb.set(wc.x + r * cs, wc.y + r * sn, wc.z);
                    pc.set(wc.x, wc.y + r * sn, wc.z + r * cs);
                    if (i)
                    {
                        line(prev_a, pa, col_vol);
                        line(prev_b, pb, col_vol);
                        line(prev_c, pc, col_vol);
                    }
                    prev_a = pa;
                    prev_b = pb;
                    prev_c = pc;
                }
            }
        }

        UIRender->SetShader(*shader());
        UIRender->FlushPrimitive();
    }

    if (g_cl_zone_debug >= 2)
    {
        CGameFont* F = HUD().Font().pFontSmall;
        if (!F)
            return;
        F->SetAligment(CGameFont::alCenter);
        for (const auto& cand : found)
        {
            Fvector centre;
            cand.sr->Center(centre);
            const ClipVertex cc = to_clip(centre);
            if (cc.w < NEAR_W)
                continue;
            float sx, sy;
            to_screen(cc, sx, sy);
            F->SetColor(cand.live ? color_rgba(255, 232, 70, 255) : color_rgba(190, 190, 190, 200));
            F->Out(sx, sy, "%s", cand.sr->cNameSect().c_str());
            if (cand.zone)
                F->Out(sx, sy + 14.f, "r=%.1fm  hit=%.2f  %.0fm%s", cand.sr->Radius(), cand.zone->GetMaxPower(), cand.dist, cand.live ? "" : "  (off)");
            else
                F->Out(sx, sy + 14.f, "r=%.1fm  %.0fm  (restrictor)", cand.sr->Radius(), cand.dist);
        }
        F->SetAligment(CGameFont::alLeft);
    }
}
