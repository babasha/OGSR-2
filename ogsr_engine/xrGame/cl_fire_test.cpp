#include "StdAfx.h"
#include "cl_fire_test.h"
#include "shot_latency.h"

#include "Level.h"
#include "Actor.h"
#include "ActorEffector.h" // CActorCameraManager -- Actor.h only forward-declares it
#include "entity_alive.h"
#include "Inventory.h"
#include "Weapon.h"
#include "HudItem.h"
#include "xr_level_controller.h"
#include "../xr_3da/CameraManager.h"
#include "../xr_3da/CameraBase.h"

namespace
{
enum EState
{
    stOff = 0,
    stWait,    // level up, actor alive, weapon in hand, target in sight
    stProbe0,  // remember where the camera points
    stProbe1,  // nudge both axes and see which way the world moved
    stAim,     // closed loop onto the target
    stPress,   // trigger down
    stRelease, // trigger up, then wait out the gap
    stDone
};

EState s_state = stOff;
int s_want_shots = 0;
int s_fired = 0;
u32 s_gap_ms = 400;
float s_max_range = 60.f;

u16 s_target = 0xffff;
bool s_at_point = false; // StartAtPoint: aim at s_point, not at a creature
Fvector s_point{};
u32 s_next_shot_ms = 0;
u32 s_aim_frames = 0;
u32 s_waited_frames = 0;

// Camera axis -> world direction, measured rather than assumed. See the header.
float s_k_yaw = -1.f, s_k_pitch = -1.f;
bool s_calibrated = false;
Fvector s_probe_dir{};

// The nudge used to measure the mapping. Big enough that a frame of camera
// noise cannot swamp it, small enough that it is not a visible flick.
constexpr float PROBE_RAD = 0.08f;

// Aim is good enough to shoot: ~0.5 degrees, which is 17 cm at 20 m -- inside a
// torso. Breathing and weapon sway mean the loop cannot converge much past this
// no matter how long it is given.
constexpr float AIM_TOL_RAD = 0.009f;
// ...and if it never gets there, shoot anyway after this many frames and say
// what the residual was. A test that quietly waits forever reports nothing.
constexpr u32 AIM_MAX_FRAMES = 90;
constexpr u32 WAIT_WARN_FRAMES = 300;

CActor* actor()
{
    if (!g_pGameLevel)
        return nullptr;
    return smart_cast<CActor*>(Level().CurrentControlEntity());
}

// Nearest living creature that is not us. Nearest rather than "the one with
// the right id": the server places the puppets, and this side should not need
// to be told which ids it chose.
u16 pick_target(CActor* A, Fvector& out_center, float& out_dist)
{
    u16 best = 0xffff;
    float best_d = s_max_range;

    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CObject* O = Level().Objects.o_get_by_iterator(i);
        if (!O || O == A || O->getDestroy())
            continue;
        CEntityAlive* E = smart_cast<CEntityAlive*>(O);
        if (!E || !E->g_Alive() || !E->Visual())
            continue;

        Fvector c;
        E->Center(c);
        const float d = c.distance_to(A->Position());
        if (d < best_d)
        {
            best_d = d;
            best = u16(O->ID());
            out_center = c;
            out_dist = d;
        }
    }
    return best;
}

CWeapon* weapon_in_hand(CActor* A)
{
    CWeapon* W = smart_cast<CWeapon*>(A->inventory().ActiveItem());
    if (W)
        return W;

    // Nothing in hand: take out whatever is in a weapon slot. The server hands
    // the actor a gun by spawning it parented, and an item that arrives that
    // way is not necessarily drawn. Asked for at most twice a second -- the
    // draw takes an animation, and re-asking every frame would restart it.
    static u32 s_last_draw_ms = 0;
    if (Device.dwTimeGlobal - s_last_draw_ms < 500)
        return nullptr;

    const u32 weapon_slots[] = {FIRST_WEAPON_SLOT, SECOND_WEAPON_SLOT};
    for (u32 slot : weapon_slots)
    {
        if (A->inventory().ItemFromSlot(slot))
        {
            s_last_draw_ms = Device.dwTimeGlobal;
            A->inventory().Activate(slot);
            return nullptr; // next frame it will be the active item
        }
    }
    return nullptr;
}

void aim_error(CActor* A, const Fvector& target, float& err_h, float& err_p, float& err_total)
{
    Fvector want;
    want.sub(target, A->Cameras().Position());
    want.normalize_safe();

    const Fvector& act = A->Cameras().Direction();

    err_h = angle_difference_signed(want.getH(), act.getH());
    err_p = want.getP() - act.getP();
    err_total = acosf(_max(-1.f, _min(1.f, want.dotproduct(act))));
}
} // namespace

void CLFire::Start(int shots, u32 gap_ms, float max_range)
{
    s_want_shots = _max(1, shots);
    s_gap_ms = gap_ms ? gap_ms : 400;
    s_max_range = max_range > 1.f ? max_range : 60.f;
    s_fired = 0;
    s_target = 0xffff;
    s_at_point = false;
    s_calibrated = false;
    s_waited_frames = 0;
    s_state = stWait;

    // Whoever asked for the fire test wants the measurement it exists for.
    ShotLatency::g_shot_trace = 1;
    Msg("~ [cl_fire] armed: %d shots, %u ms apart, targets within %.0f m", s_want_shots, s_gap_ms, s_max_range);
}

void CLFire::StartAtPoint(int shots, u32 gap_ms, const Fvector& point)
{
    Start(shots, gap_ms, 1000.f);
    s_at_point = true;
    s_point = point;
    Msg("~ [cl_fire] ...at the point (%.1f, %.1f, %.1f)", point.x, point.y, point.z);
}

void CLFire::Stop()
{
    if (CActor* A = actor())
        A->IR_OnKeyboardRelease(kWPN_FIRE);
    s_state = stOff;
    Msg("~ [cl_fire] stopped after %d shots", s_fired);
}

void CLFire::Status()
{
    Msg("~ [cl_fire] state=%d fired=%d/%d target=%u yaw_k=%.2f pitch_k=%.2f calibrated=%d", int(s_state), s_fired, s_want_shots, s_target, s_k_yaw, s_k_pitch, s_calibrated ? 1 : 0);
    ShotLatency::Report();
}

void CLFire::Frame()
{
    // Command line arming, same shape as -net_move_test: the client is started
    // by a script that has no way to type into its console.
    static bool s_cmdline_checked = false;
    static u32 s_arm_at_ms = 0;
    if (!s_cmdline_checked)
    {
        s_cmdline_checked = true;
        if (const char* p = strstr(Core.Params, "-net_fire_test"))
        {
            int shots = 8, delay_s = 20;
            u32 gap = 500;
            sscanf(p + xr_strlen("-net_fire_test"), "%d %d %u", &shots, &delay_s, &gap);
            s_want_shots = shots;
            s_gap_ms = gap;
            s_arm_at_ms = Device.dwTimeGlobal + u32(_max(0, delay_s) * 1000);
            Msg("~ [cl_fire] -net_fire_test: %d shots, first in %d s, %u ms apart", shots, delay_s, gap);
        }
    }
    if (s_arm_at_ms && Device.dwTimeGlobal >= s_arm_at_ms)
    {
        s_arm_at_ms = 0;
        Start(s_want_shots, s_gap_ms, s_max_range);
    }

    if (s_state == stOff || s_state == stDone)
        return;

    CActor* A = actor();
    if (!A || !A->g_Alive())
        return;

    Fvector target_center{};
    float target_dist = 0.f;

    switch (s_state)
    {
    case stWait: {
        ++s_waited_frames;
        CWeapon* W = weapon_in_hand(A);
        const u16 t = s_at_point ? u16(0) : pick_target(A, target_center, target_dist);
        if (s_at_point)
            target_dist = s_point.distance_to(A->Position());

        if (!W || W->IsPending() || W->GetState() != CHUDState::eIdle || t == 0xffff)
        {
            // Periodically, not once: "stuck" needs to be visible for as long
            // as it lasts, and which of the three preconditions is missing is
            // the whole diagnosis.
            if ((s_waited_frames % WAIT_WARN_FRAMES) == 0)
                Msg("! [cl_fire] still waiting: weapon=%s state=%d target=%s (%u frames)", W ? W->cNameSect().c_str() : "none", W ? int(W->GetState()) : -1,
                    t == 0xffff ? "none in range" : "found", s_waited_frames);
            return;
        }

        s_target = t;
        Msg("~ [cl_fire] target %u at %.1f m, weapon '%s' with %d rounds", s_target, target_dist, W->cNameSect().c_str(), W->GetAmmoElapsed());
        s_state = stProbe0;
        return;
    }

    case stProbe0: {
        s_probe_dir.set(A->Cameras().Direction());
        A->cam_Active()->yaw += PROBE_RAD;
        A->cam_Active()->pitch += PROBE_RAD;
        s_state = stProbe1;
        return;
    }

    case stProbe1: {
        const Fvector& now = A->Cameras().Direction();
        const float dH = angle_difference_signed(now.getH(), s_probe_dir.getH());
        const float dP = now.getP() - s_probe_dir.getP();

        // A near-zero response means the axis did not move the world (a clamped
        // camera, a cutscene effector). Falling back to the documented sign is
        // better than dividing by noise, but it has to be said out loud.
        if (_abs(dH) > 0.01f)
            s_k_yaw = PROBE_RAD / dH;
        else
            Msg("! [cl_fire] yaw probe moved the view by %.4f rad -- keeping k=%.2f", dH, s_k_yaw);
        if (_abs(dP) > 0.01f)
            s_k_pitch = PROBE_RAD / dP;
        else
            Msg("! [cl_fire] pitch probe moved the view by %.4f rad -- keeping k=%.2f", dP, s_k_pitch);

        A->cam_Active()->yaw -= PROBE_RAD;
        A->cam_Active()->pitch -= PROBE_RAD;

        s_calibrated = true;
        Msg("~ [cl_fire] camera mapping measured: yaw k=%.2f (dH=%.4f), pitch k=%.2f (dP=%.4f)", s_k_yaw, dH, s_k_pitch, dP);
        s_aim_frames = 0;
        s_state = stAim;
        return;
    }

    case stAim: {
        CObject* T = s_at_point ? nullptr : Level().Objects.net_Find(s_target);
        CEntityAlive* E = smart_cast<CEntityAlive*>(T);
        if (s_at_point)
            target_center = s_point;
        else if (!E || !E->g_Alive())
        {
            // It died or left. Finding the next one is the whole point of a
            // burst test, so go back and look rather than stopping.
            Msg("~ [cl_fire] target %u is gone, looking for another", s_target);
            s_waited_frames = 0;
            s_state = stWait;
            return;
        }
        if (!s_at_point)
            E->Center(target_center);

        float eh, ep, et;
        aim_error(A, target_center, eh, ep, et);

        CCameraBase* C = A->cam_Active();
        C->yaw += s_k_yaw * eh;
        C->pitch += s_k_pitch * ep;

        ++s_aim_frames;
        if (et > AIM_TOL_RAD && s_aim_frames < AIM_MAX_FRAMES)
            return;

        CWeapon* W = smart_cast<CWeapon*>(A->inventory().ActiveItem());
        if (!W || W->IsPending() || W->GetState() != CHUDState::eIdle)
            return; // reloading; the aim loop keeps holding
        // a jam (or an empty magazine) does not clear itself: the trigger would click forever
        if (W->IsMisfire() || W->GetAmmoElapsed() == 0)
        {
            static u32 s_last_reload_ms = 0;
            if (Device.dwTimeGlobal - s_last_reload_ms > 1000)
            {
                s_last_reload_ms = Device.dwTimeGlobal;
                Msg("~ [cl_fire] %s -- reloading", W->IsMisfire() ? "jammed" : "empty");
                W->Action(kWPN_RELOAD, CMD_START);
            }
            return;
        }

        Msg("~ [cl_fire] shot %d/%d: aim residual %.3f deg after %u frames, target %u at %.1f m, %d rounds left", s_fired + 1, s_want_shots, rad2deg(et), s_aim_frames,
            s_target, target_center.distance_to(A->Cameras().Position()), W->GetAmmoElapsed());

        A->IR_OnKeyboardPress(kWPN_FIRE);
        s_state = stPress;
        return;
    }

    case stPress: {
        // Held for exactly one frame: long enough for the weapon state machine
        // to start a shot, short enough that an automatic gun sends one round.
        A->IR_OnKeyboardRelease(kWPN_FIRE);
        ++s_fired;
        s_next_shot_ms = Device.dwTimeGlobal + s_gap_ms;
        s_state = stRelease;
        return;
    }

    case stRelease: {
        if (Device.dwTimeGlobal < s_next_shot_ms)
            return;
        if (s_fired >= s_want_shots)
        {
            Msg("~ [cl_fire] done: %d shots fired", s_fired);
            ShotLatency::Report();
            s_state = stDone;
            return;
        }
        s_aim_frames = 0;
        s_state = stAim;
        return;
    }

    default: return;
    }
}
