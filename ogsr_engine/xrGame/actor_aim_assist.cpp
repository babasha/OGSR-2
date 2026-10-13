#include "StdAfx.h"
#include "actor_aim_assist.h"
#include "Actor.h"
#include "ActorEffector.h"
#include "actor_defs.h"
#include "Inventory.h"
#include "Level.h"
#include "HUDManager.h"
#include "UIGameCustom.h"
#include "Weapon.h"
#include "WeaponKnife.h"
#include "WeaponBinoculars.h"
#include "entity_alive.h"
#include "xr_level_controller.h"
#include "../xr_3da/CameraBase.h"
#include "../xr_3da/IInputReceiver.h"
#include "../Include/xrRender/Kinematics.h"

// 0 off, 1 while the look comes from a pad, 2 the mouse too (a PC test)
int g_aim_assist = 1;
// everything at once: 0 = nothing, 1 = as tuned, 2 = twice. The Switch's sticks are the worst, so the strongest there.
#if defined(__ANDROID__)
float g_aa_strength = .75f;
#else
float g_aa_strength = 1.f;
#endif
// the assist zone around the aim point: this many metres to the side (hip / down the sights), as an angle within the limits
float g_aa_zone_m = 1.1f;
float g_aa_zone_ads_m = 1.4f;
float g_aa_zone_min_deg = 2.5f;
float g_aa_zone_max_deg = 14.f;
float g_aa_range = 90.f; // m
// the look at the centre of the zone runs at (1 - this); down the sights x1.3, never below 0.15
float g_aa_friction = .55f;
// the share of the target's angular motion the camera follows (while the player is engaged: stick, move, fire, sights)
float g_aa_track = .85f;
// deg/s onto the aim point while the stick moves towards it (down the sights x1.6)
float g_aa_pull = 7.f;
// aiming down the sights within this angle (or the zone, if wider) turns onto the target in g_aa_snap_time
float g_aa_snap_deg = 9.f;
float g_aa_snap_time = .16f; // s
// a shot that misses the aim point by less than this many metres (and g_aa_bullet_max_deg) goes onto it
float g_aa_bullet_m = .5f;
float g_aa_bullet_max_deg = 2.5f;
// 1: a line about the target twice a second
int g_aa_dbg = 0;

namespace
{
constexpr u32 kRefreshMs = 100; // the enemy list around the player
constexpr u32 kVisMs = 120; // one sight ray per target per this
constexpr float kStickEps = .05f;

struct Cand
{
    u16 id;
    u16 bone; // BI_NONE: the bounding sphere's centre
    u32 vis_time;
    bool vis;
};
xr_vector<Cand> s_cands;
xr_vector<CObject*> s_nearest;
xr_vector<ISpatial*> s_spatial;
u32 s_refresh_time{};

// this frame's target
u32 s_frame{};
bool s_has{};
u16 s_id{u16(-1)};
Fvector s_point{};
float s_friction = 1.f;
// the previous frame's direction to it, for the tracking
u16 s_prev_id{u16(-1)};
u32 s_prev_frame{};
float s_prev_yaw{}, s_prev_pitch{};
// the snap
bool s_was_zoomed{};
u16 s_snap_id{u16(-1)};
float s_snap_left{};
u32 s_dbg_time{};

// aa_test
struct STest
{
    int mode{}; // 0 none, 1 snap, 2 track
    bool pending{}; // no enemy in sight yet
    u32 wait_until{}, try_time{};
    float wait{}, off_deg{};
    u16 id{u16(-1)};
    u16 bone{BI_NONE};
    u32 t0{};
    float secs{}, off0{};
    u32 frames{}, inside{};
    float sum{}, max{}, conv_max{};
    float off_at[3]{};
    bool got_at[3]{};
};
STest s_test;
constexpr u32 kSnapMarks[3] = {100, 200, 400}; // ms

bool pad_look_active()
{
    if (g_aim_assist >= 2 || (g_aim_assist == 1 && s_test.mode && !s_test.pending))
        return true;
    return g_aim_assist == 1 && g_pad_look.frame && Device.dwFrame - g_pad_look.frame <= 2;
}

// the camera turned by these (radians, in the camera's own yaw/pitch); Move() clamps the pitch
void turn(CCameraBase* cam, float dyaw, float dpitch)
{
    if (dyaw != 0.f)
        cam->Move(dyaw > 0.f ? kRIGHT : kLEFT, _abs(dyaw));
    if (dpitch != 0.f)
        cam->Move(dpitch > 0.f ? kUP : kDOWN, _abs(dpitch));
}

Fvector aim_point(CEntityAlive& E, u16 bone)
{
    Fvector p;
    IKinematics* K = bone != BI_NONE && E.Visual() ? E.Visual()->dcast_PKinematics() : nullptr;
    if (K && bone < K->LL_BoneCount())
    {
        Fmatrix M;
        M.mul_43(E.XFORM(), K->LL_GetTransform(bone));
        p.set(M.c);
    }
    else
        E.Center(p);
    return p;
}

void refresh(CActor& actor, const Fvector& from)
{
    s_refresh_time = Device.dwTimeGlobal;
    xr_vector<Cand> old;
    old.swap(s_cands);
    Level().ObjectSpace.GetNearest(s_spatial, s_nearest, from, g_aa_range, &actor);
    for (CObject* O : s_nearest)
    {
        CEntityAlive* E = smart_cast<CEntityAlive*>(O);
        if (!E || !E->g_Alive() || !E->is_relation_enemy(&actor))
            continue;
        Cand c{E->ID(), BI_NONE, 0, false};
        if (!E->cast_base_monster() && E->Visual())
            if (IKinematics* K = E->Visual()->dcast_PKinematics())
                c.bone = K->LL_BoneID("bip01_spine2");
        for (const Cand& o : old) // keep the sight ray's answer
            if (o.id == c.id)
            {
                c.vis_time = o.vis_time;
                c.vis = o.vis;
            }
        s_cands.push_back(c);
    }
}

bool visible(CActor& actor, Cand& c, const Fvector& from, const Fvector& dir, float dist)
{
    if (c.vis_time && Device.dwTimeGlobal - c.vis_time < kVisMs)
        return c.vis;
    c.vis_time = Device.dwTimeGlobal;
    collide::rq_result RQ;
    c.vis = !Level().ObjectSpace.RayPick(from, dir, dist, collide::rqtStatic, RQ, &actor) || RQ.range > dist - .3f;
    return c.vis;
}

void reset()
{
    s_has = false;
    s_friction = 1.f;
    s_prev_id = u16(-1);
    s_snap_left = 0.f;
}

// the miss of the crosshair from the test target's aim point, radians, < 0 = no target. Measured on the REAL camera
// direction (shake included), not on the yaw/pitch the assist works in: a wrong sign there would not hide here.
// conv: the angle between the two, which must stay small (the shake, a frame of lag) if the convention is right.
float test_off(CActor& actor, float* dist, float* conv)
{
    CEntityAlive* E = smart_cast<CEntityAlive*>(Level().Objects.net_Find(s_test.id));
    if (!E || !E->g_Alive())
        return -1.f;
    CCameraBase* cam = actor.cam_Active();
    Fvector mine, to;
    mine.setHP(-cam->yaw, -cam->pitch);
    const Fvector& cam_dir = actor.Cameras().Direction();
    *conv = acosf(clampr(cam_dir.dotproduct(mine), -1.f, 1.f));
    to.sub(aim_point(*E, s_test.bone), actor.Cameras().Position());
    *dist = to.magnitude();
    to.div(*dist);
    return acosf(clampr(cam_dir.dotproduct(to), -1.f, 1.f));
}

// the nearest enemy in sight; the crosshair put on it, then off_deg to the right
bool test_acquire(CActor& actor)
{
    const Fvector from = actor.Cameras().Position();
    refresh(actor, from);
    Cand* best = nullptr;
    float best_dist = flt_max;
    Fvector best_point{};
    for (Cand& c : s_cands)
    {
        CEntityAlive* E = smart_cast<CEntityAlive*>(Level().Objects.net_Find(c.id));
        if (!E)
            continue;
        const Fvector p = aim_point(*E, c.bone);
        Fvector dir;
        dir.sub(p, from);
        const float dist = dir.magnitude();
        if (dist < 1.f || dist >= best_dist)
            continue;
        dir.div(dist);
        c.vis_time = 0;
        if (!visible(actor, c, from, dir, dist))
            continue;
        best = &c;
        best_dist = dist;
        best_point = p;
    }
    if (!best)
        return false;

    Fvector dir;
    dir.sub(best_point, from).normalize();
    float h, p;
    dir.getHP(h, p);
    CCameraBase* cam = actor.cam_Active();
    cam->yaw = -h + deg2rad(s_test.off_deg);
    cam->pitch = -p;

    s_test.pending = false;
    s_test.id = best->id;
    s_test.bone = best->bone;
    s_test.t0 = Device.dwTimeGlobal;
    s_test.off0 = deg2rad(_abs(s_test.off_deg));
    s_was_zoomed = false;
    reset();
    CObject* O = Level().Objects.net_Find(best->id);
    Msg("[aa_test] %s: '%s' at %.1f m, the crosshair %.1f deg off (aim_assist %d strength %.2f)", s_test.mode == 1 ? "snap" : "track", O ? O->cName().c_str() : "?",
        best_dist, s_test.off_deg, g_aim_assist, g_aa_strength);
    return true;
}

// once a frame, before the assist acts: what the previous frames did to the crosshair
void test_measure(CActor& actor)
{
    if (!s_test.mode)
        return;
    const char* what = s_test.mode == 1 ? "snap" : "track";
    if (s_test.pending)
    {
        if (Device.dwTimeGlobal >= s_test.wait_until)
        {
            Msg("! [aa_test] %s: no enemy in sight for %.0f s (%u enemies within %.0f m)", what, s_test.wait, u32(s_cands.size()), g_aa_range);
            const Fvector from = actor.Cameras().Position();
            for (const Cand& c : s_cands)
                if (CEntityAlive* E = smart_cast<CEntityAlive*>(Level().Objects.net_Find(c.id)))
                {
                    Fvector dir;
                    dir.sub(aim_point(*E, c.bone), from);
                    const float dist = dir.magnitude();
                    dir.div(dist);
                    collide::rq_result RQ;
                    const bool hit = Level().ObjectSpace.RayPick(from, dir, dist, collide::rqtStatic, RQ, &actor);
                    Msg("  [aa_test] '%s' %.1f m, the sight ray %s %.1f m", E->cName().c_str(), dist, hit ? "stops at" : "is clear,", hit ? RQ.range : dist);
                }
            s_test.mode = 0;
        }
        else if (Device.dwTimeGlobal - s_test.try_time >= 100)
        {
            s_test.try_time = Device.dwTimeGlobal;
            test_acquire(actor);
        }
        return;
    }
    float dist = 0.f, conv = 0.f;
    const float off = test_off(actor, &dist, &conv);
    const u32 t = Device.dwTimeGlobal - s_test.t0;
    CObject* O = Level().Objects.net_Find(s_test.id);
    const char* name = O ? O->cName().c_str() : "?";
    if (off < 0.f)
    {
        Msg("[aa_test] %s: the target is gone after %u ms (aim_assist %d)", what, t, g_aim_assist);
        s_test.mode = 0;
        return;
    }
    if (t >= 50) // the first frames still show the camera from before the test turned it
        s_test.conv_max = std::max(s_test.conv_max, conv);
    if (s_test.mode == 1)
    {
        for (int i = 0; i < 3; ++i)
            if (!s_test.got_at[i] && t >= kSnapMarks[i])
            {
                s_test.got_at[i] = true;
                s_test.off_at[i] = off;
            }
        if (s_test.got_at[2])
        {
            Msg("[aa_test] snap '%s' %.1f m: off %.2f deg -> 100 ms %.2f, 200 ms %.2f, 400 ms %.2f (body %.2f deg; conv %.2f deg; aim_assist %d strength %.2f)", name,
                dist, rad2deg(s_test.off0), rad2deg(s_test.off_at[0]), rad2deg(s_test.off_at[1]), rad2deg(s_test.off_at[2]), rad2deg(atanf(.3f / dist)),
                rad2deg(s_test.conv_max), g_aim_assist, g_aa_strength);
            s_test.mode = 0;
        }
        return;
    }
    ++s_test.frames;
    s_test.sum += off;
    s_test.max = std::max(s_test.max, off);
    if (off < atanf(.3f / dist)) // within a torso's half-width
        ++s_test.inside;
    if (float(t) >= s_test.secs * 1000.f)
    {
        Msg("[aa_test] track '%s' %.1f m, %.1f s: mean off %.2f deg, max %.2f, on the body %u%% of %u frames (conv %.2f deg; aim_assist %d strength %.2f)", name, dist,
            s_test.secs, rad2deg(s_test.sum / float(s_test.frames)), rad2deg(s_test.max), s_test.inside * 100 / s_test.frames, s_test.frames,
            rad2deg(s_test.conv_max), g_aim_assist, g_aa_strength);
        s_test.mode = 0;
    }
}
} // namespace

namespace AimAssist
{
void update(CActor& actor)
{
    if (s_frame == Device.dwFrame)
        return;
    s_frame = Device.dwFrame;
    s_has = false;
    s_friction = 1.f;
    test_measure(actor);

    CWeapon* W = smart_cast<CWeapon*>(actor.inventory().ActiveItem());
    const bool zoomed = (W && W->IsZoomed()) || (s_test.mode == 1 && !s_test.pending); // aa_test snap: the sights come up
    const bool zoom_started = zoomed && !s_was_zoomed;
    s_was_zoomed = zoomed;

    if (g_aim_assist <= 0 || g_aa_strength <= 0.f || !pad_look_active() || !actor.g_Alive() || actor.Holder() ||
        actor.active_cam() != eacFirstEye || !W || smart_cast<CWeaponKnife*>(W) || smart_cast<CWeaponBinoculars*>(W) ||
        (actor.MovingState() & mcSprint) || HUD().GetUI()->MainInputReceiver())
    {
        reset();
        return;
    }

    CCameraBase* cam = actor.cam_Active();
    const Fvector from = actor.Cameras().Position();

    if (s_refresh_time == 0 || Device.dwTimeGlobal - s_refresh_time >= kRefreshMs || Device.dwTimeGlobal < s_refresh_time)
        refresh(actor, from);

    const float strength = std::min(g_aa_strength, 2.f);
    const float zone_m = (zoomed ? g_aa_zone_ads_m : g_aa_zone_m) * std::max(strength, .5f);
    const float zone_min = deg2rad(g_aa_zone_min_deg), zone_max = deg2rad(g_aa_zone_max_deg);
    const float snap_cone = deg2rad(g_aa_snap_deg);

    Fvector cam_dir;
    cam_dir.setHP(-cam->yaw, -cam->pitch);

    // the best target: the smallest miss in units of its zone; a sight ray only for the ones that would win
    struct Hit
    {
        Cand* c;
        Fvector point, dir;
        float dist, off, zone, score;
    };
    Hit best{};
    best.score = flt_max;
    // while the snap runs (and on the frame it starts) a target out to the snap cone still counts
    const bool snapping = zoom_started || s_snap_left > 0.f;
    const float score_max = snapping ? 1.f + snap_cone / zone_min : 1.f;
    for (Cand& c : s_cands)
    {
        CEntityAlive* E = smart_cast<CEntityAlive*>(Level().Objects.net_Find(c.id));
        if (!E || !E->g_Alive() || E->getDestroy())
            continue;
        Hit h;
        h.c = &c;
        h.point = aim_point(*E, c.bone);
        h.dir.sub(h.point, from);
        h.dist = h.dir.magnitude();
        if (h.dist < .5f || h.dist > g_aa_range)
            continue;
        h.dir.div(h.dist);
        h.off = acosf(clampr(cam_dir.dotproduct(h.dir), -1.f, 1.f));
        h.zone = clampr(atanf(zone_m / h.dist), zone_min, zone_max);
        const float reach = snapping ? std::max(h.zone, snap_cone) : h.zone;
        if (h.off > reach)
            continue;
        h.score = h.off / h.zone;
        if (h.score >= best.score || h.score > score_max)
            continue;
        if (!visible(actor, c, from, h.dir, h.dist))
            continue;
        best = h;
    }

    if (!best.c)
    {
        s_prev_id = u16(-1);
        s_snap_left = 0.f;
        return;
    }

    s_has = true;
    s_id = best.c->id;
    s_point = best.point;

    float h_t, p_t;
    best.dir.getHP(h_t, p_t);
    const float yaw_t = -h_t, pitch_t = -p_t;
    const float dyaw = angle_normalize_signed(yaw_t - cam->yaw);
    const float dpitch = pitch_t - cam->pitch;

    // inside the zone 0 at the edge .. 1 at the centre, smooth
    const float x = 1.f - clampr(best.off / best.zone, 0.f, 1.f);
    const float w = x * x * (3.f - 2.f * x);

    // 1. friction
    const float fr = std::min(g_aa_friction * strength * (zoomed ? 1.3f : 1.f), .85f);
    s_friction = 1.f - fr * w;

    const float dt = std::min(Device.fTimeDelta, .1f);
    const float stick = _sqrt(g_pad_look.x * g_pad_look.x + g_pad_look.y * g_pad_look.y);
    const bool stick_on = stick > kStickEps || g_aim_assist >= 2;
    const bool engaged = stick_on || zoomed || (s_test.mode && !s_test.pending) || (actor.MovingState() & mcAnyMove) || W->GetState() == CWeapon::eFire;

    // 4. the snap: started when the sights come up, cancelled when the target changes
    if (zoom_started && best.off <= std::max(best.zone, snap_cone))
    {
        s_snap_id = s_id;
        s_snap_left = std::max(g_aa_snap_time, .01f);
    }
    if (s_snap_left > 0.f)
    {
        if (s_snap_id != s_id || !zoomed)
            s_snap_left = 0.f;
        else
        {
            const float f = std::min(dt / s_snap_left, 1.f) * std::min(strength, 1.f);
            turn(cam, dyaw * f, dpitch * f);
            s_snap_left -= dt;
            s_prev_id = s_id; // the tracking picks up from where the snap leaves it
            s_prev_yaw = yaw_t;
            s_prev_pitch = pitch_t;
            s_prev_frame = Device.dwFrame;
            return;
        }
    }

    // 2. tracking: the change of the direction to the target since the last frame, whatever moved
    if (s_prev_id == s_id && Device.dwFrame - s_prev_frame <= 2 && engaged && w > 0.f)
    {
        const float my = angle_normalize_signed(yaw_t - s_prev_yaw);
        const float mp = pitch_t - s_prev_pitch;
        if (_abs(my) < .15f && _abs(mp) < .15f) // a jump is a respawn or a teleport, not motion
        {
            const float k = std::min(g_aa_track * strength * (zoomed ? 1.1f : 1.f), 1.f) * std::min(2.f * w, 1.f);
            turn(cam, my * k, mp * k);
        }
    }
    s_prev_id = s_id;
    s_prev_yaw = yaw_t;
    s_prev_pitch = pitch_t;
    s_prev_frame = Device.dwFrame;

    // 3. pull: only while the stick points towards the target (stick right = yaw +, stick up = pitch -)
    if (stick_on && g_aa_pull > 0.f && g_aim_assist == 1)
    {
        const float up = psMouseInvert.test(1) ? -g_pad_look.y : g_pad_look.y;
        const float towards = g_pad_look.x * dyaw - up * dpitch;
        if (towards > 0.f)
        {
            const float rate = deg2rad(g_aa_pull) * strength * (zoomed ? 1.6f : 1.f) * std::min(stick * 2.f, 1.f);
            const float len = _sqrt(dyaw * dyaw + dpitch * dpitch);
            if (len > EPS_L)
            {
                const float step = std::min(rate * dt, len);
                turn(cam, dyaw / len * step, dpitch / len * step);
            }
        }
    }

    if (g_aa_dbg && Device.dwTimeGlobal - s_dbg_time > 500)
    {
        s_dbg_time = Device.dwTimeGlobal;
        CObject* O = Level().Objects.net_Find(s_id);
        Msg("[aa] %s d=%.1f m off=%.2f zone=%.2f deg fr=%.2f%s%s", O ? O->cName().c_str() : "?", best.dist, rad2deg(best.off), rad2deg(best.zone),
            s_friction, zoomed ? " ads" : "", engaged ? " engaged" : "");
    }
}

float look_factor()
{
    if (!s_has || Device.dwFrame - s_frame > 1 || g_aim_assist <= 0)
        return 1.f;
    if (g_aim_assist == 1 && !pad_look_active())
        return 1.f;
    return s_friction;
}

void bend_shot(const Fvector& fire_pos, Fvector& fire_dir)
{
    if (!s_has || Device.dwFrame - s_frame > 1 || g_aa_bullet_m <= 0.f)
        return;
    Fvector to;
    to.sub(s_point, fire_pos);
    const float dist = to.magnitude();
    if (dist < .5f)
        return;
    to.div(dist);
    const float off = acosf(clampr(fire_dir.dotproduct(to), -1.f, 1.f));
    const float lim = std::min(atanf(g_aa_bullet_m * std::min(g_aa_strength, 2.f) / dist), deg2rad(g_aa_bullet_max_deg));
    if (off <= lim)
    {
        if (g_aa_dbg)
            Msg("[aa] shot bent by %.2f deg (limit %.2f) at %.1f m", rad2deg(off), rad2deg(lim), dist);
        fire_dir.set(to);
    }
}

void test_command(LPCSTR args)
{
    CActor* actor = g_pGameLevel ? Actor() : nullptr;
    if (!actor || !args || !args[0])
    {
        Msg("[aa_test] usage: aa_test snap [off=6] [wait=15] | track [time=4] [off=0] [wait=15] | stop (a firearm in the hands, an enemy in sight)");
        return;
    }
    if (!strncmp(args, "stop", 4))
    {
        s_test.mode = 0;
        return;
    }
    const int mode = !strncmp(args, "snap", 4) ? 1 : !strncmp(args, "track", 5) ? 2 : 0;
    if (!mode)
    {
        Msg("! [aa_test] unknown '%s'", args);
        return;
    }
    s_test = STest{};
    s_test.mode = mode;
    s_test.off_deg = mode == 1 ? 6.f : 0.f;
    s_test.secs = 4.f;
    s_test.wait = 15.f;
    if (const char* p = strstr(args, "off="))
        s_test.off_deg = float(atof(p + 4));
    if (const char* p = strstr(args, "time="))
        s_test.secs = float(atof(p + 5));
    if (const char* p = strstr(args, "wait="))
        s_test.wait = float(atof(p + 5));
    // the enemy may be behind a house: keep looking (test_measure) until one is in sight or the wait runs out
    s_test.pending = true;
    s_test.wait_until = Device.dwTimeGlobal + u32(s_test.wait * 1000.f);
    s_test.try_time = Device.dwTimeGlobal;
    test_acquire(*actor);
}
} // namespace AimAssist
