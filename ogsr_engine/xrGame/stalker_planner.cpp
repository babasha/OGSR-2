////////////////////////////////////////////////////////////////////////////
//	Module 		: motivation_action_manager_stalker.cpp
//	Created 	: 26.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker motivation action manager class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_planner.h"
#include "stalker_property_evaluators.h"
#include "stalker_danger_property_evaluators.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_decision_space.h"
#include "script_game_object.h"
#include "stalker_alife_planner.h"
#include "stalker_anomaly_planner.h"
#include "stalker_death_planner.h"
#include "stalker_danger_planner.h"
#include "stalker_alife_actions.h"
#include "stalker_combat_planner.h"
#include "Actor.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "visual_memory_manager.h"
//#include "stalker_combat_planner_new.h"

//#define GOAP_DEBUG

using namespace StalkerDecisionSpace;

CStalkerPlanner::CStalkerPlanner()
{
    m_active = true;
    m_affect_cover = false;
}

CStalkerPlanner::~CStalkerPlanner() {}

#ifdef LOG_ACTION
LPCSTR CStalkerPlanner::action2string(const _action_id_type& action_id) { return (inherited::action2string(action_id)); }

LPCSTR CStalkerPlanner::property2string(const _condition_type& property_id) { return (inherited::property2string(property_id)); }

LPCSTR CStalkerPlanner::object_name() const
{
    VERIFY(m_object);
    return (*m_object->cName());
}
#endif

void CStalkerPlanner::setup(CAI_Stalker* object)
{
#ifdef LOG_ACTION
    set_use_log(!!psAI_Flags.test(aiGOAP));
#endif

    inherited::setup(object);

    clear();
    add_evaluators();
    add_actions();

    m_alive_goal.clear();
    m_alive_goal.add_condition(CWorldProperty(eWorldPropertyPuzzleSolved, true));

    m_dead_goal.clear();
    m_dead_goal.add_condition(CWorldProperty(eWorldPropertyAlreadyDead, true));

    m_affect_cover = false;
    m_active = true;
}

void CStalkerPlanner::update(u32 time_delta)
{
    if (!active())
        return;

#ifdef LOG_ACTION
    if ((psAI_Flags.test(aiGOAP) && !m_use_log) || (!psAI_Flags.test(aiGOAP) && m_use_log))
        set_use_log(!!psAI_Flags.test(aiGOAP));
#endif

    if (m_object->g_Alive())
        set_target_state(m_alive_goal);
    else
        set_target_state(m_dead_goal);

    inherited::update();

    extern int g_dbg_npc_tactics;
    if (g_dbg_npc_tactics)
        dbg_tactics();

#ifdef GOAP_DEBUG
    if (m_failed)
    {
        {
            Msg("%d", evaluators().size());
            EVALUATORS::const_iterator I = evaluators().begin();
            EVALUATORS::const_iterator E = evaluators().end();
            for (; I != E; ++I)
                Msg("%d,%d", (*I).first, (*I).second->evaluate() ? 1 : 0);
        }
        {
            Msg("%d", target_state().conditions().size());
            xr_vector<COperatorCondition>::const_iterator I = target_state().conditions().begin();
            xr_vector<COperatorCondition>::const_iterator E = target_state().conditions().end();
            for (; I != E; ++I)
                Msg("%d,%d", (*I).condition(), (*I).value() ? 1 : 0);
        }
        {
            Msg("%d", operators().size());
            const_iterator I = operators().begin();
            const_iterator E = operators().end();
            for (; I != E; ++I)
            {
                Msg("%d,%d", (*I).m_operator_id, (*I).m_operator->weight(target_state(), target_state()));
                {
                    Msg("%d", (*I).m_operator->conditions().conditions().size());
                    xr_vector<COperatorCondition>::const_iterator i = (*I).m_operator->conditions().conditions().begin();
                    xr_vector<COperatorCondition>::const_iterator e = (*I).m_operator->conditions().conditions().end();
                    for (; i != e; ++i)
                        Msg("%d,%d", (*i).condition(), (*i).value() ? 1 : 0);
                }
                {
                    Msg("%d", (*I).m_operator->effects().conditions().size());
                    xr_vector<COperatorCondition>::const_iterator i = (*I).m_operator->effects().conditions().begin();
                    xr_vector<COperatorCondition>::const_iterator e = (*I).m_operator->effects().conditions().end();
                    for (; i != e; ++i)
                        Msg("%d,%d", (*i).condition(), (*i).value() ? 1 : 0);
                }
            }
        }
    }
#endif
}

void CStalkerPlanner::add_evaluators()
{
    add_evaluator(eWorldPropertyAlreadyDead, xr_new<CStalkerPropertyEvaluatorConst>(false, "is_already_dead"));
    add_evaluator(eWorldPropertyPuzzleSolved, xr_new<CStalkerPropertyEvaluatorConst>(false, "is_zone_puzzle_solved"));
    add_evaluator(eWorldPropertyAlive, xr_new<CStalkerPropertyEvaluatorAlive>(m_object, "is_alive"));
    add_evaluator(eWorldPropertyEnemy, xr_new<CStalkerPropertyEvaluatorEnemies>(m_object, "is_there_enemies", CStalkerCombatPlanner::POST_COMBAT_WAIT_INTERVAL));
    add_evaluator(eWorldPropertyDanger, xr_new<CStalkerPropertyEvaluatorDangers>(m_object, "is_there_danger"));
    add_evaluator(eWorldPropertyAnomaly, xr_new<CStalkerPropertyEvaluatorAnomaly>(m_object, "is_there_anomalies"));
    add_evaluator(eWorldPropertyItems, xr_new<CStalkerPropertyEvaluatorItems>(m_object, "is_there_items_to_pick_up"));

    // ⭐No C++ action in THIS planner reads it -- the grenade property belongs
    // to the combat and danger planners, which register their own. It is here
    // because script logic adds actions to this brain (the mod's motivation
    // manager takes it from 7 evaluators to 38), and one of them is gated on
    // stalker_ids.property_danger_grenade without ever adding an evaluator for
    // it. The solver then cannot judge that condition and complains -- 176
    // times in a two-minute run -- while the action it belongs to is quietly
    // unplannable. Registering the engine's own evaluator answers the question
    // instead of leaving it unanswerable.
    add_evaluator(eWorldPropertyDangerGrenade, xr_new<CStalkerPropertyEvaluatorDangerWithGrenade>(m_object, "danger with grenade"));
}

void CStalkerPlanner::add_actions()
{
    CActionPlannerAction* planner;

    planner = xr_new<CStalkerDeathPlanner>(m_object, "death_planner");
    add_condition(planner, eWorldPropertyAlive, false);
    add_condition(planner, eWorldPropertyAlreadyDead, false);
    add_effect(planner, eWorldPropertyAlreadyDead, true);
    add_operator(eWorldOperatorDeathPlanner, planner);

    planner = xr_new<CStalkerALifePlanner>(m_object, "alife_planner");
    add_condition(planner, eWorldPropertyAlive, true);
    add_condition(planner, eWorldPropertyEnemy, false);
    add_condition(planner, eWorldPropertyAnomaly, false);
    add_condition(planner, eWorldPropertyDanger, false);
    add_condition(planner, eWorldPropertyItems, false);
    add_condition(planner, eWorldPropertyPuzzleSolved, false);
    add_effect(planner, eWorldPropertyPuzzleSolved, true);
    add_operator(eWorldOperatorALifePlanner, planner);

    planner = xr_new<CStalkerCombatPlanner>(m_object, "combat_planner");
    //	planner					= xr_new<CStalkerCombatPlannerNew>(m_object,"combat_planner_new");
    add_condition(planner, eWorldPropertyAlive, true);
    add_condition(planner, eWorldPropertyAnomaly, false);
    add_condition(planner, eWorldPropertyEnemy, true);
    add_effect(planner, eWorldPropertyEnemy, false);
    add_operator(eWorldOperatorCombatPlanner, planner);

    planner = xr_new<CStalkerDangerPlanner>(m_object, "danger_planner");
    add_condition(planner, eWorldPropertyAlive, true);
    add_condition(planner, eWorldPropertyEnemy, false);
    add_condition(planner, eWorldPropertyAnomaly, false);
    add_condition(planner, eWorldPropertyDanger, true);
    add_effect(planner, eWorldPropertyDanger, false);
    add_operator(eWorldOperatorDangerPlanner, planner);

    planner = xr_new<CStalkerAnomalyPlanner>(m_object, "anomaly_planner");
    add_condition(planner, eWorldPropertyAlive, true);
    add_condition(planner, eWorldPropertyAnomaly, true);
    add_effect(planner, eWorldPropertyAnomaly, false);
    add_operator(eWorldOperatorAnomalyPlanner, planner);

    CStalkerActionBase* action;

    action = xr_new<CStalkerActionGatherItems>(m_object, "gather_items");
    add_condition(action, eWorldPropertyAlive, true);
    add_condition(action, eWorldPropertyEnemy, false);
    add_condition(action, eWorldPropertyAnomaly, false);
    add_condition(action, eWorldPropertyDanger, false);
    add_condition(action, eWorldPropertyItems, true);
    add_effect(action, eWorldPropertyItems, false);
    add_operator(eWorldOperatorGatherItems, action);
}

// dbg_npc_tactics, top level: while a stalker near the player has an enemy but the
// combat planner is not the one running (a mod script action took over, danger,
// anomaly...), say which action is. The combat planner prints its own lines.
void CStalkerPlanner::dbg_tactics()
{
    extern int g_dbg_npc_tactics;
    const CActor* actor = Actor();
    const CEntityAlive* enemy = m_object->memory().enemy().selected();
    if (!actor || !enemy || !m_object->g_Alive() || m_object->Position().distance_to(actor->Position()) > float(g_dbg_npc_tactics))
        return;

    const u32 action = solution().empty() ? u32(-1) : current_action_id();
    if (action == eWorldOperatorCombatPlanner)
    {
        m_dbg_action = action;
        return;
    }

    const bool changed = action != m_dbg_action;
    if (!changed && Device.dwTimeGlobal < m_dbg_print_time + 1000)
        return;

    m_dbg_print_time = Device.dwTimeGlobal;
    m_dbg_action = action;

    LPCSTR name = nullptr;
    switch (action)
    {
    case eWorldOperatorDangerPlanner: name = "danger_planner"; break;
    case eWorldOperatorALifePlanner: name = "alife_planner"; break;
    case eWorldOperatorAnomalyPlanner: name = "anomaly_planner"; break;
    case eWorldOperatorDeathPlanner: name = "death_planner"; break;
    case eWorldOperatorGatherItems: name = "gather_items"; break;
    case u32(-1): name = "none"; break;
    }
    string32 script;
    if (!name)
        name = (xr_sprintf(script, "script#%u", action), script);

    const Fvector& p = m_object->Position();
    Msg("[tac]%s%u '%s' %s top=%s (not combat) @(%.1f,%.1f,%.1f) | enemy=%s d=%.1f sees=%d", changed ? " * " : "   ", m_object->ID(), m_object->Name(), m_object->cNameSect().c_str(), name,
        p.x, p.y, p.z, enemy->cName().c_str(), enemy->Position().distance_to(p), m_object->memory().visual().visible_now(enemy) ? 1 : 0);
}
