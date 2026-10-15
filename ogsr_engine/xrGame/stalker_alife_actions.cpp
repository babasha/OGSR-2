////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_alife_actions.cpp
//	Created 	: 25.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker alife action classes
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_alife_actions.h"
#include "ai/stalker/ai_stalker.h"
#include "inventory_item.h"
#include "script_game_object.h"
#include "Inventory.h"
#include "WeaponMagazined.h"
#include "movement_manager_space.h"
#include "detail_path_manager_space.h"
#include "memory_manager.h"
#include "item_manager.h"
#include "sight_manager.h"
#include "ai_object_location.h"
#include "stalker_movement_manager.h"
#include "patrol_path_manager.h"
#include "sound_player.h"
#include "ai/stalker/ai_stalker_space.h"
#include "restricted_object.h"
// "is this creature already carrying its weapon slung" -- see
// stalker_weapon_handling_deadline.
#include "object_handler_planner.h"
#include "object_handler_space.h"

using namespace StalkerSpace;

u32 stalker_weapon_handling_deadline(CAI_Stalker& object)
{
    const u32 now = Device.dwTimeGlobal;

    // No weapon of its own to think about: nothing to hold, nothing to grant.
    const bool carries_best =
        object.inventory().ActiveItem() && object.best_weapon() && (object.inventory().ActiveItem()->object().ID() == object.best_weapon()->object().ID());

    CObjectHandlerPlanner& oh = object.CObjectHandler::planner();
    const bool slung = oh.initialized() && !!oh.m_storage.property(ObjectHandlerSpace::eWorldPropertyStrapped);

    const bool grant = carries_best && !slung;
    const u32 window = grant ? (::Random32.random(30000) + 30000) : 0;

    // ⭐The re-entry, named at the moment it happens. That the deadline was
    // being RESET was an inference from a flip period far shorter than the
    // window it hands out; an inference is not a measurement, and this channel
    // has already cost two evenings to hypotheses that read as obviously true.
    // ⚠Throttled by count, never silenced: after the cap it keeps a running
    // total, so a quiet log means "no more re-entries", not "the counter ran
    // out".
    static u32 s_entries = 0;
    ++s_entries;
    if (s_entries <= 24 || 0 == (s_entries % 200))
        Msg("~ [strap] window: %u '%s' -> %s | carries_best=%d already_slung=%d (%u re-entries so far)", u16(object.ID()), object.cName().c_str(),
            grant ? "HOLD IT OUT" : "leave it where it is", carries_best ? 1 : 0, slung ? 1 : 0, s_entries);

    return now + window;
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionNoALife
//////////////////////////////////////////////////////////////////////////

CStalkerActionNoALife::CStalkerActionNoALife(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionNoALife::initialize()
{
    inherited::initialize();
    object().movement().set_desired_position(0);
    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeGamePath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_body_state(eBodyStateStand);
    object().movement().set_movement_type(eMovementTypeWalk);
    object().movement().set_mental_state(eMentalStateFree);
    object().sight().setup(CSightAction(SightManager::eSightTypeCover, false, true));

    m_stop_weapon_handling_time = stalker_weapon_handling_deadline(object());
}

void CStalkerActionNoALife::finalize()
{
    inherited::finalize();

    object().movement().set_desired_position(0);

    if (!object().g_Alive())
        return;

    object().sound().remove_active_sounds(u32(eStalkerSoundMaskNoHumming));
}

void CStalkerActionNoALife::execute()
{
    inherited::execute();
    object().sound().play(eStalkerSoundHumming, 60000, 10000);
    if (Device.dwTimeGlobal >= m_stop_weapon_handling_time)
        if (!object().best_weapon())
            object().CObjectHandler::set_goal(eObjectActionIdle);
        else
            object().CObjectHandler::set_goal(eObjectActionStrapped, object().best_weapon());
    else
        object().CObjectHandler::set_goal(eObjectActionIdle, object().best_weapon());
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionGatherItems
//////////////////////////////////////////////////////////////////////////

CStalkerActionGatherItems::CStalkerActionGatherItems(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionGatherItems::initialize()
{
    inherited::initialize();
    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_body_state(eBodyStateStand);
    object().movement().set_movement_type(eMovementTypeWalk);
    object().movement().set_mental_state(eMentalStateDanger);
    object().sound().remove_active_sounds(u32(eStalkerSoundMaskNoHumming));
    if (!object().inventory().ActiveItem())
        object().CObjectHandler::set_goal(eObjectActionIdle);
    else
        object().CObjectHandler::set_goal(eObjectActionIdle, object().inventory().ActiveItem());
}

void CStalkerActionGatherItems::finalize()
{
    inherited::finalize();

    object().movement().set_desired_position(0);

    if (!object().g_Alive())
        return;

    object().sound().set_sound_mask(0);
}

void CStalkerActionGatherItems::execute()
{
    inherited::execute();

    if (!object().memory().item().selected())
        return;

    u32 level_vertex_id = object().memory().item().selected()->ai_location().level_vertex_id();
    object().movement().set_level_dest_vertex(level_vertex_id);
    object().movement().set_desired_position(&object().memory().item().selected()->Position());

    object().sight().setup(SightManager::eSightTypePosition, &object().memory().item().selected()->Position());
}
