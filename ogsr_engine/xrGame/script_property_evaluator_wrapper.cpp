////////////////////////////////////////////////////////////////////////////
//	Module 		: script_property_evaluator_wrapper.cpp
//	Created 	: 19.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script property evaluator wrapper
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "xr_platform_seh.h"
#include "script_property_evaluator_wrapper.h"
#include "script_game_object.h"
#include "ai_space.h"
#include "script_engine.h"
#include "goap_stats.h"
#include <luabind/detail/object_rep.hpp>
#include <luabind/detail/class_rep.hpp>

// goap_stats 2: the Lua class of a script evaluator/action (the C++ side is the same wrapper for all of them).
// class_rep lives as long as the Lua state, so the pointer is a stable key.
const char* GoapStats_LuaClass(const luabind::wrap_base* w)
{
    const auto& self = luabind::detail::wrap_access::ref(*w);
    lua_State* L = self.state();
    if (!L)
        return "?";
    const int top = lua_gettop(L);
    self.get(L);
    const char* name = "?";
    if (luabind::detail::object_rep* o = luabind::detail::is_class_object(L, -1))
        if (o->crep())
            name = o->crep()->name();
    lua_settop(L, top);
    return name;
}

// see CScriptActionWrapper::setup: the Lua round trip only when a script defines setup
void CScriptPropertyEvaluatorWrapper::setup(CScriptGameObject* object, CPropertyStorage* storage)
{
    if (!has_lua_override("setup"))
        return CScriptPropertyEvaluator::setup(object, storage);
    luabind::call_member<void>(this, "setup", object, storage);
}

void CScriptPropertyEvaluatorWrapper::setup_static(CScriptPropertyEvaluator* evaluator, CScriptGameObject* object, CPropertyStorage* storage)
{
    evaluator->CScriptPropertyEvaluator::setup(object, storage);
}

// ⭐ai_eval_cache (11-10, Switch fight). The stalker planner asks every evaluator of its current state on each
// update only to learn "is the plan still valid" (goap_stats.h: 9400 evaluate()/s, 99.6 % "yes"), and the script
// ones are a Lua round trip each. Some of them answer slowly changing questions (is there a campfire, a job, a
// wounded friend...). For the Lua classes listed in ai_eval_cache the answer is kept ai_eval_cache_ms (per
// evaluator object, i.e. per NPC; the deadline is staggered by the NPC's id so they do not refresh in step).
// Empty list or 0 ms = stock.
char g_eval_cache_list[512] = "";
int g_eval_cache_ms = 0;
u32 g_eval_cache_gen = 1;

static bool eval_cache_listed(const char* name)
{
    const size_t n = xr_strlen(name);
    if (!n)
        return false;
    for (const char* p = g_eval_cache_list; *p;)
    {
        const char* e = p;
        while (*e && *e != ',')
            ++e;
        if (size_t(e - p) == n && 0 == strncmp(p, name, n))
            return true;
        p = *e ? e + 1 : e;
    }
    return false;
}

bool CScriptPropertyEvaluatorWrapper::evaluate()
{
    if (GoapStats::enabled >= 2)
        GoapStats::SetLabel(GoapStats::PK_EVAL, GoapStats_LuaClass(this));
    if (g_eval_cache_ms > 0 && g_eval_cache_list[0])
    {
        if (m_cache_gen != g_eval_cache_gen)
        {
            m_cache_gen = g_eval_cache_gen;
            m_cache_on = eval_cache_listed(GoapStats_LuaClass(this));
            m_cache_until = 0;
        }
        if (m_cache_on && Device.dwTimeGlobal < m_cache_until)
            return m_cache_val;
    }
    else
    {
        m_cache_on = false;
        m_cache_gen = 0;
    }
    XR_SEH_TRY
    {
        const bool val = luabind::call_member<bool>(this, "evaluate");
        if (m_cache_on)
        {
            m_cache_val = val;
            const u32 id = m_object ? u32(m_object->ID()) : 0u;
            m_cache_until = Device.dwTimeGlobal + u32(g_eval_cache_ms) - (id * 7u) % (u32(g_eval_cache_ms) / 4u + 1u);
        }
        return val;
    }
    XR_SEH_EXCEPT(ExceptStackTrace("[CScriptPropertyEvaluatorWrapper::evaluate] stack_trace:\n"))
    {
        Msg("!![CScriptPropertyEvaluatorWrapper::evaluate] Fatal Error in object [%s], evaluator: [%s]", m_object->cName().c_str(), m_evaluator_name);
    }
    return false;
}

bool CScriptPropertyEvaluatorWrapper::evaluate_static(CScriptPropertyEvaluator* evaluator) { return (evaluator->CScriptPropertyEvaluator::evaluate()); }
