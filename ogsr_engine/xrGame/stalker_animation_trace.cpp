////////////////////////////////////////////////////////////////////////////
//  Stalker animation trace: what the animator chose, and what it chose it from.
//
//  ⭐WHY THIS EXISTS.
//
//  The networked puppet's animation was being built by hand -- pick a cycle
//  name from a speed band, PlayCycle it, hope. Every defect it produced
//  ("дёрганое", "путаются в ногах", "телепортируются в 3 fps") was answered
//  with another threshold, and none of them converged, because the thing being
//  imitated is not a name lookup: CStalkerAnimationPair::play CROSS-BLENDS,
//  and a hand-rolled PlayCycle RESTARTS. Restarting where the engine blends is
//  a defect no amount of tuning removes.
//
//  So the target stopped being "looks smooth" and became "identical to single
//  player" -- and that is a claim only a measurement can settle. This prints
//  one line per frame per traced creature: the motion each bone part is
//  actually playing, by NAME, with the blend weights, next to every input the
//  selector read to get there.
//
//  ⭐The point is that ONE format serves BOTH sides. Run it in single player
//  and it traces the real animator; run it on a network client and it traces
//  whatever is driving the puppet. Two logs of the same creature doing the
//  same thing, diffed line by line, turn "выглядит не так" into "at t+340 ms
//  single player went to norm_torso_8_aim_0 and the client stayed on idle".
//
//  Console (both binaries link xrGame, so it works in the client console and
//  in the server's live command channel):
//     cl_anim_trace  0|1     off / on
//     cl_anim_trace_id <id>  one creature, or -1 for every stalker
//
//  ⚠Both are SetCanSave(false): a measurement knob that survives into
//  user.ltx is a measurement running in somebody's game (see the
//  anticheat-testcvars-leaked-into-userltx lesson).
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_animation_trace.h"
#include "stalker_animation_manager.h"
#include "stalker_animation_pair.h"
#include "stalker_movement_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "object_handler_planner.h"
#include "object_handler_space.h"
#include "sight_manager.h"
#include "Level.h"
#include "Inventory.h"
#include "Weapon.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../Include/xrRender/animation_blend.h"

int g_cl_anim_trace = 0;
int g_cl_anim_trace_id = -1;

namespace
{
// The name a MotionID resolves to, or a marker for "nothing playing". Motion
// names are the only readable identity a blend has -- the ids are indices into
// per-visual tables and mean nothing across two processes.
LPCSTR motion_name(IKinematicsAnimated* ka, const MotionID& m)
{
    if (!ka || !m.valid())
        return "-";
    const std::pair<LPCSTR, LPCSTR> nm = ka->LL_MotionDefName_dbg(m);
    return nm.first ? nm.first : "?";
}

LPCSTR body_name(u32 v)
{
    switch (v)
    {
    case MonsterSpace::eBodyStateCrouch: return "crouch";
    case MonsterSpace::eBodyStateStand: return "stand";
    default: return "?";
    }
}

LPCSTR mental_name(u32 v)
{
    switch (v)
    {
    case MonsterSpace::eMentalStateDanger: return "danger";
    case MonsterSpace::eMentalStateFree: return "free";
    case MonsterSpace::eMentalStatePanic: return "panic";
    default: return "?";
    }
}

LPCSTR movement_name(u32 v)
{
    switch (v)
    {
    case MonsterSpace::eMovementTypeWalk: return "walk";
    case MonsterSpace::eMovementTypeRun: return "run";
    case MonsterSpace::eMovementTypeStand: return "stand";
    default: return "?";
    }
}

// Blend weight and playback speed, the two numbers that say whether a
// transition is being BLENDED or restarted. A hand-rolled PlayCycle shows
// amt≈1.0 the instant it starts; a real cross-blend ramps.
void blend_info(CBlend* B, float& amount, float& speed)
{
    amount = B ? B->blendAmount : 0.f;
    speed = B ? B->speed : 0.f;
}
} // namespace

void StalkerAnimTrace::Frame(CAI_Stalker* S)
{
    if (!g_cl_anim_trace || !S)
        return;
    if (g_cl_anim_trace_id >= 0 && u16(g_cl_anim_trace_id) != u16(S->ID()))
        return;
    if (!S->g_Alive())
        return;

    IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(S->Visual());
    if (!ka)
        return;

    CStalkerAnimationManager& A = S->animation();

    // cl_anim_trace 2: the SEQUENCE of choices, one line per change of clip on
    // any part. Per-frame lines drown a session (a headless host ticks at
    // ~160 Hz); the sequence is what two runs are compared on.
    if (2 == g_cl_anim_trace)
    {
        struct Chosen
        {
            MotionID legs, torso, head, glob, scr;
        };
        static xr_map<u16, Chosen> s_last;
        const Chosen now{A.legs().animation(), A.torso().animation(), A.head().animation(), A.global().animation(), A.script().animation()};
        Chosen& prev = s_last[u16(S->ID())];
        if (prev.legs == now.legs && prev.torso == now.torso && prev.head == now.head && prev.glob == now.glob && prev.scr == now.scr)
            return;
        prev = now;
    }

    float legs_amt, legs_spd, torso_amt, torso_spd, head_amt, head_spd, glob_amt, glob_spd;
    blend_info(A.legs().blend(), legs_amt, legs_spd);
    blend_info(A.torso().blend(), torso_amt, torso_spd);
    blend_info(A.head().blend(), head_amt, head_spd);
    blend_info(A.global().blend(), glob_amt, glob_spd);

    const CStalkerMovementManager& M = S->movement();

    // What the creature is holding, because the torso set is indexed by it and
    // a mismatch here explains a whole class of "wrong pose" on its own.
    u32 slot = 0;
    if (CWeapon* w = smart_cast<CWeapon*>(S->inventory().ActiveItem()))
        slot = w->animation_slot();

    // ⭐The transform as RENDERED, alongside the state, because an animation
    // that is right while the body it is played on is turning wrongly still
    // looks wrong -- and the two are fixed in different places.
    const float rendered_yaw = atan2f(S->XFORM().k.x, S->XFORM().k.z);
    const float twist = angle_normalize_signed(M.head_orientation().current.yaw - M.body_orientation().current.yaw);

    Msg("~ [anim] id=%u t=%u | legs='%s' amt=%.2f spd=%.2f | torso='%s' amt=%.2f spd=%.2f | head='%s' amt=%.2f | glob='%s' amt=%.2f | scr='%s' "
        "|| body=%s mental=%s move=%s act=%u slot=%u speed=%.2f | yaw=%.3f body_yaw=%.3f head_yaw=%.3f twist=%.1fdeg pitch=%.1fdeg | net=%d",
        S->ID(), Device.dwTimeGlobal, motion_name(ka, A.legs().animation()), legs_amt, legs_spd, motion_name(ka, A.torso().animation()), torso_amt, torso_spd,
        motion_name(ka, A.head().animation()), head_amt, motion_name(ka, A.global().animation()), glob_amt, motion_name(ka, A.script().animation()),
        body_name(M.body_state()), mental_name(M.mental_state()),
        movement_name(M.movement_type()), S->CObjectHandler::planner().current_action_state_id(), slot, M.speed(), rendered_yaw, M.body_orientation().current.yaw,
        M.head_orientation().current.yaw, rad2deg(twist), rad2deg(S->NET_Last.o_torso.pitch), (nullptr == Level().Server) ? 1 : 0);
}
