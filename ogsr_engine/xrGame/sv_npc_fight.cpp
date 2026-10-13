#include "StdAfx.h"
#include "xr_platform_seh.h"
#include "sv_npc_fight.h"

#include "Level.h"
#include "entity_alive.h"
#include "CustomMonster.h"
#include "memory_manager.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "Weapon.h"
#include "sv_npc.h"

#include "Actor.h" // a player's body is in the object list too, and has no brain
#include "ai/stalker/ai_stalker.h"
// ⭐The other kind of brain. A mutant runs a state machine, not a planner, so
// nothing in the stalker headers can read one.
#include "ai/monsters/basemonster/base_monster.h"
#include "ai/monsters/state_manager.h" // IStateManagerBase::get_state_type
#include "ai/monsters/state_defs.h" // make_xrstr, is_state, eStateAttack
#include "stalker_planner.h"
#include "stalker_combat_planner.h"
#include "stalker_decision_space.h"
#include "object_handler.h"
#include "object_handler_planner.h"
#include "object_handler_space.h"
#include "visual_memory_manager.h"
#include "sound_memory_manager.h"
#include "hit_memory_manager.h"
#include "enemy_manager.h"
#include "danger_manager.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "movement_manager.h"
// ⚠movement_manager.h only forward-declares MovementManager::EPathType, so the
// enumerators are unknown to anyone who does not ask for the space header too.
#include "movement_manager_space.h"
#include "stalker_movement_manager.h"
#include "detail_path_manager.h"
#include "sv_world_lookup.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "character_community.h"
#include "ai/ai_monsters_misc.h"
#include "ai_object_location.h"
#include "ai_space.h"
#include "level_graph.h"

namespace
{
CEntityAlive* alive_at(u16 id)
{
    if (!g_pGameLevel)
        return nullptr;
    return smart_cast<CEntityAlive*>(Level().Objects.net_Find(id));
}

// A creature with an empty pair of hands cannot start a firefight no matter how
// certain it is that somebody is its enemy. Spawned loot goes to the inventory,
// and drawing it is normally the brain's business.
void put_a_gun_in_its_hands(CEntityAlive* E)
{
    CInventoryOwner* io = smart_cast<CInventoryOwner*>(E);
    if (!io)
        return;

    if (smart_cast<CWeapon*>(io->inventory().ActiveItem()))
        return;

    const u32 slots[] = {FIRST_WEAPON_SLOT, SECOND_WEAPON_SLOT};
    for (u32 s : slots)
    {
        if (io->inventory().ItemFromSlot(s))
        {
            io->inventory().Activate(s);
            Msg("~ [sv_fight] %u drawing the weapon from slot %u", E->ID(), s);
            return;
        }
    }
    Msg("! [sv_fight] %u has no weapon in any slot -- it cannot shoot back", E->ID());
}

void introduce(CEntityAlive* who, CEntityAlive* enemy)
{
    CCustomMonster* m = smart_cast<CCustomMonster*>(who);
    if (!m)
    {
        Msg("! [sv_fight] %u is not a creature with a memory -- cannot be given an enemy", who->ID());
        return;
    }

    // The diagnosis, printed whatever happens next: if the content says these
    // two are not enemies, the brain will drop the target the moment it looks
    // at it, and no amount of visibility changes that.
    const bool hostile = who->is_relation_enemy(enemy);
    Msg("~ [sv_fight] %u -> %u: relation says %s", who->ID(), enemy->ID(), hostile ? "ENEMY" : "not an enemy (it will ignore it)");

    // ⚠⚠A MUTANT IS NOT A STALKER WITH FUR, and this call is where that stops
    // being a detail. CMemoryManager::make_object_visible_somewhen opens with
    // `stalker().agent_manager()` -- an unchecked downcast of the owning object
    // -- so pointing it at a monster dereferences null and takes the server
    // with it. Measured the hard way 30-07-2026: `sv_npc_attack 2050 0` on a
    // fracture crashed a live session with a player connected.
    //
    // ⭐The monster's own channel is its enemy manager, which is what its state
    // machine reads. Same principle as the stalker branch -- hand the target
    // over the way the engine does and let the brain do the rest -- just a
    // different engine.
    if (CBaseMonster* beast = smart_cast<CBaseMonster*>(who))
    {
        beast->EnemyMan.force_enemy(enemy);
        Msg("~ [sv_fight] %u is a mutant -- enemy forced through its own manager (no squad visibility to share)", who->ID());
        return;
    }

    put_a_gun_in_its_hands(who);
    m->memory().make_object_visible_somewhen(enemy);
}

// ⚠⚠`hostile_only` is not a refinement, it is the difference between working
// and not. Introducing a creature to its nearest NEIGHBOUR is right when there
// is exactly one other creature (which is what the two-stalker harness has) and
// wrong the moment they stand in formation: the nearest body is then a
// squadmate, is_relation_enemy says "not an enemy", and the introduction is
// silently discarded. Measured 28-07-2026, writing the five-a-side test.
CEntityAlive* nearest_other(CEntityAlive* A, bool hostile_only = false)
{
    CEntityAlive* best = nullptr;
    float best_d = 1e9f;
    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CObject* O = Level().Objects.o_get_by_iterator(i);
        if (!O || O == A || O->getDestroy())
            continue;
        CEntityAlive* E = smart_cast<CEntityAlive*>(O);
        if (!E || !E->g_Alive())
            continue;
        if (hostile_only && !A->is_relation_enemy(E))
            continue;
        const float d = E->Position().distance_to(A->Position());
        if (d < best_d)
        {
            best_d = d;
            best = E;
        }
    }
    return best;
}
} // namespace

void SVFight::Attack(u16 attacker, u16 target, bool mutual)
{
    CEntityAlive* A = alive_at(attacker);
    // ⚠Entity ids shift with whatever else spawned, so naming the opponent by
    // number in a script is a guess that costs a run when it is wrong (it did).
    // 0xffff means "whoever is closest", which is what a test actually wants.
    CEntityAlive* B = (target == 0xffff && A) ? nearest_other(A) : alive_at(target);

    if (!A || !B)
    {
        Msg("! [sv_fight] need two ONLINE creatures: %u is %s, %u is %s", attacker, A ? "there" : "missing", target, B ? "there" : "missing");
        return;
    }
    if (A == B)
    {
        Msg("! [sv_fight] %u cannot fight itself", attacker);
        return;
    }
    if (!A->g_Alive() || !B->g_Alive())
    {
        Msg("! [sv_fight] one of them is already dead (%u alive=%d, %u alive=%d)", attacker, A->g_Alive() ? 1 : 0, target, B->g_Alive() ? 1 : 0);
        return;
    }

    // B->ID(), not the argument: with "nearest" the argument is the 0xffff
    // placeholder, and printing that instead of who was actually found makes
    // the line lie about the very thing it exists to report.
    Msg("~ [sv_fight] %u '%s' vs %u '%s', %.1f m apart", attacker, A->cNameSect().c_str(), B->ID(), B->cNameSect().c_str(), A->Position().distance_to(B->Position()));

    introduce(A, B);
    if (mutual)
        introduce(B, A);
}

void SVFight::AttackAll()
{
    xr_vector<u16> ids;
    SVNpc::Ids(ids);
    if (ids.size() < 2)
    {
        Msg("! [sv_fight] %u tracked puppets -- need at least two to start a fight", u32(ids.size()));
        return;
    }

    Msg("~ [sv_fight] introducing %u puppets to their nearest neighbours", u32(ids.size()));
    for (u16 id : ids)
    {
        CEntityAlive* A = alive_at(id);
        if (!A || !A->g_Alive())
        {
            Msg("! [sv_fight] %u is not an online living creature yet", id);
            continue;
        }
        CEntityAlive* B = nearest_other(A, true); // the nearest ENEMY, not the nearest body
        if (!B)
        {
            Msg("! [sv_fight] %u has nobody hostile online -- nothing to introduce it to", id);
            continue;
        }
        // mutual=false: every puppet gets its own turn in this loop, so doing
        // both sides here would introduce each pair twice.
        Attack(id, u16(B->ID()), false);
    }
}

// =====================================================================
//  Why it is not shooting
// =====================================================================

namespace
{
using namespace StalkerDecisionSpace;

// The engine names these only in DEBUG builds (action_management_config.h ties
// LOG_ACTION to DEBUG), and the server that has the problem is a Release build.
// A bare "current action 12" is a number nobody can act on, so the names are
// repeated here rather than the diagnosis being run blind.
const char* property_name(u32 id)
{
    switch (id)
    {
    case eWorldPropertyAlive: return "alive";
    case eWorldPropertyDead: return "dead";
    case eWorldPropertyAlreadyDead: return "already_dead";
    case eWorldPropertyALife: return "alife";
    case eWorldPropertyPuzzleSolved: return "puzzle_solved";
    case eWorldPropertySmartTerrainTask: return "smart_terrain_task";
    case eWorldPropertyItems: return "items";
    case eWorldPropertyEnemy: return "ENEMY";
    case eWorldPropertyDanger: return "danger";
    case eWorldPropertyItemToKill: return "item_to_kill";
    case eWorldPropertyFoundItemToKill: return "found_item_to_kill";
    case eWorldPropertyItemCanKill: return "item_can_kill";
    case eWorldPropertyFoundAmmo: return "found_ammo";
    case eWorldPropertyReadyToKill: return "ready_to_kill";
    case eWorldPropertyReadyToDetour: return "ready_to_detour";
    case eWorldPropertySeeEnemy: return "see_enemy";
    case eWorldPropertyEnemySeeMe: return "enemy_see_me";
    case eWorldPropertyPanic: return "panic";
    case eWorldPropertyInCover: return "in_cover";
    case eWorldPropertyLookedOut: return "looked_out";
    case eWorldPropertyPositionHolded: return "position_holded";
    case eWorldPropertyEnemyDetoured: return "enemy_detoured";
    case eWorldPropertyUseSuddenness: return "use_suddenness";
    case eWorldPropertyPureEnemy: return "pure_enemy";
    case eWorldPropertyUseCrouchToLookOut: return "crouch_to_look_out";
    case eWorldPropertyEnemyWounded: return "enemy_wounded";
    case eWorldPropertyWoundedEnemyReached: return "wounded_enemy_reached";
    case eWorldPropertyWoundedEnemyPrepared: return "wounded_enemy_prepared";
    case eWorldPropertyPlayerOnThePath: return "player_on_the_path";
    case eWorldPropertyCriticallyWounded: return "critically_wounded";
    case eWorldPropertyEnemyCriticallyWounded: return "enemy_critically_wounded";
    case eWorldPropertyWoundedEnemyAimed: return "wounded_enemy_aimed";
    case eWorldPropertyPausedAfterKill: return "paused_after_kill";
    case eWorldPropertyKilledWounded: return "killed_wounded";
    case eWorldPropertyDangerUnknown: return "danger_unknown";
    case eWorldPropertyDangerInDirection: return "danger_in_direction";
    case eWorldPropertyDangerGrenade: return "danger_grenade";
    case eWorldPropertyDangerBySound: return "danger_by_sound";
    case eWorldPropertyCoverActual: return "cover_actual";
    case eWorldPropertyCoverReached: return "cover_reached";
    case eWorldPropertyLookedAround: return "looked_around";
    case eWorldPropertyGrenadeExploded: return "grenade_exploded";
    case eWorldPropertyAnomaly: return "anomaly";
    case eWorldPropertyInsideAnomaly: return "inside_anomaly";
    case eWorldPropertyScript: return "script";
    case eWorldPropertyHasTask: return "has_task";
    case eWorldPropertyAtPlace: return "at_place";
    default: return nullptr; // a script-added property: it prints as a number
    }
}

const char* brain_action_name(u32 id)
{
    switch (id)
    {
    case eWorldOperatorDeathPlanner: return "death_planner";
    case eWorldOperatorALifePlanner: return "alife_planner";
    case eWorldOperatorCombatPlanner: return "COMBAT_planner";
    case eWorldOperatorAnomalyPlanner: return "anomaly_planner";
    case eWorldOperatorDangerPlanner: return "danger_planner";
    case eWorldOperatorGatherItems: return "gather_items";
    case eWorldOperatorScript: return "script";
    case u32(-1): return "<none>";
    default: return nullptr;
    }
}

const char* combat_action_name(u32 id)
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
    case eWorldOperatorKillEnemy: return "KILL_ENEMY";
    case eWorldOperatorRetreatFromEnemy: return "retreat_from_enemy";
    case eWorldOperatorTakeCover: return "take_cover";
    case eWorldOperatorLookOut: return "look_out";
    case eWorldOperatorHoldPosition: return "hold_position";
    case eWorldOperatorGetDistance: return "get_distance";
    case eWorldOperatorDetourEnemy: return "detour_enemy";
    case eWorldOperatorSearchEnemy: return "search_enemy";
    case eWorldOperatorHideFromGrenade: return "hide_from_grenade";
    case eWorldOperatorSuddenAttack: return "sudden_attack";
    case eWorldOperatorKillEnemyIfNotVisible: return "kill_enemy_if_not_visible";
    case eWorldOperatorPostCombatWait: return "post_combat_wait";
    case eWorldOperatorKillEnemyIfPlayerOnThePath: return "kill_enemy_if_player_on_path";
    case eWorldOperatorCriticallyWounded: return "critically_wounded";
    case eWorldOperatorKillEnemyIfCriticallyWounded: return "kill_enemy_if_crit_wounded";
    case eWorldOperatorPauseAfterKill: return "pause_after_kill";
    case eWorldOperatorReachPlace: return "reach_place";
    case eWorldOperatorFightFromPlace: return "fight_from_place";
    case u32(-1): return "<none>";
    default: return nullptr;
    }
}

const char* weapon_state_name(u32 state)
{
    switch (state)
    {
    case ObjectHandlerSpace::eWorldOperatorShow: return "Show";
    case ObjectHandlerSpace::eWorldOperatorHide: return "Hide";
    case ObjectHandlerSpace::eWorldOperatorDrop: return "Drop";
    case ObjectHandlerSpace::eWorldOperatorStrapping: return "Strapping";
    case ObjectHandlerSpace::eWorldOperatorStrapping2Idle: return "Strapping2Idle";
    case ObjectHandlerSpace::eWorldOperatorUnstrapping: return "Unstrapping";
    case ObjectHandlerSpace::eWorldOperatorUnstrapping2Idle: return "Unstrapping2Idle";
    case ObjectHandlerSpace::eWorldOperatorStrapped: return "StrappedIdle";
    case ObjectHandlerSpace::eWorldOperatorIdle: return "Idle";
    case ObjectHandlerSpace::eWorldOperatorAim1: return "Aim1";
    case ObjectHandlerSpace::eWorldOperatorAim2: return "Aim2";
    case ObjectHandlerSpace::eWorldOperatorReload1: return "Reload1";
    case ObjectHandlerSpace::eWorldOperatorReload2: return "Reload2";
    case ObjectHandlerSpace::eWorldOperatorForceReload1: return "ForceReload1";
    case ObjectHandlerSpace::eWorldOperatorForceReload2: return "ForceReload2";
    case ObjectHandlerSpace::eWorldOperatorFire1: return "FIRE1";
    case ObjectHandlerSpace::eWorldOperatorFire2: return "FIRE2";
    case ObjectHandlerSpace::eWorldOperatorAimingReady1: return "AimingReady1";
    case ObjectHandlerSpace::eWorldOperatorAimingReady2: return "AimingReady2";
    case ObjectHandlerSpace::eWorldOperatorSwitch1: return "Switch1";
    case ObjectHandlerSpace::eWorldOperatorSwitch2: return "Switch2";
    case ObjectHandlerSpace::eWorldOperatorQueueWait1: return "QueueWait1";
    case ObjectHandlerSpace::eWorldOperatorQueueWait2: return "QueueWait2";
    case ObjectHandlerSpace::eWorldOperatorGetAmmo1: return "GetAmmo1";
    case ObjectHandlerSpace::eWorldOperatorGetAmmo2: return "GetAmmo2";
    default: return nullptr;
    }
}

CStalkerCombatPlanner* combat_planner_of(CAI_Stalker* S)
{
    // Deliberately NOT brain().action(id): that goes through get_operator,
    // whose lower_bound is dereferenced whether or not the id is there. If
    // something removed the combat planner -- which is one of the things being
    // investigated -- asking for it must report its absence, not read past the
    // end of a vector.
    for (const auto& op : S->brain().operators())
        if (op.m_operator_id == u32(eWorldOperatorCombatPlanner))
            return dynamic_cast<CStalkerCombatPlanner*>(op.m_operator);
    return nullptr;
}

const char* oh_property_name(u32 id)
{
    using namespace ObjectHandlerSpace;
    switch (id)
    {
    case eWorldPropertyItemID: return "item_id";
    case eWorldPropertyHidden: return "hidden";
    case eWorldPropertyStrapped: return "strapped";
    case eWorldPropertyStrapped2Idle: return "strapped2idle";
    case eWorldPropertySwitch1: return "switch1";
    case eWorldPropertySwitch2: return "switch2";
    case eWorldPropertyAimed1: return "aimed1";
    case eWorldPropertyAimed2: return "aimed2";
    case eWorldPropertyAiming1: return "aiming1";
    case eWorldPropertyAiming2: return "aiming2";
    case eWorldPropertyEmpty1: return "empty1";
    case eWorldPropertyEmpty2: return "empty2";
    case eWorldPropertyFull1: return "full1";
    case eWorldPropertyFull2: return "full2";
    case eWorldPropertyReady1: return "ready1";
    case eWorldPropertyReady2: return "ready2";
    case eWorldPropertyFiring1: return "FIRING1";
    case eWorldPropertyFiring2: return "FIRING2";
    case eWorldPropertyAmmo1: return "ammo1";
    case eWorldPropertyAmmo2: return "ammo2";
    case eWorldPropertyIdle: return "idle";
    case eWorldPropertyIdleStrap: return "idle_strap";
    case eWorldPropertyDropped: return "dropped";
    case eWorldPropertyQueueWait1: return "queue_wait1";
    case eWorldPropertyQueueWait2: return "queue_wait2";
    case eWorldPropertyAimingReady1: return "aiming_ready1";
    case eWorldPropertyAimingReady2: return "aiming_ready2";
    case eWorldPropertyAimForceFull1: return "aim_force_full1";
    case eWorldPropertyAimForceFull2: return "aim_force_full2";
    default: return nullptr;
    }
}

// A creature that has no mental state at all, as opposed to one whose state
// could not be read. Only stalkers keep one; the mutant population is steered
// by a state machine instead, and printing "?" for all of them would read as a
// failure to look.
constexpr u32 k_no_mental = 0xffffffff;

const char* mental_state_name(u32 s)
{
    switch (s)
    {
    case k_no_mental: return "-";
    case MonsterSpace::eMentalStateDanger: return "danger";
    case MonsterSpace::eMentalStateFree: return "free";
    case MonsterSpace::eMentalStatePanic: return "panic";
    default: return "?";
    }
}

// Both planners answer evaluators() with the same shape, and the interesting
// question -- which precondition is false right now -- is the same for both, so
// one loop serves the brain and the combat planner alike.
template <class TPlanner>
void dump_properties(TPlanner& planner, char* out, size_t out_size)
{
    out[0] = 0;
    for (const auto& e : planner.evaluators())
    {
        const char* pn = property_name(e.first);
        if (!pn)
            continue; // script-added ones are not what this is diagnosing
        string64 one;
        xr_sprintf(one, "%s=%d ", pn, e.second->evaluate() ? 1 : 0);
        if (xr_strlen(out) + xr_strlen(one) < out_size - 1)
            xr_strcat(out, out_size, one);
    }
}

// The nearest living creature this one is at war with. That is the shot that
// should be happening, so it is the one every line below is about.
//
// ⚠monster_community is what decides hostility and it is a raw pointer filled
// in during net_Spawn: a creature caught mid-spawn has none, and asking such a
// one whether it is an enemy reads through null. On a whole-map-online host
// this loop meets every creature on the level, so that is a certainty, not a
// corner case.
CEntityAlive* nearest_hostile(CEntityAlive* A)
{
    if (!A->monster_community)
        return nullptr;

    CEntityAlive* best = nullptr;
    float best_d = 250.f; // no shot is being missed beyond this, and it keeps the sweep cheap
    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CObject* O = Level().Objects.o_get_by_iterator(i);
        if (!O || O == A || O->getDestroy())
            continue;
        CEntityAlive* E = smart_cast<CEntityAlive*>(O);
        if (!E || !E->g_Alive() || !E->monster_community)
            continue;
        const float d = E->Position().distance_to(A->Position());
        if (d >= best_d)
            continue;
        if (!A->is_relation_enemy(E))
            continue;
        best_d = d;
        best = E;
    }
    return best;
}

void explain_one(CAI_Stalker* S)
{
    const u16 id = u16(S->ID());
    CEntityAlive* T = nearest_hostile(S);

    // --- 1. is it even being thought about ---------------------------
    Msg("~ [why] --- %u '%s' team=%u alive=%d script_control=%d remote=%d ---", id, S->cNameSect().c_str(), u32(S->g_Team()), S->g_Alive() ? 1 : 0,
        S->GetScriptControl() ? 1 : 0, S->Remote() ? 1 : 0);

    // ⚠A corpse still answers most of these calls, and one of them faulted --
    // the death planner tears the combat one down while the memory and weapon
    // managers are half-dismantled. There is nothing to diagnose about a
    // creature that has already lost the fight, so it is not asked.
    if (!S->g_Alive())
    {
        Msg("~ [why] %u is dead -- the rest of the chain no longer applies", id);
        return;
    }

    // ⚠⚠The target-independent half runs FIRST, and unconditionally.
    //
    // It used to sit below `if (!T) return`, so a creature with nobody hostile
    // online printed one line and stopped -- and that is exactly the state in
    // which the interesting question about its hands is asked. "Why is this man
    // still holding a rifle in a quiet field" was not answerable by the
    // instrument built to answer "why is this man not shooting", because the
    // second question begins by demanding somebody to shoot at.
    // ⭐The order in the log changed with it: identity, brain, hands, stance,
    // and only then the parts that need an enemy.

    // --- 4. the brain -------------------------------------------------
    CStalkerPlanner& brain = S->brain();
    const u32 brain_action = brain.initialized() ? brain.current_action_id() : u32(-1);
    const char* bname = brain_action_name(brain_action);

    string512 props;
    dump_properties(brain, props, sizeof(props));
    Msg("~ [why] %u brain: init=%d action=%s(%u) plan=%u steps | %s", id, brain.initialized() ? 1 : 0, bname ? bname : "<script/unknown>", brain_action,
        u32(brain.solution().size()), props);

    // --- 5. the combat planner ---------------------------------------
    //
    // ⭐Its properties are the point. "combat planner is running" says nothing
    // about why nobody fires; "ready_to_kill=0 see_enemy=1" names the link.
    CStalkerCombatPlanner* combat = combat_planner_of(S);
    if (!combat)
        Msg("! [why] %u has NO combat planner among its %u brain operators", id, u32(brain.operators().size()));
    else
    {
        const u32 ca = combat->initialized() ? combat->current_action_id() : u32(-1);
        const char* cn = combat_action_name(ca);
        Msg("~ [why] %u combat: init=%d action=%s(%u) plan=%u steps evaluators=%u", id, combat->initialized() ? 1 : 0, cn ? cn : "<script/unknown>", ca,
            u32(combat->solution().size()), u32(combat->evaluators().size()));

        if (brain_action == u32(eWorldOperatorCombatPlanner))
        {
            dump_properties(*combat, props, sizeof(props));
            Msg("~ [why] %u combat props: %s", id, props);
        }
    }

    // --- 6. the weapon and its state machine --------------------------
    CInventoryItem* active = S->inventory().ActiveItem();
    CWeapon* weapon = smart_cast<CWeapon*>(active);
    CObjectHandlerPlanner& handler = S->CObjectHandler::planner();
    const u32 oh_state = handler.initialized() ? handler.current_action_state_id() : u32(-1);
    const char* on = weapon_state_name(oh_state);

    Msg("~ [why] %u weapon: active=%s ammo=%d state=%d strapped=%d | handler=%s(%u) plan=%u steps", id, active ? active->object().cNameSect().c_str() : "<none>",
        weapon ? weapon->GetAmmoElapsed() : -1, weapon ? int(weapon->GetState()) : -1, S->weapon_strapped() ? 1 : 0, on ? on : "<unknown>", oh_state,
        u32(handler.solution().size()));

    // ⭐The three separate reasons a weapon can fail to end up on a back, told
    // apart. They fail in different places and only one of them is code:
    //   can_be_strapped=0  the CONTENT gave this weapon no strap bones or no
    //                      strap offset, so nothing can ever put it away;
    //   goap_strapped=0    the AI never asked for it;
    //   mode=0 with goap=1 it was asked for and the hands never got there --
    //                      every state in that machine is left by an animation
    //                      callback, and this host has no animations.
    if (weapon)
        Msg("~ [why] %u strap: can_be_strapped=%d goap_strapped=%d mode=%d bones='%s'/'%s'", id, weapon->can_be_strapped() ? 1 : 0,
            handler.m_storage.property(ObjectHandlerSpace::eWorldPropertyStrapped) ? 1 : 0, weapon->strapped_mode() ? 1 : 0,
            weapon->strap_bone0() ? weapon->strap_bone0() : "-", weapon->strap_bone1() ? weapon->strap_bone1() : "-");

    // ⭐What the brain ORDERED the hands to do, as opposed to what they are
    // doing. These two lines separate "combat never asked for a shot" from
    // "combat asked and the hands never got there" -- and on a host whose
    // creatures have no skeleton, the second is the likelier of the two,
    // because every state in that machine is left by an animation callback.
    string512 goal;
    goal[0] = 0;
    for (const auto& c : handler.target_state().conditions())
    {
        const char* pn = oh_property_name(c.condition() & 0xffff);
        string64 one;
        if (pn)
            xr_sprintf(one, "%s(%u)=%d ", pn, c.condition() >> 16, c.value() ? 1 : 0);
        else
            xr_sprintf(one, "p%u(%u)=%d ", c.condition() & 0xffff, c.condition() >> 16, c.value() ? 1 : 0);
        if (xr_strlen(goal) + xr_strlen(one) < sizeof(goal) - 1)
            xr_strcat(goal, one);
    }
    Msg("~ [why] %u hands want: %s", id, goal[0] ? goal : "<nothing>");

    // --- 6b. the stance the movement manager is holding ----------------
    //
    // A stalker in a free mental state carries its weapon strapped, and the
    // first thing combat does is change that. If this says free while the
    // combat planner is running, the two disagree and that is the bug.
    Msg("~ [why] %u stance: mental=%s(%u) body=%u movement=%u | path_completed=%d", id, mental_state_name(u32(S->movement().mental_state())), u32(S->movement().mental_state()),
        u32(S->movement().body_state()), u32(S->movement().movement_type()), S->movement().path_completed() ? 1 : 0);

    if (!T)
    {
        Msg("~ [why] %u has nobody hostile online -- eyes, memory and the fire gate need a target and are skipped", id);
        return;
    }

    const float dist = S->Position().distance_to(T->Position());

    // --- 2. eyes: does the raw vision system have it at all ----------
    //
    // feel_visible is the layer BELOW memory: the frustum query plus ray
    // tests, before anything decides what the sighting means. If the target is
    // missing here, nothing further up can possibly work, and the fault is in
    // sight range/FOV or the ray test, not in the AI.
    float fuzzy = -2.f;
    bool in_feel = false;
    for (const auto& item : S->feel_visible)
        if (item.O == T)
        {
            in_feel = true;
            fuzzy = item.fuzzy;
            break;
        }

    float range = 0.f, fov = 0.f;
    S->update_range_fov(range, fov, S->memory().visual().current_state().m_max_view_distance * S->eye_range, S->eye_fov);

    Msg("~ [why] %u eyes: target %u at %.1f m | in_feel_vision=%d fuzzy=%+.2f | eye=(%.1f,%.1f,%.1f) range=%.1f fov=%.1f", id, u16(T->ID()), dist, in_feel ? 1 : 0, fuzzy,
        S->eye_matrix.c.x, S->eye_matrix.c.y, S->eye_matrix.c.z, range, fov);

    // --- 3. memory and enemy selection -------------------------------
    const bool useful_spatial = (T->spatial.type & STYPE_VISIBLEFORAI) == STYPE_VISIBLEFORAI;
    const bool useful_vertex = ai().get_level_graph() && ai().level_graph().valid_vertex_id(T->ai_location().level_vertex_id());
    const CEntityAlive* selected = S->memory().enemy().selected();

    Msg("~ [why] %u memory: visual=%u sound=%u hit=%u enemies=%u danger=%u | vis_enabled=%d visible_now=%d right_now=%d", id, u32(S->memory().visual().objects().size()),
        u32(S->memory().sound().objects().size()), u32(S->memory().hit().objects().size()), u32(S->memory().enemy().objects().size()),
        u32(S->memory().danger().objects().size()), S->memory().visual().enabled() ? 1 : 0, S->memory().visual().visible_now(T) ? 1 : 0,
        S->memory().visual().visible_right_now(T) ? 1 : 0);

    Msg("~ [why] %u useful(%u): relation_enemy=%d spatial_visible_for_ai=%d level_vertex_ok=%d (%u) => selected=%s", id, u16(T->ID()), S->is_relation_enemy(T) ? 1 : 0,
        useful_spatial ? 1 : 0, useful_vertex ? 1 : 0, T->ai_location().level_vertex_id(), selected ? "YES" : "no");

    // --- 7. the gate that decides a shot is worth taking --------------
    //
    // ⭐fire_make_sense is where a stalker that has done everything right can
    // still stay silent, and every one of its clauses is a plausible way for a
    // headless host to differ from a game with a screen: pick_distance comes
    // out of a ray query from the weapon's muzzle bone, and bones are exactly
    // what this server does not have.
    Msg("~ [why] %u fire gate: pick_distance=%.1f (need >%.1f and >= %.1f to target) dy=%.2f | can_kill_enemy=%d ready_to_kill=%d fire_make_sense=%d", id, S->pick_distance(),
        1.f, dist, _abs(S->Position().y - T->Position().y), S->can_kill_enemy() ? 1 : 0, S->ready_to_kill() ? 1 : 0, S->fire_make_sense() ? 1 : 0);
}

void explain_strap_flip(CObject* O, bool strapped)
{
    CAI_Stalker* S = smart_cast<CAI_Stalker*>(O);
    if (!S || !S->g_Alive())
        return;

    const u16 id = u16(S->ID());
    CStalkerPlanner& brain = S->brain();
    const u32 ba = brain.initialized() ? brain.current_action_id() : u32(-1);
    const char* bn = brain_action_name(ba);

    CObjectHandlerPlanner& handler = S->CObjectHandler::planner();
    const u32 oh = handler.initialized() ? handler.current_action_state_id() : u32(-1);
    const char* on = weapon_state_name(oh);

    // What the hands were ASKED for, which is the line that names the culprit:
    // the object handler only ever does what somebody set as its goal.
    string512 goal;
    goal[0] = 0;
    if (handler.initialized())
        for (const auto& c : handler.target_state().conditions())
        {
            const char* pn = oh_property_name(c.condition() & 0xffff);
            string64 one;
            if (pn)
                xr_sprintf(one, "%s=%d ", pn, c.value() ? 1 : 0);
            else
                xr_sprintf(one, "p%u=%d ", c.condition() & 0xffff, c.value() ? 1 : 0);
            if (xr_strlen(goal) + xr_strlen(one) < sizeof(goal) - 1)
                xr_strcat(goal, one);
        }

    const CEntityAlive* enemy = S->memory().enemy().selected();

    Msg("~ [strap] server: %u '%s' -> %s | brain=%s(%u) hands=%s(%u) want:[%s] mental=%s enemy=%s danger=%u", id, S->cName().c_str(), strapped ? "ON THE BACK" : "in hands",
        bn ? bn : "<script/unknown>", ba, on ? on : "<unknown>", oh, goal[0] ? goal : "-", mental_state_name(u32(S->movement().mental_state())),
        enemy ? enemy->cName().c_str() : "-", u32(S->memory().danger().objects().size()));
}

// ⚠The 20:04 run died here, silently, one second after the watch was armed and
// before a single line was printed -- so the previous attempt at this diagnosis
// cost a whole run and returned nothing. A tool that reads a dozen subsystems
// of a creature mid-frame WILL eventually touch one that is half-built; it must
// not take the server with it, and it must say where it fell over.
//
// Same mechanism the engine's own planner uses for exactly this reason
// (CPlanner::update), so the failure arrives as a stack trace rather than as a
// missing log tail.
void explain_one_guarded(CAI_Stalker* S)
{
    XR_SEH_TRY
    {
        explain_one(S);
    }
    XR_SEH_EXCEPT(ExceptStackTrace("[why] stack_trace:\n"))
    {
        Msg("!! [why] the diagnostic itself faulted on %u -- see the stack above", u16(S->ID()));
    }
}

// Watch state. Deliberately not a console-settable cvar: an instrument that
// survives into user.ltx has already cost this project a bug hunt.
u32 s_watch_until = 0;
u32 s_watch_next = 0;
u32 s_mind_until = 0;
u32 s_mind_next = 0;
u32 s_mind_period = 1000;

// ---------------------------------------------------------------------
//  The crowd census. See SVFight::Census.
// ---------------------------------------------------------------------

const char* path_type_name(u32 t)
{
    switch (t)
    {
    case MovementManager::ePathTypeGamePath: return "game";
    case MovementManager::ePathTypeLevelPath: return "level";
    case MovementManager::ePathTypePatrolPath: return "patrol";
    case MovementManager::ePathTypeNoPath: return "none";
    default: return "?";
    }
}

// A tally over short labels. It owns its strings because the interesting ones
// are not literals: ⚠every action the MOD registers from Lua lands outside the
// engine's enumeration and would otherwise collapse into a single
// "<script/unknown>" bucket -- which measured 36 of 49 living creatures on
// 28-07 and hid the fact that they were all in the same script action.
struct Tally
{
    struct Row
    {
        string64 label;
        u32 n;
    };
    xr_vector<Row> rows;

    void add(const char* label)
    {
        for (Row& r : rows)
            if (!xr_strcmp(r.label, label))
            {
                ++r.n;
                return;
            }
        rows.push_back(Row());
        xr_strcpy(rows.back().label, label);
        rows.back().n = 1;
    }

    void print(const char* what)
    {
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.n > b.n; });
        string1024 line;
        line[0] = 0;
        for (const Row& r : rows)
        {
            string128 one;
            xr_sprintf(one, "%ux %s   ", r.n, r.label);
            if (xr_strlen(line) + xr_strlen(one) < sizeof(line) - 1)
                xr_strcat(line, one);
        }
        Msg("~ [mind]   %-9s %s", what, line[0] ? line : "-");
    }
};

struct MindSample
{
    u16 id;
    const char* faction;
    bool alive;
    float hp;
    u32 brain;
    u32 mental;
    u32 path;
    bool has_dest;
    Fvector dest;
    float to_go;
    bool enemy;
    u16 enemy_id;
    float enemy_dist;
    bool arrived;
    // ⭐The three that separate "it never wanted to shoot" from "it wanted to
    // and could not". Measured 28-07-2026: five creatures held an enemy for a
    // minute and landed nothing, and the census as it stood could not say which.
    string64 weapon;
    int ammo;
    bool ready_to_kill;
    bool fire_makes_sense;
    // ⭐⭐⭐"COMBAT_planner" is not an answer, it is a folder. Inside it are
    // kill_enemy, take_cover, get_distance, look_out, search_enemy -- and only
    // one of them pulls a trigger. Measured 28-07-2026: NINE of ten creatures
    // sat in the combat planner for six minutes with an enemy, ready_to_kill=1
    // and a full magazine, while one did all the shooting. The top-level action
    // was identical for all ten and said nothing.
    u32 combat_action;
    u32 hands_state; // what the weapon state machine is doing, if anything
    // ⭐⭐Which way it is LOOKING, in degrees, and how far off that is from the
    // enemy it has. Measured 28-07-2026 and it invalidated three days of
    // skirmish results: nothing in the spawn path ever sets a creature's angle,
    // so both sides of a two-anchor test face the same absolute direction --
    // one at the enemy, the other at the horizon. "One side never noticed the
    // other" was not an AI finding, it was a line of creatures with their backs
    // turned, and no report printed the one number that would have said so.
    float yaw_deg;
    float bearing_err_deg; // -1 when it has no enemy to be turned away from
    // ⭐⭐MUTANTS DO NOT HAVE A GOAP PLANNER. Everything above was written for
    // stalkers, and with the leash off the level's population is mostly not
    // stalkers -- a census that can only read one of the two kinds of brain
    // reports an empty world and calls it idle. A monster is steered by a state
    // machine (CMonsterStateManager), so its "what is it doing" is a state name
    // and not an action id, and it has no mental state or weapon at all.
    bool monster;
    string64 state; // the monster's state name; empty for a stalker
    bool fighting; // in the combat branch, whichever kind of brain it has
};

// The name of whatever is running, engine operator or a mod's script action.
void brain_label(u32 id, char* out, size_t out_size)
{
    const char* n = brain_action_name(id);
    if (n)
        xr_strcpy(out, out_size, n);
    else
        xr_sprintf(out, out_size, "script#%u", id);
}

// One token for what a creature is doing, whichever kind of brain it has.
void what_label(const MindSample& s, char* out, size_t out_size)
{
    if (s.monster)
        xr_strcpy(out, out_size, s.state[0] ? s.state : "-");
    else
        brain_label(s.brain, out, out_size);
}

// ⭐⭐With ten creatures the histogram is the wrong shape: at that size the
// question stops being "what are they all doing" and becomes "what is EACH of
// them doing", and a squad of five reads as five sentences. Above that it is
// noise, which is why it is not printed for a hundred.
void print_each(const xr_vector<MindSample>& g)
{
    for (const MindSample& s : g)
    {
        string64 what;
        what_label(s, what, sizeof(what));

        string128 target;
        if (!s.enemy)
            xr_strcpy(target, "no enemy");
        else
            xr_sprintf(target, "enemy %u at %.0f m", s.enemy_id, s.enemy_dist);

        string128 where;
        if (s.arrived)
            xr_strcpy(where, "arrived");
        else if (s.has_dest)
            xr_sprintf(where, "-> (%.0f, %.0f) %.0f m", s.dest.x, s.dest.z, s.to_go);
        else
            xr_strcpy(where, "nowhere");

        const char* cn = combat_action_name(s.combat_action);
        const char* hn = weapon_state_name(s.hands_state);

        // ⚠One token, no padding INSIDE it. Printed as "%-14s/%-14s" the pair
        // came out as "death_planner /<none>" -- a space in the middle of what
        // reads as one field, which silently broke the harness that parses this
        // line and made it report "0 ever entered the combat planner" for a run
        // where both sides had. A field a parser cannot take is a field that
        // will be misread. 28-07-2026.
        string64 plan;
        xr_sprintf(plan, "%s/%s", what, cn ? cn : "-");

        string64 facing;
        if (s.bearing_err_deg >= 0.f)
            xr_sprintf(facing, "face=%4.0f off=%3.0f", s.yaw_deg, s.bearing_err_deg);
        else
            xr_sprintf(facing, "face=%4.0f", s.yaw_deg);

        Msg("~ [mind]   %5u %-8s hp=%5.2f %-30s %-7s | %-19s | %-10s ammo=%-3d rtk=%d fire=%d hands=%s | %-18s | %s", s.id, s.faction, s.hp, plan, mental_state_name(s.mental),
            target, s.weapon, s.ammo, s.ready_to_kill ? 1 : 0, s.fire_makes_sense ? 1 : 0, hn ? hn : "-", facing, where);
    }
}

// ⭐Where they are STEERING, not where they are. Fifty creatures whose
// destinations collapse into one cluster are following one decision; fifty
// scattered ones are each following their own, and those are different bugs.
struct Cluster
{
    Fvector centre;
    u32 n;
    float sum_to_go;
};

void print_destinations(const xr_vector<MindSample>& g)
{
    xr_vector<Cluster> clusters;
    u32 arrived = 0, no_dest = 0;

    for (const MindSample& s : g)
    {
        if (s.arrived)
        {
            ++arrived;
            continue;
        }
        if (!s.has_dest)
        {
            ++no_dest;
            continue;
        }
        bool placed = false;
        for (Cluster& c : clusters)
            if (c.centre.distance_to(s.dest) < 10.f) // one cluster is a place, not a point
            {
                ++c.n;
                c.sum_to_go += s.to_go;
                placed = true;
                break;
            }
        if (!placed)
            clusters.push_back({s.dest, 1, s.to_go});
    }

    std::sort(clusters.begin(), clusters.end(), [](const Cluster& a, const Cluster& b) { return a.n > b.n; });

    if (clusters.empty())
        Msg("~ [mind]   going to  nowhere -- %u arrived, %u never had a destination", arrived, no_dest);
    else
    {
        for (const Cluster& c : clusters)
            Msg("~ [mind]   going to  %2u -> (%.0f, %.0f, %.0f), %.0f m to go", c.n, VPUSH(c.centre), c.sum_to_go / float(c.n));
        if (arrived || no_dest)
            Msg("~ [mind]   idle      %u arrived, %u never had a destination", arrived, no_dest);
    }
}

void print_group(const char* faction, const xr_vector<MindSample>& g)
{
    if (g.empty())
        return;

    u32 dead = 0, with_enemy = 0, in_combat = 0, armed_but_not_fighting = 0;
    Tally brain, mental, path, inside_combat;
    for (const MindSample& s : g)
    {
        if (!s.alive)
            ++dead;
        if (s.enemy)
            ++with_enemy;
        if (s.fighting)
        {
            ++in_combat;
            // Only meaningful for those actually inside it. A monster's combat
            // branch has no sub-planner to name, so its own state stands in.
            string64 named;
            if (s.monster)
                xr_strcpy(named, s.state[0] ? s.state : "attack");
            else if (const char* cn = combat_action_name(s.combat_action))
                xr_strcpy(named, cn);
            else
                xr_sprintf(named, "combat#%u", s.combat_action);
            inside_combat.add(named);
        }
        else if (s.enemy && s.alive)
            ++armed_but_not_fighting;

        // ⚠Script actions are NOT one bucket. A mod's Lua registers its scheme
        // actions into this same planner with ids of its own, and lumping them
        // together turns "all of them are stuck in one script action" -- which
        // is a diagnosis -- into "unknown", which is not.
        string64 named;
        what_label(s, named, sizeof(named));
        brain.add(named);
        mental.add(mental_state_name(s.mental));
        path.add(path_type_name(s.path));
    }

    Msg("~ [mind] --- %s: %u (%u dead) ---", faction, u32(g.size()), dead);
    brain.print("brain");
    // ⭐The line that says whether anyone is shooting. Everything else in this
    // block can look like a firefight while every one of them is taking cover.
    inside_combat.print("in combat");
    mental.print("mental");
    path.print("path");

    // ⭐⭐The line that names the disagreement. A creature that has picked an
    // enemy and is NOT running the combat planner has been outbid for its own
    // brain by something else -- and on this server that something else is the
    // mod's script layer, whose smart terrain lookup fails for every creature.
    // "36 have an enemy, 4 are fighting" is a bug report; two separate
    // histograms saying the same thing are not.
    Msg("~ [mind]   enemy     %u of %u have one selected | %u in the combat planner | %u have an enemy and are NOT fighting", with_enemy, u32(g.size()), in_combat,
        armed_but_not_fighting);
    print_destinations(g);

    // Small enough to read one line each.
    if (g.size() <= 8)
        print_each(g);
}

// Everything the census can read off a mutant. A monster's brain is a state
// machine, not a planner, so the two halves of the sample barely overlap: no
// mental state, no weapon, no combat sub-action, and "what is it doing" is a
// state name.
void sample_monster(CBaseMonster* M, MindSample& s)
{
    s.monster = true;
    s.alive = !!M->g_Alive();
    s.hp = M->GetfHealth();
    s.brain = u32(-1);
    s.mental = k_no_mental;
    s.combat_action = u32(-1);
    s.hands_state = u32(-1);
    xr_strcpy(s.weapon, "<claws>");
    s.ammo = -1;

    const EMonsterState st = M->StateMan ? M->StateMan->get_state_type() : eStateUnknown;
    xr_strcpy(s.state, make_xrstr(st).c_str());
    s.fighting = is_state(st, eStateAttack);

    s.path = u32(M->movement().path_type());
    s.arrived = M->movement().path_completed();

    const Fvector fwd = M->XFORM().k;
    s.yaw_deg = rad2deg(atan2f(fwd.x, fwd.z));
    s.bearing_err_deg = -1.f;

    const CEntityAlive* target = M->EnemyMan.get_enemy();
    s.enemy = !!target;
    if (target)
    {
        s.enemy_id = u16(target->ID());
        s.enemy_dist = M->Position().distance_to(target->Position());

        Fvector to_enemy;
        to_enemy.sub(target->Position(), M->Position());
        to_enemy.y = 0.f;
        if (to_enemy.magnitude() > EPS_L)
        {
            float err = rad2deg(atan2f(to_enemy.x, to_enemy.z)) - s.yaw_deg;
            while (err > 180.f)
                err -= 360.f;
            while (err < -180.f)
                err += 360.f;
            s.bearing_err_deg = _abs(err);
        }
    }

    // A monster with an enemy and a claw is always "ready to kill" in the sense
    // the stalker columns mean it; there is no trigger gate to fail.
    s.ready_to_kill = s.enemy && s.alive;
    s.fire_makes_sense = s.ready_to_kill;

    if (s.path != u32(MovementManager::ePathTypeNoPath) && s.path != u32(MovementManager::ePathTypeDummy))
    {
        s.has_dest = true;
        s.dest = M->movement().detail().dest_position();
        s.to_go = M->Position().distance_to(s.dest);
    }
}

// ⭐⭐⭐THE POPULATION IS THE WHOLE WORLD, NOT OUR PUPPETS.
//
// Until 30-07-2026 this walked SVNpc::Ids -- the roster of creatures the server
// harness spawned and drives. That was right while the harness WAS the content;
// it stopped being right the moment the leash came off and the level's own
// A-Life took over, because the roster is then empty and the census answers
// "no tracked puppets" for a map with hundreds of creatures on it. An
// instrument that reports nothing when the thing it measures is at its busiest
// is worse than no instrument.
//
// ⚠And it cast every one of them to CAI_Stalker. Even walking the world that
// way would have found only the humans and silently dropped every mutant --
// which on this level is most of what is alive, and all of what was killing
// the player.
void census_guarded()
{
    xr_vector<MindSample> samples;
    u32 skipped_actors = 0;

    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CObject* O = Level().Objects.o_get_by_iterator(i);
        if (!O || O->getDestroy())
            continue;

        CEntityAlive* E = smart_cast<CEntityAlive*>(O);
        if (!E)
            continue;

        // Players are in this list too and belong in a different report: they
        // have no brain to census, and one of them sitting in the "no enemy,
        // nowhere to go" bucket reads as an idle NPC.
        if (smart_cast<CActor*>(O))
        {
            ++skipped_actors;
            continue;
        }

        CAI_Stalker* S = smart_cast<CAI_Stalker*>(O);
        CBaseMonster* M = S ? nullptr : smart_cast<CBaseMonster*>(O);
        if (!S && !M)
            continue;

        const u16 id = u16(E->ID());

        MindSample s = {};
        s.id = id;

        // Same route to the faction name that sv_npc_list takes: the community
        // lives on the server entity, and it is the one that was actually set.
        // Mutants have none, and the section name is the useful grouping for
        // them -- "dog" and "flesh" are the two sides of a mutant census the way
        // two communities are the two sides of a stalker one.
        CSE_Abstract* e = SVWorld::Find(id);
        CSE_ALifeTraderAbstract* trader = smart_cast<CSE_ALifeTraderAbstract*>(e);
        if (trader && trader->m_community_index != NO_COMMUNITY_INDEX)
            s.faction = trader->CommunityName();
        else if (e && e->s_name.size())
            s.faction = e->s_name.c_str();
        else
            s.faction = "-";

        if (M)
        {
            sample_monster(M, s);
            samples.push_back(s);
            continue;
        }

        s.alive = !!S->g_Alive();
        s.hp = S->GetfHealth();
        s.brain = S->brain().initialized() ? S->brain().current_action_id() : u32(-1);
        s.mental = u32(S->movement().mental_state());
        s.path = u32(S->movement().path_type());
        s.arrived = S->movement().path_completed();

        // Which way it is looking. XFORM().k is the forward vector, and the
        // engine builds it from o_Angle at spawn (CGameObject::net_Spawn ->
        // XFORM().setXYZ), so a creature nobody has turned faces yaw 0.
        const Fvector fwd = S->XFORM().k;
        s.yaw_deg = rad2deg(atan2f(fwd.x, fwd.z));
        s.bearing_err_deg = -1.f;

        const CEntityAlive* target = S->memory().enemy().selected();
        s.enemy = !!target;
        if (target)
        {
            s.enemy_id = u16(target->ID());
            s.enemy_dist = S->Position().distance_to(target->Position());

            // How far its nose is from the thing it is fighting. Past about 60
            // degrees the enemy is outside any sensible field of view, and
            // "it has an enemy but is not fighting" acquires a dull answer.
            Fvector to_enemy;
            to_enemy.sub(target->Position(), S->Position());
            to_enemy.y = 0.f;
            if (to_enemy.magnitude() > EPS_L)
            {
                float err = rad2deg(atan2f(to_enemy.x, to_enemy.z)) - s.yaw_deg;
                while (err > 180.f)
                    err -= 360.f;
                while (err < -180.f)
                    err += 360.f;
                s.bearing_err_deg = _abs(err);
            }
        }

        // What is in its hands and whether the trigger would allow a shot.
        // ⚠The two gates are only asked when there IS an enemy: without one
        // they read whatever the last combat left behind, which is worse than
        // no answer because it looks like one.
        CInventoryItem* active = S->inventory().ActiveItem();
        CWeapon* weapon = smart_cast<CWeapon*>(active);
        xr_strcpy(s.weapon, active ? active->object().cNameSect().c_str() : "<empty>");
        s.ammo = weapon ? int(weapon->GetAmmoElapsed()) : -1;
        s.ready_to_kill = target && S->g_Alive() && S->ready_to_kill();
        s.fire_makes_sense = target && S->g_Alive() && S->fire_make_sense();

        CStalkerCombatPlanner* combat = combat_planner_of(S);
        s.combat_action = (combat && combat->initialized()) ? combat->current_action_id() : u32(-1);
        CObjectHandlerPlanner& hands = S->CObjectHandler::planner();
        s.hands_state = hands.initialized() ? hands.current_action_state_id() : u32(-1);
        s.fighting = (s.brain == u32(eWorldOperatorCombatPlanner));

        // ⚠A destination only means anything while a path type is in force;
        // read out of ePathTypeNoPath it is whatever the last order left behind.
        if (s.path != u32(MovementManager::ePathTypeNoPath) && s.path != u32(MovementManager::ePathTypeDummy))
        {
            s.has_dest = true;
            s.dest = S->movement().detail().dest_position();
            s.to_go = S->Position().distance_to(s.dest);
        }

        samples.push_back(s);
    }

    if (samples.empty())
    {
        Msg("! [mind] not one creature is online on this level (%u player bodies skipped) -- there is nothing here to have a mind", skipped_actors);
        return;
    }

    u32 humans = 0, mutants = 0, harnessed = 0;
    for (const MindSample& s : samples)
    {
        if (s.monster)
            ++mutants;
        else
            ++humans;
    }
    {
        // How much of this is ours. With the leash off it should be zero, and
        // saying so is the difference between "the world is behaving like this"
        // and "our test rig is".
        xr_vector<u16> ours;
        SVNpc::Ids(ours);
        harnessed = u32(ours.size());
    }

    Msg("~ [mind] %u creatures online: %u human, %u mutant (%u driven by the harness, %u player bodies skipped) -- frame %u", u32(samples.size()), humans, mutants, harnessed,
        skipped_actors, Device.dwFrame);

    // Group by faction, preserving first-seen order so the two sides come out
    // in spawn order rather than alphabetically.
    xr_vector<const char*> factions;
    for (const MindSample& s : samples)
    {
        bool seen = false;
        for (const char* f : factions)
            if (!xr_strcmp(f, s.faction))
            {
                seen = true;
                break;
            }
        if (!seen)
            factions.push_back(s.faction);
    }

    for (const char* f : factions)
    {
        xr_vector<MindSample> g;
        for (const MindSample& s : samples)
            if (!xr_strcmp(s.faction, f))
                g.push_back(s);
        print_group(f, g);
    }
}
} // namespace

void SVFight::NoteStrapFlip(CObject* creature, bool strapped) { explain_strap_flip(creature, strapped); }

void SVFight::Explain(u16 id)
{
    if (!g_pGameLevel)
    {
        Msg("! [why] no level");
        return;
    }

    xr_vector<u16> ids;
    if (id == 0xffff)
        SVNpc::Ids(ids);
    else
        ids.push_back(id);

    if (ids.empty())
    {
        Msg("! [why] no tracked puppets -- name an id, or spawn some");
        return;
    }

    for (u16 i : ids)
    {
        CAI_Stalker* S = smart_cast<CAI_Stalker*>(Level().Objects.net_Find(i));
        if (!S)
        {
            Msg("! [why] %u is not an online stalker", i);
            continue;
        }
        explain_one_guarded(S);
    }
}

void SVFight::Census()
{
    if (!g_pGameLevel)
    {
        Msg("! [mind] no level");
        return;
    }

    // Same reasoning as explain_one_guarded: this reads a dozen subsystems of a
    // hundred creatures, and one half-built creature must not take the server
    // with it.
    XR_SEH_TRY
    {
        census_guarded();
    }
    XR_SEH_EXCEPT(ExceptStackTrace("[mind] stack_trace:\n"))
    {
        Msg("!! [mind] the census itself faulted -- see the stack above");
    }
}

void SVFight::CensusWatch(int seconds, int period_ms)
{
    if (seconds <= 0)
    {
        s_mind_until = 0;
        Msg("~ [mind] watch off");
        return;
    }
    s_mind_period = (period_ms > 0) ? u32(period_ms) : 1000;
    s_mind_until = Device.dwTimeGlobal + u32(seconds) * 1000;
    s_mind_next = 0;
    Msg("~ [mind] census every %u ms for %d s", s_mind_period, seconds);
}

void SVFight::Watch(int seconds)
{
    if (seconds <= 0)
    {
        s_watch_until = 0;
        Msg("~ [why] watch off");
        return;
    }
    s_watch_until = Device.dwTimeGlobal + u32(seconds) * 1000;
    s_watch_next = 0;
    Msg("~ [why] watching every tracked puppet once a second for %d s", seconds);
}

void SVFight::WatchTick(CCustomMonster* monster)
{
    // The census timeline.
    //
    // ⚠⚠This hook lives inside the LIVING branch of shedule_Update, so a
    // creature stops driving it the moment it dies. The first version gated on
    // "the first puppet in the roster prints the block" -- and that puppet was
    // shot at t=56 s, which silently ended the timeline: 2 censuses where 78
    // were asked for, and the fight went unrecorded. Measured 28-07-2026.
    //
    // ⭐Never gate a periodic job on ONE participant. Any creature's tick may
    // drive it; the per-frame stamp is what keeps it to one block, and it holds
    // no matter who is left alive.
    if (s_mind_until)
    {
        if (Device.dwTimeGlobal > s_mind_until)
        {
            s_mind_until = 0;
            Msg("~ [mind] census watch finished");
        }
        else if (Device.dwTimeGlobal >= s_mind_next)
        {
            static u32 s_mind_frame = u32(-1);
            if (s_mind_frame != Device.dwFrame)
            {
                s_mind_frame = Device.dwFrame;
                s_mind_next = Device.dwTimeGlobal + s_mind_period;
                Census();
            }
        }
    }

    if (!s_watch_until)
        return;

    if (Device.dwTimeGlobal > s_watch_until)
    {
        s_watch_until = 0;
        Msg("~ [why] watch finished");
        return;
    }

    // One creature's tick prints the whole roster, so the block stays a
    // coherent snapshot instead of interleaving two creatures a frame apart.
    if (Device.dwTimeGlobal < s_watch_next)
        return;

    xr_vector<u16> ids;
    SVNpc::Ids(ids);
    if (ids.empty() || ids.front() != u16(monster->ID()))
        return;

    s_watch_next = Device.dwTimeGlobal + 1000;
    Explain(0xffff);
}
