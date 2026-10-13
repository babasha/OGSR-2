#include "StdAfx.h"
#include "stalker_combat_task_actions.h"

#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_space.h"
#include "script_game_object.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "visual_memory_manager.h"
#include "sight_manager.h"
#include "sight_action.h"
#include "stalker_movement_manager.h"
#include "movement_manager_space.h"
#include "detail_path_manager_space.h"
#include "sound_player.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "ai_space.h"
#include "level_graph.h"
#include "ai_commander.h"

using namespace StalkerSpace;

constexpr u32 STUCK_TIME = 4000; // not a metre nearer his place in this long: he gets no nearer
constexpr float FACE_THE_WAY = 8.f; // farther from his place than this, he looks where he runs

CStalkerPropertyEvaluatorHasTask::CStalkerPropertyEvaluatorHasTask(CAI_Stalker* object, LPCSTR evaluator_name)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

CStalkerPropertyEvaluatorHasTask::_value_type CStalkerPropertyEvaluatorHasTask::evaluate() { return !!Commander::Task(*m_object); }

CStalkerPropertyEvaluatorAtPlace::CStalkerPropertyEvaluatorAtPlace(CAI_Stalker* object, LPCSTR evaluator_name)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

CStalkerPropertyEvaluatorAtPlace::_value_type CStalkerPropertyEvaluatorAtPlace::evaluate() { return Commander::AtPlace(*m_object); }

//////////////////////////////////////////////////////////////////////////
// CStalkerActionReachPlace
//////////////////////////////////////////////////////////////////////////

CStalkerActionReachPlace::CStalkerActionReachPlace(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionReachPlace::initialize()
{
    inherited::initialize();
    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_mental_state(eMentalStateDanger);
    object().movement().set_body_state(eBodyStateStand);
    object().movement().set_movement_type(eMovementTypeWalk);
    aim_ready();
    m_stuck_position = object().Position();
    m_stuck_time = Device.dwTimeGlobal;
    m_completed_since = 0;
}

void CStalkerActionReachPlace::finalize() { inherited::finalize(); }

void CStalkerActionReachPlace::execute()
{
    inherited::execute();
    const Commander::STask* task = Commander::Task(object());
    const CEntityAlive* enemy = object().memory().enemy().selected();
    if (!task || !enemy)
        return;

    m_target = task->position;
    const CLevelGraph& graph = ai().level_graph();
    u32 vertex = task->vertex;
    if (!graph.valid_vertex_id(vertex))
        vertex = graph.vertex_id(m_target);
    if (!graph.valid_vertex_id(vertex))
    {
        Commander::Unreachable(object());
        return;
    }
    if (object().movement().accessible(vertex))
    {
        object().movement().set_level_dest_vertex(vertex);
        object().movement().set_desired_position(&m_target);
    }
    else
        object().movement().set_nearest_accessible_position(m_target, vertex);

    object().movement().set_body_state(eBodyStateStand);
    object().movement().set_movement_type(task->run ? eMovementTypeRun : eMovementTypeWalk);

    // he shows on the way: fire at him, walking
    if (object().memory().visual().visible_now(enemy))
    {
        object().sight().setup(CSightAction(enemy, true, true));
        if (fire_make_sense())
            fire();
        else
            aim_ready();
    }
    else if (object().Position().distance_to_sqr(m_target) > _sqr(FACE_THE_WAY))
    {
        // far off: eyes on the way -- 📏06-10 20:04, posts sent 40 m ran sideways
        // with the gun on the window at 1-2 m/s, the ring 20-35 s late
        object().sight().setup(CSightAction(SightManager::eSightTypePathDirection, true, true));
        aim_ready();
    }
    else
    {
        object().sight().setup(CSightAction(SightManager::eSightTypePosition, task->focus, true));
        aim_ready();
    }

    // 📏06-10 13:59: a man "going round" 15 s with his goal 0.2 m off -- another
    // stood on it, the path never completed. Not a metre in STUCK_TIME, or the
    // path done short of it: the place is where he got to.
    if (Device.dwTimeGlobal > m_stuck_time + STUCK_TIME)
    {
        if (object().Position().distance_to_sqr(m_stuck_position) < _sqr(1.f))
            Commander::Unreachable(object());
        m_stuck_position = object().Position();
        m_stuck_time = Device.dwTimeGlobal;
    }
    // the path done short of it, and not just because a new one is being built
    if (!object().movement().path_completed() || m_target.distance_to_sqr(m_completed_for) > _sqr(.5f))
    {
        m_completed_since = 0;
        m_completed_for = m_target;
    }
    else if (!m_completed_since)
        m_completed_since = Device.dwTimeGlobal;
    else if (Device.dwTimeGlobal > m_completed_since + 600)
        Commander::Unreachable(object());
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionFightFromPlace
//////////////////////////////////////////////////////////////////////////

CStalkerActionFightFromPlace::CStalkerActionFightFromPlace(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name) {}

void CStalkerActionFightFromPlace::initialize()
{
    inherited::initialize();
    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_mental_state(eMentalStateDanger);
    object().movement().set_movement_type(eMovementTypeStand);
    aim_ready();
    m_serial = u32(-1);
}

void CStalkerActionFightFromPlace::finalize() { inherited::finalize(); }

void CStalkerActionFightFromPlace::execute()
{
    inherited::execute();
    const Commander::STask* task = Commander::Task(object());
    const CEntityAlive* enemy = object().memory().enemy().selected();
    if (!task || !enemy)
        return;

    if (task->give_up)
    {
        object().memory().enable(enemy, false);
        return;
    }

    // a new task, or here for this one now
    if (task->serial != m_serial)
    {
        m_serial = task->serial;
        m_arrived = Device.dwTimeGlobal;
        m_done = false;
        m_cried = false;
        // looking round: first the way he came, out from the spot if he was
        // sent away from it
        Fvector out = Fvector().sub(task->position, task->focus);
        out.y = 0.f;
        float pitch;
        if (task->place && out.square_magnitude() > 1.f)
            out.getHP(m_heading, pitch);
        else
            m_heading = -object().movement().body_orientation().current.yaw;
    }

    // keep the place, or the ground he stands on
    if (task->place)
    {
        m_target = task->position;
        const CLevelGraph& graph = ai().level_graph();
        if (graph.valid_vertex_id(task->vertex) && object().movement().accessible(task->vertex))
        {
            object().movement().set_level_dest_vertex(task->vertex);
            object().movement().set_desired_position(&m_target);
        }
        else
            object().movement().set_nearest_accessible_position();
    }
    else
        object().movement().set_nearest_accessible_position();
    // the last metre too: 📏06-10 21:57, a post 'at his place' 1.1 m off it,
    // crouched where it saw nothing of the window it was for
    const bool short_of = task->place && !object().movement().path_completed() && object().Position().distance_to_xz(m_target) > .4f;
    object().movement().set_movement_type(short_of ? eMovementTypeWalk : eMovementTypeStand);
    object().movement().set_body_state(task->stand || short_of ? eBodyStateStand : eBodyStateCrouch);

    // he shows: fire from here. 📏06-10 16:27: the stock fired only "in cover"
    // and ran to its own cover first -- posts left 9-20 m off for it.
    if (object().memory().visual().visible_now(enemy))
    {
        object().sight().setup(CSightAction(enemy, true, true));
        fire();
        return;
    }

    if (task->suppress && suppress_at(task->focus))
        return;

    if (task->look && !m_done)
    {
        const u32 elapsed = Device.dwTimeGlobal - m_arrived;
        if (task->turn)
        {
            // ahead, then right, left and behind
            static constexpr float turns[] = {0.f, PI_DIV_2, -PI_DIV_2, PI};
            const u32 turn = std::min<u32>(elapsed * u32(std::size(turns)) / std::max<u32>(task->look, 1), u32(std::size(turns)) - 1);
            Fvector direction;
            direction.setHP(m_heading + turns[turn], 0.f);
            object().sight().setup(CSightAction(SightManager::eSightTypeDirection, direction, true));
        }
        else
            object().sight().setup(CSightAction(SightManager::eSightTypePosition, task->focus, true));
        aim_ready();
        if (elapsed >= task->look)
        {
            m_done = true;
            Commander::Done(object());
        }
        return;
    }

    object().sight().setup(CSightAction(SightManager::eSightTypePosition, task->focus, true));
    aim_ready();

    // at his place round where the enemy is: "come out!"
    using namespace Commander;
    if (!m_cried && (task->role == eRolePost || task->role == eRoleDoor || task->role == eRoleWatch))
    {
        m_cried = true;
        if (object().agent_manager().member().can_cry_noninfo_phrase())
            object().sound().play(eStalkerSoundComeOut, 3000, 1000, 15000, 9000);
    }
}
