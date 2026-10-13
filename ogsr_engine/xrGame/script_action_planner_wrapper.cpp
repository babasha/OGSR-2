////////////////////////////////////////////////////////////////////////////
//	Module 		: script_action_planner_wrapper.cpp
//	Created 	: 19.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script action planner wrapper
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "script_action_planner_wrapper.h"
#include "script_game_object.h"
#include "ai_debug.h"

void CScriptActionPlannerWrapper::setup(CScriptGameObject* object)
{
#ifdef LOG_ACTION
    set_use_log(!!psAI_Flags.test(aiGOAPScript));
#endif
    // see CScriptActionWrapper::setup: the Lua round trip only when a script defines setup
    if (!has_lua_override("setup"))
        return CScriptActionPlanner::setup(object);
    luabind::call_member<void>(this, "setup", object);
}

void CScriptActionPlannerWrapper::setup_static(CScriptActionPlanner* planner, CScriptGameObject* object) { planner->CScriptActionPlanner::setup(object); }

void CScriptActionPlannerWrapper::update()
{
#ifdef LOG_ACTION
    if ((psAI_Flags.test(aiGOAPScript) && !m_use_log) || (!psAI_Flags.test(aiGOAPScript) && m_use_log))
        set_use_log(!!psAI_Flags.test(aiGOAPScript));
#endif

    luabind::call_member<void>(this, "update");
}

void CScriptActionPlannerWrapper::update_static(CScriptActionPlanner* planner) { planner->CScriptActionPlanner::update(); }
