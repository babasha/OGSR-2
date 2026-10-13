////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_property_evaluators.cpp
//	Created 	: 25.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker property evaluators classes
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_property_evaluators.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_decision_space.h"
#include "script_game_object.h"
#include "ai/ai_monsters_misc.h"
#include "Inventory.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "memory_manager.h"
#include "visual_memory_manager.h"
#include "item_manager.h"
#include "enemy_manager.h"
#include "danger_manager.h"
#include "ai_space.h"
#include "ai/stalker/ai_stalker.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "alife_human_brain.h"
#include "Actor.h"
#include "actor_memory.h"
#include "stalker_movement_manager.h"
#include "agent_manager.h"
#include "agent_enemy_manager.h"

using namespace StalkerDecisionSpace;

typedef CStalkerPropertyEvaluator::_value_type _value_type;

const float wounded_enemy_reached_distance = 3.f;

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorALife
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorALife::CStalkerPropertyEvaluatorALife(CAI_Stalker* object, LPCSTR evaluator_name) : inherited(object ? object->lua_game_object() : 0, evaluator_name) {}

_value_type CStalkerPropertyEvaluatorALife::evaluate() { return (!!ai().get_alife()); }

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorAlive
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorAlive::CStalkerPropertyEvaluatorAlive(CAI_Stalker* object, LPCSTR evaluator_name) : inherited(object ? object->lua_game_object() : 0, evaluator_name) {}

_value_type CStalkerPropertyEvaluatorAlive::evaluate() { return (!!object().g_Alive()); }

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorItems
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorItems::CStalkerPropertyEvaluatorItems(CAI_Stalker* object, LPCSTR evaluator_name) : inherited(object ? object->lua_game_object() : 0, evaluator_name) {}

_value_type CStalkerPropertyEvaluatorItems::evaluate() { return (!!m_object->memory().item().selected()); }

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorEnemies
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorEnemies::CStalkerPropertyEvaluatorEnemies(CAI_Stalker* object, LPCSTR evaluator_name, u32 time_to_wait, const bool* dont_wait)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{
    m_time_to_wait = time_to_wait;
    m_dont_wait = dont_wait;
}

_value_type CStalkerPropertyEvaluatorEnemies::evaluate()
{
    if (m_object->memory().enemy().selected())
        return (true);

    if (m_dont_wait && *m_dont_wait)
        return (false);

    if (Device.dwTimeGlobal < m_object->memory().enemy().last_enemy_time() + m_time_to_wait)
        return (true);

    return (false);
}

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorSeeEnemy
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorSeeEnemy::CStalkerPropertyEvaluatorSeeEnemy(CAI_Stalker* object, LPCSTR evaluator_name) : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

_value_type CStalkerPropertyEvaluatorSeeEnemy::evaluate()
{
    return (m_object->memory().enemy().selected() ? m_object->memory().visual().visible_now(m_object->memory().enemy().selected()) : false);
}

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorEnemySeeMe
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorEnemySeeMe::CStalkerPropertyEvaluatorEnemySeeMe(CAI_Stalker* object, LPCSTR evaluator_name)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

_value_type CStalkerPropertyEvaluatorEnemySeeMe::evaluate()
{
    const CEntityAlive* enemy = m_object->memory().enemy().selected();
    if (!enemy)
        return (false);

    const CCustomMonster* enemy_monster = smart_cast<const CCustomMonster*>(enemy);
    if (enemy_monster)
        return (enemy_monster->memory().visual().visible_now(m_object));

    const CActor* actor = smart_cast<const CActor*>(enemy);
    if (actor)
        return (actor->memory().visual().visible_now(m_object));

    return (false);
}

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorItemToKill
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorItemToKill::CStalkerPropertyEvaluatorItemToKill(CAI_Stalker* object, LPCSTR evaluator_name)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

_value_type CStalkerPropertyEvaluatorItemToKill::evaluate() { return (!!m_object->item_to_kill()); }

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorItemCanKill
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorItemCanKill::CStalkerPropertyEvaluatorItemCanKill(CAI_Stalker* object, LPCSTR evaluator_name)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

_value_type CStalkerPropertyEvaluatorItemCanKill::evaluate() { return (m_object->item_can_kill()); }

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorFoundItemToKill
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorFoundItemToKill::CStalkerPropertyEvaluatorFoundItemToKill(CAI_Stalker* object, LPCSTR evaluator_name)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

_value_type CStalkerPropertyEvaluatorFoundItemToKill::evaluate() { return (m_object->remember_item_to_kill()); }

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorFoundAmmo
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorFoundAmmo::CStalkerPropertyEvaluatorFoundAmmo(CAI_Stalker* object, LPCSTR evaluator_name)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

_value_type CStalkerPropertyEvaluatorFoundAmmo::evaluate() { return (m_object->remember_ammo()); }

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorReadyToKill
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorReadyToKill::CStalkerPropertyEvaluatorReadyToKill(CAI_Stalker* object, LPCSTR evaluator_name)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

_value_type CStalkerPropertyEvaluatorReadyToKill::evaluate() { return (m_object->ready_to_kill()); }

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorReadyToDetour
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorReadyToDetour::CStalkerPropertyEvaluatorReadyToDetour(CAI_Stalker* object, LPCSTR evaluator_name)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

_value_type CStalkerPropertyEvaluatorReadyToDetour::evaluate() { return (m_object->ready_to_detour()); }

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorAnomaly
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorAnomaly::CStalkerPropertyEvaluatorAnomaly(CAI_Stalker* object, LPCSTR evaluator_name) : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

_value_type CStalkerPropertyEvaluatorAnomaly::evaluate()
{
    if (!m_object->undetected_anomaly())
        return (false);

    if (!m_object->memory().enemy().selected())
        return (true);

    u32 result = dwfChooseAction(2000, m_object->panic_threshold(), 0.f, 0.f, 0.f, m_object->g_Team(), m_object->g_Squad(), m_object->g_Group(), 0, 1, 2, 3, 4, m_object, 300.f);
    return (!result);
}

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorInsideAnomaly
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorInsideAnomaly::CStalkerPropertyEvaluatorInsideAnomaly(CAI_Stalker* object, LPCSTR evaluator_name)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

_value_type CStalkerPropertyEvaluatorInsideAnomaly::evaluate()
{
    if (!m_object->inside_anomaly())
        return (false);

    if (!m_object->memory().enemy().selected())
        return (true);

    u32 result = dwfChooseAction(2000, m_object->panic_threshold(), 0.f, 0.f, 0.f, m_object->g_Team(), m_object->g_Squad(), m_object->g_Group(), 0, 1, 2, 3, 4, m_object, 300.f);
    return (!result);
}

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorPanic
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorPanic::CStalkerPropertyEvaluatorPanic(CAI_Stalker* object, LPCSTR evaluator_name) : inherited(object ? object->lua_game_object() : 0, evaluator_name) {}

_value_type CStalkerPropertyEvaluatorPanic::evaluate()
{
    u32 result = dwfChooseAction(2000, m_object->panic_threshold(), 0.f, 0.f, 0.f, m_object->g_Team(), m_object->g_Squad(), m_object->g_Group(), 0, 1, 2, 3, 4, m_object, 300.f);
    return (!!result);
}

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorSmartTerrainTask
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorSmartTerrainTask::CStalkerPropertyEvaluatorSmartTerrainTask(CAI_Stalker* object, LPCSTR evaluator_name)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

// ⭐⭐⭐THE 3021-FAULT STORM OF 05-08-2026 WAS THIS LINE.
//
// The evaluator asks the ALife registry for the creature's OWN server entity
// and then walks straight into it. `VERIFY` compiles to nothing in Release
// (xrDebug_macros.h: `#define VERIFY(...) ((void)0)`, and THROW2 inside the
// registry is the same macro), so the lookup returning null is not a caught
// error -- it is a null dereference on the next line, every single time this
// creature thinks.
//
// ⭐What made it a STORM rather than a crash: CPlanner::update wraps itself in
// __except, so the fault is swallowed, the creature stays alive, and it thinks
// again 200 ms later. Measured live: one orphaned stalker produced 3021 faults
// in ten minutes -- and the first eight of them cost 400-650 ms EACH in stack
// symbolisation, inside the frame. That is what cut the server's frames short,
// which is why A-Life stopped switching creatures online, which is why the
// human at the client saw "NPCs standing in a heap" and eventually a dead
// client. Not one link in that chain looks like an AI bug.
//
// ⚠⚠And the state is REACHABLE, so this is a guard and not an assert: the
// creature it happened to (stalker5376) had been despawned by `sv_npc_del all`
// twelve seconds earlier. Its ALife object was released, its server entity was
// destroyed -- and its level object went on living and thinking. That
// lifetime bug is a separate defect and is NOT fixed here; what is fixed here
// is that a missing server entity must not be a fatal read. An object with no
// entity has no smart-terrain task by definition, which is exactly `false`.
//
// ⭐The complaint names the id, because "who" was the question the storm could
// not answer: 3021 identical anonymous lines. Rate-limited to first-per-id and
// then every 1024th, because the diagnostic turning into the outage is a
// failure mode this codebase has already lived through once.
// ⚠⚠TEST SWITCH, and it turns the crash back ON. Set to 1 and this evaluator
// goes back to dereferencing whatever the registry returned, which for a
// stranded creature is null.
//
// It exists because "the guard fixed it" is a claim, and the only way to show
// it is the guard rather than something else that changed between two runs is
// to run BOTH arms from ONE binary: sv_orphan_make produces the state, this
// decides whether it is survived. ⚠Non-savable (console_commands.cpp) --
// see the rule about instrument cvars leaking into user.ltx.
int g_dbg_orphan_unguarded = 0;

_value_type CStalkerPropertyEvaluatorSmartTerrainTask::evaluate()
{
    if (!ai().get_alife())
        return (false);

    CSE_ALifeHumanAbstract* stalker = smart_cast<CSE_ALifeHumanAbstract*>(ai().alife().objects().object(m_object->ID(), true));
    if (!stalker && !g_dbg_orphan_unguarded)
    {
        // ⚠Throttled by the CLOCK, not by identity. The first version keyed on
        // "is this a different creature from last time", which reads as a
        // sensible way to catch each one once -- and with four orphans taking
        // turns it never repeats itself, so it printed at full speed: 1799
        // lines in seventy seconds, measured. A diagnostic for a storm cannot
        // have a storm-shaped output.
        //
        // ⭐The running total is printed with it because a throttled line is
        // not a rate: without the count, "one line every five seconds" hides
        // whether that is one creature or forty.
        static u32 s_seen = 0;
        static u32 s_next_ms = 0;
        ++s_seen;
        if (Device.dwTimeGlobal >= s_next_ms)
        {
            s_next_ms = Device.dwTimeGlobal + 5000;
            Msg("!! [orphan] creature %u '%s' has no server entity -- it outlived its own despawn and is still thinking (%u such reads so far)", u32(m_object->ID()),
                m_object->Name(), s_seen);
        }
        return (false);
    }

    stalker->brain().select_task();
    return (stalker->m_smart_terrain_id != 0xffff);
}

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorEnemyReached
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorEnemyReached::CStalkerPropertyEvaluatorEnemyReached(CAI_Stalker* object, LPCSTR evaluator_name)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

_value_type CStalkerPropertyEvaluatorEnemyReached::evaluate()
{
    const CEntityAlive* enemy = object().memory().enemy().selected();
    if (!enemy)
        return (false);

    ALife::_OBJECT_ID processor_id = object().agent_manager().enemy().wounded_processor(enemy);
    if (processor_id != object().ID())
        return (false);

    return ((object().Position().distance_to_sqr(enemy->Position()) <= _sqr(wounded_enemy_reached_distance)));
}

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorPlayerOnThePath
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorPlayerOnThePath::CStalkerPropertyEvaluatorPlayerOnThePath(CAI_Stalker* object, LPCSTR evaluator_name)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

_value_type CStalkerPropertyEvaluatorPlayerOnThePath::evaluate()
{
    const CEntityAlive* enemy = object().memory().enemy().selected();
    if (!enemy)
        return (false);

    if (!object().is_relation_enemy(Actor()))
        return (false);

    if (!m_object->memory().visual().visible_now(Actor()))
        return (false);

    return (object().movement().is_object_on_the_way(Actor(), 2.f));
}

//////////////////////////////////////////////////////////////////////////
// CStalkerPropertyEvaluatorEnemyCriticallyWounded
//////////////////////////////////////////////////////////////////////////

CStalkerPropertyEvaluatorEnemyCriticallyWounded::CStalkerPropertyEvaluatorEnemyCriticallyWounded(CAI_Stalker* object, LPCSTR evaluator_name)
    : inherited(object ? object->lua_game_object() : 0, evaluator_name)
{}

_value_type CStalkerPropertyEvaluatorEnemyCriticallyWounded::evaluate()
{
    const CEntityAlive* enemy = object().memory().enemy().selected();
    if (!enemy)
        return (false);

    const CAI_Stalker* enemy_stalker = smart_cast<const CAI_Stalker*>(enemy);
    if (!enemy_stalker)
        return (false);

    return (const_cast<CAI_Stalker*>(enemy_stalker)->critically_wounded());
}
