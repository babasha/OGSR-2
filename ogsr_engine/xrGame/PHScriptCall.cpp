#include "StdAfx.h"
#include "PHCommander.h"
#include "script_callback_ex.h"
#include "../xr_3da/xr_object.h"
#include "PHScriptCall.h"

/*
IC bool compare_safe(const luabind::object &o1 , const luabind::object &o2)
{
    return (o1.type()==LUA_TNIL && o2.type()==LUA_TNIL) || o1==o2;
}
/**/

CPHScriptCondition::CPHScriptCondition(const luabind::functor<bool>& func) { m_lua_function = xr_new<luabind::functor<bool>>(func); }

CPHScriptCondition::CPHScriptCondition(const CPHScriptCondition& func) { m_lua_function = xr_new<luabind::functor<bool>>(*func.m_lua_function); }

CPHScriptCondition::~CPHScriptCondition() { xr_delete(m_lua_function); }

// Mod-compat guard (see [[pripyat-mod-compat]]): CScriptEngine::lua_error now THROWS a
// C++ exception so guarded call-sites survive a script runtime error instead of
// std::terminate. The physics commander runs these callbacks every frame from
// CLevel::OnFrame — an unguarded throw here unwinds straight to WinMain (crash). A
// mod's story-script (e.g. ogse_signals.script) erroring on a foreign map must NOT
// take the whole game down: log + fall back (condition→false, action→obsolete/skip).
// Errors don't kill the call immediately — "retry until game state is ready" is a
// legitimate mod pattern and transient nil-errors during level init must survive. But
// a PERMANENTLY broken call can't stay immortal either: scripts (ogse_signals) keep
// adding new calls while broken ones pile up, and the leaked luabind refs blow up the
// LuaJIT heap in minutes ("not enough memory" in lj_tab_resize). 30 straight failures
// (≈ half a second of frames) => obsolete => purged by CPHCommander.
static constexpr u16 kMaxConditionFails = 30;

bool CPHScriptCondition::is_true()
{
    try
    {
        const bool r = (*m_lua_function)();
        m_fails = 0;
        return r;
    }
    catch (...)
    {
        if (++m_fails == kMaxConditionFails)
            Msg("![PHScript] condition failed %u times — dropping command (mod-compat)", (u32)m_fails);
        return false;
    }
}

bool CPHScriptCondition::obsolete() const { return m_fails >= kMaxConditionFails; }

// "script.lua:line" of the wrapped Lua function — names WHO leaked when the
// commander's call list grows (see the histogram in CPHCommander::update).
void CPHScriptCondition::dump_source(char* buf, u32 n) const
{
    if (!n) return;
    buf[0] = 0;
    if (!m_lua_function || !m_lua_function->is_valid()) return;
    lua_State* L = m_lua_function->lua_state();
    if (!L) return;
    m_lua_function->pushvalue();
    lua_Debug ar{};
    if (lua_getinfo(L, ">S", &ar))   // ">" pops the pushed function
    {
        _snprintf(buf, n - 1, "%s:%d", ar.short_src, ar.linedefined);
        buf[n - 1] = 0;
    }
}

//
CPHScriptAction::CPHScriptAction(const luabind::functor<void>& func)
{
    b_obsolete = false;
    m_lua_function = xr_new<luabind::functor<void>>(func);
}

CPHScriptAction::CPHScriptAction(const CPHScriptAction& action)
{
    b_obsolete = action.b_obsolete;
    m_lua_function = xr_new<luabind::functor<void>>(*action.m_lua_function);
}

CPHScriptAction::~CPHScriptAction() { xr_delete(m_lua_function); }

void CPHScriptAction::run()
{
    try { (*m_lua_function)(); }
    catch (...) { Msg("![PHScript] action Lua error — skipping command (mod-compat)"); }
    b_obsolete = true;   // mark done regardless so a broken command isn't re-run every frame
}

bool CPHScriptAction::obsolete() const { return b_obsolete; }

/////////////////////////////////////////////////////////////////////////////////////////////
CPHScriptObjectAction::CPHScriptObjectAction(const luabind::object& lua_object, LPCSTR method)
{
    b_obsolete = false;
    m_lua_object = xr_new<luabind::object>(lua_object);
    m_method_name = method;
}

CPHScriptObjectAction::CPHScriptObjectAction(const CPHScriptObjectAction& object)
{
    b_obsolete = object.b_obsolete;
    m_lua_object = xr_new<luabind::object>(*object.m_lua_object);
    m_method_name = object.m_method_name;
}

CPHScriptObjectAction::~CPHScriptObjectAction() { xr_delete(m_lua_object); }

bool CPHScriptObjectAction::compare(const CPHScriptObjectAction* v) const { return m_method_name == v->m_method_name && compare_safe(*m_lua_object, *(v->m_lua_object)); }
void CPHScriptObjectAction::run()
{
    try { luabind::call_member<void>(*m_lua_object, *m_method_name); }
    catch (...) { Msg("![PHScript] object-action '%s' Lua error — skipping (mod-compat)", m_method_name.c_str()); }
    b_obsolete = true;
}

bool CPHScriptObjectAction::obsolete() const { return b_obsolete; }

//
CPHScriptObjectCondition::CPHScriptObjectCondition(const luabind::object& lua_object, LPCSTR method)
{
    m_lua_object = xr_new<luabind::object>(lua_object);
    m_method_name = method;
}

CPHScriptObjectCondition::CPHScriptObjectCondition(const CPHScriptObjectCondition& object)
{
    m_lua_object = xr_new<luabind::object>(*object.m_lua_object);
    m_method_name = object.m_method_name;
}

CPHScriptObjectCondition::~CPHScriptObjectCondition() { xr_delete(m_lua_object); }
bool CPHScriptObjectCondition::compare(const CPHScriptObjectCondition* v) const { return m_method_name == v->m_method_name && compare_safe(*m_lua_object, *(v->m_lua_object)); }

bool CPHScriptObjectCondition::is_true()
{
    try
    {
        const bool r = luabind::call_member<bool>(*m_lua_object, *m_method_name);
        m_fails = 0;
        return r;
    }
    catch (...)
    {
        if (++m_fails == kMaxConditionFails)
            Msg("![PHScript] object-condition '%s' failed %u times — dropping command (mod-compat)", m_method_name.c_str(), (u32)m_fails);
        return false;
    }
}
bool CPHScriptObjectCondition::obsolete() const { return m_fails >= kMaxConditionFails; }

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
CPHScriptObjectActionN::CPHScriptObjectActionN(const luabind::object& object, const luabind::functor<void>& functor)
{
    b_obsolete = false;
    m_callback.set(functor, object);
}

CPHScriptObjectActionN::~CPHScriptObjectActionN() { m_callback.clear(); }

void CPHScriptObjectActionN::run()
{
    try { m_callback(); }
    catch (...) { Msg("![PHScript] object-action-N Lua error — skipping (mod-compat)"); }
    b_obsolete = true;
}

bool CPHScriptObjectActionN::obsolete() const { return b_obsolete; }

CPHScriptObjectConditionN::CPHScriptObjectConditionN(const luabind::object& object, const luabind::functor<bool>& functor) { m_callback.set(functor, object); }

CPHScriptObjectConditionN::~CPHScriptObjectConditionN() { m_callback.clear(); }

bool CPHScriptObjectConditionN::is_true()
{
    try
    {
        const bool r = m_callback();
        m_fails = 0;
        return r;
    }
    catch (...)
    {
        if (++m_fails == kMaxConditionFails)
            Msg("![PHScript] object-condition-N failed %u times — dropping command (mod-compat)", (u32)m_fails);
        return false;
    }
}
bool CPHScriptObjectConditionN::obsolete() const { return m_fails >= kMaxConditionFails; }
