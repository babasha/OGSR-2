////////////////////////////////////////////////////////////////////////////
//	Module 		: script_action_wrapper.h
//	Created 	: 19.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script action wrapper
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "script_action_wrapper.h"
#include "script_game_object.h"
#include "goap_stats.h"
const char* GoapStats_LuaClass(const luabind::wrap_base* w); // script_property_evaluator_wrapper.cpp
#include "ai_space.h"
#include "script_engine.h"

// 📏08-10-2026 Switch: CPlanner::add_operator/add_evaluator call setup() at once -- through Lua that was ~8 us of the
// 10 us an add_action/add_evaluator costs (two new userdata for the arguments, a pcall, then this same C++ setup),
// ~170 times per NPC coming online. No script of the mod overrides setup; one that does still gets the Lua call.
void CScriptActionWrapper::setup(CScriptGameObject* object, CPropertyStorage* storage)
{
    if (!has_lua_override("setup"))
        return CScriptActionBase::setup(object, storage);
    luabind::call_member<void>(this, "setup", object, storage);
}

void CScriptActionWrapper::setup_static(CScriptActionBase* action, CScriptGameObject* object, CPropertyStorage* storage) { action->CScriptActionBase::setup(object, storage); }

void CScriptActionWrapper::initialize()
{
    if (GoapStats::enabled >= 2)
        GoapStats::SetLabel(GoapStats::PK_INIT, GoapStats_LuaClass(this));
    luabind::call_member<void>(this, "initialize");
}

void CScriptActionWrapper::initialize_static(CScriptActionBase* action) { action->CScriptActionBase::initialize(); }

void CScriptActionWrapper::execute()
{
    if (GoapStats::enabled >= 2)
        GoapStats::SetLabel(GoapStats::PK_EXEC, GoapStats_LuaClass(this));
    luabind::call_member<void>(this, "execute");
}

void CScriptActionWrapper::execute_static(CScriptActionBase* action) { action->CScriptActionBase::execute(); }

void CScriptActionWrapper::finalize()
{
    if (GoapStats::enabled >= 2)
        GoapStats::SetLabel(GoapStats::PK_INIT, GoapStats_LuaClass(this));
    luabind::call_member<void>(this, "finalize");
}

void CScriptActionWrapper::finalize_static(CScriptActionBase* action) { action->CScriptActionBase::finalize(); }

CScriptActionWrapper::_edge_value_type CScriptActionWrapper::weight(const CSConditionState& condition0, const CSConditionState& condition1) const
{
    _edge_value_type _weight = luabind::call_member<_edge_value_type>(const_cast<CScriptActionWrapper*>(this), "weight", condition0, condition1);
    if (_weight < min_weight())
    {
        ai().script_engine().script_log(eLuaMessageTypeError, "Weight is less than effect count! It is corrected from %d to %d", _weight, min_weight());
        _weight = min_weight();
    }
    return (_weight);
}

CScriptActionWrapper::_edge_value_type CScriptActionWrapper::weight_static(CScriptActionBase* action, const CSConditionState& condition0, const CSConditionState& condition1)
{
    return (((const CScriptActionWrapper*)action)->CScriptActionBase::weight(condition0, condition1));
}
