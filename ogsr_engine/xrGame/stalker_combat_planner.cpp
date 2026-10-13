////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_combat_planner.cpp
//	Created 	: 25.03.2004
//  Modified 	: 27.09.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker combat planner
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_combat_planner.h"
#include "stalker_combat_actions.h"
#include "stalker_danger_property_evaluators.h"
#include "stalker_decision_space.h"
#include "stalker_property_evaluators.h"
#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_impl.h"
#include "script_game_object.h"
#include "cover_evaluators.h"
#include "cover_manager.h"
#include "cover_point.h"
#include "stalker_movement_restriction.h"
#include "memory_manager.h"
#include "visual_memory_manager.h"
#include "enemy_manager.h"
#include "danger_manager.h"
#include "sound_player.h"
#include "Missile.h"
#include "Explosive.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "member_order.h"
#include "ai/stalker/ai_stalker_space.h"
#include "stalker_planner.h"
#include "stalker_kill_wounded_planner.h"
#include "stalker_movement_manager.h"
#include "Actor.h"
#include "Level.h"
#include "ai_space.h"
#include "../xr_3da/GameMtlLib.h"
#include "level_graph.h"
#include "Weapon.h"
#include "ai_commander.h"
#include "stalker_combat_task_actions.h"

using namespace StalkerSpace;
using namespace StalkerDecisionSpace;

CStalkerCombatPlanner::CStalkerCombatPlanner(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

CStalkerCombatPlanner::~CStalkerCombatPlanner()
{
    auto temp = fastdelegate::MakeDelegate(this, &CStalkerCombatPlanner::on_best_cover_changed);
    object().unsubscribe_on_best_cover_changed(temp);
}

void CStalkerCombatPlanner::on_best_cover_changed(const CCoverPoint* new_cover, const CCoverPoint* old_cover)
{
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyInCover, false);
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyLookedOut, false);
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyPositionHolded, false);
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyEnemyDetoured, false);
}

void CStalkerCombatPlanner::setup(CAI_Stalker* object, CPropertyStorage* storage)
{
    inherited::setup(object, storage);

    CScriptActionPlanner::m_storage.set_property(eWorldPropertyInCover, false);
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyLookedOut, false);
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyPositionHolded, false);
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyEnemyDetoured, false);
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyUseSuddenness, true);
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyUseCrouchToLookOut, true);
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyKilledWounded, false);

    this->object().brain().CStalkerPlanner::m_storage.set_property(eWorldPropertyCriticallyWounded, false);

    clear();
    add_evaluators();
    add_actions();

    auto temp = fastdelegate::MakeDelegate(this, &CStalkerCombatPlanner::on_best_cover_changed);
    this->object().subscribe_on_best_cover_changed(temp);
}

void CStalkerCombatPlanner::execute() { inherited::execute(); }

void CStalkerCombatPlanner::update()
{
    // ai_tactics_v2: the commander has his task ready for the plan this update
    const bool commanded = Commander::Applies(object());
    if (commanded)
        Commander::Report(object());

    inherited::update();

    object().react_on_grenades();
    object().react_on_member_death();
    cry_enemy_lost();

    extern int g_dbg_npc_tactics;
    if (commanded)
    {
        // no flags forced: a task is the plan's own property (has task, at place)
        if (g_dbg_npc_tactics)
            dbg_tactics();
        return;
    }

    // ⭐Holed up in a house, the posts round it are the plan for everybody.
    // 📏06-10 16:14: the siege was the search's, and the search came only when
    // nobody in the squad had seen him a while -- a post seeing him at a window
    // put the whole squad back to hiding, looking out and going round 30-45 m
    // off across the street. Now each man who has not seen him himself for
    // SIEGE_OWN_LOST goes to his post, whoever else sees him; one who does
    // shoots from where he is. No suppressing or flanking: the posts are that.
    const CEntityAlive* enemy = object().memory().enemy().selected();
    bool siege = false;
    if (enemy)
    {
        if (object().memory().visual().visible_now(enemy))
            m_own_seen_time = Device.dwTimeGlobal;
        const CMemoryInfo memory = object().memory().memory(enemy);
        siege = memory.m_object && stalker_siege(enemy->ID(), memory.m_object_params.m_position, memory.m_level_time);
    }
    if (!siege)
        update_suppression();

    // Nobody in the fight has seen him for SEARCH_DELAY: go and look for him.
    // The stock plan gets to the search only after every stalker has hidden,
    // looked out, held and flanked in turn -- the instrument showed half a
    // minute of that against a spot the player had long left.
    const bool lost = siege ? Device.dwTimeGlobal > m_own_seen_time + SIEGE_OWN_LOST : m_enemy_seen_time && Device.dwTimeGlobal > m_enemy_seen_time + SEARCH_DELAY;
    if (lost)
    {
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyInCover, false);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyLookedOut, true);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyPositionHolded, true);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyEnemyDetoured, true);
    }
    // ...and one who sees him from his post shoots from it: the stock plan fires
    // only "in cover" and ran to its own cover first -- 📏06-10 16:27, posts
    // left 9-20 m off for it each time the player showed at a window
    else if (siege && Device.dwTimeGlobal == m_own_seen_time)
    {
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyInCover, true);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyLookedOut, true);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyPositionHolded, true);
    }

    if (g_dbg_npc_tactics)
        dbg_tactics();

    //	const CEntityAlive				*enemy = object().memory().enemy().selected();
    //	VERIFY							(enemy);
    //	const CAI_Stalker				*stalker = smart_cast<const CAI_Stalker*>(enemy);
    //	m_last_wounded					= stalker && stalker->wounded();
}

static LPCSTR dbg_combat_action_name(u32 id)
{
    switch (id)
    {
    case eWorldOperatorGetItemToKill: return "get_item_to_kill";
    case eWorldOperatorFindItemToKill: return "find_item_to_kill";
    case eWorldOperatorMakeItemKilling: return "make_item_killing";
    case eWorldOperatorFindAmmo: return "find_ammo";
    case eWorldOperatorAimEnemy: return "aim_enemy";
    case eWorldOperatorGetReadyToKill: return "get_ready_to_kill";
    case eWorldOperatorGetReadyToDetour: return "get_ready_to_detour";
    case eWorldOperatorKillEnemy: return "kill_enemy";
    case eWorldOperatorRetreatFromEnemy: return "retreat";
    case eWorldOperatorTakeCover: return "take_cover";
    case eWorldOperatorLookOut: return "look_out";
    case eWorldOperatorHoldPosition: return "hold_position";
    case eWorldOperatorGetDistance: return "get_distance";
    case eWorldOperatorDetourEnemy: return "detour";
    case eWorldOperatorSearchEnemy: return "search";
    case eWorldOperatorHideFromGrenade: return "hide_from_grenade";
    case eWorldOperatorSuddenAttack: return "sudden_attack";
    case eWorldOperatorKillEnemyIfNotVisible: return "kill_if_not_visible";
    case eWorldOperatorReachWoundedEnemy: return "reach_wounded";
    case eWorldOperatorAimWoundedEnemy: return "aim_wounded";
    case eWorldOperatorPrepareWoundedEnemy: return "prepare_wounded";
    case eWorldOperatorKillWoundedEnemy: return "kill_wounded";
    case eWorldOperatorPostCombatWait: return "post_combat_wait";
    case eWorldOperatorKillEnemyIfPlayerOnThePath: return "kill_player_on_path";
    case eWorldOperatorCriticallyWounded: return "critically_wounded";
    case eWorldOperatorKillEnemyIfCriticallyWounded: return "kill_if_crit_wounded";
    case eWorldOperatorPauseAfterKill: return "pause_after_kill";
    case eWorldOperatorReachPlace: return "reach_place";
    case eWorldOperatorFightFromPlace: return "fight_from_place";
    default: return nullptr;
    }
}

static void dbg_action_name(u32 id, string32& out)
{
    if (LPCSTR known = dbg_combat_action_name(id))
        xr_strcpy(out, known);
    else if (id == u32(-1))
        xr_strcpy(out, "none");
    else
        xr_sprintf(out, "script#%u", id);
}

void stalker_dbg_action(CAI_Stalker& npc, string32& out)
{
    CStalkerPlanner& brain = npc.brain();
    if (brain.solution().empty())
    {
        xr_strcpy(out, "none");
        return;
    }
    switch (brain.current_action_id())
    {
    case eWorldOperatorCombatPlanner:
    {
        CStalkerCombatPlanner* combat = smart_cast<CStalkerCombatPlanner*>(&brain.current_action());
        if (combat && !combat->solution().empty())
            dbg_action_name(combat->current_action_id(), out);
        else
            xr_strcpy(out, "combat");
        return;
    }
    case eWorldOperatorALifePlanner: xr_strcpy(out, "alife"); return;
    case eWorldOperatorDangerPlanner: xr_strcpy(out, "danger"); return;
    case eWorldOperatorAnomalyPlanner: xr_strcpy(out, "anomaly"); return;
    case eWorldOperatorDeathPlanner: xr_strcpy(out, "dead"); return;
    default: xr_sprintf(out, "planner#%u", brain.current_action_id()); return;
    }
}

// Instrument for the tactics work: what every fighting stalker near the player
// is doing, where it is going and which cover it holds. A line on every action
// change (marked *), and a snapshot once a second. dbg_npc_tactics <radius m>, 0 = off.
void CStalkerCombatPlanner::dbg_tactics()
{
    extern int g_dbg_npc_tactics;
    const CActor* actor = Actor();
    if (!actor || object().Position().distance_to(actor->Position()) > float(g_dbg_npc_tactics))
        return;

    const u32 action = this->solution().empty() ? u32(-1) : this->current_action_id();
    const bool changed = action != m_dbg_action;
    if (!changed && Device.dwTimeGlobal < m_dbg_print_time + 1000)
        return;

    m_dbg_print_time = Device.dwTimeGlobal;

    string32 action_name, prev_name;
    dbg_action_name(action, action_name);
    dbg_action_name(m_dbg_action, prev_name);
    m_dbg_action = action;

    const CEntityAlive* enemy = object().memory().enemy().selected();
    const bool sees = enemy && object().memory().visual().visible_now(enemy);
    bool squad_sees = sees;
    for (CMemberOrder* member : object().agent_manager().member().combat_members())
        squad_sees = squad_sees || (enemy && member->object().memory().visual().visible_now(enemy));

    const CStalkerMovementManager& move = object().movement();
    static constexpr LPCSTR mental[] = {"danger", "free", "panic"};
    static constexpr LPCSTR body[] = {"crouch", "stand"};
    static constexpr LPCSTR gait[] = {"walk", "run", "stand"};

    const Fvector& p = object().Position();
    Fvector dest = p;
    const u32 dest_vertex = move.level_dest_vertex_id();
    if (ai().level_graph().valid_vertex_id(dest_vertex))
        dest = ai().level_graph().vertex_position(dest_vertex);

    const CMemberOrder& order = object().agent_manager().member().member(&object());
    string64 cover = "-";
    if (const CCoverPoint* c = order.cover())
        xr_sprintf(cover, "(%.1f,%.1f) %.1fm", c->position().x, c->position().z, c->position().distance_to(p));

    // hx: how far the eyes sit right of the feet along the body's right axis.
    // Leaning right must push it up: that is the check on the roll's sign.
    Fvector forward, right, eye;
    forward.setHP(-move.body_orientation().current.yaw, 0.f);
    right.set(forward.z, 0.f, -forward.x);
    eye.sub(object().eye_matrix.c, p);

    // Can he see behind his back? ang: between where the head looks and the
    // enemy, 0 = in his face, 180 = behind him (the eye cone is eye_fov wide).
    // src/age: what last told him where the enemy is -- v eyes (his or the
    // squad's), s a sound, h a hit -- and how long ago.
    float ang = 0.f, age = 0.f;
    int cone = 0, wall = 0, head = 0, fz = -1;
    float tr = -1.f;
    string64 head_hit{};
    string16 src = "-";
    if (enemy)
    {
        Fvector look, to_enemy;
        look.setHP(-move.head_orientation().current.yaw, 0.f);
        to_enemy.sub(enemy->Position(), p);
        to_enemy.y = 0.f;
        to_enemy.normalize_safe();
        ang = rad2deg(acosf(std::clamp(look.dotproduct(to_enemy), -1.f, 1.f)));

        const CMemoryInfo info = object().memory().memory(enemy);
        xr_sprintf(src, "%s%s%s", info.m_visual_info ? "v" : "", info.m_sound_info ? "s" : "", info.m_hit_info ? "h" : "");
        age = info.m_object ? float(Device.dwTimeGlobal - info.m_level_time) / 1000.f : 0.f;

        // in his eye cone but walled off? (static geometry only; bushes are
        // the transparency factor, not a wall)
        cone = ang <= object().eye_fov * .5f ? 1 : 0;
        Fvector center, dir;
        enemy->Center(center);
        dir.sub(center, object().eye_matrix.c);
        const float range = dir.magnitude();
        if (range > EPS_L)
        {
            dir.div(range);
            collide::rq_result hit;
            wall = Level().ObjectSpace.RayPick(object().eye_matrix.c, dir, range - .5f, collide::rqtStatic, hit, nullptr) ? 1 : 0;
        }
        // head: the same to where his head is -- what shows over a sill;
        // fz/tr: the engine's own eyes, -1 out of the eye frustum, 0 no ray
        // got there, 1 seen, and how much of it got through
        const CActor* actor = smart_cast<const CActor*>(enemy);
        Fvector top = enemy->Position();
        top.y += actor && (actor->MovingState() & mcCrouch) ? 1.f : 1.6f;
        dir.sub(top, object().eye_matrix.c);
        const float top_range = dir.magnitude();
        if (top_range > EPS_L)
        {
            dir.div(top_range);
            collide::rq_result hit;
            head = Level().ObjectSpace.RayPick(object().eye_matrix.c, dir, top_range - .3f, collide::rqtStatic, hit, nullptr) ? 1 : 0;
            // what stops it and where: a sill 2 m off, a stove next to him
            if (head && hit.element >= 0)
            {
                const CDB::TRI* T = Level().ObjectSpace.GetStaticTris() + hit.element;
                xr_sprintf(head_hit, "@%.1fm:%s", hit.range, GMLib.GetMaterialByIdx(T->material)->m_Name.c_str());
            }
        }
        fz = object().feel_vision_state(enemy);
        tr = object().feel_vision_get_transparency(enemy);
    }

    Msg("[tac]%s%u '%s' %s hp=%.2f @(%.1f,%.1f,%.1f) act=%s%s%s | %s/%s/%s path_done=%d dest=(%.1f,%.1f) %.1fm | cover=%s in=%d looked=%d held=%d detoured=%d detour_order=%d | enemy=%s d=%.1f sees=%d spot=%.0f ang=%.0f cone=%d wall=%d head=%d%s fz=%d tr=%.2f src=%s age=%.1f squad_sees=%d members=%u resumes=%u searcher=%d sweep=%d role=%s | lean=%+.0f>%+.0f(%s) hx=%+.2f hit=%.1f L=%.1f R=%.1f shift=%.2f",
        changed ? " * " : "   ", object().ID(), object().Name(), object().cNameSect().c_str(), object().GetfHealth(), p.x, p.y, p.z, action_name,
        changed ? " <- " : "", changed ? prev_name : "", mental[std::min<u32>(move.mental_state(), 2)], body[std::min<u32>(move.body_state(), 1)],
        gait[std::min<u32>(move.movement_type(), 2)], move.path_completed() ? 1 : 0, dest.x, dest.z, dest.distance_to(p), cover,
        CScriptActionPlanner::m_storage.property(eWorldPropertyInCover) ? 1 : 0, CScriptActionPlanner::m_storage.property(eWorldPropertyLookedOut) ? 1 : 0,
        CScriptActionPlanner::m_storage.property(eWorldPropertyPositionHolded) ? 1 : 0, CScriptActionPlanner::m_storage.property(eWorldPropertyEnemyDetoured) ? 1 : 0,
        order.detour() ? 1 : 0, enemy ? enemy->cName().c_str() : "-", enemy ? enemy->Position().distance_to(p) : 0.f, sees ? 1 : 0, enemy ? 100.f * object().memory().visual().spot_progress(enemy) : 0.f, ang, cone, wall, head, head_hit, fz, tr, src, age, squad_sees ? 1 : 0,
        u32(object().agent_manager().member().members().size()), m_dbg_resumes, enemy && stalker_search_holder(enemy->ID()) == object().ID() ? 1 : 0,
        enemy && stalker_sweep_start(enemy->ID(), object().memory().memory(enemy).m_object_params.m_position) ? 1 : 0,
        !enemy                                                ? "-" :
            stalker_suppressor(enemy->ID()) == object().ID()  ? "suppress" :
            stalker_is_flanker(enemy->ID(), object().ID())    ? "flank" :
                                                                "-",
        rad2deg(object().lean()), rad2deg(object().lean_target()), object().lean_reason(), eye.dotproduct(right), object().lean_hit(), object().lean_side_hit(0), object().lean_side_hit(1), object().lean_shift());
}

// "Where did he go?" the moment the squad loses sight of a human enemy, as in
// F.E.A.R. The stock search line plays only after reaching his last position,
// so breaking line of sight went unremarked. One line per loss: every member
// marks it as said, and can_cry_noninfo_phrase lets only the first one speak.
void CStalkerCombatPlanner::cry_enemy_lost()
{
    const CEntityAlive* enemy = object().memory().enemy().selected();
    if (!enemy || !enemy->human_being())
    {
        m_enemy_seen_time = 0;
        return;
    }

    if (enemy->ID() != m_lost_enemy_id)
    {
        m_lost_enemy_id = enemy->ID();
        m_enemy_seen_time = 0;
        m_enemy_lost_cried = false;
    }

    bool seen = object().memory().visual().visible_now(enemy);
    for (CMemberOrder* member : object().agent_manager().member().combat_members())
        seen = seen || member->object().memory().visual().visible_now(enemy);

    if (seen)
    {
        // Found again after the squad had lost him: "there he is!", from the one
        // whose own eyes found him. The alarm lines played only on entering the
        // fight, so a squad that searched and spotted the player said nothing.
        if (m_enemy_lost_cried && object().memory().visual().visible_now(enemy) && object().agent_manager().member().can_cry_noninfo_phrase())
            object().sound().play(eStalkerSoundAlarm, 0, 0, 6000, 4000);

        m_enemy_seen_time = Device.dwTimeGlobal;
        m_enemy_lost_cried = false;
        return;
    }

    if (m_enemy_lost_cried || !m_enemy_seen_time || Device.dwTimeGlobal < m_enemy_seen_time + ENEMY_LOST_DELAY)
        return;

    m_enemy_lost_cried = true;
    // somebody is firing into where he went: the squad knows where he is
    if (stalker_suppressor(enemy->ID()) != u16(-1))
        return;
    if (Device.dwTimeGlobal < m_enemy_seen_time + ENEMY_LOST_WINDOW && object().agent_manager().member().can_cry_noninfo_phrase())
        object().sound().play(eStalkerSoundEnemyLost, 0, 0, 15000, 10000);
}

float g_ai_suppress_time = 6.f; // s after the squad lost sight of him; 0 = no suppressing fire

// Who suppresses and who goes round, once the player has ducked out of the
// squad's sight. One stalker in the cover part of the plan, with an automatic
// weapon, claims the suppressor's role and fires into where the player went
// to ground (CStalkerActionCombatBase::suppress); up to two of the others are
// sent round him at once -- the stock detour came only after every stalker had
// hidden, looked out and held in turn. It all stops when somebody sees him
// again or after ai_suppress_time; the search (SEARCH_DELAY) comes after that.
void CStalkerCombatPlanner::update_suppression()
{
    const CEntityAlive* enemy = object().memory().enemy().selected();
    if (!enemy || g_ai_suppress_time <= 0.f || !smart_cast<const CActor*>(enemy) || !m_enemy_seen_time)
        return;

    const u32 lost = Device.dwTimeGlobal - m_enemy_seen_time;
    if (lost < SUPPRESS_START || lost > u32(g_ai_suppress_time * 1000.f))
        return;

    const CMemoryInfo memory = object().memory().memory(enemy);
    if (!memory.m_object || memory.m_object_params.m_position.distance_to(object().Position()) > 70.f)
        return;

    const u32 action = this->solution().empty() ? u32(-1) : this->current_action_id();
    const bool in_cover = action == eWorldOperatorTakeCover || action == eWorldOperatorLookOut || action == eWorldOperatorHoldPosition;

    // an automatic with something in the magazine
    const CWeapon* weapon = smart_cast<const CWeapon*>(object().best_weapon());
    const bool automatic = weapon && weapon->ef_weapon_type() == 6 && weapon->GetAmmoElapsed() > 0;

    if (in_cover && automatic && stalker_search_holder(enemy->ID()) != object().ID() && stalker_claim_suppress(enemy->ID(), object().ID()))
        return;

    const u16 suppressor = stalker_suppressor(enemy->ID());
    if (suppressor == u16(-1) || suppressor == object().ID() || stalker_search_holder(enemy->ID()) == object().ID())
        return;

    if (m_flank_loss == m_enemy_seen_time)
    {
        // on the way round: keep the place
        if (action == eWorldOperatorDetourEnemy || action == eWorldOperatorGetReadyToDetour)
            stalker_claim_flank(enemy->ID(), object().ID());
        return;
    }

    if (!stalker_claim_flank(enemy->ID(), object().ID()))
        return;

    // the detour's conditions: done hiding, looking out and holding
    m_flank_loss = m_enemy_seen_time;
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyInCover, false);
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyLookedOut, true);
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyPositionHolded, true);
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyEnemyDetoured, false);

    extern int g_dbg_npc_tactics;
    if (g_dbg_npc_tactics)
        Msg("[tac] %u '%s' goes round %u while %u suppresses", object().ID(), object().Name(), enemy->ID(), suppressor);
}

void CStalkerCombatPlanner::initialize()
{
    inherited::initialize();

    // A mod script action (stepping out of a friend's line of fire, the state
    // manager settling the body) takes a stalker out of combat for a second or
    // two. Starting over from take_cover each time kept half a squad cycling
    // take_cover -> step aside -> take_cover and never getting to look out,
    // hold or flank. Coming back to the same enemy that soon, keep the cover
    // and the progress made.
    const CEntityAlive* enemy = object().memory().enemy().selected();
    const bool resume = !m_loaded && m_finalize_time && Device.dwTimeGlobal < m_finalize_time + RESUME_INTERVAL && enemy && enemy->ID() == m_finalize_enemy_id;
    if (resume)
        ++m_dbg_resumes;

    if (!m_loaded && !resume)
    {
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyInCover, false);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyLookedOut, false);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyPositionHolded, false);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyEnemyDetoured, false);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyUseSuddenness, true);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyKilledWounded, false);

        object().brain().CStalkerPlanner::m_storage.set_property(eWorldPropertyCriticallyWounded, false);
    }

    if (!resume)
        object().agent_manager().member().member(m_object).cover(0);
    // this is fake, should be revisited
    // we must clear path, since it can be built using eMentalStateFree velocities
    // and our new path may not be ready
    object().movement().clear_path();

    object().m_clutched_hammer_enabled = true;

    m_last_enemy_id = u16(-1);
    m_last_level_time = 0;
    m_last_wounded = false;

    if (!resume)
    {
        m_lost_enemy_id = u16(-1);
        m_enemy_seen_time = 0;
        m_enemy_lost_cried = false;
    }

    if (!m_loaded && !resume && object().memory().enemy().selected())
    {
        CVisualMemoryManager* visual_memory_manager = object().memory().enemy().selected()->visual_memory();
        VERIFY(visual_memory_manager);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyUseSuddenness, !visual_memory_manager->visible_now(&object()));
    }

    m_loaded = false;

    if (!object().agent_manager().member().combat_members().empty())
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyUseSuddenness, false);

    //  this is possible when i enter combat when it is wait after combat stage
    //	VERIFY					(object().memory().enemy().selected());

    if (!resume && m_object->memory().visual().visible_now(m_object->memory().enemy().selected()))
    {
        if (m_object->memory().enemy().selected()->human_being())
            if (object().agent_manager().member().can_cry_noninfo_phrase())
                if (object().agent_manager().member().members().size() > 1)
                    if (!CScriptActionPlanner::m_storage.property(eWorldPropertyUseSuddenness))
                        object().sound().play(eStalkerSoundAlarm);
    }

    object().agent_manager().member().register_in_combat(m_object);
}

void CStalkerCombatPlanner::finalize()
{
    inherited::finalize();

    if (!object().g_Alive())
        return;

    const CEntityAlive* enemy = object().memory().enemy().selected();
    m_finalize_time = Device.dwTimeGlobal;
    m_finalize_enemy_id = enemy ? enemy->ID() : u16(-1);

    object().memory().danger().time_line(Device.dwTimeGlobal + 3000);
    if (object().agent_manager().member().registered_in_combat(m_object))
        object().agent_manager().member().unregister_in_combat(m_object);

    object().m_clutched_hammer_enabled = false;

    //	object().sound().remove_active_sounds					(eStalkerSoundMaskNoDanger);
}

void CStalkerCombatPlanner::add_evaluators()
{
    add_evaluator(eWorldPropertyPureEnemy, xr_new<CStalkerPropertyEvaluatorEnemies>(m_object, "is_there_enemies", 0));
    add_evaluator(eWorldPropertyEnemy, xr_new<CStalkerPropertyEvaluatorEnemies>(m_object, "is_there_enemies_delayed", POST_COMBAT_WAIT_INTERVAL, &m_last_wounded));
    add_evaluator(eWorldPropertySeeEnemy, xr_new<CStalkerPropertyEvaluatorSeeEnemy>(m_object, "see enemy"));
    add_evaluator(eWorldPropertyEnemySeeMe, xr_new<CStalkerPropertyEvaluatorEnemySeeMe>(m_object, "enemy see me"));
    add_evaluator(eWorldPropertyItemToKill, xr_new<CStalkerPropertyEvaluatorItemToKill>(m_object, "item to kill"));
    add_evaluator(eWorldPropertyItemCanKill, xr_new<CStalkerPropertyEvaluatorItemCanKill>(m_object, "item can kill"));
    add_evaluator(eWorldPropertyFoundItemToKill, xr_new<CStalkerPropertyEvaluatorFoundItemToKill>(m_object, "found item to kill"));
    add_evaluator(eWorldPropertyFoundAmmo, xr_new<CStalkerPropertyEvaluatorFoundAmmo>(m_object, "found ammo"));
    add_evaluator(eWorldPropertyReadyToKill, xr_new<CStalkerPropertyEvaluatorReadyToKill>(m_object, "ready to kill"));
    add_evaluator(eWorldPropertyReadyToDetour, xr_new<CStalkerPropertyEvaluatorReadyToDetour>(m_object, "ready to detour"));
    add_evaluator(eWorldPropertyPanic, xr_new<CStalkerPropertyEvaluatorPanic>(m_object, "panic"));
    add_evaluator(eWorldPropertyDangerGrenade, xr_new<CStalkerPropertyEvaluatorGrenadeToExplode>(m_object, "is there grenade to explode"));
    add_evaluator(eWorldPropertyEnemyWounded, xr_new<CStalkerPropertyEvaluatorEnemyWounded>(m_object, "is enemy wounded"));
    add_evaluator(eWorldPropertyPlayerOnThePath, xr_new<CStalkerPropertyEvaluatorPlayerOnThePath>(m_object, "player on the path"));
    add_evaluator(eWorldPropertyEnemyCriticallyWounded, xr_new<CStalkerPropertyEvaluatorEnemyCriticallyWounded>(m_object, "enemy_critically_wounded"));
    add_evaluator(eWorldPropertyHasTask, xr_new<CStalkerPropertyEvaluatorHasTask>(m_object, "has task"));
    add_evaluator(eWorldPropertyAtPlace, xr_new<CStalkerPropertyEvaluatorAtPlace>(m_object, "at place"));

    add_evaluator(eWorldPropertyInCover, xr_new<CStalkerPropertyEvaluatorMember>((CPropertyStorage*)0, eWorldPropertyInCover, true, true, "in cover"));
    add_evaluator(eWorldPropertyLookedOut, xr_new<CStalkerPropertyEvaluatorMember>((CPropertyStorage*)0, eWorldPropertyLookedOut, true, true, "looked out"));
    add_evaluator(eWorldPropertyPositionHolded, xr_new<CStalkerPropertyEvaluatorMember>((CPropertyStorage*)0, eWorldPropertyPositionHolded, true, true, "position holded"));
    add_evaluator(eWorldPropertyEnemyDetoured, xr_new<CStalkerPropertyEvaluatorMember>((CPropertyStorage*)0, eWorldPropertyEnemyDetoured, true, true, "enemy detoured"));
    add_evaluator(eWorldPropertyUseSuddenness, xr_new<CStalkerPropertyEvaluatorMember>((CPropertyStorage*)0, eWorldPropertyUseSuddenness, true, true, "use suddenness"));
    add_evaluator(eWorldPropertyCriticallyWounded,
                  xr_new<CStalkerPropertyEvaluatorMember>(&object().brain().CStalkerPlanner::m_storage, eWorldPropertyCriticallyWounded, true, true, "critically wounded"));
    add_evaluator(eWorldPropertyKilledWounded,
                  xr_new<CStalkerPropertyEvaluatorMember>(&object().brain().CStalkerPlanner::m_storage, eWorldPropertyKilledWounded, true, true, "killed critically wounded"));
}

void CStalkerCombatPlanner::add_actions()
{
    CStalkerActionBase* action;

    action = xr_new<CStalkerActionGetItemToKill>(m_object, "get_item_to_kill");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyFoundItemToKill, true);
    add_condition(action, eWorldPropertyItemToKill, false);
    add_effect(action, eWorldPropertyItemToKill, true);
    add_effect(action, eWorldPropertyItemCanKill, true);
    add_operator(eWorldOperatorGetItemToKill, action);

    action = xr_new<CStalkerActionMakeItemKilling>(m_object, "make_item_killing");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyFoundAmmo, true);
    add_condition(action, eWorldPropertyItemCanKill, false);
    add_effect(action, eWorldPropertyItemCanKill, true);
    add_operator(eWorldOperatorMakeItemKilling, action);

    action = xr_new<CStalkerActionRetreatFromEnemy>(m_object, "retreat_from_enemy");
    add_effect(action, eWorldPropertyPureEnemy, false);
    add_operator(eWorldOperatorRetreatFromEnemy, action);

    action = xr_new<CStalkerActionGetReadyToKill>(true, m_object, "get_ready_to_kill");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, false);
    add_condition(action, eWorldPropertyUseSuddenness, false);
    add_condition(action, eWorldPropertyItemToKill, true);
    add_condition(action, eWorldPropertyItemCanKill, true);
    add_condition(action, eWorldPropertyReadyToKill, false);
    add_condition(action, eWorldPropertyEnemyWounded, false);
    add_condition(action, eWorldPropertyPlayerOnThePath, false);
    add_effect(action, eWorldPropertyReadyToKill, true);

    add_effect(action, eWorldPropertyInCover, false);
    add_effect(action, eWorldPropertyLookedOut, false);
    add_effect(action, eWorldPropertyPositionHolded, false);
    add_effect(action, eWorldPropertyEnemyDetoured, false);

    add_operator(eWorldOperatorGetReadyToKill, action);

    action = xr_new<CStalkerActionGetReadyToKill>(false, m_object, "get_ready_to_detour");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, false);
    add_condition(action, eWorldPropertyUseSuddenness, false);
    add_condition(action, eWorldPropertyItemToKill, true);
    add_condition(action, eWorldPropertyItemCanKill, true);
    add_condition(action, eWorldPropertyReadyToDetour, false);
    add_condition(action, eWorldPropertyEnemyWounded, false);
    add_condition(action, eWorldPropertyPlayerOnThePath, false);
    add_effect(action, eWorldPropertyReadyToDetour, true);
    add_operator(eWorldOperatorGetReadyToDetour, action);

    action = xr_new<CStalkerActionKillEnemy>(m_object, "kill_enemy");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, false);
    add_condition(action, eWorldPropertyUseSuddenness, false);
    add_condition(action, eWorldPropertyReadyToKill, true);
    add_condition(action, eWorldPropertySeeEnemy, true);
    add_condition(action, eWorldPropertyInCover, true);
    add_condition(action, eWorldPropertyPanic, false);
    add_condition(action, eWorldPropertyEnemyWounded, false);
    add_condition(action, eWorldPropertyHasTask, false);
    add_effect(action, eWorldPropertyPureEnemy, false);

    add_effect(action, eWorldPropertyLookedOut, false);
    add_effect(action, eWorldPropertyPositionHolded, false);
    add_effect(action, eWorldPropertyEnemyDetoured, false);

    add_operator(eWorldOperatorKillEnemy, action);

    action = xr_new<CStalkerActionTakeCover>(m_object, "take_cover");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, false);
    add_condition(action, eWorldPropertyUseSuddenness, false);
    add_condition(action, eWorldPropertyItemToKill, true);
    add_condition(action, eWorldPropertyItemCanKill, true);
    add_condition(action, eWorldPropertyReadyToKill, true);
    add_condition(action, eWorldPropertyInCover, false);
    add_condition(action, eWorldPropertyEnemyWounded, false);
    add_condition(action, eWorldPropertyPlayerOnThePath, false);
    add_condition(action, eWorldPropertyHasTask, false);
    add_effect(action, eWorldPropertyInCover, true);

    add_effect(action, eWorldPropertyLookedOut, false);
    add_effect(action, eWorldPropertyPositionHolded, false);
    add_effect(action, eWorldPropertyEnemyDetoured, false);

    add_operator(eWorldOperatorTakeCover, action);

    action = xr_new<CStalkerActionLookOut>(m_object, "look_out");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, false);
    add_condition(action, eWorldPropertyUseSuddenness, false);
    add_condition(action, eWorldPropertyReadyToKill, true);
    add_condition(action, eWorldPropertyInCover, true);
    add_condition(action, eWorldPropertyLookedOut, false);
    add_condition(action, eWorldPropertySeeEnemy, false);
    add_condition(action, eWorldPropertyEnemyWounded, false);
    add_condition(action, eWorldPropertyPlayerOnThePath, false);
    add_condition(action, eWorldPropertyHasTask, false);
    add_effect(action, eWorldPropertyLookedOut, true);
    add_operator(eWorldOperatorLookOut, action);

    action = xr_new<CStalkerActionHoldPosition>(m_object, "hold_position");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, false);
    add_condition(action, eWorldPropertyUseSuddenness, false);
    add_condition(action, eWorldPropertyReadyToKill, true);
    add_condition(action, eWorldPropertyInCover, true);
    add_condition(action, eWorldPropertyLookedOut, true);
    add_condition(action, eWorldPropertySeeEnemy, false);
    add_condition(action, eWorldPropertyPositionHolded, false);
    add_condition(action, eWorldPropertyEnemyWounded, false);
    add_condition(action, eWorldPropertyPlayerOnThePath, false);
    add_condition(action, eWorldPropertyHasTask, false);
    add_effect(action, eWorldPropertyInCover, false);
    add_effect(action, eWorldPropertyPositionHolded, true);
    add_operator(eWorldOperatorHoldPosition, action);

    action = xr_new<CStalkerActionDetourEnemy>(m_object, "detour_enemy");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, false);
    add_condition(action, eWorldPropertyUseSuddenness, false);
    add_condition(action, eWorldPropertyReadyToKill, true);
    add_condition(action, eWorldPropertyReadyToDetour, true);
    add_condition(action, eWorldPropertyInCover, false);
    add_condition(action, eWorldPropertyEnemyDetoured, false);
    add_condition(action, eWorldPropertySeeEnemy, false);
    add_condition(action, eWorldPropertyLookedOut, true);
    add_condition(action, eWorldPropertyPositionHolded, true);
    add_condition(action, eWorldPropertyPanic, false);
    add_condition(action, eWorldPropertyEnemyWounded, false);
    add_condition(action, eWorldPropertyPlayerOnThePath, false);
    add_condition(action, eWorldPropertyHasTask, false);
    add_effect(action, eWorldPropertyEnemyDetoured, true);
    add_operator(eWorldOperatorDetourEnemy, action);

    action = xr_new<CStalkerActionSearchEnemy>(m_object, "search_enemy");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, false);
    add_condition(action, eWorldPropertyUseSuddenness, false);
    add_condition(action, eWorldPropertyReadyToKill, true);
    add_condition(action, eWorldPropertySeeEnemy, false);
    add_condition(action, eWorldPropertyInCover, false);
    add_condition(action, eWorldPropertyLookedOut, true);
    add_condition(action, eWorldPropertyPositionHolded, true);
    add_condition(action, eWorldPropertyEnemyDetoured, true);
    add_condition(action, eWorldPropertyPanic, false);
    add_condition(action, eWorldPropertyEnemyWounded, false);
    add_condition(action, eWorldPropertyPlayerOnThePath, false);
    add_condition(action, eWorldPropertyHasTask, false);
    add_effect(action, eWorldPropertyPureEnemy, false);
    add_operator(eWorldOperatorSearchEnemy, action);
    action->set_inertia_time(120000);

    action = xr_new<CStalkerActionKillEnemy>(m_object, "kill_if_not_visible");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, false);
    add_condition(action, eWorldPropertyUseSuddenness, false);
    add_condition(action, eWorldPropertyReadyToKill, true);
    //	add_condition			(action,eWorldPropertyReadyToDetour,	true);
    //	add_condition			(action,eWorldPropertyInCover,			false);
    //	add_condition			(action,eWorldPropertyEnemyDetoured,	false);
    add_condition(action, eWorldPropertySeeEnemy, true);
    add_condition(action, eWorldPropertyEnemySeeMe, false);
    //	add_condition			(action,eWorldPropertyLookedOut,		true);
    //	add_condition			(action,eWorldPropertyPositionHolded,	true);
    //	add_effect				(action,eWorldPropertyEnemyDetoured,	true);
    add_condition(action, eWorldPropertyPanic, false);
    add_condition(action, eWorldPropertyEnemyWounded, false);
    add_condition(action, eWorldPropertyHasTask, false);
    add_effect(action, eWorldPropertyPureEnemy, false);
    add_operator(eWorldOperatorKillEnemyIfNotVisible, action);

    action = xr_new<CStalkerActionKillEnemy>(m_object, "kill_if_critically_wounded");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, false);
    add_condition(action, eWorldPropertyUseSuddenness, false);
    add_condition(action, eWorldPropertyReadyToKill, true);
    //	add_condition			(action,eWorldPropertyReadyToDetour,	true);
    //	add_condition			(action,eWorldPropertyInCover,			false);
    //	add_condition			(action,eWorldPropertyEnemyDetoured,	false);
    add_condition(action, eWorldPropertySeeEnemy, true);
    add_condition(action, eWorldPropertyEnemyCriticallyWounded, true);
    //	add_condition			(action,eWorldPropertyLookedOut,		true);
    //	add_condition			(action,eWorldPropertyPositionHolded,	true);
    //	add_effect				(action,eWorldPropertyEnemyDetoured,	true);
    add_condition(action, eWorldPropertyPanic, false);
    add_condition(action, eWorldPropertyEnemyWounded, false);
    add_effect(action, eWorldPropertyPureEnemy, false);
    add_operator(eWorldOperatorKillEnemyIfCriticallyWounded, action);

    // ⭐ai_tactics_v2: the commander's task -- to its place, then the fight from it
    action = xr_new<CStalkerActionReachPlace>(m_object, "reach_place");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, false);
    add_condition(action, eWorldPropertyItemToKill, true);
    add_condition(action, eWorldPropertyItemCanKill, true);
    add_condition(action, eWorldPropertyReadyToKill, true);
    add_condition(action, eWorldPropertyEnemyWounded, false);
    add_condition(action, eWorldPropertyPlayerOnThePath, false);
    add_condition(action, eWorldPropertyHasTask, true);
    add_condition(action, eWorldPropertyAtPlace, false);
    add_effect(action, eWorldPropertyAtPlace, true);
    add_operator(eWorldOperatorReachPlace, action);

    action = xr_new<CStalkerActionFightFromPlace>(m_object, "fight_from_place");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, false);
    add_condition(action, eWorldPropertyItemToKill, true);
    add_condition(action, eWorldPropertyItemCanKill, true);
    add_condition(action, eWorldPropertyReadyToKill, true);
    add_condition(action, eWorldPropertyEnemyWounded, false);
    add_condition(action, eWorldPropertyPlayerOnThePath, false);
    add_condition(action, eWorldPropertyPanic, false);
    add_condition(action, eWorldPropertyHasTask, true);
    add_condition(action, eWorldPropertyAtPlace, true);
    add_effect(action, eWorldPropertyPureEnemy, false);
    add_operator(eWorldOperatorFightFromPlace, action);

    action = xr_new<CStalkerActionPostCombatWait>(m_object, "post_combat_wait");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, false);
    add_condition(action, eWorldPropertyPureEnemy, false);
    add_condition(action, eWorldPropertyEnemy, true);
    add_effect(action, eWorldPropertyEnemy, false);
    add_operator(eWorldOperatorPostCombatWait, action);

    action = xr_new<CStalkerActionHideFromGrenade>(m_object, "hide from grenade");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, true);
    add_effect(action, eWorldPropertyEnemy, false);

    add_effect(action, eWorldPropertyInCover, false);
    add_effect(action, eWorldPropertyLookedOut, false);
    add_effect(action, eWorldPropertyPositionHolded, false);
    add_effect(action, eWorldPropertyEnemyDetoured, false);

    add_operator(eWorldOperatorHideFromGrenade, action);

    action = xr_new<CStalkerActionSuddenAttack>(m_object, "sudden attack");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyDangerGrenade, false);
    add_condition(action, eWorldPropertyUseSuddenness, true);
    add_condition(action, eWorldPropertyEnemyWounded, false);
    add_condition(action, eWorldPropertyHasTask, false);
    add_effect(action, eWorldPropertyEnemy, false);
    add_operator(eWorldOperatorSuddenAttack, action);

    action = xr_new<CStalkerActionKillEnemyIfPlayerOnThePath>(m_object, "kill enemy, if player is on my path");
    add_condition(action, eWorldPropertyCriticallyWounded, false);
    add_condition(action, eWorldPropertyUseSuddenness, false);
    add_condition(action, eWorldPropertySeeEnemy, true);
    add_condition(action, eWorldPropertyPanic, false);
    add_condition(action, eWorldPropertyPlayerOnThePath, true);
    add_condition(action, eWorldPropertyEnemyWounded, false);
    add_effect(action, eWorldPropertyEnemy, false);

    add_effect(action, eWorldPropertyInCover, false);
    add_effect(action, eWorldPropertyLookedOut, false);
    add_effect(action, eWorldPropertyPositionHolded, false);
    add_effect(action, eWorldPropertyEnemyDetoured, false);

    add_operator(eWorldOperatorKillEnemyIfPlayerOnThePath, action);

    CStalkerKillWoundedPlanner* planner = xr_new<CStalkerKillWoundedPlanner>(m_object, "kill wounded enemy");
    add_condition(planner, eWorldPropertyCriticallyWounded, false);
    add_condition(planner, eWorldPropertyDangerGrenade, false);
    add_condition(planner, eWorldPropertyEnemyWounded, true);
    add_condition(planner, eWorldPropertyItemToKill, true);
    add_condition(planner, eWorldPropertyItemCanKill, true);
    add_condition(planner, eWorldPropertyEnemy, true);
    add_effect(planner, eWorldPropertyEnemy, false);
    add_operator(eWorldOperatorKillWoundedEnemy, planner);

    action = xr_new<CStalkerActionCriticalHit>(m_object, "critical hit");
    add_condition(action, eWorldPropertyCriticallyWounded, true);
    add_condition(action, eWorldPropertyPanic, false);
    add_effect(action, eWorldPropertyCriticallyWounded, false);
    add_operator(eWorldOperatorCriticallyWounded, action);
}

void CStalkerCombatPlanner::save(NET_Packet& packet) { inherited::save(packet); }

void CStalkerCombatPlanner::load(IReader& packet) { inherited::load(packet); }
