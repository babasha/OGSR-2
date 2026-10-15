////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_combat_actions.cpp
//	Created 	: 25.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker combat action classes
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_combat_actions.h"
#include "ai/stalker/ai_stalker.h"
#include "script_game_object.h"
#include "stalker_decision_space.h"
#include "Inventory.h"
#include "cover_evaluators.h"
#include "cover_point.h"
#include "cover_manager.h"
#include "Missile.h"
#include "stalker_movement_restriction.h"
#include "movement_manager_space.h"
#include "detail_path_manager_space.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "visual_memory_manager.h"
#include "sight_manager.h"
#include "ai_object_location.h"
#include "stalker_movement_manager.h"
#include "sound_player.h"
#include "stalker_planner.h"
#include "agent_member_manager.h"
#include "agent_location_manager.h"
#include "danger_cover_location.h"
#include "ai/stalker/ai_stalker_space.h"
#include "Weapon.h"
#include "danger_manager.h"
#include "detail_path_manager.h"
#include "WeaponMagazined.h"
#include "stalker_animation_manager.h"
#include "Level.h"
#include "level_graph.h"
#include "ai_debug_draw.h"
#include "ai_position_query.h"
#include "stalker_combat_planner.h"
#include "ai_house.h"
#include "ai_commander.h"
#include "ai_threat_map.h"
#include "../xrCore/load_prof.h" // [frame prof]: srch.* parts of the search action

const float TEMP_DANGER_DISTANCE = 5.f;
const u32 TEMP_DANGER_INTERVAL = 120000;

const float CLOSE_MOVE_DISTANCE = -10.f;

const u32 CROUCH_LOOK_OUT_DELTA = 5000;
constexpr u32 DETOUR_STUCK_TIME = 3000; // a detour that has not moved him a metre in this long is over

using namespace StalkerSpace;
using namespace StalkerDecisionSpace;

typedef CStalkerActionBase::_edge_value_type _edge_value_type;

//////////////////////////////////////////////////////////////////////////
// CStalkerActionGetItemToKill
//////////////////////////////////////////////////////////////////////////

CStalkerActionGetItemToKill::CStalkerActionGetItemToKill(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionGetItemToKill::initialize()
{
    inherited::initialize();

    object().sound().remove_active_sounds(u32(eStalkerSoundMaskNoHumming));

    object().sight().setup(CSightAction(object().m_best_found_item_to_kill ? &object().m_best_found_item_to_kill->object() : 0, true));

    object().movement().set_mental_state(eMentalStateDanger);
}

void CStalkerActionGetItemToKill::finalize()
{
    inherited::finalize();

    object().sight().clear();
    object().sight().setup(CSightAction(SightManager::eSightTypePathDirection, false));

    if (!object().g_Alive())
        return;

    object().sound().set_sound_mask(0);
}

void CStalkerActionGetItemToKill::execute()
{
    inherited::execute();

    if (!object().m_best_found_item_to_kill)
        return;

    object().movement().set_level_dest_vertex(object().m_best_found_item_to_kill->object().ai_location().level_vertex_id());
    object().movement().set_desired_position(&object().m_best_found_item_to_kill->object().Position());
    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_body_state(object().movement().body_state() == eBodyStateCrouch ? eBodyStateCrouch : eBodyStateStand);
    object().movement().set_movement_type(eMovementTypeWalk);
    object().set_goal(eObjectActionIdle);
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionMakeItemKilling
//////////////////////////////////////////////////////////////////////////

CStalkerActionMakeItemKilling::CStalkerActionMakeItemKilling(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionMakeItemKilling::initialize()
{
    inherited::initialize();

    object().sound().remove_active_sounds(u32(eStalkerSoundMaskNoHumming));

    object().sight().clear();
    object().sight().add_action(eSightActionTypeWatchItem, xr_new<CSightControlAction>(1.f, 3000, CSightAction(SightManager::eSightTypePathDirection)));
    object().sight().add_action(eSightActionTypeWatchEnemy,
                                xr_new<CSightControlAction>(1.f, 3000, CSightAction(SightManager::eSightTypePosition, object().memory().enemy().selected()->Position(), false)));

    object().movement().set_mental_state(eMentalStateDanger);
}

void CStalkerActionMakeItemKilling::finalize()
{
    inherited::finalize();

    object().sight().clear();
    object().sight().setup(CSightAction(SightManager::eSightTypePathDirection, false));

    if (!object().g_Alive())
        return;

    object().sound().set_sound_mask(0);
}

void CStalkerActionMakeItemKilling::execute()
{
    inherited::execute();

    if (!object().m_best_found_ammo)
        return;

    object().movement().set_level_dest_vertex(object().m_best_found_ammo->object().ai_location().level_vertex_id());
    object().movement().set_desired_position(&object().m_best_found_ammo->object().Position());
    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_body_state(eBodyStateStand);
    object().movement().set_movement_type(eMovementTypeWalk);
    object().sight().action(eSightActionTypeWatchEnemy).set_vector3d(object().memory().enemy().selected()->Position());
    object().set_goal(eObjectActionIdle);
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionRetreatFromEnemy
//////////////////////////////////////////////////////////////////////////

CStalkerActionRetreatFromEnemy::CStalkerActionRetreatFromEnemy(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionRetreatFromEnemy::initialize() { inherited::initialize(); }

void CStalkerActionRetreatFromEnemy::finalize()
{
    inherited::finalize();

    if (!object().g_Alive())
        return;

    object().sound().set_sound_mask(0);
}

void CStalkerActionRetreatFromEnemy::execute()
{
    inherited::execute();

    if (!object().memory().enemy().selected())
        return;

    if (!object().memory().enemy().selected())
        return;

    CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

    object().movement().set_movement_type(eMovementTypeRun);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_mental_state(eMentalStatePanic);
    object().movement().set_body_state(eBodyStateStand);

    object().m_ce_far->setup(mem_object.m_object_params.m_position, 0.f, 300.f);
    const CCoverPoint* point = ai().cover_manager().best_cover(object().Position(), 30.f, *object().m_ce_far, CStalkerMovementRestrictor(m_object, true));
    if (!point)
    {
        object().m_ce_far->setup(mem_object.m_object_params.m_position, 0.f, 300.f);
        point = ai().cover_manager().best_cover(object().Position(), 50.f, *object().m_ce_far, CStalkerMovementRestrictor(m_object, true));
    }

    if (point)
    {
        object().movement().set_level_dest_vertex(point->level_vertex_id());
        object().movement().set_desired_position(&point->position());
        object().CObjectHandler::set_goal(eObjectActionIdle);
        object().sight().setup(CSightAction(SightManager::eSightTypePathDirection, false));
    }
    else
    {
        if (object().memory().visual().visible_now(object().memory().enemy().selected()))
        {
            object().movement().set_mental_state(eMentalStateDanger);
            fire();
            object().sight().setup(CSightAction(object().memory().enemy().selected(), true, true));
        }
        else
        {
            object().CObjectHandler::set_goal(eObjectActionIdle);
            object().sight().setup(CSightAction(SightManager::eSightTypeCover, true));
        }
    }

    play_panic_sound(0, 0, 10000);
}

_edge_value_type CStalkerActionRetreatFromEnemy::weight(const CSConditionState& condition0, const CSConditionState& condition1) const { return (_edge_value_type(100)); }

//////////////////////////////////////////////////////////////////////////
// CStalkerActionGetReadyToKill
//////////////////////////////////////////////////////////////////////////

CStalkerActionGetReadyToKill::CStalkerActionGetReadyToKill(bool affect_properties, CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name)
{
    m_affect_properties = affect_properties;
}

void CStalkerActionGetReadyToKill::initialize()
{
    inherited::initialize();

    m_body_state = object().movement().body_state();
    //	m_movement_type										= Random.randI(2) ? eMovementTypeRun : eMovementTypeWalk;
    //	m_movement_type										= eMovementTypeRun;

    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_nearest_accessible_position();
    object().movement().set_mental_state(eMentalStateDanger);
    object().movement().set_body_state(m_body_state);
    //	object().movement().set_movement_type				(eMovementTypeRun);
    //	object().sight().setup								(CSightAction(SightManager::eSightTypePathDirection));
    if (m_affect_properties)
    {
        m_storage->set_property(eWorldPropertyInCover, false);
        m_storage->set_property(eWorldPropertyLookedOut, false);
        m_storage->set_property(eWorldPropertyPositionHolded, false);
        m_storage->set_property(eWorldPropertyEnemyDetoured, false);

        m_enable_enemy_change = object().memory().enemy().enable_enemy_change();
        object().memory().enemy().enable_enemy_change(false);
    }
}

void CStalkerActionGetReadyToKill::finalize()
{
    inherited::finalize();

    if (m_affect_properties)
        object().memory().enemy().enable_enemy_change(m_enable_enemy_change);
}

void CStalkerActionGetReadyToKill::execute()
{
    inherited::execute();

    if (!object().m_best_item_to_kill)
        return;

    if (!object().memory().enemy().selected())
        return;

    if (object().movement().detail().distance_to_target() < 2.f)
    {
        object().movement().set_movement_type(eMovementTypeWalk);
        object().sight().setup(CSightAction(SightManager::eSightTypeCurrentDirection));
    }
    else
    {
        object().movement().set_movement_type(eMovementTypeRun);
        object().sight().setup(CSightAction(SightManager::eSightTypePathDirection));
    }

    if (object().movement().detail().distance_to_target() > CLOSE_MOVE_DISTANCE)
        object().movement().set_body_state(eBodyStateStand);
    //	else {
    //		object().movement().set_movement_type	(m_movement_type);
    //	}

    CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());
    Fvector position = mem_object.m_object_params.m_position;

    const CCoverPoint* point = object().best_cover(position);
    if (point)
    {
        object().movement().set_level_dest_vertex(point->level_vertex_id());
        object().movement().set_desired_position(&point->position());
        //		object().movement().set_movement_type		(eMovementTypeRun);
        if (object().movement().path_completed() || object().Position().distance_to(point->position()) < 1.f)
        {
            //			object().movement().set_body_state		(eBodyStateCrouch);
            object().brain().affect_cover(true);
        }
        else
        {
            //			object().movement().set_body_state		(eBodyStateStand);
            object().brain().affect_cover(false);
        }
    }
    else
    {
        object().brain().affect_cover(true);
        object().movement().set_movement_type(eMovementTypeStand);
        //		object().movement().set_body_state			(eBodyStateCrouch);
        object().movement().set_nearest_accessible_position();
    }

    //	if (object().memory().visual().visible_now(object().memory().enemy().selected()))
    //		object().sight().setup	(CSightAction(object().memory().enemy().selected(),true));
    //	else
    //		object().sight().setup	(CSightAction(SightManager::eSightTypePosition,mem_object.m_object_params.m_position,true));

    //	if ((point && !point->position().similar(object().Position(),.5f)) || !object().movement().path_completed())
    //		object().sight().setup			(CSightAction(SightManager::eSightTypePathDirection));

    if (m_affect_properties)
        aim_ready();
    else
        aim_ready_force_full();

    if (object().movement().path_completed())
        object().best_cover_can_try_advance();
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionKillEnemy
//////////////////////////////////////////////////////////////////////////

CStalkerActionKillEnemy::CStalkerActionKillEnemy(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionKillEnemy::initialize()
{
    inherited::initialize();
    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_nearest_accessible_position();
    object().movement().set_mental_state(eMentalStateDanger);
    //	object().movement().set_body_state			(m_storage->property(eWorldPropertyUseCrouchToLookOut) ? eBodyStateCrouch : eBodyStateStand);
    object().movement().set_movement_type(eMovementTypeStand);
    m_storage->set_property(eWorldPropertyLookedOut, false);
    m_storage->set_property(eWorldPropertyPositionHolded, false);
    m_storage->set_property(eWorldPropertyEnemyDetoured, false);
    play_attack_sound(0, 0, 6000, 4000);
    object().brain().affect_cover(true);
}

void CStalkerActionKillEnemy::finalize() { inherited::finalize(); }

void CStalkerActionKillEnemy::execute()
{
    inherited::execute();

    object().sight().setup(CSightAction(object().memory().enemy().selected(), true, true));

    //	u32									min_queue_size, max_queue_size, min_queue_interval, max_queue_interval;
    //	float								distance = object().memory().enemy().selected()->Position().distance_to(object().Position());
    //	select_queue_params					(distance,min_queue_size, max_queue_size, min_queue_interval, max_queue_interval);
    //	object().CObjectHandler::set_goal	(eObjectActionFire1,object().best_weapon(),min_queue_size, max_queue_size, min_queue_interval, max_queue_interval);
    fire();

    if (object().memory().enemy().selected())
    {
        CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

        if (mem_object.m_object)
        {
            object().best_cover(mem_object.m_object_params.m_position);
        }
    }
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionTakeCover
//////////////////////////////////////////////////////////////////////////

CStalkerActionTakeCover::CStalkerActionTakeCover(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionTakeCover::initialize()
{
    inherited::initialize();

    m_body_state = object().movement().body_state();
    //	m_movement_type								= Random.randI(2) ? eMovementTypeRun : eMovementTypeWalk;
    m_movement_type = eMovementTypeWalk;

    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_mental_state(eMentalStateDanger);
    object().movement().set_body_state(m_body_state);
    object().movement().set_movement_type(m_movement_type);
    m_storage->set_property(eWorldPropertyLookedOut, false);
    m_storage->set_property(eWorldPropertyPositionHolded, false);
    m_storage->set_property(eWorldPropertyEnemyDetoured, false);

    if (object().memory().enemy().selected()->human_being())
    {
        if (object().agent_manager().member().can_cry_noninfo_phrase())
            if (object().Position().distance_to_sqr(object().memory().enemy().selected()->Position()) < _sqr(10.f))
                if (object().memory().visual().visible_now(object().memory().enemy().selected()) && object().agent_manager().member().group_behaviour())
                    object().sound().play(eStalkerSoundBackup, 0, 0, 6000, 4000);
    }
}

void CStalkerActionTakeCover::finalize() { inherited::finalize(); }

void CStalkerActionTakeCover::execute()
{
    inherited::execute();

    CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

    if (!mem_object.m_object)
        return;

    if (object().movement().detail().distance_to_target() > CLOSE_MOVE_DISTANCE)
        object().movement().set_body_state(eBodyStateStand);
    else
        object().movement().set_movement_type(m_movement_type);

    Fvector position = mem_object.m_object_params.m_position;
    const CCoverPoint* point = object().best_cover(position);
    if (point)
    {
        object().movement().set_level_dest_vertex(point->level_vertex_id());
        object().movement().set_desired_position(&point->position());
        if (object().movement().path_completed() && object().Position().distance_to(point->position()) < 1.f)
            object().brain().affect_cover(true);
        else
            object().brain().affect_cover(false);
    }
    else if (PositionQuery::Pending(u16(object().ID()), PositionQuery::eCover))
    {
        // ai_tactics_v2: the first cover is a frame away; not "in cover" meanwhile
        object().brain().affect_cover(false);
    }
    else
    {
        object().movement().set_nearest_accessible_position();
        object().brain().affect_cover(true);
    }

    //.	Add fire here
    //	if (object().memory().visual().visible_now(object().memory().enemy().selected()) && object().can_kill_enemy())
    //	if (object().memory().visual().visible_now(object().memory().enemy().selected()))
    if (fire_make_sense())
    {
        fire();
    }
    else
    {
        aim_ready();
    }

    if (object().movement().path_completed())
    { // && (object().memory().enemy().selected()->Position().distance_to_sqr(object().Position()) >= 10.f))
        object().best_cover_can_try_advance();
        m_storage->set_property(eWorldPropertyInCover, true);
    }

    if (object().memory().visual().visible_now(object().memory().enemy().selected()))
        object().sight().setup(CSightAction(object().memory().enemy().selected(), true, true));
    else
        object().sight().setup(CSightAction(SightManager::eSightTypePosition, mem_object.m_object_params.m_position, true));

    // 📏06-10 13:55: Волк held the suppressor's role ten seconds walking to a
    // cover 30-40 m off, and only one burst came near the player. Covering fire
    // is laid down from where he stands; he goes on to the cover after.
    const bool suppressing = suppress() && !suppress_blocked();
    object().movement().set_movement_type(suppressing ? eMovementTypeStand : m_movement_type);
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionLookOut
//////////////////////////////////////////////////////////////////////////

CStalkerActionLookOut::CStalkerActionLookOut(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name)
{
    m_crouch_look_out_random.seed(u32(CPU::QPC() & 0xffffffff));
    m_last_change_time = 0;
}

void CStalkerActionLookOut::initialize()
{
    inherited::initialize();

    if (Device.dwTimeGlobal >= m_last_change_time + CROUCH_LOOK_OUT_DELTA)
    {
        m_storage->set_property(eWorldPropertyUseCrouchToLookOut, !!m_crouch_look_out_random.random(2));
        m_last_change_time = Device.dwTimeGlobal;
    }

    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_mental_state(eMentalStateDanger);

    object().movement().set_body_state(m_storage->property(eWorldPropertyUseCrouchToLookOut) ? eBodyStateCrouch : eBodyStateStand);
    object().movement().set_movement_type(eMovementTypeWalk);
    object().movement().set_nearest_accessible_position();

    if (object().ready_to_detour())
        aim_ready();
    else
    {
        aim_ready_force_full();
        object().movement().set_movement_type(eMovementTypeStand);
    }

    set_inertia_time(1000);
    object().brain().affect_cover(true);
}

// A wide wall in the way and a lean not enough: step out to its edge, where
// CAI_Stalker::find_peek_step found a clear line to the enemy. Seeing him from
// there hands the plan to the shooting actions; take_cover walks back after.
static bool step_out_of_cover(CAI_Stalker& npc)
{
    const Fvector* position = nullptr;
    u32 vertex = u32(-1);
    if (!npc.peek_step(position, vertex))
        return false;
    npc.movement().set_level_dest_vertex(vertex);
    npc.movement().set_desired_position(position);
    npc.movement().set_movement_type(eMovementTypeWalk);
    return true;
}

float current_cover(CAI_Stalker* object)
{
    Fvector position, direction;
    position = object->eye_matrix.c;
    direction = object->eye_matrix.k;
    collide::rq_result ray_query_result;
    BOOL result = Level().ObjectSpace.RayPick(position, direction, 10.f, collide::rqtStatic, ray_query_result, NULL);

    if (!result)
        return (100.f);

    return (ray_query_result.range);
}

void CStalkerActionLookOut::finalize() { inherited::finalize(); }

void CStalkerActionLookOut::execute()
{
    inherited::execute();

    CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

    if (!mem_object.m_object)
        return;

    object().sight().setup(CSightAction(SightManager::eSightTypePosition, mem_object.m_object_params.m_position, true));
    suppress();

    if (step_out_of_cover(object()))
        return;

    if (current_cover(m_object) >= 3.f)
    {
        object().movement().set_nearest_accessible_position();
        m_storage->set_property(eWorldPropertyLookedOut, true);
        return;
    }

    Fvector position = mem_object.m_object_params.m_position;
    object().m_ce_close->setup(position, 0.f, 170.f, 10.f);
    const CCoverPoint* point = ai().cover_manager().best_cover(object().Position(), 10.f, *object().m_ce_close); //,CStalkerMovementRestrictor(m_object,true,false));
    if (!point || (point->position().similar(object().Position()) && object().movement().path_completed()))
    {
        object().m_ce_close->setup(position, 0.f, 170.f, 10.f);
        point = ai().cover_manager().best_cover(object().Position(), 30.f, *object().m_ce_close); //,CStalkerMovementRestrictor(m_object,true,false));
    }

    if (point)
    {
        object().movement().set_level_dest_vertex(point->level_vertex_id());
        object().movement().set_desired_position(&point->position());
    }
    else
        object().movement().set_nearest_accessible_position();

    //	if (point && point->position().similar(object().Position(),.5f) && object().movement().path_completed()) {
    //		m_storage->set_property			(eWorldPropertyLookedOut,true);
    //		object().movement().set_nearest_accessible_position	();
    //	}

    if (object().memory().enemy().selected())
    {
        CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

        if (mem_object.m_object)
        {
            object().best_cover(mem_object.m_object_params.m_position);
        }
    }
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionHoldPosition
//////////////////////////////////////////////////////////////////////////

CStalkerActionHoldPosition::CStalkerActionHoldPosition(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionHoldPosition::initialize()
{
    inherited::initialize();
    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_nearest_accessible_position();
    object().movement().set_mental_state(eMentalStateDanger);
    object().movement().set_body_state(m_storage->property(eWorldPropertyUseCrouchToLookOut) ? eBodyStateCrouch : eBodyStateStand);
    object().movement().set_movement_type(eMovementTypeStand);

    aim_ready();

    set_inertia_time(1000 + ::Random32.random(2000));
    object().brain().affect_cover(true);
}

void CStalkerActionHoldPosition::finalize() { inherited::finalize(); }

void CStalkerActionHoldPosition::execute()
{
    inherited::execute();

    CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

    if (!mem_object.m_object)
        return;

    if (current_cover(m_object) < 3.f)
        m_storage->set_property(eWorldPropertyLookedOut, false);

    object().sight().setup(CSightAction(SightManager::eSightTypePosition, mem_object.m_object_params.m_position, true));
    step_out_of_cover(object());

    // the suppressor holds here for as long as the others need him to
    const bool suppressing = suppress();

    if (completed() && !suppressing)
    {
        if (object().agent_manager().member().can_detour() || !object().agent_manager().member().cover_detouring() || !fire_make_sense())
        {
            m_storage->set_property(eWorldPropertyPositionHolded, true);
            m_storage->set_property(eWorldPropertyInCover, false);
        }
    }

    if (!suppressing)
    {
        if (object().agent_manager().member().cover_detouring() && fire_make_sense())
        {
            //		object().sound().play		(eStalkerSoundDetour,3000,3000,10000,10000);
            object().sound().play(eStalkerSoundNeedBackup, 3000, 3000, 10000, 10000);
            fire();
        }
        else
            aim_ready();
    }

    if (object().memory().enemy().selected())
    {
        CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

        if (mem_object.m_object)
        {
            object().best_cover(mem_object.m_object_params.m_position);
        }
    }
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionDetourEnemy
//////////////////////////////////////////////////////////////////////////

CStalkerActionDetourEnemy::CStalkerActionDetourEnemy(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionDetourEnemy::initialize()
{
    inherited::initialize();
    object().agent_manager().member().member(&object()).detour(true);
    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_mental_state(eMentalStateDanger);
    object().movement().set_body_state(eBodyStateStand);
    object().movement().set_movement_type(eMovementTypeRun);

    aim_ready();

    if (/**(Random.randF(1.f) < .8f) && /**/ object().agent_manager().member().member(m_object).cover())
        object().agent_manager().location().add(xr_new<CDangerCoverLocation>(object().agent_manager().member().member(m_object).cover(), Device.dwTimeGlobal, TEMP_DANGER_INTERVAL,
                                                                             TEMP_DANGER_DISTANCE, object().agent_manager().member().mask(&object())));

    object().agent_manager().member().member(m_object).cover(0);

    m_stuck_position = object().Position();
    m_stuck_time = Device.dwTimeGlobal;

    if (object().memory().enemy().selected()->human_being() && object().agent_manager().member().group_behaviour())
    {
        //		object().sound().play			(eStalkerSoundNeedBackup);
        // sent round while another one suppresses: "flanking!" after his "covering fire!", not over it
        if (stalker_is_flanker(object().memory().enemy().selected()->ID(), object().ID()))
            object().sound().play(eStalkerSoundDetour, 2500, 1000, 8000, 6000);
        else
            object().sound().play(eStalkerSoundDetour);
    }
}

void CStalkerActionDetourEnemy::finalize()
{
    inherited::finalize();

    if (object().g_Alive())
        object().agent_manager().member().member(&object()).detour(false);
}

void CStalkerActionDetourEnemy::execute()
{
    inherited::execute();

    CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

    if (!mem_object.m_object)
        return;

    if (object().movement().path_completed())
    {
        Fvector position = mem_object.m_object_params.m_position;

        object().m_ce_angle->setup(position, 10.f, object().ffGetRange(), mem_object.m_object_params.m_level_vertex_id);
        const CCoverPoint* point = ai().cover_manager().best_cover(object().Position(), 10.f, *object().m_ce_angle, CStalkerMovementRestrictor(m_object, true));
        if (!point)
        {
            object().m_ce_angle->setup(position, 10.f, object().ffGetRange(), mem_object.m_object_params.m_level_vertex_id);
            point = ai().cover_manager().best_cover(object().Position(), 30.f, *object().m_ce_angle, CStalkerMovementRestrictor(m_object, true));
        }

        if (point)
        {
            object().movement().set_level_dest_vertex(point->level_vertex_id());
            object().movement().set_desired_position(&point->position());
        }
        else
            object().movement().set_nearest_accessible_position();

        if (object().movement().path_completed())
            m_storage->set_property(eWorldPropertyEnemyDetoured, true);
    }

    // 📏06-10 13:59: Упырь "going round" 15 s with his goal 0.2 m off -- Волк
    // stood on it, the path never completed. Not a metre in 3 s: he is there,
    // or as near as he gets; the detour is done.
    if (Device.dwTimeGlobal > m_stuck_time + DETOUR_STUCK_TIME)
    {
        if (object().Position().distance_to_sqr(m_stuck_position) < _sqr(1.f))
            m_storage->set_property(eWorldPropertyEnemyDetoured, true);
        m_stuck_position = object().Position();
        m_stuck_time = Device.dwTimeGlobal;
    }

    object().sight().setup(CSightAction(SightManager::eSightTypePosition, mem_object.m_object_params.m_position, true));
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionSearchEnemy
//////////////////////////////////////////////////////////////////////////

CStalkerActionSearchEnemy::CStalkerActionSearchEnemy(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionSearchEnemy::initialize()
{
    inherited::initialize();
    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_mental_state(eMentalStateDanger);
    object().movement().set_body_state(eBodyStateStand);
    object().movement().set_movement_type(eMovementTypeWalk);

    aim_ready();

    object().agent_manager().member().member(m_object).cover(0);

    m_watch_start = Device.dwTimeGlobal;
    m_watch_cover = nullptr;
    m_watch_cover_time = 0;
    m_watch_ground_v2 = false;
    m_watch_asked = 0;
    m_watch_point = Fvector{flt_max, flt_max, flt_max}; // asked for nowhere yet
    m_post = -1;
    m_post_built = 0;
    m_post_time = 0;
    m_post_surplus = false;
    m_post_pending = false;
    m_stacked = false;
    m_stack_vertex = u32(-1);
    m_stack_time = 0;

    m_watch_stand = false;

    m_phase_time = Device.dwTimeGlobal;
    m_search_origin = object().Position();
    m_sweep_going = false;
    m_sweep_points = 0;
    m_sweep_arrived = false;

    const CEntityAlive* enemy = object().memory().enemy().selected();
    m_watcher = enemy && !claim_search(enemy->ID(), m_object->ID());
    // back from a step aside with the comb already under way: rejoin it
    // ⚠No default-constructed CMemoryInfo here: its base's constructor lives in
    // memory_space_impl.h, and GCC (the Switch build) does not link without it
    bool sweeping = false;
    if (enemy)
    {
        const CMemoryInfo info = object().memory().memory(enemy);
        sweeping = info.m_object && stalker_sweep_start(enemy->ID(), info.m_object_params.m_position);
    }
    m_phase = sweeping ? eSearchSweep : eSearchApproach;
}

void CStalkerActionSearchEnemy::finalize()
{
    inherited::finalize();
    stalker_release_claims(m_object->ID());
}

extern int g_dbg_npc_tactics;

// Who does what against which enemy, across every squad. The village fights as
// several squads, and per-squad bookkeeping let each squad send its own man to
// the door and bunch its watchers behind the same wall as the next squad's.
namespace
{
struct SClaim
{
    u16 npc{u16(-1)};
    u32 time{};
};

constexpr u32 CLAIM_TTL = 3000; // a claim not refreshed this long is free
constexpr u32 WATCH_SLOTS = 8; // sectors around the enemy's last position
constexpr float WATCH_RADIUS = 18.f; // how far out from it the watchers sit
constexpr float WATCH_RADIUS_SIEGE = 10.f; // ...round a house being kept, those with no post on it
constexpr float WATCH_REPICK_SHIFT = 8.f; // the spot moved this far: a watcher picks his cover anew

constexpr float SWEEP_TAKEN_RADIUS = 6.f; // a hiding place this close to one already checked is the same place

constexpr u32 SUPPRESS_TTL = 500; // the suppressor stops refreshing: the fire stops at once, not in CLAIM_TTL
constexpr u32 FLANK_SLOTS = 2;

// ⭐The siege. 📏06-10 14:29: the player sat in a village house; the watchers
// took "sectors" round his last spot as round a spot in a field, half of them
// behind the blind walls of the house, and nobody looked in at a window or kept
// a door. When he was last seen indoors -- a roof over him, walls round him --
// the house is read for its openings, rays out from where he was at three
// heights: one that gets out under the open sky passed a window, one that gets
// out at knee height too passed a door. Each opening gets its posts outside,
// places that see in through it, and each watcher his own, the nearest free
// one; a second man on the doors, then on the windows. Nobody walks in for
// SIEGE_HOLD.
// 📏06-10 14:43: "out" was a ray running 10 m clear -- but a window gives on a
// yard with the next house, a fence, a shed in it, and the house read as one or
// two openings; read afresh at each step he took inside, the posts shuffling
// after it. Out now is the first point along the ray with nothing overhead, and
// the house keeps what was read of it while he stays inside: each new spot adds
// the openings it sees.
constexpr float SIEGE_POST_SPACING = 4.f; // no two posts in front of the house nearer than this
constexpr u32 SIEGE_POSTS = 3; // men on a door: one in front, one either side of it; a window takes two in front
constexpr float SIEGE_WATCH_NEAR = 3.5f; // a post in front of an opening: no nearer than this
constexpr float SIEGE_WATCH_FAR = 14.f; // ...nor farther
constexpr float SIEGE_WATCH_MARK = 7.f; // ...and best about this far
constexpr float SIEGE_FLANK_SPACING = 1.5f; // the men beside a door: not on top of one another
constexpr u32 SIEGE_HOLD = 60000; // nobody goes in for this long; the stock search then
constexpr u32 POST_TTL = 15000; // a post stays its man's while he is off shooting, for this long
constexpr u32 SIEGE_BLIND_RETRY = 30000; // an opening struck off is tried again after this long

struct SPost
{
    SClaim claim;
    bool placed{};
    Fvector place{}; // where its man keeps it from
};

struct SOpening : AIHouse::SOpening
{
    bool blind{}; // nowhere outside sees in through it
    u32 blind_time{}; // ...as found then; tried again after SIEGE_BLIND_RETRY
    bool full[SIEGE_POSTS]{}; // no room outside for that man
    SPost posts[SIEGE_POSTS];
};

using SSiege = AIHouse::THouse<SOpening>;

struct SEnemyClaims
{
    SClaim searcher;
    SClaim sudden;
    SClaim suppressor;
    SClaim flankers[FLANK_SLOTS];
    u32 suppress_cried{}; // when a suppressor last called it out
    SClaim slots[WATCH_SLOTS];
    u32 sweep_start{}; // the searcher's call: he is not where we saw him
    Fvector sweep_from{}; // where he was last seen when the call went out
    xr_vector<Fvector> checked; // hiding places somebody went to look at
    u32 go_time{}; // when somebody last said he was going to look
    Fvector go_from{}; // ...and at which spot
    SSiege siege;
};

xr_map<u16, SEnemyClaims> g_enemy_claims;

bool fresh(const SClaim& claim) { return claim.npc != u16(-1) && Device.dwTimeGlobal < claim.time + CLAIM_TTL; }
// a post: its man may be off shooting at a window a while
bool post_fresh(const SClaim& claim) { return claim.npc != u16(-1) && Device.dwTimeGlobal < claim.time + POST_TTL; }

bool sweep_taken(const SEnemyClaims& claims, const Fvector& position)
{
    for (const Fvector& p : claims.checked)
        if (p.distance_to_sqr(position) < _sqr(SWEEP_TAKEN_RADIUS))
            return true;
    return false;
}

bool ray_blocked(const Fvector& from, const Fvector& to, float slack)
{
    Fvector dir;
    dir.sub(to, from);
    const float length = dir.magnitude();
    const float range = length - slack;
    if (range <= 0.f)
        return false;
    dir.div(length);
    collide::rq_result result;
    return Level().ObjectSpace.RayPick(from, dir, range, collide::rqtStatic, result, nullptr);
}

enum EWatchView : u8
{
    eWatchViewNone,
    eWatchViewCrouch,
    eWatchViewStand,
};

// What a watcher posted at `position` sees of `spot`: a crouched or a standing
// eye, straight on or a lean's width aside, that gets near it past the static
// geometry. A cover is a wall towards the enemy, and "the best cover" is the
// wall right in front of the nose: a bandit crouched behind a gate stared at it
// for a minute and a half, the player sitting 10 m beyond it.
// `gun`: from where the muzzle is, not the eyes -- a post must be able to
// shoot in, not only look in. 📏06-10 16:00: a man posted behind a fence with
// only the top of his head over it: his eyes cleared it, his gun did not.
EWatchView view_into(const Fvector& position, const Fvector& target, float slack, bool gun = false)
{
    constexpr float lean_width = .5f;
    Fvector way;
    way.sub(target, position);
    way.y = 0.f;
    const float distance = way.magnitude();
    if (distance < 1.f)
        return eWatchViewCrouch;
    way.div(distance);
    const Fvector side{way.z, 0.f, -way.x};

    for (const auto [height, view] : {std::pair{gun ? .9f : 1.f, eWatchViewCrouch}, std::pair{gun ? 1.4f : 1.6f, eWatchViewStand}})
    {
        const Fvector eye = Fvector(position).add(Fvector{0.f, height, 0.f});
        if (!ray_blocked(eye, target, slack))
            return view;
        for (const float sign : {1.f, -1.f})
        {
            const Fvector aside = Fvector(eye).mad(side, sign * lean_width);
            // room to lean there, so the ray does not start inside a wall
            if (!ray_blocked(eye, aside, 0.f) && !ray_blocked(aside, target, slack))
                return view;
        }
    }
    return eWatchViewNone;
}

EWatchView watch_view(const Fvector& position, const Fvector& spot)
{
    Fvector target = spot;
    target.y += 1.f;
    // seeing up to a few metres short of it is seeing it: he went to ground there
    const float distance = Fvector().sub(target, position).magnitude();
    return view_into(position, target, std::min(3.f, distance * .3f));
}

bool siege_inside(const SSiege& siege, const Fvector& p) { return AIHouse::Inside(siege, p); }

bool struck_off(const SOpening& o) { return o.blind && Device.dwTimeGlobal < o.blind_time + SIEGE_BLIND_RETRY; }

// The siege of the house the enemy was last seen in; nullptr when that spot is
// not indoors or the house has no way in anybody can watch. Each stalker asks
// with his own memory of him; only a fresher sighting than the one last read
// for reads it again, or two memories would read it in turn.
SSiege* siege_at(u16 enemy_id, const Fvector& spot, u32 seen)
{
    SSiege& siege = g_enemy_claims[enemy_id].siege;
    // 📏06-10 15:23: two watchers' memories of him -- one in the house, one
    // on the doorstep, each kept fresh by what they heard -- read the house in
    // turn twice a second, "out" and "back in". Once a second at most.
    if (!siege.built || (seen > siege.seen && Device.dwTimeGlobal > siege.built + AIHouse::REREAD && siege.spot.distance_to_sqr(spot) > _sqr(AIHouse::RESPOT)))
    {
        AIHouse::Update(siege, spot, g_dbg_npc_tactics != 0);
        siege.seen = seen;
    }
    if (!siege.indoors)
        return nullptr;
    const bool watchable = std::any_of(siege.openings.begin(), siege.openings.end(), [](const SOpening& o) { return !struck_off(o); });
    return watchable ? &siege : nullptr;
}

// The post this stalker keeps, as opening * SIEGE_POSTS + place: the one he
// holds, else the nearest opening with nobody on it, else a second man on the
// nearest door, then window; -1 if none is left.
int claim_post(SSiege& siege, u16 npc, const Fvector& from)
{
    for (u32 k = 0; k < siege.openings.size(); ++k)
        for (u32 slot = 0; slot < SIEGE_POSTS; ++slot)
        {
            SClaim& claim = siege.openings[k].posts[slot].claim;
            if (claim.npc == npc)
            {
                claim.time = Device.dwTimeGlobal;
                return int(k * SIEGE_POSTS + slot);
            }
        }

    // the first men on every opening; then beside the doors; then a second man
    // in front of the windows
    static constexpr std::pair<u32, int> rounds[] = {{0, -1}, {1, 1}, {2, 1}, {1, 0}};
    for (const auto [slot, door] : rounds)
    {
        int best = -1;
        float best_distance = flt_max;
        for (u32 k = 0; k < siege.openings.size(); ++k)
        {
            const SOpening& o = siege.openings[k];
            if (struck_off(o) || o.full[slot] || (door >= 0 && o.door != !!door) || post_fresh(o.posts[slot].claim))
                continue;
            const float distance = from.distance_to_sqr(o.mouth);
            if (distance < best_distance)
            {
                best_distance = distance;
                best = int(k);
            }
        }
        if (best >= 0)
        {
            siege.openings[best].posts[slot].claim = {npc, Device.dwTimeGlobal};
            return best * int(SIEGE_POSTS) + int(slot);
        }
    }
    return -1;
}

// another post already stands this near
bool post_crowded(const SSiege& siege, const Fvector& place, u16 npc, float spacing)
{
    for (const SOpening& o : siege.openings)
        for (const SPost& post : o.posts)
            if (post.placed && post.claim.npc != npc && post_fresh(post.claim) && post.place.distance_to_sqr(place) < _sqr(spacing))
                return true;
    return false;
}
} // namespace

bool stalker_siege(u16 enemy_id, const Fvector& last_seen, u32 seen) { return !!siege_at(enemy_id, last_seen, seen); }

bool stalker_siege_inside(u16 enemy_id, const Fvector& position)
{
    if (Commander::Owns(enemy_id))
        return Commander::InsideHouse(enemy_id, position);
    const auto I = g_enemy_claims.find(enemy_id);
    return I != g_enemy_claims.end() && siege_inside(I->second.siege, position);
}

bool stalker_siege_holds(u16 enemy_id, const Fvector& last_seen, u32 seen)
{
    const SSiege* siege = siege_at(enemy_id, last_seen, seen);
    return siege && Device.dwTimeGlobal < siege->start + SIEGE_HOLD;
}

u32 stalker_sweep_start(u16 enemy_id, const Fvector& last_seen)
{
    const auto I = g_enemy_claims.find(enemy_id);
    if (I == g_enemy_claims.end() || !I->second.sweep_start)
        return 0;
    if (Device.dwTimeGlobal > I->second.sweep_start + CStalkerActionSearchEnemy::SWEEP_TIME)
        return 0;
    // seen again since and lost somewhere else: that is a new search
    if (I->second.sweep_from.distance_to_sqr(last_seen) > _sqr(10.f))
        return 0;
    return I->second.sweep_start;
}

// "I'll go and look" once per loss of him, not every time a step aside drops a
// stalker out of the search and back in
static bool stalker_first_to_say_go(u16 enemy_id, const Fvector& last_seen)
{
    SEnemyClaims& claims = g_enemy_claims[enemy_id];
    if (claims.go_time && Device.dwTimeGlobal < claims.go_time + 30000 && claims.go_from.distance_to_sqr(last_seen) < _sqr(10.f))
        return false;
    claims.go_time = Device.dwTimeGlobal;
    claims.go_from = last_seen;
    return true;
}

// the sweep for this loss of him ran its full time
static bool stalker_sweep_over(u16 enemy_id, const Fvector& last_seen)
{
    const auto I = g_enemy_claims.find(enemy_id);
    return I != g_enemy_claims.end() && I->second.sweep_start && I->second.sweep_from.distance_to_sqr(last_seen) <= _sqr(10.f) &&
        Device.dwTimeGlobal > I->second.sweep_start + CStalkerActionSearchEnemy::SWEEP_TIME;
}

static void stalker_sweep_begin(u16 enemy_id, const Fvector& last_seen)
{
    SEnemyClaims& claims = g_enemy_claims[enemy_id];
    if (stalker_sweep_start(enemy_id, last_seen))
        return;
    claims.sweep_start = Device.dwTimeGlobal;
    claims.sweep_from = last_seen;
    claims.checked.clear();
    claims.checked.push_back(last_seen); // the searcher has just looked there
}

u16 stalker_search_holder(u16 enemy_id)
{
    if (Commander::Owns(enemy_id))
        return Commander::Searcher(enemy_id);
    const auto I = g_enemy_claims.find(enemy_id);
    return I != g_enemy_claims.end() && fresh(I->second.searcher) ? I->second.searcher.npc : u16(-1);
}

bool CStalkerActionSearchEnemy::claim_search(u16 enemy_id, u16 npc)
{
    SClaim& searcher = g_enemy_claims[enemy_id].searcher;
    if (fresh(searcher) && searcher.npc != npc)
        return false;

    searcher = {npc, Device.dwTimeGlobal};
    return true;
}

// The free sector nearest to where the stalker already is, so nobody crosses the
// line of fire to get to his; the one he holds if he has one. u32(-1) when all
// sectors are taken.
u32 CStalkerActionSearchEnemy::claim_watch_slot(u16 enemy_id, u16 npc, float bearing)
{
    SEnemyClaims& claims = g_enemy_claims[enemy_id];
    for (u32 i = 0; i < WATCH_SLOTS; ++i)
    {
        if (claims.slots[i].npc == npc)
        {
            claims.slots[i].time = Device.dwTimeGlobal;
            return i;
        }
    }

    u32 best = u32(-1);
    float best_difference = flt_max;
    for (u32 i = 0; i < WATCH_SLOTS; ++i)
    {
        if (fresh(claims.slots[i]))
            continue;

        const float difference = angle_difference(bearing, float(i) * PI_MUL_2 / float(WATCH_SLOTS));
        if (difference < best_difference)
        {
            best_difference = difference;
            best = i;
        }
    }

    if (best != u32(-1))
        claims.slots[best] = {npc, Device.dwTimeGlobal};
    return best;
}

bool stalker_claim_sudden_attack(u16 enemy_id, u16 npc)
{
    SClaim& sudden = g_enemy_claims[enemy_id].sudden;
    if (fresh(sudden) && sudden.npc != npc)
        return false;

    sudden = {npc, Device.dwTimeGlobal};
    return true;
}

bool stalker_claim_suppress(u16 enemy_id, u16 npc)
{
    SClaim& suppressor = g_enemy_claims[enemy_id].suppressor;
    if (suppressor.npc != npc && suppressor.npc != u16(-1) && Device.dwTimeGlobal < suppressor.time + SUPPRESS_TTL)
        return false;
    suppressor = {npc, Device.dwTimeGlobal};
    return true;
}

u16 stalker_suppressor(u16 enemy_id)
{
    if (Commander::Owns(enemy_id))
        return Commander::Suppressor(enemy_id);
    const auto I = g_enemy_claims.find(enemy_id);
    if (I == g_enemy_claims.end())
        return u16(-1);
    const SClaim& suppressor = I->second.suppressor;
    return suppressor.npc != u16(-1) && Device.dwTimeGlobal < suppressor.time + SUPPRESS_TTL ? suppressor.npc : u16(-1);
}

bool stalker_claim_flank(u16 enemy_id, u16 npc)
{
    SEnemyClaims& claims = g_enemy_claims[enemy_id];
    for (SClaim& flanker : claims.flankers)
    {
        if (flanker.npc == npc)
        {
            flanker.time = Device.dwTimeGlobal;
            return true;
        }
    }
    for (SClaim& flanker : claims.flankers)
    {
        if (!fresh(flanker))
        {
            flanker = {npc, Device.dwTimeGlobal};
            return true;
        }
    }
    return false;
}

// "covering fire!" once per bout of it, whoever of the suppressors gets to it
bool stalker_first_to_cry_suppress(u16 enemy_id)
{
    if (Commander::Owns(enemy_id))
        return Commander::FirstToCry(enemy_id, eStalkerSoundNeedBackup, 10000);
    SEnemyClaims& claims = g_enemy_claims[enemy_id];
    if (claims.suppress_cried && Device.dwTimeGlobal < claims.suppress_cried + 10000)
        return false;
    claims.suppress_cried = Device.dwTimeGlobal;
    return true;
}

bool stalker_is_flanker(u16 enemy_id, u16 npc)
{
    if (Commander::Owns(enemy_id))
        return Commander::Flanker(enemy_id, npc);
    const auto I = g_enemy_claims.find(enemy_id);
    if (I == g_enemy_claims.end())
        return false;
    for (const SClaim& flanker : I->second.flankers)
        if (flanker.npc == npc && fresh(flanker))
            return true;
    return false;
}

void stalker_release_claims(u16 npc)
{
    for (auto& [enemy_id, claims] : g_enemy_claims)
    {
        if (claims.searcher.npc == npc)
            claims.searcher = {};
        if (claims.sudden.npc == npc)
            claims.sudden = {};
        if (claims.suppressor.npc == npc)
            claims.suppressor = {};
        for (SClaim& flanker : claims.flankers)
            if (flanker.npc == npc)
                flanker = {};
        for (SClaim& slot : claims.slots)
            if (slot.npc == npc)
                slot = {};
    }
}

EStalkerDbgRole stalker_dbg_role(u16 enemy_id, u16 npc, string64& text)
{
    if (Commander::Owns(enemy_id))
        return Commander::DbgRole(enemy_id, npc, text);
    text[0] = 0;
    const auto I = g_enemy_claims.find(enemy_id);
    if (I == g_enemy_claims.end())
        return eStalkerDbgRoleNone;
    const SEnemyClaims& claims = I->second;

    // as the claims themselves lapse: a role not refreshed is no role
    if (fresh(claims.searcher) && claims.searcher.npc == npc)
    {
        xr_strcpy(text, claims.sweep_start ? "searcher (sweep)" : "searcher");
        return eStalkerDbgRoleSearcher;
    }
    if (fresh(claims.sudden) && claims.sudden.npc == npc)
    {
        xr_strcpy(text, "sudden attack");
        return eStalkerDbgRoleSudden;
    }
    if (stalker_suppressor(enemy_id) == npc)
    {
        xr_strcpy(text, "suppress");
        return eStalkerDbgRoleSuppressor;
    }
    if (stalker_is_flanker(enemy_id, npc))
    {
        xr_strcpy(text, "flank");
        return eStalkerDbgRoleFlanker;
    }
    const SSiege& siege = claims.siege;
    for (u32 k = 0; k < siege.openings.size(); ++k)
        for (u32 slot = 0; slot < SIEGE_POSTS; ++slot)
        {
            const SPost& post = siege.openings[k].posts[slot];
            if (post.claim.npc == npc && post_fresh(post.claim))
            {
                xr_sprintf(text, "post %s #%u/%u", siege.openings[k].door ? "door" : "window", k, slot + 1);
                return eStalkerDbgRolePost;
            }
        }
    for (u32 i = 0; i < WATCH_SLOTS; ++i)
        if (claims.slots[i].npc == npc && fresh(claims.slots[i]))
        {
            xr_sprintf(text, "watch sector %u", i);
            return eStalkerDbgRoleWatch;
        }
    return eStalkerDbgRoleNone;
}

bool stalker_dbg_siege(u16 enemy_id, SStalkerDbgSiege& out)
{
    if (Commander::Owns(enemy_id))
        return Commander::DbgHouse(enemy_id, out);
    out = {};
    const auto I = g_enemy_claims.find(enemy_id);
    if (I == g_enemy_claims.end() || !I->second.siege.start)
        return false;
    const SSiege& siege = I->second.siege;
    out.indoors = siege.indoors;
    out.start = siege.start;
    out.origin = siege.origin;
    out.openings.reserve(siege.openings.size());
    for (const SOpening& o : siege.openings)
        out.openings.push_back({o.mouth, o.inside, o.dir, o.width, o.door, struck_off(o)});
    return true;
}

void stalker_dbg_forget(u16 enemy_id)
{
    g_enemy_claims.erase(enemy_id);
    Commander::Forget(enemy_id);
}

namespace
{
constexpr u32 DBG_ROLE_COLOR[] = {
    color_rgba(200, 200, 200, 230), // none
    color_rgba(255, 150, 40, 255), // searcher
    color_rgba(255, 60, 200, 255), // sudden
    color_rgba(255, 70, 60, 255), // suppressor
    color_rgba(255, 220, 60, 255), // flanker
    color_rgba(80, 255, 120, 255), // post
    color_rgba(80, 200, 255, 255), // watch
};

void dbg_draw_stalkers(const Fvector& eye, float range)
{
    using namespace AIDebugDraw;
    const bool roles = on(eRoles), moves = on(eMoves);
    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CObject* O = Level().Objects.o_get_by_iterator(i);
        CAI_Stalker* npc = O && !O->getDestroy() ? smart_cast<CAI_Stalker*>(O) : nullptr;
        if (!npc || !npc->g_Alive() || eye.distance_to_sqr(npc->Position()) > _sqr(range))
            continue;
        // the fight only: a camp of fifty idle men would bury it
        const CEntityAlive* enemy = npc->memory().enemy().selected();
        if (!enemy)
            continue;

        const Fvector& p = npc->Position();
        string64 role_text;
        const EStalkerDbgRole role = stalker_dbg_role(enemy->ID(), npc->ID(), role_text);
        const u32 color = DBG_ROLE_COLOR[role];

        if (roles)
        {
            string32 action;
            stalker_dbg_action(*npc, action);
            const bool sees = npc->memory().visual().visible_now(enemy);
            Text(Fvector(p).add(Fvector{0.f, 2.25f, 0.f}), color, 0, "%s\n%s: %s%s", role == eStalkerDbgRoleNone ? "- no role -" : role_text, npc->Name(), action,
                 sees ? " | SEES" : "");
        }

        if (moves)
        {
            const Fvector feet = Fvector(p).add(Fvector{0.f, .1f, 0.f});
            const u32 dest_vertex = npc->movement().level_dest_vertex_id();
            if (ai().level_graph().valid_vertex_id(dest_vertex) && !npc->movement().path_completed())
            {
                const Fvector dest = Fvector(ai().level_graph().vertex_position(dest_vertex)).add(Fvector{0.f, .1f, 0.f});
                Line(feet, dest, color_rgba(230, 230, 230, 140));
                Cross(dest, .2f, color_rgba(230, 230, 230, 200));
            }
            const CMemberOrder& order = npc->agent_manager().member().member(npc);
            if (const CCoverPoint* cover = order.cover())
            {
                const Fvector c = Fvector(cover->position()).add(Fvector{0.f, .15f, 0.f});
                Cross(c, .3f, color_rgba(255, 160, 40, 230));
                Line(feet, c, color_rgba(255, 160, 40, 110));
            }
            // where he thinks the enemy is: the one point the threat map will
            // replace -- watch it jump at each sound
            const CMemoryInfo info = npc->memory().memory(enemy);
            if (info.m_object)
            {
                const Fvector spot = Fvector(info.m_object_params.m_position).add(Fvector{0.f, 1.f, 0.f});
                Cross(spot, .25f, color_rgba(255, 50, 50, 230));
                Line(Fvector(p).add(Fvector{0.f, 1.7f, 0.f}), spot, color_rgba(255, 50, 50, 70));
            }
        }
    }
}

void dbg_draw_houses(const Fvector& eye, float range)
{
    using namespace AIDebugDraw;
    for (const auto& [enemy_id, claims] : g_enemy_claims)
    {
        const SSiege& siege = claims.siege;
        if (!siege.indoors || eye.distance_to_sqr(siege.origin) > _sqr(range + AIHouse::RADIUS))
            continue;

        Ring(siege.origin, AIHouse::RADIUS, color_rgba(200, 200, 200, 70));
        const u32 held = (Device.dwTimeGlobal - siege.start) / 1000;
        Text(Fvector(siege.origin).add(Fvector{0.f, 3.f, 0.f}), color_rgba(255, 255, 255, 230), 0, "house: %u opening(s), held %u s of %u", u32(siege.openings.size()), held,
             SIEGE_HOLD / 1000);

        for (u32 k = 0; k < siege.openings.size(); ++k)
        {
            const SOpening& o = siege.openings[k];
            const bool off = struck_off(o);
            const u32 color = off ? color_rgba(150, 150, 150, 200) : o.door ? color_rgba(60, 255, 120, 255) : color_rgba(80, 200, 255, 255);
            const Fvector side{o.dir.z, 0.f, -o.dir.x};
            const Fvector l = Fvector(o.mouth).mad(side, o.width * .5f), r = Fvector(o.mouth).mad(side, -o.width * .5f);
            const Fvector up{0.f, .35f, 0.f};
            Line(l, r, color);
            Line(Fvector(l).sub(up), Fvector(l).add(up), color);
            Line(Fvector(r).sub(up), Fvector(r).add(up), color);
            Line(o.mouth, Fvector(o.mouth).mad(o.dir, 1.f), color); // outwards
            Cross(o.inside, .1f, color);
            Text(Fvector(o.mouth).add(Fvector{0.f, .55f, 0.f}), color, 0, "#%u %s %.1fm%s", k, o.door ? "door" : o.knee ? "window (knee)" : "window", o.width,
                 off ? " blind" : "");

            for (const SPost& post : o.posts)
            {
                if (!post.placed)
                    continue;
                const bool held_now = post_fresh(post.claim);
                const Fvector at = Fvector(post.place).add(Fvector{0.f, 1.f, 0.f});
                Cross(at, .3f, held_now ? color : color_rgba(150, 150, 150, 150));
                if (held_now)
                    Line(at, o.inside, subst_alpha(color, 120));
            }
        }
    }
}
} // namespace

void stalker_dbg_draw(const Fvector& eye, float range)
{
    if (AIDebugDraw::on(AIDebugDraw::eRoles | AIDebugDraw::eMoves))
        dbg_draw_stalkers(eye, range);
    if (AIDebugDraw::on(AIDebugDraw::eHouses))
    {
        dbg_draw_houses(eye, range);
        Commander::DbgDrawHouses(eye, range);
        AIHouse::DbgDraw(eye, range);
    }
    if (AIDebugDraw::on(AIDebugDraw::eThreat))
        ThreatMap::DbgDraw(eye, range);
}

namespace
{
// Where everybody else in the fight against this enemy is, whatever his squad:
// the holders of its claims, and the posts' places. The position query keeps
// a new place apart from these.
void fight_places(u16 enemy_id, u16 self, xr_vector<Fvector>& out)
{
    const auto I = g_enemy_claims.find(enemy_id);
    if (I == g_enemy_claims.end())
        return;
    const SEnemyClaims& claims = I->second;
    // where he is going, if he is on his way: two men running for one place
    // must see each other's goal, not each other's back
    const auto add = [&](const SClaim& claim) {
        if (claim.npc == self || claim.npc == u16(-1) || !fresh(claim))
            return;
        CAI_Stalker* S = smart_cast<CAI_Stalker*>(Level().Objects.net_Find(claim.npc));
        if (!S)
            return;
        const u32 dest = S->movement().level_dest_vertex_id();
        out.push_back(!S->movement().path_completed() && ai().level_graph().valid_vertex_id(dest) ? ai().level_graph().vertex_position(dest) : S->Position());
    };
    add(claims.searcher);
    add(claims.suppressor);
    for (const SClaim& c : claims.flankers)
        add(c);
    for (const SClaim& c : claims.slots)
        add(c);
    for (const SOpening& o : claims.siege.openings)
        for (const SPost& post : o.posts)
            if (post.placed && post.claim.npc != self && post_fresh(post.claim))
                out.push_back(post.place);
}

void fight_house(u16 enemy_id, PositionQuery::SRequest& r)
{
    const auto I = g_enemy_claims.find(enemy_id);
    if (I == g_enemy_claims.end() || !I->second.siege.indoors)
        return;
    r.house = true;
    r.house_origin = I->second.siege.origin;
    r.house_radius = AIHouse::RADIUS;
}
} // namespace

// The siege: keep a post on an opening of the house he was last seen in -- a
// cover outside that sees in through it, else bare ground that does, apart from
// the other posts -- aiming in through it, crouched if that still sees in. An
// opening nothing outside sees into is struck off. See SSiege.
bool CStalkerActionSearchEnemy::execute_post(const Fvector& enemy_position, u16 enemy_id, u32 seen)
{
    SSiege* siege = siege_at(enemy_id, enemy_position, seen);
    // no post for him: he watches as round a spot in the open -- nearer in, a
    // house still being kept
    const auto drop = [&] {
        if (m_post >= 0)
        {
            m_watch_cover = nullptr; // a post's cover: pick one for the open anew
            m_watch_cover_time = 0;
        }
        m_post = -1;
        m_post_vertex = u32(-1);
        m_post_surplus = !!siege;
        if (siege)
            m_siege_spot = siege->spot;
        return false;
    };
    if (!siege)
        return drop();

    // ai_tactics_v2: the post's place was asked of the position query; its answer
    // does what the loops below do with theirs
    if (m_post_pending)
    {
        PositionQuery::SResult answer;
        const u16 npc = u16(m_object->ID());
        if (!PositionQuery::Poll(npc, PositionQuery::ePost, answer))
        {
            if (PositionQuery::Pending(npc, PositionQuery::ePost))
            {
                object().sight().setup(CSightAction(SightManager::eSightTypePosition, m_post_target, true));
                return true; // a frame: he goes on as he was
            }
            m_post_pending = false; // forgotten: claim it anew
            m_post = -1;
        }
        else
        {
            m_post_pending = false;
            // the house read anew meanwhile: the opening may be another one now
            if (m_post < 0 || m_post_built != siege->version || u32(m_post) / SIEGE_POSTS >= siege->openings.size())
                m_post = -1;
            else
            {
                SOpening& opening = siege->openings[m_post / int(SIEGE_POSTS)];
                const u32 slot = u32(m_post) % SIEGE_POSTS;
                SPost& place = opening.posts[slot];
                if (!answer.found)
                {
                    if (slot == 0 && !opening.door)
                    {
                        opening.blind = true;
                        opening.blind_time = Device.dwTimeGlobal;
                    }
                    else
                        opening.full[slot] = true;
                    place.claim = {};
                    if (g_dbg_npc_tactics)
                        Msg("[tac] %u '%s' finds nowhere to keep %s #%d (man %u, position query): %s", npc, m_object->Name(), opening.door ? "door" : "window",
                            m_post / int(SIEGE_POSTS), slot + 1, slot == 0 && !opening.door ? "struck off" : "no room");
                    return drop();
                }
                m_watch_cover = answer.point.cover;
                m_post_vertex = answer.point.vertex;
                m_post_position = answer.point.position;
                m_watch_stand = answer.stand;
                place.placed = true;
                place.place = m_post_position;
                opening.blind = false;
                if (g_dbg_npc_tactics)
                    Msg("[tac] %u '%s' keeps %s #%d (man %u) %s (%.1f,%.1f,%.1f), %s (position query, score %.2f)", npc, m_object->Name(), opening.door ? "door" : "window",
                        m_post / int(SIEGE_POSTS), slot + 1, m_watch_cover ? "from cover" : "from open ground", VPUSH(m_post_position), m_watch_stand ? "standing" : "crouched",
                        answer.score);
                object().agent_manager().member().member(m_object).cover(m_watch_cover);
            }
        }
    }

    if (!m_post_time || Device.dwTimeGlobal > m_post_time + 1000 || m_post_built != siege->version)
    {
        m_post_time = Device.dwTimeGlobal;
        const int post = claim_post(*siege, m_object->ID(), object().Position());
        // more of us than places to keep: watch as round a spot in the open
        if (post < 0)
            return drop();

        if (post != m_post || m_post_built != siege->version)
        {
            SOpening& opening = siege->openings[post / int(SIEGE_POSTS)];
            SPost& place = opening.posts[post % int(SIEGE_POSTS)];
            m_post = post;
            m_post_built = siege->version;
            const u32 slot = u32(post) % SIEGE_POSTS;
            const bool flank = opening.door && slot > 0;
            const u16 npc = m_object->ID();
            m_watch_cover = nullptr;
            m_post_vertex = u32(-1);
            Fvector floor = opening.mouth;
            floor.y -= opening.height;
            const Fvector side{opening.dir.z, 0.f, -opening.dir.x};
            // why a place was not found, for the instrument: covers round it, in
            // front and outside, reachable, seeing in; bare ground the same
            struct
            {
                u32 covers, front, reach, view, ground, ground_reach;
            } count{};
            // ...and for the overlay, every point looked at and its verdict
            // (the first, spaced pass only: the second repeats it)
            const bool dbg = AIDebugDraw::on(AIDebugDraw::eCandidates);
            const auto candidate = [&](const Fvector& p, float score, char verdict) {
                if (dbg)
                    AIDebugDraw::Candidate(npc, AIDebugDraw::eChannelPost, m_post_time, p, score, verdict);
            };

            if (flank)
            {
                // ⭐Against the wall beside the door, out of the line from inside:
                // whoever comes out comes out at arm's length. The second man the
                // nearer side, the third the other.
                // 📏06-10 15:15: posts on a door 20-22 m off across the road,
                // behind the fence there -- "the best cover" knows how well a
                // place hides, not how far it is.
                m_post_target = Fvector(opening.mouth).mad(opening.dir, .8f);
                const Fvector deep = Fvector(opening.mouth).mad(opening.dir, -1.5f);
                float sides[] = {1.f, -1.f};
                if (side.dotproduct(Fvector().sub(object().Position(), opening.mouth)) < 0.f)
                    std::swap(sides[0], sides[1]);
                for (const float sign : sides)
                    for (const float out : {opening.width * .5f + 1.f, opening.width * .5f + 1.6f, opening.width * .5f + 2.3f})
                    {
                        if (m_post_vertex != u32(-1))
                            break;
                        Fvector spot = Fvector(floor).mad(side, sign * out).mad(opening.dir, .7f);
                        const u32 vertex = ai().level_graph().vertex_id(spot);
                        if (!ai().level_graph().valid_vertex_id(vertex) || !object().movement().accessible(vertex))
                            continue;
                        const Fvector position = ai().level_graph().vertex_position(vertex);
                        if (post_crowded(*siege, position, npc, SIEGE_FLANK_SPACING))
                        {
                            candidate(position, 0.f, 'n');
                            continue;
                        }
                        const Fvector eye = Fvector(position).add(Fvector{0.f, 1.5f, 0.f});
                        // hidden from inside, and the doorway in sight
                        if (!ray_blocked(deep, eye, 0.f) || ray_blocked(eye, m_post_target, .2f))
                        {
                            candidate(position, 0.f, 'v');
                            continue;
                        }
                        candidate(position, 0.f, ' ');
                        m_post_vertex = vertex;
                        m_post_position = position;
                    }
                m_watch_stand = true;
            }
            else if (PositionQuery::g_ai_tactics_v2)
            {
                // in front of it, about SIEGE_WATCH_MARK off, seeing in at the
                // muzzle's height, outside the house, apart from the other posts:
                // asked, and answered next frame (m_post_pending above)
                using namespace PositionQuery;
                m_post_target = opening.inside;
                SRequest r;
                r.kind = ePost;
                r.npc = npc;
                r.self = object().Position();
                r.threat = enemy_position;
                r.focus = opening.inside;
                r.focus_dir = opening.dir;
                r.focus_floor = floor;
                r.near_ = SIEGE_WATCH_NEAR;
                r.ideal = SIEGE_WATCH_MARK;
                r.far_ = SIEGE_WATCH_FAR;
                fight_house(enemy_id, r);
                for (const SOpening& o : siege->openings)
                    for (const SPost& other : o.posts)
                        if (other.placed && other.claim.npc != npc && post_fresh(other.claim))
                            r.friends.push_back(other.place);
                r.spacing = SIEGE_POST_SPACING * .75f;
                GatherCovers(r, *m_object, floor, SIEGE_WATCH_FAR, 40, SIEGE_WATCH_MARK);
                for (const float out : {7.f, 5.f, 9.f, 11.f, 4.f})
                    for (const float aside : {0.f, 2.f, -2.f})
                        GatherPoint(r, *m_object, Fvector(floor).mad(opening.dir, out).mad(side, aside));
                Issue(std::move(r));
                m_post_pending = true;
                object().sight().setup(CSightAction(SightManager::eSightTypePosition, m_post_target, true));
                return true;
            }
            else
            {
                m_post_target = opening.inside;
                // in through it to anywhere in the room behind: by the window, a
                // step in, the middle -- 📏06-10 16:14: one point just inside the
                // back window, and none of 35 places outside saw it
                const Fvector into[] = {opening.inside, Fvector(opening.mouth).mad(opening.dir, -1.5f), Fvector(opening.mouth).mad(opening.dir, -3.f)};
                const auto post_view = [&into](const Fvector& position) {
                    // crouched if that sees anywhere in, else standing
                    EWatchView best = eWatchViewNone;
                    for (const Fvector& target : into)
                    {
                        const EWatchView view = view_into(position, target, .3f, true);
                        if (view == eWatchViewCrouch)
                            return view;
                        if (view != eWatchViewNone)
                            best = view;
                    }
                    return best;
                };
                // A place in front of the opening that sees in through it, near
                // rather than far -- 5-9 m is the mark -- and the more covered
                // from the house the better; apart from the other posts if it
                // can be, next to one rather than leave the opening unwatched.
                xr_vector<CCoverPoint*> nearest;
                ai().cover_manager().covers().nearest(floor, SIEGE_WATCH_FAR, nearest);
                count.covers = u32(nearest.size());
                const CStalkerMovementRestrictor restrictor(m_object, true, false);
                for (const bool spaced : {true, false})
                {
                    if (m_post_vertex != u32(-1))
                        break;
                    float best = flt_max;
                    for (const CCoverPoint* cover : nearest)
                    {
                        const Fvector& position = cover->position();
                        Fvector away = Fvector().sub(position, floor);
                        away.y = 0.f;
                        const float distance = away.magnitude();
                        const auto reject = [&](char verdict) {
                            if (spaced)
                                candidate(position, 0.f, verdict);
                        };
                        // the cheap tests first, as before; the verdict says which
                        if (away.dotproduct(opening.dir) < 1.f)
                        {
                            reject('f');
                            continue;
                        }
                        if (distance < SIEGE_WATCH_NEAR || distance > SIEGE_WATCH_FAR)
                        {
                            reject('d');
                            continue;
                        }
                        if (siege_inside(*siege, position))
                        {
                            reject('i');
                            continue;
                        }
                        count.front += spaced;
                        if (!object().movement().accessible(cover->level_vertex_id()) || !restrictor(cover))
                        {
                            reject('r');
                            continue;
                        }
                        count.reach += spaced;
                        if (post_crowded(*siege, position, npc, spaced ? SIEGE_POST_SPACING : SIEGE_FLANK_SPACING))
                        {
                            reject('n');
                            continue;
                        }
                        if (post_view(position) == eWatchViewNone)
                        {
                            reject('v');
                            continue;
                        }
                        count.view += spaced;
                        float yaw, pitch;
                        Fvector().sub(floor, position).getHP(yaw, pitch);
                        const float covered = std::min(ai().level_graph().high_cover_in_direction(yaw, cover->level_vertex_id()),
                                                       ai().level_graph().low_cover_in_direction(yaw, cover->level_vertex_id()));
                        const float score = _abs(distance - SIEGE_WATCH_MARK) * .25f + covered * 2.f;
                        if (spaced)
                            candidate(position, score, ' ');
                        if (score < best)
                        {
                            best = score;
                            m_watch_cover = cover;
                            m_post_position = position;
                            m_post_vertex = cover->level_vertex_id();
                        }
                    }
                    // no cover sees in there: the open ground along the line in,
                    // or a little to either side of it
                    for (const float out : {7.f, 5.f, 9.f, 11.f, 4.f})
                        for (const float aside : {0.f, 2.f, -2.f})
                        {
                            if (m_post_vertex != u32(-1))
                                break;
                            const Fvector spot = Fvector(floor).mad(opening.dir, out).mad(side, aside);
                            const u32 vertex = ai().level_graph().vertex_id(spot);
                            if (!ai().level_graph().valid_vertex_id(vertex))
                                continue;
                            const Fvector position = ai().level_graph().vertex_position(vertex);
                            const auto reject = [&](char verdict) {
                                if (spaced)
                                    candidate(position, 0.f, verdict);
                            };
                            if (siege_inside(*siege, position))
                            {
                                reject('i');
                                continue;
                            }
                            count.ground += spaced;
                            if (!object().movement().accessible(vertex))
                            {
                                reject('r');
                                continue;
                            }
                            count.ground_reach += spaced;
                            if (post_crowded(*siege, position, npc, spaced ? SIEGE_POST_SPACING : SIEGE_FLANK_SPACING))
                            {
                                reject('n');
                                continue;
                            }
                            if (post_view(position) == eWatchViewNone)
                            {
                                reject('v');
                                continue;
                            }
                            candidate(position, 0.f, ' ');
                            m_post_vertex = vertex;
                            m_post_position = position;
                        }
                }
                m_watch_stand = m_post_vertex != u32(-1) && post_view(m_post_position) == eWatchViewStand;
            }

            if (m_post_vertex == u32(-1))
            {
                // the first man finds nowhere: nobody will; another: no room
                // a door with nowhere in front still has its sides
                // -- for a while: another man, another way round, may find one
                if (slot == 0 && !opening.door)
                {
                    opening.blind = true;
                    opening.blind_time = Device.dwTimeGlobal;
                }
                else
                    opening.full[slot] = true;
                place.claim = {};
                if (g_dbg_npc_tactics)
                    Msg("[tac] %u '%s' finds nowhere to keep %s #%d from (man %u): %s | covers %u, in front %u, reachable %u, seeing in %u; ground %u, reachable %u | out [%s] in [%s]",
                        npc, m_object->Name(), opening.door ? "door" : "window", post / int(SIEGE_POSTS), slot + 1, slot == 0 ? "struck off" : "no room", count.covers,
                        count.front, count.reach, count.view, count.ground, count.ground_reach, object().movement().restrictions().out_restrictions().c_str(),
                        object().movement().restrictions().in_restrictions().c_str());
                return drop();
            }

            place.placed = true;
            place.place = m_post_position;
            opening.blind = false;
            if (g_dbg_npc_tactics)
                Msg("[tac] %u '%s' keeps %s #%d (man %u) %s (%.1f,%.1f,%.1f) %.1f m off, %s", npc, m_object->Name(), opening.door ? "door" : "window",
                    post / int(SIEGE_POSTS), slot + 1, flank ? "beside it at" : m_watch_cover ? "from cover" : "from open ground", VPUSH(m_post_position),
                    m_post_position.distance_to(floor), m_watch_stand ? "standing" : "crouched");
        }
        object().agent_manager().member().member(m_object).cover(m_watch_cover);
    }
    if (m_post < 0)
        return false; // none this second either
    m_post_surplus = false;

    object().movement().set_level_dest_vertex(m_post_vertex);
    object().movement().set_desired_position(&m_post_position);

    if (object().movement().path_completed())
    {
        object().movement().set_body_state(m_watch_stand ? eBodyStateStand : eBodyStateCrouch);
        object().movement().set_movement_type(eMovementTypeStand);
        if (object().agent_manager().member().can_cry_noninfo_phrase())
            object().sound().play(eStalkerSoundComeOut, 3000, 1000, 15000, 9000);
    }
    else
    {
        object().movement().set_body_state(eBodyStateStand);
        object().movement().set_movement_type(eMovementTypeRun);
    }

    // in through his opening; the bursts go in if he shows there
    object().sight().setup(CSightAction(SightManager::eSightTypePosition, m_post_target, true));

    // the hold is over: one of the posts goes in, the rest keep theirs
    if (Device.dwTimeGlobal > siege->start + SIEGE_HOLD && Device.dwTimeGlobal > m_watch_start + WATCH_TIME && claim_search(enemy_id, m_object->ID()))
    {
        m_watcher = false;
        m_phase = eSearchApproach;
        m_stacked = false;
        m_stack_vertex = u32(-1);
        m_stack_time = 0;
        m_phase_time = Device.dwTimeGlobal;
        object().agent_manager().member().member(m_object).cover(0);
    }
    return true;
}

// ai_tactics_v2: the watcher's place through the position query -- round the
// spot, seeing it, covered from it, apart from everybody else in the fight
// (the sectors as a cost on bearing, not eight fixed slots). Asked when the
// spot moved, or again a while after nothing was found; the answer is taken
// the frame after.
void CStalkerActionSearchEnemy::pick_watch_v2(const Fvector& enemy_position, u16 enemy_id)
{
    using namespace PositionQuery;
    const u16 npc = u16(m_object->ID());
    SResult answer;
    if (Poll(npc, eWatch, answer))
    {
        m_watch_cover = answer.found ? answer.point.cover : nullptr;
        m_watch_ground_v2 = answer.found && !answer.point.cover;
        if (answer.found)
        {
            m_watch_ground = answer.point.position;
            m_watch_ground_vertex = answer.point.vertex;
        }
        m_watch_stand = answer.found && answer.stand;
        m_watch_blind = !answer.found;
        object().agent_manager().member().member(m_object).cover(m_watch_cover);
    }

    if (m_watch_cover_time && Device.dwTimeGlobal < m_watch_cover_time + 1000)
        return;
    m_watch_cover_time = Device.dwTimeGlobal;

    // the role, as before: a watcher holds a sector claim (the overlay and the
    // autotest read it); the place is the query's
    Fvector from_enemy = Fvector().sub(object().Position(), enemy_position);
    claim_watch_slot(enemy_id, npc, atan2f(from_enemy.x, from_enemy.z));

    const bool moved = m_watch_point.distance_to_sqr(enemy_position) > _sqr(WATCH_REPICK_SHIFT);
    const bool nothing = !m_watch_cover && !m_watch_ground_v2 && Device.dwTimeGlobal > m_watch_asked + 3000;
    if (!(moved || nothing) || Pending(npc, eWatch))
        return;
    m_watch_point = enemy_position;
    m_watch_asked = Device.dwTimeGlobal;

    SRequest r;
    r.kind = eWatch;
    r.npc = npc;
    r.self = object().Position();
    r.threat = enemy_position;
    r.focus = enemy_position;
    // round a house being kept by those with no post: nearer in (WATCH_RADIUS_SIEGE)
    r.near_ = m_post_surplus ? 4.f : 6.f;
    r.ideal = m_post_surplus ? WATCH_RADIUS_SIEGE : 14.f;
    r.far_ = m_post_surplus ? 16.f : 22.f;
    fight_house(enemy_id, r);
    fight_places(enemy_id, npc, r.friends);
    r.spacing = 3.f;
    GatherCovers(r, *m_object, enemy_position, r.far_, 40, r.ideal);
    GatherRings(r, *m_object, enemy_position, {r.ideal - 4.f, r.ideal, r.ideal + 4.f}, 8);
    Issue(std::move(r));
}

// The stalkers who did not go to look surround the enemy's last known position:
// each takes his own sector around it, whatever his squad, and holds a cover
// about WATCH_RADIUS out, crouched and aiming at it, shouting at him to come out.
// Whoever shows himself there is in their sights; after WATCH_TIME they give up.
void CStalkerActionSearchEnemy::execute_watch(const Fvector& remembered, u16 enemy_id, u32 seen)
{
    // the searcher found the place empty: everybody combs the area now
    if (stalker_sweep_start(enemy_id, remembered))
    {
        object().agent_manager().member().member(m_object).cover(0);
        execute_sweep(remembered, enemy_id);
        return;
    }

    {
        LoadProf::Scope _lp("srch.post");
        if (execute_post(remembered, enemy_id, seen))
            return;
    }

    // no post for him on a house being kept: round the house, not round where
    // he himself last had him -- 📏06-10 15:51: three men 33-42 m off, ringing
    // a stale sighting in the village street
    const Fvector& enemy_position = m_post_surplus ? m_siege_spot : remembered;

    // 📏06-10 13:59: re-picked every second, from a spot that moved with each
    // sound and a slot taken by the angle he stood at on the way, Герман Радист
    // changed his cover four times in three seconds. Keep it while the spot
    // stays put and it still sees it.
    bool repick = false;
    if (PositionQuery::g_ai_tactics_v2)
        pick_watch_v2(enemy_position, enemy_id);
    else if (!m_watch_cover_time || Device.dwTimeGlobal > m_watch_cover_time + 1000)
    {
        m_watch_cover_time = Device.dwTimeGlobal;
        repick = !m_watch_cover || m_watch_point.distance_to_sqr(enemy_position) > _sqr(WATCH_REPICK_SHIFT) || (!m_watch_blind && watch_view(m_watch_cover->position(), enemy_position) == eWatchViewNone);
    }
    if (repick)
    {
        LoadProf::Scope _lp("srch.repick"); // best_cover: watch_view rays for every candidate
        m_watch_cover_time = Device.dwTimeGlobal;
        m_watch_point = enemy_position;

        Fvector from_enemy;
        from_enemy.sub(object().Position(), enemy_position);
        const u32 slot = claim_watch_slot(enemy_id, m_object->ID(), atan2f(from_enemy.x, from_enemy.z));

        const CCoverPoint* point = nullptr;
        if (slot != u32(-1))
        {
            const float angle = float(slot) * PI_MUL_2 / float(WATCH_SLOTS);
            Fvector sector_point = enemy_position;
            const float radius = m_post_surplus ? WATCH_RADIUS_SIEGE : WATCH_RADIUS;
            sector_point.x += _sin(angle) * radius;
            sector_point.z += _cos(angle) * radius;
            m_watch_ground = sector_point;
            m_watch_ground_vertex = ai().level_graph().vertex_id(sector_point);
            if (!ai().level_graph().valid_vertex_id(m_watch_ground_vertex))
                m_watch_ground_vertex = ai().level_graph().nearest_vertex_id(sector_point);

            // the most hidden place he can still see the spot from; a blind
            // wall only if there is nothing else
            const auto sees_spot = [&enemy_position, enemy_id](const CCoverPoint* cover) {
                return !stalker_siege_inside(enemy_id, cover->position()) && watch_view(cover->position(), enemy_position) != eWatchViewNone;
            };
            for (const float radius : m_post_surplus ? std::initializer_list<float>{5.f, 9.f} : std::initializer_list<float>{8.f, 16.f})
            {
                object().m_ce_best->setup(enemy_position, 10.f, 35.f, 10.f, sees_spot);
                point = ai().cover_manager().best_cover(sector_point, radius, *object().m_ce_best, CStalkerMovementRestrictor(m_object, true));
                // the evaluator may hand back a choice it cached a moment ago
                if (point && watch_view(point->position(), enemy_position) != eWatchViewNone)
                    break;
                point = nullptr;
            }
        }

        // round a house being kept, no cover in his sector: as near to it as he
        // may go, not "the best cover" from where he stands -- 📏06-10 16:00: that
        // sent the men with no post 32-45 m off, across the street
        if (!point && !m_post_surplus)
            point = object().best_cover(enemy_position);
        if (point != m_watch_cover)
        {
            m_watch_cover = point;
            const EWatchView view = point ? watch_view(point->position(), enemy_position) : eWatchViewNone;
            m_watch_stand = view == eWatchViewStand;
            m_watch_blind = view == eWatchViewNone;
        }
        object().agent_manager().member().member(m_object).cover(m_watch_cover);
    }

    if (const CCoverPoint* point = m_watch_cover)
    {
        object().movement().set_level_dest_vertex(point->level_vertex_id());
        object().movement().set_desired_position(&point->position());
    }
    else if ((m_post_surplus || m_watch_ground_v2) && ai().level_graph().valid_vertex_id(m_watch_ground_vertex))
        object().movement().set_nearest_accessible_position(m_watch_ground, m_watch_ground_vertex);
    else
        object().movement().set_nearest_accessible_position();

    if (object().movement().path_completed())
    {
        object().movement().set_body_state(m_watch_stand ? eBodyStateStand : eBodyStateCrouch);
        object().movement().set_movement_type(eMovementTypeStand);
        if (object().agent_manager().member().can_cry_noninfo_phrase())
            object().sound().play(eStalkerSoundComeOut, 3000, 1000, 15000, 9000);
    }
    else
    {
        object().movement().set_body_state(eBodyStateStand);
        object().movement().set_movement_type(eMovementTypeRun);
    }

    object().sight().setup(CSightAction(SightManager::eSightTypePosition, enemy_position, true));

    // ⚠No giving up here. The memory of an enemy is the squad's: one watcher
    // forgetting him made the whole squad forget at once, the searcher on his
    // way included. Only the end of the sweep lets go. A searcher who is gone
    // -- dead, or pulled out by a script -- stops refreshing his claim, and a
    // watcher takes his place instead.
    if (Device.dwTimeGlobal > m_watch_start + WATCH_TIME && claim_search(enemy_id, m_object->ID()))
    {
        m_watcher = false;
        m_phase = eSearchApproach;
        m_stacked = false;
        m_stack_vertex = u32(-1);
        m_stack_time = 0;
        m_phase_time = Device.dwTimeGlobal;
        object().agent_manager().member().member(m_object).cover(0);
    }
}

bool CStalkerActionSearchEnemy::look_around(u32 since, u32 duration, float heading)
{
    static constexpr float turns[] = {0.f, PI_DIV_2, -PI_DIV_2, PI};
    const u32 elapsed = Device.dwTimeGlobal - since;
    const u32 turn = std::min<u32>(elapsed * u32(std::size(turns)) / std::max<u32>(duration, 1), u32(std::size(turns)) - 1);

    Fvector direction;
    direction.setHP(heading + turns[turn], 0.f);
    object().sight().setup(CSightAction(SightManager::eSightTypeDirection, direction, true));
    return elapsed >= duration;
}

// Combing the area: walk to a hiding place near where he was last seen -- one
// that hides from where we came, since that is where he was running from --
// look round there, then the next one. Each stalker his own places, nobody the
// same one twice; the watchers comb out from their own sectors. Then give up.
void CStalkerActionSearchEnemy::execute_sweep(const Fvector& enemy_position, u16 enemy_id)
{
    const u32 start = stalker_sweep_start(enemy_id, enemy_position);
    if (!start)
    {
        if (stalker_sweep_over(enemy_id, enemy_position))
        {
            // the squad combed the area for SWEEP_TIME: he is gone. Everybody's
            // clock is the same call, so they all let go together.
            extern int g_dbg_npc_tactics;
            if (g_dbg_npc_tactics)
                Msg("[tac] %u '%s' gives up on %u after the sweep (%u places checked by the squad)", m_object->ID(), m_object->Name(), enemy_id,
                    u32(g_enemy_claims[enemy_id].checked.size()));
            object().memory().enable(object().memory().enemy().selected(), false);
            return;
        }

        // he turned up somewhere else (heard, seen): the searcher goes there,
        // a watcher back to watching
        m_phase = eSearchApproach;
        m_phase_time = Device.dwTimeGlobal;
        m_sweep_going = false;
        m_sweep_points = 0;
        return;
    }

    if (!m_sweep_going && m_sweep_points >= SWEEP_POINTS)
    {
        // my places are done: hold, gun on the spot, until the others are too
        object().movement().set_body_state(eBodyStateStand);
        object().movement().set_movement_type(eMovementTypeStand);
        object().sight().setup(CSightAction(SightManager::eSightTypePosition, enemy_position, true));
        return;
    }

    SEnemyClaims& claims = g_enemy_claims[enemy_id];
    if (!m_sweep_going)
    {
        // the searcher round the spot itself, a watcher out on his own side of it
        Fvector center = enemy_position;
        if (m_watcher)
        {
            Fvector side;
            side.sub(object().Position(), enemy_position);
            side.y = 0.f;
            if (side.square_magnitude() > EPS_L)
                center.mad(side.normalize(), 10.f);
        }

        const auto free_place = [&claims](const CCoverPoint* point) { return !sweep_taken(claims, point->position()); };
        const CCoverPoint* point = nullptr;
        for (const float radius : {15.f, 25.f})
        {
            object().m_ce_best->setup(m_search_origin, 0.f, 170.f, 0.f, free_place);
            point = ai().cover_manager().best_cover(center, radius, *object().m_ce_best, CStalkerMovementRestrictor(m_object, true));
            // the evaluator may hand back a choice it cached a moment ago
            if (point && !sweep_taken(claims, point->position()))
                break;
            point = nullptr;
        }

        if (point)
        {
            m_sweep_target = point->position();
            m_sweep_vertex = point->level_vertex_id();
        }
        else if (!sweep_open_ground(center, enemy_id))
        {
            // nowhere left to look: keep the gun on the spot until the time is up
            object().movement().set_body_state(eBodyStateStand);
            object().movement().set_movement_type(eMovementTypeStand);
            object().sight().setup(CSightAction(SightManager::eSightTypePosition, enemy_position, true));
            if (Device.dwTimeGlobal > start + SWEEP_TIME)
                object().memory().enable(object().memory().enemy().selected(), false);
            return;
        }

        claims.checked.push_back(m_sweep_target);
        m_sweep_going = true;
        m_sweep_arrived = false;
        ++m_sweep_points;
    }

    object().movement().set_level_dest_vertex(m_sweep_vertex);
    object().movement().set_desired_position(&m_sweep_target);
    object().movement().set_body_state(eBodyStateStand);

    if (!object().movement().path_completed())
    {
        object().movement().set_movement_type(eMovementTypeWalk);
        object().sight().setup(CSightAction(SightManager::eSightTypePathDirection, true, true));
        return;
    }

    if (!m_sweep_arrived)
    {
        m_sweep_arrived = true;
        m_sweep_arrive_time = Device.dwTimeGlobal;
    }

    object().movement().set_movement_type(eMovementTypeStand);
    Fvector out;
    out.sub(m_sweep_target, enemy_position);
    float heading, pitch;
    out.getHP(heading, pitch);
    if (look_around(m_sweep_arrive_time, SWEEP_LOOK_TIME, heading))
        m_sweep_going = false;
}

// No hiding place left round the spot -- or none the cover points know of: a
// watcher crouched behind a gate found nothing and sat there the whole sweep.
// Walk out to a bit of open ground near it instead, nearest first.
bool CStalkerActionSearchEnemy::sweep_open_ground(const Fvector& center, u16 enemy_id)
{
    const SEnemyClaims& claims = g_enemy_claims[enemy_id];
    for (const float radius : {6.f, 12.f})
    {
        for (u32 i = 0; i < 8; ++i)
        {
            const float angle = float(i) * PI_MUL_2 / 8.f;
            const Fvector probe{center.x + _sin(angle) * radius, center.y, center.z + _cos(angle) * radius};
            const u32 vertex = ai().level_graph().nearest_vertex_id(probe);
            if (!ai().level_graph().valid_vertex_id(vertex) || !object().movement().accessible(vertex))
                continue;

            const Fvector position = ai().level_graph().vertex_position(vertex);
            // the grid snapped it inside a house or onto a roof: not this one
            if (position.distance_to_xz(probe) > 3.f || _abs(position.y - center.y) > 3.f)
                continue;
            if (sweep_taken(claims, position) || position.distance_to_xz(object().Position()) < 4.f)
                continue;

            m_sweep_target = position;
            m_sweep_vertex = vertex;
            extern int g_dbg_npc_tactics;
            if (g_dbg_npc_tactics)
                Msg("[tac] %u '%s' sweep: no hiding place left, open ground at (%.1f,%.1f)", m_object->ID(), m_object->Name(), position.x, position.z);
            return true;
        }
    }
    return false;
}

// ⭐Going into a house held against us: to the wall beside its door first,
// crouched there for STACK_TIME with the gun on the doorway, then in. Before,
// the searcher walked straight at the door from wherever he was. True while
// that takes him; false when there is no house, no door, or it is done.
bool CStalkerActionSearchEnemy::stack_at_door(u16 enemy_id, const Fvector& last_seen, u32 seen)
{
    constexpr u32 STACK_TIME = 2000;
    if (m_stacked)
        return false;

    SSiege* siege = siege_at(enemy_id, last_seen, seen);
    if (!siege)
    {
        m_stacked = true;
        return false;
    }

    if (m_stack_vertex == u32(-1))
    {
        // the nearest door, the nearer side of it
        const SOpening* door = nullptr;
        for (const SOpening& o : siege->openings)
            if (o.door && (!door || object().Position().distance_to_sqr(o.mouth) < object().Position().distance_to_sqr(door->mouth)))
                door = &o;
        if (!door)
        {
            m_stacked = true;
            return false;
        }

        Fvector floor = door->mouth;
        floor.y -= door->height;
        const Fvector side{door->dir.z, 0.f, -door->dir.x};
        float sides[] = {1.f, -1.f};
        if (side.dotproduct(Fvector().sub(object().Position(), door->mouth)) < 0.f)
            std::swap(sides[0], sides[1]);
        for (const float sign : sides)
            for (const float out : {door->width * .5f + .9f, door->width * .5f + 1.5f})
            {
                if (m_stack_vertex != u32(-1))
                    break;
                const Fvector spot = Fvector(floor).mad(side, sign * out).mad(door->dir, .7f);
                const u32 vertex = ai().level_graph().vertex_id(spot);
                if (!ai().level_graph().valid_vertex_id(vertex) || !object().movement().accessible(vertex))
                    continue;
                m_stack_vertex = vertex;
                m_stack_position = ai().level_graph().vertex_position(vertex);
            }
        m_stack_aim = Fvector(door->mouth).mad(door->dir, .5f);
        if (m_stack_vertex == u32(-1))
        {
            m_stacked = true;
            return false;
        }
        if (g_dbg_npc_tactics)
            Msg("[tac] %u '%s' going in: to the wall by the door first, (%.1f,%.1f,%.1f)", m_object->ID(), m_object->Name(), VPUSH(m_stack_position));
    }

    object().movement().set_level_dest_vertex(m_stack_vertex);
    object().movement().set_desired_position(&m_stack_position);
    object().sight().setup(CSightAction(SightManager::eSightTypePosition, m_stack_aim, true));

    if (!object().movement().path_completed() && object().Position().distance_to_sqr(m_stack_position) > _sqr(1.f))
    {
        object().movement().set_body_state(eBodyStateStand);
        object().movement().set_movement_type(eMovementTypeRun);
        m_stack_time = 0;
        return true;
    }

    object().movement().set_body_state(eBodyStateCrouch);
    object().movement().set_movement_type(eMovementTypeStand);
    if (!m_stack_time)
        m_stack_time = Device.dwTimeGlobal;
    else if (Device.dwTimeGlobal > m_stack_time + STACK_TIME)
    {
        m_stacked = true;
        m_phase_time = Device.dwTimeGlobal; // the approach's own clock starts now
    }
    return true;
}

void CStalkerActionSearchEnemy::execute()
{
    inherited::execute();

    CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

    if (!mem_object.m_object)
        return;

    const u16 enemy_id = mem_object.m_object->ID();
    const Fvector& last_seen = mem_object.m_object_params.m_position;
    // [frame prof] parts: 📏07-10-2026 Switch v218 one execute took 26 ms of a frame
    // holed up in a house: nobody walks in at his gun yet, everybody keeps a way out
    bool siege_holds;
    {
        LoadProf::Scope _lp("srch.siege");
        siege_holds = m_watcher || stalker_siege_holds(enemy_id, last_seen, mem_object.m_level_time);
    }
    if (siege_holds)
    {
        LoadProf::Scope _lp("srch.watch");
        execute_watch(last_seen, enemy_id, mem_object.m_level_time);
        return;
    }

    // keep it fresh while walking up; lost while the siege held: another went
    if (!claim_search(enemy_id, m_object->ID()))
    {
        m_watcher = true;
        LoadProf::Scope _lp("srch.watch");
        execute_watch(last_seen, enemy_id, mem_object.m_level_time);
        return;
    }

    switch (m_phase)
    {
    case eSearchApproach: {
        // "going to check": the squad hears who goes and where
        if (stalker_first_to_say_go(enemy_id, last_seen))
            object().sound().play(eStalkerSoundSearchGo, 0, 0, 10000, 8000);
        // into a house he holds: not straight in at the door, but to the wall
        // beside it first, a breath there with the gun on the doorway
        {
            LoadProf::Scope _lp("srch.door");
            if (stack_at_door(enemy_id, last_seen, mem_object.m_level_time))
                return;
        }
        // straight to where he was last seen, gun on the spot, the last metres
        // crouched: he may be lying in wait right there
        {
            LoadProf::Scope _lp("srch.dest");
            if (object().movement().accessible(mem_object.m_object_params.m_level_vertex_id))
                object().movement().set_level_dest_vertex(mem_object.m_object_params.m_level_vertex_id);
            else
                object().movement().set_nearest_accessible_position(last_seen, mem_object.m_object_params.m_level_vertex_id);
        }
        object().movement().set_desired_position(nullptr);

        const bool close = object().Position().distance_to(last_seen) < 8.f;
        object().movement().set_body_state(close ? eBodyStateCrouch : eBodyStateStand);
        object().movement().set_movement_type(eMovementTypeWalk);
        object().sight().setup(CSightAction(SightManager::eSightTypePosition, last_seen, true));

        // a point he cannot get to must not keep him walking for ever
        if (object().movement().path_completed() || Device.dwTimeGlobal > m_phase_time + APPROACH_TIME)
        {
            m_phase = eSearchCheck;
            m_phase_time = Device.dwTimeGlobal;
        }
        return;
    }
    case eSearchCheck: {
        object().movement().set_body_state(eBodyStateStand);
        object().movement().set_movement_type(eMovementTypeStand);

        Fvector way;
        way.sub(last_seen, m_search_origin);
        float heading, pitch;
        way.getHP(heading, pitch);
        if (!look_around(m_phase_time, CHECK_TIME, heading))
            return;

        // not here: "spread out, find him"
        m_phase = eSearchSweep;
        m_phase_time = Device.dwTimeGlobal;
        stalker_sweep_begin(enemy_id, last_seen);
        // "he's not here": enemy_lost_, or the search lines for packs without it
        object().sound().play(eStalkerSoundEnemyLost, 0, 0, 10000, 8000);
        return;
    }
    case eSearchSweep: {
        LoadProf::Scope _lp("srch.sweep");
        execute_sweep(last_seen, enemy_id);
        return;
    }
    }
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionPostCombatWait
//////////////////////////////////////////////////////////////////////////

CStalkerActionPostCombatWait::CStalkerActionPostCombatWait(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionPostCombatWait::initialize()
{
    inherited::initialize();

    object().movement().set_movement_type(eMovementTypeStand);

    EObjectAction action = eObjectActionAimReady1;
    if (m_storage->property(eWorldPropertyKilledWounded))
        action = eObjectActionIdle;

    if (object().inventory().ActiveItem() && object().best_weapon() && (object().inventory().ActiveItem()->object().ID() == object().best_weapon()->object().ID()))
        object().set_goal(action, object().best_weapon());
    else
    {
        if (object().inventory().m_slots[1].m_pIItem)
        {
            CWeaponMagazined* temp = smart_cast<CWeaponMagazined*>(object().inventory().m_slots[1].m_pIItem);
            if (object().inventory().ActiveItem() && temp && (object().inventory().ActiveItem()->object().ID() == temp->ID()))
                object().set_goal(action, object().inventory().m_slots[1].m_pIItem);
        }
    }

    if (m_storage->property(eWorldPropertyKilledWounded))
        return;

    if (object().memory().enemy().last_enemy() && object().memory().visual().visible_now(object().memory().enemy().last_enemy()))
        object().sight().setup(CSightAction(object().memory().enemy().last_enemy(), true, true));
}

void CStalkerActionPostCombatWait::execute() { inherited::execute(); }

void CStalkerActionPostCombatWait::finalize() { inherited::finalize(); }

//////////////////////////////////////////////////////////////////////////
// CStalkerActionGetDistance
//////////////////////////////////////////////////////////////////////////

CStalkerActionGetDistance::CStalkerActionGetDistance(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionGetDistance::initialize()
{
    inherited::initialize();
    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_mental_state(eMentalStateDanger);
    object().movement().set_body_state(eBodyStateStand);
    object().movement().set_movement_type(eMovementTypeRun);
}

void CStalkerActionGetDistance::execute()
{
    inherited::execute();

    if (!object().memory().enemy().selected())
        return;

    CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

    if (!mem_object.m_object)
        return;

    Fvector position = mem_object.m_object_params.m_position;

    if (object().inventory().ActiveItem() && object().best_weapon() && (object().inventory().ActiveItem()->object().ID() == object().best_weapon()->object().ID()))
        aim_ready();

    object().sight().setup(CSightAction(SightManager::eSightTypePosition, position, true));

    if (!object().movement().path_completed() && (Device.dwTimeGlobal >= (mem_object.m_level_time + 1000)))
        return;

    object().m_ce_best_by_time->setup(position, 10.f, object().ffGetRange(), 10.f);
    const CCoverPoint* point = ai().cover_manager().best_cover(object().Position(), 10.f, *object().m_ce_best_by_time, CStalkerMovementRestrictor(m_object, true));
    if (!point)
    {
        object().m_ce_best_by_time->setup(position, 10.f, object().ffGetRange(), 10.f);
        point = ai().cover_manager().best_cover(object().Position(), 30.f, *object().m_ce_best_by_time, CStalkerMovementRestrictor(m_object, true));
    }

    if (point)
    {
        object().movement().set_level_dest_vertex(point->level_vertex_id());
        object().movement().set_desired_position(&point->position());
    }
    else
        object().movement().set_nearest_accessible_position();
}

void CStalkerActionGetDistance::finalize() { inherited::finalize(); }

//////////////////////////////////////////////////////////////////////////
// CStalkerActionHideFromGrenade
//////////////////////////////////////////////////////////////////////////

CStalkerActionHideFromGrenade::CStalkerActionHideFromGrenade(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionHideFromGrenade::initialize()
{
    inherited::initialize();

    m_storage->set_property(eWorldPropertyUseSuddenness, false);

    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_mental_state(eMentalStateDanger);
    object().movement().set_body_state(eBodyStateStand);
    object().movement().set_movement_type(eMovementTypeRun);
    object().m_ce_best->invalidate();

    m_storage->set_property(eWorldPropertyInCover, false);
    m_storage->set_property(eWorldPropertyLookedOut, false);
    m_storage->set_property(eWorldPropertyPositionHolded, false);
    m_storage->set_property(eWorldPropertyEnemyDetoured, false);
}

void CStalkerActionHideFromGrenade::execute()
{
    inherited::execute();

    if (!object().memory().danger().selected())
        return;

    Fvector position = object().memory().danger().selected()->position();
    const CCoverPoint* point = object().best_cover(position);
    if (point)
    {
        object().movement().set_level_dest_vertex(point->level_vertex_id());
        object().movement().set_desired_position(&point->position());
        object().movement().set_movement_type(eMovementTypeRun);
    }
    else
    {
        object().movement().set_movement_type(eMovementTypeStand);
        object().movement().set_body_state(eBodyStateCrouch);
    }

    if (!object().memory().enemy().selected())
        object().sight().setup(CSightAction(SightManager::eSightTypePathDirection, true, true));
    else
    {
        CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

        if (!mem_object.m_object)
        { // || (!m_object->memory().visual().visible_now(object().memory().enemy().selected()) && !object().movement().path_completed())) {
            object().sight().setup(CSightAction(SightManager::eSightTypePathDirection, true, true));
            aim_ready();
        }
        else
        {
            if (!m_object->memory().visual().visible_now(object().memory().enemy().selected()))
            {
                //				object().sight().setup				(CSightAction(SightManager::eSightTypePosition,mem_object.m_object_params.m_position,true));
                object().sight().setup(CSightAction(SightManager::eSightTypePathDirection, true, true));
                aim_ready();
            }
            else
            {
                object().sight().setup(CSightAction(object().memory().enemy().selected(), true, true));
                fire();
            }
        }
    }

    if (object().movement().path_completed())
        object().movement().set_body_state(eBodyStateCrouch);
}

void CStalkerActionHideFromGrenade::finalize() { inherited::finalize(); }

//////////////////////////////////////////////////////////////////////////
// CStalkerActionSuddenAttack
//////////////////////////////////////////////////////////////////////////

CStalkerActionSuddenAttack::CStalkerActionSuddenAttack(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionSuddenAttack::initialize()
{
    inherited::initialize();

    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_mental_state(eMentalStateDanger);

    if (!object().memory().enemy().selected())
        return;

    aim_ready();
}

void CStalkerActionSuddenAttack::finalize()
{
    inherited::finalize();
    stalker_release_claims(m_object->ID());
}

void CStalkerActionSuddenAttack::execute()
{
    inherited::execute();

    if (object().agent_manager().member().combat_members().size() > 1)
        m_storage->set_property(eWorldPropertyUseSuddenness, false);

    if (!object().memory().enemy().selected())
        return;

    // The squad check above is per squad, and a village is several small ones:
    // without this, every one of them rushed the player's doorway at once.
    if (!stalker_claim_sudden_attack(object().memory().enemy().selected()->ID(), m_object->ID()))
    {
        m_storage->set_property(eWorldPropertyUseSuddenness, false);
        return;
    }

    CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

    if (!mem_object.m_object)
        return;

    bool visible_now = object().memory().visual().visible_now(object().memory().enemy().selected());
    if (visible_now)
        object().sight().setup(CSightAction(object().memory().enemy().selected(), true));
    else
        object().sight().setup(CSightAction(SightManager::eSightTypePosition, mem_object.m_object_params.m_position, true));

    if (object().movement().accessible(mem_object.m_object_params.m_level_vertex_id))
        object().movement().set_level_dest_vertex(mem_object.m_object_params.m_level_vertex_id);
    else
        object().movement().set_nearest_accessible_position(ai().level_graph().vertex_position(mem_object.m_object_params.m_level_vertex_id),
                                                            mem_object.m_object_params.m_level_vertex_id);

    float distance = object().Position().distance_to(mem_object.m_object_params.m_position);
    if (distance >= 15.f)
    {
        object().movement().set_body_state(eBodyStateStand);
        object().movement().set_movement_type(eMovementTypeRun);
    }
    else
    {
        if (distance >= 8.f)
        {
            object().movement().set_body_state(eBodyStateStand);
            object().movement().set_movement_type(eMovementTypeWalk);
        }
        else
        {
            if (distance >= 6.f)
            {
                object().movement().set_body_state(eBodyStateCrouch);
                object().movement().set_movement_type(eMovementTypeRun);
            }
            else
            {
                if ((distance >= 4.f) || !visible_now)
                {
                    object().movement().set_body_state(eBodyStateCrouch);
                    object().movement().set_movement_type(eMovementTypeRun);
                }
                else
                {
                    object().movement().set_body_state(eBodyStateCrouch);
                    object().movement().set_movement_type(eMovementTypeStand);

                    fire();
                }
            }
        }
    }

    CVisualMemoryManager* visual_memory_manager = object().memory().enemy().selected()->visual_memory();
    VERIFY(visual_memory_manager);
    if (!visual_memory_manager->visible_now(&object()))
        return;

    m_storage->set_property(eWorldPropertyUseSuddenness, false);
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionKillEnemyIfPlayerOnThePath
//////////////////////////////////////////////////////////////////////////

CStalkerActionKillEnemyIfPlayerOnThePath::CStalkerActionKillEnemyIfPlayerOnThePath(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionKillEnemyIfPlayerOnThePath::initialize()
{
    inherited::initialize();

    object().movement().set_mental_state(eMentalStateDanger);
    object().movement().set_movement_type(eMovementTypeStand);
    object().movement().force_update(true);

    m_storage->set_property(eWorldPropertyInCover, false);
    m_storage->set_property(eWorldPropertyLookedOut, false);
    m_storage->set_property(eWorldPropertyPositionHolded, false);
    m_storage->set_property(eWorldPropertyEnemyDetoured, false);

    play_attack_sound(0, 0, 6000, 4000);

    object().brain().affect_cover(true);
}

void CStalkerActionKillEnemyIfPlayerOnThePath::finalize()
{
    inherited::finalize();

    object().movement().force_update(false);
}

void CStalkerActionKillEnemyIfPlayerOnThePath::execute()
{
    inherited::execute();

    object().sight().setup(CSightAction(object().memory().enemy().selected(), true, true));

    fire();

    CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());
    Fvector position = mem_object.m_object_params.m_position;
    const CCoverPoint* point = object().best_cover(position);
    if (point)
    {
        object().movement().set_level_dest_vertex(point->level_vertex_id());
        object().movement().set_desired_position(&point->position());
    }
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionCriticalHit
//////////////////////////////////////////////////////////////////////////

CStalkerActionCriticalHit::CStalkerActionCriticalHit(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionCriticalHit::initialize()
{
    inherited::initialize();

    object().brain().affect_cover(false);
    object().movement().set_movement_type(eMovementTypeStand);

    if (object().memory().enemy().selected())
    {
        u32 min_queue_size, max_queue_size, min_queue_interval, max_queue_interval;
        float distance = object().memory().enemy().selected()->Position().distance_to(object().Position());
        select_queue_params(distance, min_queue_size, max_queue_size, min_queue_interval, max_queue_interval);
        object().CObjectHandler::set_goal(eObjectActionIdle, object().best_weapon(), min_queue_size, max_queue_size, min_queue_interval, max_queue_interval);
    }
    else
        object().CObjectHandler::set_goal(eObjectActionIdle, object().best_weapon());

    object().sight().setup(CSightAction(SightManager::eSightTypeCurrentDirection, true, true));
    object().sound().play(eStalkerSoundInjuring);
}

void CStalkerActionCriticalHit::finalize()
{
    inherited::finalize();
    m_storage->set_property(eWorldPropertyCriticallyWounded, false);
}

void CStalkerActionCriticalHit::execute() { inherited::execute(); }
