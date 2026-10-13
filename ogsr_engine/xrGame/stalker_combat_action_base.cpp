////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_combat_action_base.cpp
//	Created 	: 25.03.2004
//  Modified 	: 12.01.2007
//	Author		: Dmitriy Iassenev
//	Description : Stalker base combat action
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_combat_action_base.h"
#include "ai/stalker/ai_stalker.h"
#include "script_game_object.h"
#include "sound_player.h"
#include "ai/stalker/ai_stalker_space.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "stalker_movement_manager.h"
#include "object_handler.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "agent_enemy_manager.h"
#include "inventory_item.h"
#include "stalker_combat_actions.h"
#include "visual_memory_manager.h"
#include "sight_manager.h"
#include "sight_action.h"
#include "Level.h"
#include "Weapon.h"

using namespace StalkerSpace;

CStalkerActionCombatBase::CStalkerActionCombatBase(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionCombatBase::initialize()
{
    inherited::initialize();
    object().sound().remove_active_sounds(u32(eStalkerSoundMaskNoHumming));
}

void CStalkerActionCombatBase::finalize()
{
    inherited::finalize();

    if (!object().g_Alive())
        return;

    object().sound().set_sound_mask(0);
}

bool CStalkerActionCombatBase::fire_make_sense() const { return (object().fire_make_sense()); }

// ⭐Suppressing fire, as in F.E.A.R. The player ducked behind something: the
// stalker the squad picked (CStalkerCombatPlanner::update_suppression) keeps
// bursts going at where he went to ground -- into his cover, round its edges,
// over the top -- while two others go round him. Before, nobody fired at what
// they could not see, so behind cover the player was perfectly safe and could
// wait the squad out. The point is picked off to one side of his position, so
// a head put up into it is in danger but a player standing in the open where
// he was last seen is not hit by the first bullet; the stalker's own wall in
// the way and he holds his fire instead of shooting into it.
bool CStalkerActionCombatBase::suppress()
{
    const CEntityAlive* enemy = object().memory().enemy().selected();
    if (!enemy || stalker_suppressor(enemy->ID()) != object().ID())
        return false;
    // he shows himself: the ordinary fire takes over
    if (object().memory().visual().visible_now(enemy))
        return false;

    const CMemoryInfo memory = object().memory().memory(enemy);
    if (!memory.m_object)
        return false;
    return suppress_at(memory.m_object_params.m_position);
}

bool CStalkerActionCombatBase::suppress_at(const Fvector& spot)
{
    const CEntityAlive* enemy = object().memory().enemy().selected();
    if (!enemy)
        return false;

    if (Device.dwTimeGlobal >= m_suppress_repick)
    {
        m_suppress_repick = Device.dwTimeGlobal + 400 + ::Random32.random(350);

        Fvector target = spot;
        target.y += ::Random.randF(.4f, 1.4f);

        const Fvector& eye = object().eye_matrix.c;
        Fvector way;
        way.sub(target, eye);
        way.y = 0.f;
        if (way.square_magnitude() > EPS_L)
        {
            way.normalize();
            const Fvector side{way.z, 0.f, -way.x};
            target.mad(side, (::Random32.random(2) ? 1.f : -1.f) * ::Random.randF(.5f, 1.8f));
        }
        m_suppress_point = target;

        Fvector dir;
        dir.sub(target, eye);
        const float distance = dir.magnitude();
        m_suppress_blocked = true;
        if (distance > 2.f)
        {
            dir.div(distance);
            collide::rq_result result;
            m_suppress_blocked = !!Level().ObjectSpace.RayPick(eye, dir, 2.f, collide::rqtStatic, result, nullptr);
        }

        // whether the fire went out at all, and if not why: his own wall, a
        // friend in the line, nothing in the magazine
        extern int g_dbg_npc_tactics;
        if (g_dbg_npc_tactics)
        {
            const CWeapon* weapon = smart_cast<const CWeapon*>(object().best_weapon());
            Msg("[tac] %u '%s' suppress -> (%.1f,%.1f,%.1f) %.1f m: %s | ammo=%d %s", object().ID(), object().Name(), VPUSH(target), distance,
                m_suppress_blocked ? "own wall" : object().can_kill_member() ? "friend in line" : "fire", weapon ? weapon->GetAmmoElapsed() : -1,
                object().movement().speed() > .3f ? "moving" : "still");
        }
    }

    object().sight().setup(CSightAction(SightManager::eSightTypePosition, m_suppress_point, true));
    if (m_suppress_blocked)
    {
        aim_ready();
        return true;
    }

    // bursts of three to six, a breath between them
    object().CObjectHandler::set_goal(eObjectActionFire1, object().best_weapon(), 3, 6, 600, 1200);

#ifndef SILENT_COMBAT
    // "covering fire!"
    if (stalker_first_to_cry_suppress(enemy->ID()))
        object().sound().play(eStalkerSoundNeedBackup, 0, 0, 6000, 4000);
#endif
    return true;
}

void CStalkerActionCombatBase::fire()
{
    if (!object().can_fire_to_enemy(object().memory().enemy().selected()))
    {
        aim_ready();
        return;
    }

    u32 min_queue_size, max_queue_size, min_queue_interval, max_queue_interval;
    Fvector enemy_position = object().memory().enemy().selected()->Position();
    Fvector object_position = object().Position();
    float distance = enemy_position.distance_to(object_position);
    select_queue_params(distance, min_queue_size, max_queue_size, min_queue_interval, max_queue_interval);
    object().CObjectHandler::set_goal(eObjectActionFire1, object().best_weapon(), min_queue_size, max_queue_size, min_queue_interval, max_queue_interval);
}

void CStalkerActionCombatBase::aim_ready()
{
    u32 min_queue_size, max_queue_size, min_queue_interval, max_queue_interval;
    float distance = object().memory().enemy().selected()->Position().distance_to(object().Position());
    select_queue_params(distance, min_queue_size, max_queue_size, min_queue_interval, max_queue_interval);
    object().CObjectHandler::set_goal(eObjectActionAimReady1, object().best_weapon(), min_queue_size, max_queue_size, min_queue_interval, max_queue_interval);
}

void CStalkerActionCombatBase::aim_ready_force_full()
{
    u32 min_queue_size, max_queue_size, min_queue_interval, max_queue_interval;
    float distance = object().memory().enemy().selected()->Position().distance_to(object().Position());
    select_queue_params(distance, min_queue_size, max_queue_size, min_queue_interval, max_queue_interval);
    object().CObjectHandler::set_goal(eObjectActionAimForceFull1, object().best_weapon(), min_queue_size, max_queue_size, min_queue_interval, max_queue_interval);
}

void CStalkerActionCombatBase::select_queue_params(const float& distance, u32& min_queue_size, u32& max_queue_size, u32& min_queue_interval, u32& max_queue_interval) const
{
    /**/
    int weapon_type = 6;
    if (object().best_weapon())
        weapon_type = object().best_weapon()->object().ef_weapon_type();

    switch (weapon_type)
    {
    // pistols
    case 5: {
        if (distance > 30.f)
        {
            min_queue_size = 1;
            max_queue_size = 1;
            min_queue_interval = 1000;
            max_queue_interval = 1250;
        }
        else if (distance > 15.f)
        {
            min_queue_size = 2;
            max_queue_size = 4;
            min_queue_interval = 750;
            max_queue_interval = 1000;
        }
        else
        {
            min_queue_size = 3;
            max_queue_size = 5;
            min_queue_interval = 500;
            max_queue_interval = 750;
        }

        break;
    }
    // shotguns
    case 7: {
        if (distance > 30.f)
        {
            min_queue_size = 1;
            max_queue_size = 1;
            min_queue_interval = 1250;
            max_queue_interval = 1500;
        }
        else if (distance > 15.f)
        {
            min_queue_size = 1;
            max_queue_size = 1;
            min_queue_interval = 750;
            max_queue_interval = 1250;
        }
        else
        {
            min_queue_size = 1;
            max_queue_size = 1;
            min_queue_interval = 500;
            max_queue_interval = 1000;
        }

        break;
    }
    // sniper rifles
    case 8: {
        if (distance > 30.f)
        {
            min_queue_size = 1;
            max_queue_size = 1;
            min_queue_interval = 3000;
            max_queue_interval = 4000;
        }
        else if (distance > 15.f)
        {
            min_queue_size = 1;
            max_queue_size = 1;
            min_queue_interval = 3000;
            max_queue_interval = 4000;
        }
        else
        {
            min_queue_size = 1;
            max_queue_size = 1;
            min_queue_interval = 3000;
            max_queue_interval = 4000;
        }

        break;
    }
    default: {
        if (distance > 30.f)
        {
            min_queue_size = object().min_queue_size_far();
            max_queue_size = object().max_queue_size_far();
            min_queue_interval = object().min_queue_interval_far();
            max_queue_interval = object().max_queue_interval_far();
        }
        else if (distance > 15.f)
        {
            min_queue_size = object().min_queue_size_medium();
            max_queue_size = object().max_queue_size_medium();
            min_queue_interval = object().min_queue_interval_medium();
            max_queue_interval = object().max_queue_interval_medium();
        }
        else
        {
            min_queue_size = object().min_queue_size_close();
            max_queue_size = object().max_queue_size_close();
            min_queue_interval = object().min_queue_interval_close();
            max_queue_interval = object().max_queue_interval_close();
        }
    }
    }
    /**
    if (distance > 30.f) {
        min_queue_size					= object().min_queue_size_far();
        max_queue_size					= object().max_queue_size_far();
        min_queue_interval				= object().min_queue_interval_far();
        max_queue_interval				= object().max_queue_interval_far();
    }
    else
        if (distance > 15.f) {
            min_queue_size				= object().min_queue_size_medium();
            max_queue_size				= object().max_queue_size_medium();
            min_queue_interval			= object().min_queue_interval_medium();
            max_queue_interval			= object().max_queue_interval_medium();
        }
        else {
            min_queue_size				= object().min_queue_size_close();
            max_queue_size				= object().max_queue_size_close();
            min_queue_interval			= object().min_queue_interval_close();
            max_queue_interval			= object().max_queue_interval_close();
        }
    /**/
}

void CStalkerActionCombatBase::play_panic_sound(u32 max_start_time, u32 min_start_time, u32 max_stop_time, u32 min_stop_time, u32 id)
{
    object().sound().play(object().memory().enemy().selected()->human_being() ? eStalkerSoundPanicHuman : eStalkerSoundPanicMonster, max_start_time, min_start_time, max_stop_time,
                          min_stop_time, id);
}

void CStalkerActionCombatBase::play_attack_sound(u32 max_start_time, u32 min_start_time, u32 max_stop_time, u32 min_stop_time, u32 id)
{
    if (!object().memory().enemy().selected()->human_being())
        return;

    if (!object().agent_manager().member().can_cry_noninfo_phrase())
        return;

    u32 sound_type = eStalkerSoundAttackNoAllies;
#ifdef DEBUG
    if (object().agent_manager().member().combat_members().empty())
        Msg("! I am in combat, but there is no combat members at all (including me), npc[%s],team[%d],squad[%d],group[%d]", *object().cName(), object().g_Team(),
            object().g_Squad(), object().g_Group());
#endif // DEBUG

    if (object().agent_manager().member().combat_members().size() > 1)
    {
        if (object().agent_manager().enemy().enemies().size() > 1)
            sound_type = eStalkerSoundAttackAlliesSeveralEnemies;
        else
            sound_type = eStalkerSoundAttackAlliesSingleEnemy;
    }
    else
        sound_type = eStalkerSoundAttackNoAllies;

    object().sound().play(sound_type, max_start_time, min_start_time, max_stop_time, min_stop_time, id);
}

void CStalkerActionCombatBase::play_start_search_sound(u32 max_start_time, u32 min_start_time, u32 max_stop_time, u32 min_stop_time, u32 id)
{
    if (!object().agent_manager().member().can_cry_noninfo_phrase())
        return;

#ifdef DEBUG
    if (object().agent_manager().member().combat_members().empty())
        Msg("! I am in combat, but there is no combat members at all (including me), npc[%s],team[%d],squad[%d],group[%d]", *object().cName(), object().g_Team(),
            object().g_Squad(), object().g_Group());
#endif // DEBUG

    bool search_with_allies = object().agent_manager().member().combat_members().size() > 1;

    object().sound().play(search_with_allies ? eStalkerSoundSearch1WithAllies : eStalkerSoundSearch1NoAllies, max_start_time, min_start_time, max_stop_time, min_stop_time, id);
}
