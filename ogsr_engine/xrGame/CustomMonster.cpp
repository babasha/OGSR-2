// CustomMonster.cpp: implementation of the CCustomMonster class.
//
//////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "../xrCore/load_prof.h"   // per-spawn breakdown
#include "ai_debug.h"
#include "CustomMonster.h"
#include "sv_npc.h" // SVNpc::WatchPos -- position-ownership diagnostic
#include "sv_npc_fight.h" // SVFight::WatchTick -- why it is not shooting
#include "HUDManager.h"
#include "ai_space.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "xrServer.h"
#include "seniority_hierarchy_holder.h"
#include "team_hierarchy_holder.h"
#include "squad_hierarchy_holder.h"
#include "group_hierarchy_holder.h"
#include "CustomZone.h"
#include "clsid_game.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "detail_path_manager.h"
#include "memory_manager.h"
#include "visual_memory_manager.h"
#include "sound_memory_manager.h"
#include "enemy_manager.h"
#include "item_manager.h"
#include "danger_manager.h"
#include "ai_object_location.h"
#include "level_graph.h"
#include "game_graph.h"
#include "movement_manager.h"
#include "EntityCondition.h"
#include "sound_player.h"
#include "Level.h"
#include "sv_motion_audit.h"
#include "material_manager.h"
#include "sound_user_data_visitor.h"
#include "mt_config.h"
#include "PHMovementControl.h"

#include "date_time.h"
#include "InventoryOwner.h" // the intent block reports what is in the hands
#include "Inventory.h"
#include "HudItem.h" // ...by its animation slot, which lives on CHudItem
#include "ai/stalker/ai_stalker.h" // the backward-speed trace names the AI's own state
#include "stalker_movement_manager.h"
#include "CharacterPhysicsSupport.h"
#include "ai/monsters/snork/snork.h"
#include "ai/monsters/burer/burer.h"
#include "GamePersistent.h"
#include "Actor.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "client_spawn_manager.h"

extern int g_AI_inactive_time;

void CCustomMonster::SAnimState::Create(IKinematicsAnimated* K, LPCSTR base)
{
    char buf[128];
    fwd = K->ID_Cycle_Safe(strconcat(sizeof(buf), buf, base, "_fwd"));
    back = K->ID_Cycle_Safe(strconcat(sizeof(buf), buf, base, "_back"));
    ls = K->ID_Cycle_Safe(strconcat(sizeof(buf), buf, base, "_ls"));
    rs = K->ID_Cycle_Safe(strconcat(sizeof(buf), buf, base, "_rs"));
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CCustomMonster::CCustomMonster()
{
    m_sound_user_data_visitor = 0;
    m_memory_manager = 0;
    m_movement_manager = 0;
    m_sound_player = 0;
    m_already_dead = false;
    m_invulnerable = false;
    m_visible_for_zones = true;
    m_anomaly_detector = xr_new<CAnomalyDetector>(this);
}

CCustomMonster::~CCustomMonster()
{
    xr_delete(m_anomaly_detector);
    xr_delete(m_sound_user_data_visitor);
    xr_delete(m_memory_manager);
    xr_delete(m_movement_manager);
    xr_delete(m_sound_player);

    // Lain: added (asking GameLevel to forget about self)
    if (g_pGameLevel)
        g_pGameLevel->SoundEvent_OnDestDestroy(this);

	if ( g_pGameLevel )
	  Level().client_spawn_manager().clear(ID());
}

void CCustomMonster::Load(LPCSTR section)
{
    inherited::Load(section);
    material().Load(section);
    memory().Load(section);
    movement().Load(section);
    //////////////////////////////////////////////////////////////////////////

    ///////////
    // m_PhysicMovementControl: General

    character_physics_support()->movement()->Load(section);
    // Fbox	bb;

    // m_PhysicMovementControl: Frictions
    ////////

    Position().y += EPS_L;

    //	m_current			= 0;

    eye_fov = pSettings->r_float(section, "eye_fov");
    eye_range = pSettings->r_float(section, "eye_range");

    // Health & Armor
    //	fArmor					= 0;

    // Msg				("! cmonster size: %d",sizeof(*this));

    m_anomaly_detector->load(section);
}

void CCustomMonster::reinit()
{
    CScriptEntity::reinit();
    CEntityAlive::reinit();
    material().reinit();
    movement().reinit();
    sound().reinit();

    m_client_update_delta = 0;
    m_last_client_update_time = Device.dwTimeGlobal;

    eye_pp_stage = 0;
    m_dwLastUpdateTime = 0xffffffff;
    m_tEyeShift.set(0, 0, 0);
    m_fEyeShiftYaw = 0.f;
    NET_WasExtrapolating = FALSE;

    //////////////////////////////////////////////////////////////////////////
    // Critical Wounds
    //////////////////////////////////////////////////////////////////////////

    m_critical_wound_type = u32(-1);
    m_last_hit_time = 0;
    m_critical_wound_accumulator = 0.f;
    m_critical_wound_threshold = pSettings->r_float(cNameSect(), "critical_wound_threshold");
    m_critical_wound_decrease_quant = pSettings->r_float(cNameSect(), "critical_wound_decrease_quant");

    if (m_critical_wound_threshold >= 0)
        load_critical_wound_bones();
    //////////////////////////////////////////////////////////////////////////
    m_update_rotation_on_frame = true;
    m_movement_enabled_before_animation_controller = true;

    m_anomaly_detector->reinit();
}

void CCustomMonster::reload(LPCSTR section)
{
    sound().reload(section);
    CEntityAlive::reload(section);
    material().reload(section);
    movement().reload(section);
    load_killer_clsids(section);

    m_far_plane_factor = READ_IF_EXISTS(pSettings, r_float, section, "far_plane_factor", 1.f);
    m_fog_density_factor = READ_IF_EXISTS(pSettings, r_float, section, "fog_density_factor", .05f);

    m_panic_threshold = pSettings->r_float(section, "panic_threshold");
}

void CCustomMonster::mk_orientation(Fvector& dir, Fmatrix& mR)
{
    // orient only in XZ plane
    dir.y = 0;
    float len = dir.magnitude();
    if (len > EPS_S)
    {
        // normalize
        dir.x /= len;
        dir.z /= len;
        Fvector up;
        up.set(0, 1, 0);
        mR.rotation(dir, up);
    }
}

// ⭐⭐⭐What the SERVER puts on the wire for a creature it owns.
//
// ⚠Lives in one function called from BOTH net_Export implementations, because
// CAI_Stalker::net_Export is a verbatim copy of CCustomMonster::net_Export
// that does not call inherited -- so a rule written in one of them is dead
// code for stalkers, which is every NPC this project cares about. Measured
// 29-07-2026: a fix applied to the base class alone changed nothing at all.
//
// ⚠And the rule itself, which cost two sessions to find. The wire carried
// NET.back(): the last entry the creature's BRAIN pushed onto its queue. That
// queue gains entries on the AI's schedule -- roughly 10 Hz, and the interval
// STRETCHES with how many creatures are being scheduled. So:
//
//     ordered to walk    client saw dt = 50 ms   (and even that was a guess:
//                                                 the stamp had not moved, so
//                                                 the receiver assumed 50)
//     fighting, undriven client saw dt = 112-147 ms
//
// while the server's build rate was IDENTICAL in both cases -- 51.9 ms and
// 52.6 ms, 19 Hz. The server was building nineteen times a second and shipping
// the same stale entry two or three times in a row, and the receiver was
// reconstructing its timeline out of a mixture of honest stamps and its own
// invented ones. No amount of interpolation fixes a source that only says
// something new eight times a second, at irregular intervals.
//
// On a server-authoritative build the answer is not subtle: this process IS
// the authority for this creature, so what goes on the wire is where the
// creature IS, stamped when it was read.
//
// ⭐The value is already smooth: the server runs the creature's own UpdateCL
// every frame, which interpolates its NET queue into Position(). That
// per-frame result was being computed and then thrown away.
//
// ⚠o_torso deliberately stays on the AI's cadence -- it is the brain's
// decision about where to LOOK, and there is no higher-rate truth to sample.
void CCustomMonster::net_Export_authority(CSE_ALifeCreatureAbstract* creature)
{
    if (!creature)
        return;

    // ⚠Cleared FIRST, not written last. The CSE mirror is the server's own
    // long-lived entity: whatever was put in it stays there until somebody
    // overwrites it. Neither branch below is taken for a creature this process
    // is not the authority for -- and for a corpse -- so without this the wire
    // would keep repeating the last intent of a creature that has stopped
    // having one, which on the client reads as a dead man still walking.
    creature->o_intent.clear();
    creature->o_anim.clear();

    if (server_driven())
    {
        creature->o_Position = Position();
        creature->o_model = m_server_driven_yaw;
        creature->timestamp = Level().timeServer();
        net_Export_intent(creature);
        // ⚠NO clips while the server drives the body. The animator here reads
        // the BRAIN (movement type, body yaw), and a driven body goes where the
        // drive takes it, not where the brain thinks it is: measured 22-09-2026,
        // body and brain headings apart by more than 30 degrees on half of a
        // guard's clip changes. Its clips would be faithful to the wrong thing.
        // Humans are no longer driven this way (SVNpc::Steer); what still is --
        // a mutant under an order -- keeps the client's own selectors.
        return;
    }

    if (g_Alive() && Level().Server)
    {
        creature->o_Position = Position();
        creature->o_model = angle_normalize(atan2f(XFORM().k.x, XFORM().k.z));
        creature->timestamp = Level().timeServer();
        net_Export_intent(creature);
        net_Export_anim(creature);
    }
}

// What the animator on this process is playing, part by part -- see
// SNetAnimState. The "current" clip of a part is the newest channel-0 cycle on
// it: when an animator switches, the new clip is pushed while the old one fades
// out underneath, so the newest is the one being crossed INTO. A global clip
// (played on every part at once) simply shows up on each of them.
void CCustomMonster::net_Export_anim(CSE_ALifeCreatureAbstract* creature)
{
    SNetAnimState& out = creature->o_anim;
    out.clear();

    static_assert(SNetAnimState::kParts == MAX_PARTS, "the wire carries one entry per bone part");

    IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(Visual());
    if (!ka || 0 == ka->LL_MotionsSlotCount())
        return; // no animator on this process -- the client falls back to its own

    for (u16 p = 0; p < SNetAnimState::kParts; ++p)
    {
        CBlend* cur = nullptr;
        for (u32 n = ka->LL_PartBlendsCount(p); n > 0; --n)
        {
            CBlend* B = ka->LL_PartBlend(p, n - 1);
            if (B && 0 == B->channel && CBlend::eFREE_SLOT != B->blend_state())
            {
                cur = B;
                break;
            }
        }
        if (!cur || !cur->motionID.valid() || cur->motionID.slot > 0xff)
            continue;

        SNetAnimState::Part& o = out.part[p];
        o.slot = u8(cur->motionID.slot);
        o.idx = cur->motionID.idx;
        float t = cur->timeCurrent;
        clamp(t, 0.f, 65.f);
        o.time_ms = u16(iFloor(t * 1000.f + 0.5f));
        float s = cur->speed * 64.f;
        clamp(s, 0.f, 255.f);
        o.speed_q = u8(iFloor(s + 0.5f));
        out.mask |= u8(1u << p);
    }
}

// The presentation intent, sampled against the transform that was just written
// into `creature` -- see SNetCreatureIntent for why this channel exists at all.
//
// ⭐⭐THE VELOCITY IS DIFFERENTIATED FROM THE EXPORTED POSITION, ON PURPOSE.
// Not from the drive's intent, not from the movement manager, not from the
// physics capsule -- from the very number the client is about to receive. The
// client renders the segment between two consecutive exports; the velocity
// reported with the second one is that segment's own average. So the legs
// cannot disagree with the ground the model covers: they are the same
// measurement, made once, by the machine that owns it.
//
// ⚠Sampled once per FRAME, not once per call. With two clients connected this
// runs twice per build, and a second call in the same frame sees dt = 0 and a
// zero displacement -- which reads as "stopped" and would flap the animation at
// exactly the rate clients are added to the server.
void CCustomMonster::net_Export_intent(CSE_ALifeCreatureAbstract* creature)
{
    // The numbering is shared with the posture byte (sv_npc.h) so that
    // PostureOf can be copied straight into the wire's nibbles. Nail it down
    // here, where both headers are in scope, rather than discover it drifting
    // as a puppet quietly playing the wrong cycle.
    static_assert(npc_mental_free == 0 && npc_mental_danger == 1 && npc_mental_panic == 2, "intent mental bits must match the posture byte");
    static_assert(npc_action_idle == 0 && npc_action_aim == 1, "intent action nibble must match the posture byte");
    static_assert(npc_posture_action_mask <= 0x7, "the action must fit the intent's high nibble");

    SNetCreatureIntent& out = creature->o_intent;
    out.clear();

    const u32 now = Device.dwTimeGlobal;
    const Fvector pos = creature->o_Position;
    const float yaw = creature->o_model;

    if (m_intent_frame != Device.dwFrame)
    {
        const u32 dt_ms = now - m_intent_ms;
        // Below 10 ms the quotient is mostly quantization noise; above 250 ms
        // the creature was not being exported at all (just spawned, just came
        // online) and the "displacement" is a teleport.
        if (m_intent_ms && dt_ms >= 1 && dt_ms <= 250)
        {
            // ⭐⭐⭐ACCUMULATE, THEN DIVIDE ONCE. Do not average quotients.
            //
            // The first version of this took d/dt per sample and ran the result
            // through an EMA, and it reported a creature walking at a measured
            // 1.10 m/s as 0.72, and one being driven at 1.60 as 1.00. The
            // per-sample quotient was never wrong -- the diagnostic below
            // printed 1.10 m/s exactly, matching the content's own walk speed.
            // What was wrong was averaging those quotients.
            //
            // ⚠The reason is a rate mismatch, and it is the same shape as the
            // defect that made this whole channel necessary: this function is
            // called on the EXPORT's schedule, and the body's position advances
            // on the MOVEMENT's. So consecutive samples see 0, 0, a whole step,
            // 0 -- and an exponential average of instantaneous quotients over
            // such a train is not the mean speed, it is a number pulled toward
            // whichever kind of sample happened to be last.
            //
            // Net displacement over elapsed time has no such sensitivity: it is
            // the same answer however the motion is chunked inside the window,
            // and it needs no smoothing constant to defend it.
            Fvector d;
            d.sub(pos, m_intent_pos);
            d.y = 0.f;
            m_intent_acc.add(d);
            m_intent_acc_yaw += angle_normalize_signed(yaw - m_intent_yaw);
            m_intent_acc_ms += dt_ms;

            // One window per outgoing snapshot, near enough: the client renders
            // the segment between two snapshots, so a velocity averaged over
            // roughly that same span is the one that matches its picture.
            if (m_intent_acc_ms >= 60)
            {
                const float win = float(m_intent_acc_ms) / 1000.f;
                // world -> body frame. rotateY(yaw) puts forward at
                // (sin, 0, cos), so right is (cos, 0, -sin).
                const float s = _sin(yaw), c = _cos(yaw);
                m_intent_vfwd = (m_intent_acc.x * s + m_intent_acc.z * c) / win;
                m_intent_vstr = (m_intent_acc.x * c - m_intent_acc.z * s) / win;
                m_intent_turn = m_intent_acc_yaw / win;

                // ⭐The terms of the quotient, not just its result: a velocity
                // that disagrees with the picture is either a wrong distance or
                // a wrong interval, and those are fixed in different places.
                extern int g_dbg_intent;
                if (g_dbg_intent)
                {
                    static u32 s_diag = 0;
                    if (++s_diag % 31 == 1)
                        Msg("~ [intent] id=%u win=%ums d=%.4fm -> %.2f m/s (fwd %.2f str %.2f turn %.2f) | frame=%u driven=%d", ID(), m_intent_acc_ms,
                            m_intent_acc.magnitude(), m_intent_acc.magnitude() / win, m_intent_vfwd, m_intent_vstr, m_intent_turn, Device.dwFrame,
                            server_driven() ? 1 : 0);
                }

                // ⭐⭐WHERE A BACKWARD SPRINT COMES FROM -- printed at the one
                // sample that measured it.
                //
                // Measured 01-08 in play: a stalker covering ground at 6.17 m/s
                // with a body-frame forward component of -6.12, against a
                // backward run clip the content pairs with 2.40. No backpedal
                // can show that, and it is what "неестественно быстро назад"
                // is. ⚠The obvious culprit was checked and CLEARED: the four
                // eVelocity*Negative entries do read the forward row
                // (stalker_movement_manager.cpp:168-172), but the velocity mask
                // is built as eVelocityPositiveVelocity and the negative bit is
                // only ever CLEARED, so those entries can never be selected.
                //
                // So the number has to name its own source instead: what the
                // movement manager WANTS (m_fCurSpeed), what state it is in,
                // and whether the server drive was carrying this body at the
                // time. A desirable speed that matches means the AI asked for
                // it; one that does not means something else moved the body.
                if (m_intent_vfwd < -2.f)
                {
                    static u32 s_diag_back = 0;
                    if (s_diag_back < 40)
                    {
                        ++s_diag_back;
                        CAI_Stalker* S = smart_cast<CAI_Stalker*>(this);
                        Msg("~ [backspeed] id=%u fwd=%.2f str=%.2f |v|=%.2f | want=%.2f mental=%d movement=%d body=%d driven=%d", ID(), m_intent_vfwd, m_intent_vstr,
                            m_intent_acc.magnitude() / win, m_fCurSpeed, S ? int(S->movement().mental_state()) : -1, S ? int(S->movement().movement_type()) : -1,
                            S ? int(S->movement().body_state()) : -1, server_driven() ? 1 : 0);
                    }
                }

                m_intent_acc.set(0.f, 0.f, 0.f);
                m_intent_acc_yaw = 0.f;
                m_intent_acc_ms = 0;
            }
        }
        m_intent_frame = Device.dwFrame;
        m_intent_ms = now;
        m_intent_pos = pos;
        m_intent_yaw = yaw;

        // Style: what the picture cannot measure. Straight out of the same
        // function the 5 Hz posture event uses, so the two can never disagree
        // -- and once this rides the update stream the event's copy of it is
        // redundant.
        u16 action_state = u16(-1);
        const u16 posture = SVNpc::PostureOf(this, action_state);
        const u8 action = u8((posture >> npc_posture_action_shift) & npc_posture_action_mask);

        u8 style = 0;
        if (posture & npc_posture_crouch)
            style |= SNetCreatureIntent::fCrouch;
        if (posture & npc_posture_wounded)
            style |= SNetCreatureIntent::fWounded;
        if (npc_action_aim == action)
            style |= SNetCreatureIntent::fAim;
        style |= u8(((posture >> npc_posture_mental_shift) & npc_posture_mental_mask) << SNetCreatureIntent::fMentalShift);
        m_intent_style = style;

        // ⚠The ANIMATION slot, not the inventory slot: it is what indexes the
        // torso cycle set. An item that never declared one reads back as
        // u32(-1), which masked into a nibble becomes a confident, wrong 15.
        u8 slot = 0;
        if (CInventoryOwner* io = smart_cast<CInventoryOwner*>(this))
            if (CHudItem* held = smart_cast<CHudItem*>(io->inventory().ActiveItem()))
            {
                const u32 s = held->animation_slot();
                if (s < 15)
                    slot = u8(s);
            }
        m_intent_slot = u8(slot | (action << 4));
    }

    out.flags = u8(SNetCreatureIntent::fValid | m_intent_style);
    out.slot = m_intent_slot;
    out.v_fwd = SNetCreatureIntent::quantize(m_intent_vfwd, 8.f);
    out.v_str = SNetCreatureIntent::quantize(m_intent_vstr, 8.f);
    out.turn = SNetCreatureIntent::quantize(m_intent_turn, 16.f);

    // ⚠Held rather than sampled, and therefore NOT cached with the rest: a
    // rifle fires faster than the wire ticks, so an instantaneous flag is a
    // coin toss. The client latches the aim pose off this and needs it to
    // survive the gaps inside a burst.
    if (m_intent_fire_ms && (now - m_intent_fire_ms) < 300)
        out.flags |= SNetCreatureIntent::fFire;
    if (m_intent_flinch_ms && (now - m_intent_flinch_ms) < 300)
        out.flags |= SNetCreatureIntent::fFlinch;
    // Same reason it is latched: the drive stamps it while it drives, and a
    // released creature must stop claiming the tariff on its own.
    if (m_intent_sprint_ms && (now - m_intent_sprint_ms) < 300)
        out.slot |= SNetCreatureIntent::sSprint;
}

void CCustomMonster::net_Export(CSE_Abstract* E)
{
    R_ASSERT(Local());

    // export last known packet
    R_ASSERT(!NET.empty());
    net_update& N = NET.back();

    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(E);
    creature->fHealth = GetfHealth();
    creature->timestamp = N.dwTimeStamp;
    creature->flags = 0;
    creature->o_Position = N.p_pos;
    creature->o_model = N.o_model;
    creature->o_torso.yaw = N.o_torso.yaw;
    creature->o_torso.pitch = N.o_torso.pitch;
    creature->o_torso.roll = N.o_torso.roll;

    // While the server drives this creature, the wire must carry the assertion.
    // NET.back() is contested: net_ServerDriveTo rewrites it every server tick,
    // but the stalker's shedule_Update pushes fresh entries with the AI's OWN
    // m_body yaw (~10 Hz near a player) -- position survives that (the move
    // gate keeps Position() asserted, and the pushed entry copies Position()),
    // the heading does not. An export that samples between the push and the
    // next tick ships the AI's heading, and on the remote client the puppet
    // whipped 180 degrees for one snapshot and back -- reported from play as
    // "разворачиваются на 180 градусов за один кадр".
    net_Export_authority(creature);
    creature->s_team = u8(g_Team());
    creature->s_squad = u8(g_Squad());
    creature->s_group = u8(g_Group());

    // ⭐The wire's own record of itself. Sampled HERE and not at the drive
    // because this is the only place that knows what the client will actually
    // receive -- the drive's intent and the exported pose are two different
    // facts, and the whole point of the audit is that they might disagree.
    SVMotionAudit::Note(u16(ID()), "export", creature->o_Position, creature->o_model, server_driven(), creature->timestamp);
}

// Thin-client driven puppet: the whole local update stack is a GHOST -- the
// NET-lerp runs against stale entries, UpdatePositionAnimation consults a
// movement manager nobody drives, foot IK and animation collision read a
// physics capsule nobody simulates -- and each of them re-touches the model
// after the interpolator has placed it. Each surfaced in play as its own
// flavour of stutter (28-07: three rounds of "чуть-чуть дёргается"). A driven
// puppet needs none of it: the interpolator moves it, PlayCycle animates it,
// the renderer advances the blends on its own. The gate frees itself: on
// death server_driven() decays within a second and the full update stack --
// ragdoll included -- takes the body back.
bool CCustomMonster::net_render_only_puppet() const { return (nullptr == Level().Server) && g_Alive() && server_driven(); }

void CCustomMonster::shedule_Update(u32 DT)
{
    VERIFY(!g_Alive() || processing_enabled());
    // Queue shrink
    VERIFY(_valid(Position()));
    u32 dwTimeCL = Level().timeServer() - NET_Latency;
    VERIFY(!NET.empty());
    while ((NET.size() > 2) && (NET[1].dwTimeStamp < dwTimeCL))
        NET.pop_front();

    float dt = float(DT) / 1000.f;
    // *** general stuff
    if (g_Alive())
    {
        if (g_mt_config.test(mtAiVision))
            Device.add_to_seq_parallel(fastdelegate::MakeDelegate(this, &CCustomMonster::Exec_Visibility), "monster visibility");
        else
            Exec_Visibility();
        memory().update(dt);
        anomaly_detector().update_schedule();

        // Sampled here, in the creature's own scheduled update, so a reading
        // of its AI state cannot race the code that writes it -- and so the
        // snapshot is taken at the moment the brain has just run, not at
        // whatever point in the frame a console command happens to land.
        SVFight::WatchTick(this);
    }
    inherited::shedule_Update(DT);

    // Queue setup
    if (dt > 3)
        return;

    m_dwCurrentTime = Device.dwTimeGlobal;

    VERIFY(_valid(Position()));
    if (Remote())
    {}
    else
    {
        // here is monster AI call
        m_fTimeUpdateDelta = dt;
        Device.Statistic->AI_Think.Begin();
        if (GetScriptControl())
            ProcessScripts();
        else
        {
            if (Device.dwFrame > spawn_time() + g_AI_inactive_time)
                Think();
        }
        m_dwLastUpdateTime = Device.dwTimeGlobal;
        Device.Statistic->AI_Think.End();

        // Look and action streams
        float temp = conditions().health();
        if (temp > 0)
        {
            Exec_Action(dt);
            VERIFY(_valid(Position()));
            // Exec_Visibility		();
            VERIFY(_valid(Position()));

            net_update uNext;
            uNext.dwTimeStamp = Level().timeServer();
            uNext.o_model = movement().m_body.current.yaw;
            uNext.o_torso = movement().m_body.current;
            uNext.p_pos = Position();
            uNext.fHealth = GetfHealth();
            NET.push_back(uNext);
        }
        else
        {
            net_update uNext;
            uNext.dwTimeStamp = Level().timeServer();
            uNext.o_model = movement().m_body.current.yaw;
            uNext.o_torso = movement().m_body.current;
            uNext.p_pos = Position();
            uNext.fHealth = GetfHealth();
            NET.push_back(uNext);
        }
    }
}

void CCustomMonster::net_update::lerp(CCustomMonster::net_update& A, CCustomMonster::net_update& B, float f)
{
    //
    o_model = angle_lerp(A.o_model, B.o_model, f);
    o_torso.yaw = angle_lerp(A.o_torso.yaw, B.o_torso.yaw, f);
    o_torso.pitch = angle_lerp(A.o_torso.pitch, B.o_torso.pitch, f);
    p_pos.lerp(A.p_pos, B.p_pos, f);
    fHealth = A.fHealth * (1.f - f) + B.fHealth * f;
}

void CCustomMonster::update_sound_player() { sound().update(client_update_fdelta()); }

void CCustomMonster::UpdateCL()
{
    START_PROFILE("CustomMonster/client_update")
    m_client_update_delta = Device.dwTimeGlobal - m_last_client_update_time;
    m_last_client_update_time = Device.dwTimeGlobal;

    // render-only while the server drives this puppet on a thin client --
    // see net_render_only_puppet() for the reasoning and the release rule
    if (net_render_only_puppet())
        return;

    SVNpc::WatchPos(u16(ID()), "UpdateCL:in", Position());

    // ⭐⭐THE FOURTH POINT, and the only one that can see a creature moving
    // under its OWN brain.
    //
    // The other three are all downstream of somebody else's decision: `assert`
    // samples the server's drive, `export` and `wire` fire only when a client
    // is actually being told about this entity. A dog crossing a field on a
    // headless host passes through none of them -- measured 31-07, ZERO
    // samples on a live, running dog -- so the audit was blind to precisely
    // the case it was wanted for, and "no samples" reads in the log exactly
    // like "no motion defect".
    //
    // ⚠Taken before inherited::UpdateCL, so this is the pose the frame STARTS
    // from: whatever moves this body during the frame then shows up as the
    // step to the next sample rather than being folded into this one.
    SVMotionAudit::Note(u16(ID()), "body", Position(), angle_normalize(atan2f(XFORM().k.x, XFORM().k.z)), server_driven());

    START_PROFILE("CustomMonster/client_update/inherited")
    inherited::UpdateCL();
    STOP_PROFILE

    SVNpc::WatchPos(u16(ID()), "afterInherited", Position());

    CScriptEntity::process_sound_callbacks();

    if (g_mt_config.test(mtSoundPlayer))
        Device.add_to_seq_parallel(fastdelegate::MakeDelegate(this, &CCustomMonster::update_sound_player), "monster sound player");
    else
    {
        START_PROFILE("CustomMonster/client_update/sound_player")
        update_sound_player();
        STOP_PROFILE
    }

    START_PROFILE("CustomMonster/client_update/network extrapolation")
    if (NET.empty())
    {
        update_animation_movement_controller();
        return;
    }

    m_dwCurrentTime = Device.dwTimeGlobal;

    // distinguish interpolation/extrapolation
    u32 dwTime = Level().timeServer() - NET_Latency;
    net_update& N = NET.back();
    if ((dwTime > N.dwTimeStamp) || (NET.size() < 2))
    {
        // BAD.	extrapolation
        NET_Last = N;
    }
    else
    {
        // OK.	interpolation
        NET_WasExtrapolating = FALSE;
        // Search 2 keyframes for interpolation
        int select = -1;
        for (u32 id = 0; id < NET.size() - 1; ++id)
        {
            if ((NET[id].dwTimeStamp <= dwTime) && (dwTime <= NET[id + 1].dwTimeStamp))
                select = id;
        }
        if (select >= 0)
        {
            // Interpolate state
            net_update& A = NET[select + 0];
            net_update& B = NET[select + 1];
            u32 d1 = dwTime - A.dwTimeStamp;
            u32 d2 = B.dwTimeStamp - A.dwTimeStamp;
            //			VERIFY					(d2);
            float factor = d2 ? (float(d1) / float(d2)) : 1.f;
            Fvector l_tOldPosition = Position();
            NET_Last.lerp(A, B, factor);
            if (Local())
            {
                NET_Last.p_pos = l_tOldPosition;
            }
            else
            {
                if (!bfScriptAnimation())
                    SelectAnimation(XFORM().k, movement().detail().direction(), movement().speed());
            }
        }
    }
    STOP_PROFILE

    SVNpc::WatchPos(u16(ID()), "afterNetLerp", Position());

    if (Local() && g_Alive())
    {
#pragma todo("Dima to All : this is FAKE, network is not supported here!")

        // On a thin network client Local() is true for everything, so a
        // server-driven puppet would run the full local movement/animation
        // selection here -- a second animation master whose m_body yaw is
        // frozen at its spawn value (Exec_Look is gated out on the client),
        // fighting the cycles net_puppet_locomotion plays and whipping the
        // model toward a stale heading. The interpolator is the only mover
        // of these puppets; let it be the only animator too.
        const bool thin_client_puppet = (nullptr == Level().Server) && server_driven();
        if (!thin_client_puppet)
            UpdatePositionAnimation();
    }

    SVNpc::WatchPos(u16(ID()), "afterPosAnim", NET_Last.p_pos);

    // Use interpolated/last state
    if (g_Alive())
    {
        if (!animation_movement_controlled() && m_update_rotation_on_frame)
            XFORM().rotateY(NET_Last.o_model);

        XFORM().translate_over(NET_Last.p_pos);

        if (!animation_movement_controlled() && m_update_rotation_on_frame)
        {
            Fmatrix M;
            M.setHPB(0.0f, -NET_Last.o_torso.pitch, 0.0f);
            XFORM().mulB_43(M);
        }
    }

    update_animation_movement_controller();

    SVNpc::WatchPos(u16(ID()), "UpdateCL:out", Position());

    STOP_PROFILE
}

void CCustomMonster::UpdatePositionAnimation()
{
    START_PROFILE("CustomMonster/client_update/movement")
    movement().on_frame(character_physics_support()->movement(), NET_Last.p_pos);
    STOP_PROFILE

    START_PROFILE("CustomMonster/client_update/animation")
    if (!bfScriptAnimation())
        SelectAnimation(XFORM().k, movement().detail().direction(), movement().speed());
    STOP_PROFILE
}

BOOL CCustomMonster::feel_visible_isRelevant(CObject* O)
{
    CEntityAlive* E = smart_cast<CEntityAlive*>(O);
    if (0 == E)
        return FALSE;
    if (E->g_Team() == g_Team())
        return FALSE;
    return TRUE;
}

void CCustomMonster::eye_pp_s0()
{
    // Eye matrix
    IKinematics* V = smart_cast<IKinematics*>(Visual());
    V->CalculateBones();
    Fmatrix& mEye = V->LL_GetTransform(u16(eye_bone));
    Fmatrix X;
    X.mul_43(XFORM(), mEye);
    VERIFY(_valid(mEye));

    const MonsterSpace::SBoneRotation& rotation = head_orientation();

    eye_matrix.setHPB(-rotation.current.yaw + m_fEyeShiftYaw, -rotation.current.pitch, 0);
    eye_matrix.c.add(X.c, m_tEyeShift);

    // ⭐The eye where the body is. 📏06-10 15:37: a stalker crouched against
    // the thin log wall of the player's room put his head -- the aim pose leans
    // it forward -- through to the player's side: he saw him from there, stood
    // up to shoot, lost him behind the wall standing, crouched, saw him again;
    // his bursts all went into the wall, and his hands showed through it. From
    // the body's axis at the eye's height out to the eye: a wall on the way, and
    // the eye stays on this side of it.
    Fvector axis = XFORM().c;
    axis.y = eye_matrix.c.y;
    Fvector out;
    out.sub(eye_matrix.c, axis);
    const float reach = out.magnitude();
    if (reach > EPS_L)
    {
        constexpr float margin = .15f;
        out.div(reach);
        collide::rq_result hit;
        if (Level().ObjectSpace.RayPick(axis, out, reach + margin, collide::rqtStatic, hit, nullptr))
            eye_matrix.c.mad(axis, out, std::max(hit.range - margin, 0.f));
    }
}

void CCustomMonster::update_range_fov(float& new_range, float& new_fov, float start_range, float start_fov)
{
    const float standard_far_plane = eye_range;

    float current_fog_density = GamePersistent().Environment().CurrentEnv->fog_density;
    // 0=no_fog, 1=full_fog, >1 = super-fog
    float current_far_plane = GamePersistent().Environment().CurrentEnv->fog_far;
    // 300=standart, 50=super-fog

    new_fov = start_fov;
    new_range = start_range * (_min(m_far_plane_factor * current_far_plane, standard_far_plane) / standard_far_plane) * (1.f / (1.f + m_fog_density_factor * current_fog_density));
    clamp(new_range, 0.f, current_far_plane);
}

void CCustomMonster::eye_pp_s1()
{
    float new_range = eye_range, new_fov = eye_fov;
    if (g_Alive())
    {
        update_range_fov(new_range, new_fov, memory().visual().current_state().m_max_view_distance * eye_range, eye_fov);
    }
    // Standart visibility
    Device.Statistic->AI_Vis_Query.Begin();
    Fmatrix mProject, mFull, mView;
    mView.build_camera_dir(eye_matrix.c, eye_matrix.k, eye_matrix.j);
    VERIFY(_valid(eye_matrix));
    mProject.build_projection(deg2rad(new_fov), 1, 0.1f, new_range);
    mFull.mul(mProject, mView);
    feel_vision_query(mFull, eye_matrix.c);
    Device.Statistic->AI_Vis_Query.End();
}

void CCustomMonster::eye_pp_s2()
{
    // Tracing
    Device.Statistic->AI_Vis_RayTests.Begin();
    u32 dwTime = Level().timeServer();
    u32 dwDT = dwTime - eye_pp_timestamp;
    eye_pp_timestamp = dwTime;
    Device.add_to_awaiters(TTAPI->submit([=]() { feel_vision_update(this, eye_matrix.c, float(dwDT) / 1000.f, memory().visual().transparency_threshold()); }));
    Device.Statistic->AI_Vis_RayTests.End();
}

void CCustomMonster::Exec_Visibility()
{
    // if (0==Sector())				return;
    if (!g_Alive())
        return;

    Device.Statistic->AI_Vis.Begin();
    switch (eye_pp_stage % 2)
    {
    case 0:
        eye_pp_s0();
        eye_pp_s1();
        break;
    case 1: eye_pp_s2(); break;
    }
    ++eye_pp_stage;
    Device.Statistic->AI_Vis.End();
}

void CCustomMonster::HitSignal(float /**perc/**/, Fvector& /**vLocalDir/**/, CObject* /**who/**/) {}

void CCustomMonster::Die(CObject* who)
{
    inherited::Die(who);
    // Level().RemoveMapLocationByID(this->ID());
    Actor()->SetActorVisibility(ID(), 0.f);
}

BOOL CCustomMonster::net_Spawn(CSE_Abstract* DC)
{
    LoadProf::Scope _lpNetSpawn("custommonster.net_Spawn");
    memory().reload(*cNameSect());
    memory().reinit();

    if (!movement().net_Spawn(DC) || !inherited::net_Spawn(DC) || !CScriptEntity::net_Spawn(DC))
        return (FALSE);

    ISpatial* self = smart_cast<ISpatial*>(this);
    if (self)
    {
        self->spatial.type |= STYPE_VISIBLEFORAI;
        // enable react to sound only if alive
        if (g_Alive())
            self->spatial.type |= STYPE_REACTTOSOUND;
    }

    CSE_Abstract* e = (CSE_Abstract*)(DC);
    CSE_ALifeMonsterAbstract* E = smart_cast<CSE_ALifeMonsterAbstract*>(e);

    eye_matrix.identity();
    movement().m_body.current.yaw = movement().m_body.target.yaw = -E->o_torso.yaw;
    movement().m_body.current.pitch = movement().m_body.target.pitch = 0;
    SetfHealth(E->fHealth);
    if (!g_Alive())
    {
        set_death_time();
        //		Msg						("%6d : Object [%d][%s][%s] is spawned DEAD",Device.dwTimeGlobal,ID(),*cName(),*cNameSect());
    }

    if (ai().get_level_graph() && UsedAI_Locations() && (e->ID_Parent == 0xffff))
    {
        if (ai().game_graph().valid_vertex_id(E->m_tGraphID))
            ai_location().game_vertex(E->m_tGraphID);

        if (ai().game_graph().valid_vertex_id(E->m_tNextGraphID) && (ai().game_graph().vertex(E->m_tNextGraphID)->level_id() == ai().level_graph().level_id()) &&
            movement().restrictions().accessible(ai().game_graph().vertex(E->m_tNextGraphID)->level_vertex_id()))
            movement().set_game_dest_vertex(E->m_tNextGraphID);

        if (movement().restrictions().accessible(ai_location().level_vertex_id()))
            movement().set_level_dest_vertex(ai_location().level_vertex_id());
        else
        {
            Fvector dest_position;
            u32 level_vertex_id;
            level_vertex_id = movement().restrictions().accessible_nearest(ai().level_graph().vertex_position(ai_location().level_vertex_id()), dest_position);
            movement().set_level_dest_vertex(level_vertex_id);
            movement().detail().set_dest_position(dest_position);
        }
    }

    // Eyes
    eye_bone = smart_cast<IKinematics*>(Visual())->LL_BoneID(pSettings->r_string(cNameSect(), "bone_head"));

    // weapons
    if (Local())
    {
        net_update N;
        N.dwTimeStamp = Level().timeServer() - NET_Latency;
        N.o_model = -E->o_torso.yaw;
        N.o_torso.yaw = -E->o_torso.yaw;
        N.o_torso.pitch = 0;
        N.p_pos.set(Position());
        NET.push_back(N);

        N.dwTimeStamp += NET_Latency;
        NET.push_back(N);

        setVisible(TRUE);
        setEnabled(TRUE);
    }

    // Sheduler
    shedule.t_min = 100;
    shedule.t_max = 250; // This equaltiy is broken by Dima :-( // 30 * NET_Latency / 4;

    return TRUE;
}

void CCustomMonster::Exec_Action(float /**dt/**/) {}

// void CCustomMonster::Hit(float P, Fvector &dir,CObject* who, s16 element,Fvector position_in_object_space, float impulse, ALife::EHitType hit_type)
void CCustomMonster::Hit(SHit* pHDS)
{
    if (!invulnerable())
        inherited::Hit(pHDS);
}

void CCustomMonster::OnEvent(NET_Packet& P, u16 type) { inherited::OnEvent(P, type); }

void CCustomMonster::net_Destroy()
{
    inherited::net_Destroy();
    CScriptEntity::net_Destroy();
    sound().unload();
    movement().net_Destroy();

    Actor()->SetActorVisibility(ID(), 0.f);

    Device.remove_from_seq_parallel(fastdelegate::MakeDelegate(this, &CCustomMonster::update_sound_player));
    Device.remove_from_seq_parallel(fastdelegate::MakeDelegate(this, &CCustomMonster::Exec_Visibility));
}

BOOL CCustomMonster::UsedAI_Locations() { return (TRUE); }

void CCustomMonster::PitchCorrection()
{
    CLevelGraph::SContour contour;
    ai().level_graph().contour(contour, ai_location().level_vertex_id());

    Fplane P;
    P.build(contour.v1, contour.v2, contour.v3);

    Fvector position_on_plane;
    P.project(position_on_plane, Position());

    // находим проекцию точки, лежащей на векторе текущего направления
    Fvector dir_point, proj_point;
    dir_point.mad(position_on_plane, Direction(), 1.f);
    P.project(proj_point, dir_point);

    // получаем искомый вектор направления
    Fvector target_dir;
    target_dir.sub(proj_point, position_on_plane);

    float yaw, pitch;
    target_dir.getHP(yaw, pitch);

    movement().m_body.target.pitch = -pitch;
}

BOOL CCustomMonster::feel_touch_on_contact(CObject* O)
{
    CCustomZone* custom_zone = smart_cast<CCustomZone*>(O);
    if (!custom_zone)
        return (TRUE);

    Fsphere sphere;
    sphere.P = Position();
    sphere.R = EPS_L;
    if (custom_zone->inside(sphere))
        return (TRUE);

    return (FALSE);
}

BOOL CCustomMonster::feel_touch_contact(CObject* O)
{
    CCustomZone* custom_zone = smart_cast<CCustomZone*>(O);
    if (!custom_zone)
        return (TRUE);

    Fsphere sphere;
    sphere.P = Position();
    sphere.R = EPS_L;
    if (custom_zone->inside(sphere))
        return (TRUE);

    return (FALSE);
}

void CCustomMonster::load_killer_clsids(LPCSTR section)
{
    m_killer_clsids.clear();
    LPCSTR killers = pSettings->r_string(section, "killer_clsids");
    string16 temp;
    for (u32 i = 0, n = _GetItemCount(killers); i < n; ++i)
        m_killer_clsids.push_back(TEXT2CLSID(_GetItem(killers, i, temp)));
}

bool CCustomMonster::is_special_killer(CObject* obj) { return (obj && (std::find(m_killer_clsids.begin(), m_killer_clsids.end(), obj->CLS_ID) != m_killer_clsids.end())); }

float CCustomMonster::feel_vision_mtl_transp(CObject* O, u32 element) { return (memory().visual().feel_vision_mtl_transp(O, element)); }

void CCustomMonster::feel_sound_new(CObject* who, int type, CSound_UserDataPtr user_data, const Fvector& position, float power)
{
    if (getDestroy())
        return;
    memory().sound().feel_sound_new(who, type, user_data, position, power);
}

float CCustomMonster::feel_sound_range(CObject* who, int type) { return getDestroy() ? 1.f : memory().sound().hear_range(who, type); }

bool CCustomMonster::useful(const CItemManager* manager, const CGameObject* object) const { return (memory().item().useful(object)); }

float CCustomMonster::evaluate(const CItemManager* manager, const CGameObject* object) const { return (memory().item().evaluate(object)); }

bool CCustomMonster::useful(const CEnemyManager* manager, const CEntityAlive* object) const { return (memory().enemy().useful(object)); }

float CCustomMonster::evaluate(const CEnemyManager* manager, const CEntityAlive* object) const { return (memory().enemy().evaluate(object)); }

bool CCustomMonster::useful(const CDangerManager* manager, const CDangerObject& object) const { return (memory().danger().useful(object)); }

float CCustomMonster::evaluate(const CDangerManager* manager, const CDangerObject& object) const { return (memory().danger().evaluate(object)); }

CMovementManager* CCustomMonster::create_movement_manager() { return (xr_new<CMovementManager>(this)); }

CSound_UserDataVisitor* CCustomMonster::create_sound_visitor() { return (m_sound_user_data_visitor = xr_new<CSound_UserDataVisitor>()); }

CMemoryManager* CCustomMonster::create_memory_manager() { return (xr_new<CMemoryManager>(this, create_sound_visitor())); }

const SRotation CCustomMonster::Orientation() const { return (movement().m_body.current); };

const MonsterSpace::SBoneRotation& CCustomMonster::head_orientation() const { return (movement().m_body); }

DLL_Pure* CCustomMonster::_construct()
{
    m_memory_manager = create_memory_manager();
    m_movement_manager = create_movement_manager();
    m_sound_player = xr_new<CSoundPlayer>(this);

    inherited::_construct();
    CScriptEntity::_construct();

    return (this);
}

void CCustomMonster::net_Relcase(CObject* object)
{
    inherited::net_Relcase(object);
    memory().remove_links(object);
}

void CCustomMonster::set_fov(float new_fov)
{
    VERIFY(new_fov > 0.f);
    eye_fov = new_fov;
}

void CCustomMonster::set_range(float new_range)
{
    VERIFY(new_range > 1.f);
    eye_range = new_range;
}

void CCustomMonster::on_restrictions_change()
{
    memory().on_restrictions_change();
    movement().on_restrictions_change();
}

LPCSTR CCustomMonster::visual_name(CSE_Abstract* server_entity)
{
    m_already_dead = false;

    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(server_entity);
    VERIFY(creature);

    if (creature->g_Alive())
        return (inherited::visual_name(server_entity));

    if (creature->m_story_id != INVALID_STORY_ID)
        return (inherited::visual_name(server_entity));

    if (!creature->m_game_death_time)
        return (inherited::visual_name(server_entity));

    ALife::_TIME_ID game_death_time = creature->m_game_death_time;
    ALife::_TIME_ID time_interval = generate_time(1, 1, 1, pSettings->r_u32("monsters_common", "corpse_remove_game_time_interval"), 0, 0);
    ALife::_TIME_ID game_time = Level().GetGameTime();

    if ((game_death_time + time_interval) >= game_time)
        return (inherited::visual_name(server_entity));

    m_already_dead = true;

    return (pSettings->r_string(cNameSect(), "corpse_visual"));
}

void CCustomMonster::on_enemy_change(const CEntityAlive* enemy) {}

CVisualMemoryManager* CCustomMonster::visual_memory() const { return (&memory().visual()); }

void CCustomMonster::save(NET_Packet& packet)
{
    inherited::save(packet);
    if (g_Alive())
        memory().save(packet);
}

void CCustomMonster::load(IReader& packet)
{
    inherited::load(packet);
    if (g_Alive())
        memory().load(packet);
}

bool CCustomMonster::update_critical_wounded(const u16& bone_id, const float& power)
{
    // object should not be critical wounded
    VERIFY(m_critical_wound_type == u32(-1));
    // check 'multiple updates during last hit' situation
    VERIFY(Device.dwTimeGlobal >= m_last_hit_time);

    if (m_critical_wound_threshold < 0)
        return (false);

    float time_delta = m_last_hit_time ? float(Device.dwTimeGlobal - m_last_hit_time) / 1000.f : 0.f;
    m_critical_wound_accumulator += power - m_critical_wound_decrease_quant * time_delta;
    clamp(m_critical_wound_accumulator, 0.f, m_critical_wound_threshold);

    m_last_hit_time = Device.dwTimeGlobal;
    if (m_critical_wound_accumulator < m_critical_wound_threshold)
        return (false);

    m_last_hit_time = 0;
    m_critical_wound_accumulator = 0.f;

    if (critical_wound_external_conditions_suitable())
    {
        BODY_PART::const_iterator I = m_bones_body_parts.find(bone_id);
        if (I == m_bones_body_parts.end())
            return (false);

        m_critical_wound_type = (*I).second;

        critical_wounded_state_start();

        return (true);
    }

    return (false);
}

void CCustomMonster::create_anim_mov_ctrl(CBlend* b)
{
    inherited::create_anim_mov_ctrl(b);

    m_movement_enabled_before_animation_controller = movement().enabled();
    movement().enable_movement(false);
}

void CCustomMonster::destroy_anim_mov_ctrl()
{
    inherited::destroy_anim_mov_ctrl();

    movement().enable_movement(m_movement_enabled_before_animation_controller);

    float roll;
    XFORM().getHPB(movement().m_body.current.yaw, movement().m_body.current.pitch, roll);

    movement().m_body.current.yaw *= -1.f;
    movement().m_body.current.pitch *= -1.f;

    movement().m_body.target.yaw = movement().m_body.current.yaw;
    movement().m_body.target.pitch = movement().m_body.current.pitch;

    NET_Last.o_model = movement().m_body.current.yaw;
    NET_Last.o_torso.pitch = movement().m_body.current.pitch;
}

void CCustomMonster::ForceTransform(const Fmatrix& m)
{
    if (!g_Alive())
        return;
    XFORM().set(m);

    if (character_physics_support()->movement()->CharacterExist())
        character_physics_support()->movement()->EnableCharacter();

    character_physics_support()->set_movement_position(m.c);
    character_physics_support()->movement()->SetVelocity(0, 0, 0);
}

void CCustomMonster::net_ServerDriveTo(const Fvector& pos, float yaw)
{
    if (!g_Alive())
        return;

    // Speed, measured from the assertions rather than declared: the animation
    // selector reads it, and the server never told us how fast it meant to go.
    // Taken BEFORE the move, while Position() is still the previous assertion.
    const u32 now = Device.dwTimeGlobal;
    if (m_server_driven_at && now > m_server_driven_at)
    {
        Fvector step;
        step.sub(pos, Position());
        step.y = 0.f;
        const float dt = float(now - m_server_driven_at) / 1000.f;
        // Smoothed: one tick's worth of distance over one tick's worth of time
        // is a ratio of two small noisy numbers, and the animation should not
        // flicker between walk and stand because of it.
        m_server_driven_speed = 0.9f * m_server_driven_speed + 0.1f * (step.magnitude() / dt);
    }
    m_server_driven_at = now;

    // Claim the position first: move_along_path below consults this, and a
    // claim that lands after the move is a claim the very next frame ignores.
    // One second of authority per assertion -- long enough that an ordinary
    // hitch does not hand the creature back mid-stride, short enough that a
    // server which stops driving releases it promptly. See server_driven().
    m_server_driven_until = now + 1000;
    m_server_driven_yaw = yaw; // net_Export reads this while the claim holds

    // The transform, the physics capsule and the movement manager, as for any
    // teleport-free move.
    Fmatrix F;
    F.rotateY(yaw);
    F.c = pos;
    net_MoveTo(F);

    // ⚠And the part that actually makes it stick. UpdateCL above ends with
    //
    //     XFORM().rotateY(NET_Last.o_model);
    //     XFORM().translate_over(NET_Last.p_pos);
    //
    // unconditionally, every frame, for every living creature. So NET_Last is
    // the creature's position, and the transform is a rendering of it: a
    // server that writes only XFORM is overwritten before the next frame by a
    // value that is one refresh of NET_Last out of date, and the creature
    // creeps forward at a fraction of the speed it was given rather than not
    // moving at all -- which is why this reads as a physics or animation
    // problem instead of the assignment it is.
    //
    // NET.back() goes too: the extrapolation branch at the top of UpdateCL
    // copies the whole of it over NET_Last, so leaving it stale would put the
    // creature back the moment interpolation runs dry.
    NET_Last.p_pos = pos;
    NET_Last.o_model = yaw;
    if (!NET.empty())
    {
        NET.back().p_pos = pos;
        NET.back().o_model = yaw;
    }
}

void CCustomMonster::net_MoveTo(const Fmatrix& m)
{
    // network-puppet motion: same as ForceTransform but WITHOUT the
    // set_movement_position collision-correct/settle (see CGameObject::net_MoveTo)
    if (!g_Alive())
        return;
    XFORM().set(m);

    if (character_physics_support()->movement()->CharacterExist())
        character_physics_support()->movement()->EnableCharacter();

    character_physics_support()->movement()->SetPosition(m.c);
    character_physics_support()->movement()->SetVelocity(0, 0, 0);
}
