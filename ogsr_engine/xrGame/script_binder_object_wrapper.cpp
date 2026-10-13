////////////////////////////////////////////////////////////////////////////
//	Module 		: script_binder_object_wrapper.cpp
//	Created 	: 29.03.2004
//  Modified 	: 29.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script object binder wrapper
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "script_binder_object_wrapper.h"
#include "script_game_object.h"
#include "xrServer_Objects_ALife.h"
#include "../xr_3da/NET_Server_Trash/NET_utils.h"

CScriptBinderObjectWrapper::CScriptBinderObjectWrapper(CScriptGameObject* object) : CScriptBinderObject(object) {}

CScriptBinderObjectWrapper::~CScriptBinderObjectWrapper() {}

// Foreign content (e.g. running the Living Zone level on the Gunslinger base) makes the
// campaign's Lua binders throw on objects/story ids they don't recognise. luabind rethrows
// that as a C++ exception out of these net callbacks, which are invoked from C++ frame code
// (g_sv_Spawn / OnFrame) with no handler -> std::terminate kills the whole process. Swallow &
// log instead so one bad binder can't crash the game (a render-demo needs the level, not the AI).
#define OGSR_GUARD_BINDER(expr, onfail) \
    try { expr; } catch (...) { Msg("! [script binder] '%s' threw a Lua error — continuing", __FUNCTION__); onfail; }

void CScriptBinderObjectWrapper::reinit() { OGSR_GUARD_BINDER(luabind::call_member<void>(this, "reinit"), ); }

void CScriptBinderObjectWrapper::reinit_static(CScriptBinderObject* script_binder_object) { script_binder_object->CScriptBinderObject::reinit(); }

void CScriptBinderObjectWrapper::reload(LPCSTR section) { OGSR_GUARD_BINDER(luabind::call_member<void>(this, "reload", section), ); }

void CScriptBinderObjectWrapper::reload_static(CScriptBinderObject* script_binder_object, LPCSTR section) { script_binder_object->CScriptBinderObject::reload(section); }

bool CScriptBinderObjectWrapper::net_Spawn(SpawnType DC) { OGSR_GUARD_BINDER(return (luabind::call_member<bool>(this, "net_spawn", DC)), return true); }

bool CScriptBinderObjectWrapper::net_Spawn_static(CScriptBinderObject* script_binder_object, SpawnType DC) { return (script_binder_object->CScriptBinderObject::net_Spawn(DC)); }

void CScriptBinderObjectWrapper::net_Destroy() { OGSR_GUARD_BINDER(luabind::call_member<void>(this, "net_destroy"), ); }

void CScriptBinderObjectWrapper::net_Destroy_static(CScriptBinderObject* script_binder_object) { script_binder_object->CScriptBinderObject::net_Destroy(); }

// Repeat-offender mute: on a foreign map a binder whose Lua update throws EVERY shedule
// tick (e.g. 627 alien smart terrains with no OGSE metadata, each printing a full
// traceback) drowns the frame in error handling — tens of ms per frame gone. After
// kBinderMuteAfter consecutive failures this object's Lua update is silenced for good;
// one success resets the count. Other callbacks keep the plain guard.
static constexpr u16 kBinderMuteAfter = 10;

void CScriptBinderObjectWrapper::shedule_Update(u32 time_delta)
{
    if (m_update_fails >= kBinderMuteAfter)
    {
        CScriptBinderObject::shedule_Update(time_delta);
        return;
    }
    try
    {
        luabind::call_member<void>(this, "update", time_delta);
        m_update_fails = 0;
    }
    catch (...)
    {
        if (++m_update_fails == kBinderMuteAfter)
            Msg("! [script binder] update keeps throwing — Lua update MUTED for this object (mod-compat)");
        else
            Msg("! [script binder] 'shedule_Update' threw a Lua error — continuing");
    }
}

void CScriptBinderObjectWrapper::shedule_Update_static(CScriptBinderObject* script_binder_object, u32 time_delta)
{
    script_binder_object->CScriptBinderObject::shedule_Update(time_delta);
}

void CScriptBinderObjectWrapper::save(NET_Packet* output_packet) { OGSR_GUARD_BINDER(luabind::call_member<void>(this, "save", output_packet), ); }

void CScriptBinderObjectWrapper::save_static(CScriptBinderObject* script_binder_object, NET_Packet* output_packet)
{
    script_binder_object->CScriptBinderObject::save(output_packet);
}

void CScriptBinderObjectWrapper::load(IReader* input_packet) { OGSR_GUARD_BINDER(luabind::call_member<void>(this, "load", input_packet), ); }

void CScriptBinderObjectWrapper::load_static(CScriptBinderObject* script_binder_object, IReader* input_packet) { script_binder_object->CScriptBinderObject::load(input_packet); }

bool CScriptBinderObjectWrapper::net_SaveRelevant() { OGSR_GUARD_BINDER(return (luabind::call_member<bool>(this, "net_save_relevant")), return false); }

bool CScriptBinderObjectWrapper::net_SaveRelevant_static(CScriptBinderObject* script_binder_object) { return (script_binder_object->CScriptBinderObject::net_SaveRelevant()); }

// ⭐Asked once per binder, not per call. net_Relcase runs for EVERY bound object
// (stalkers, smart terrains, physics objects...) times EVERY object destroyed in a
// frame (CObjectList::ProcessDestroyQueue). 📏07-10 Switch, a kill in the Cordon
// village: the corpse's dropped/released items made that ~8000 trips through
// luabind in one frame -- most of them to land in the empty C++ default, because
// no script but bind_smart_terrain defines the method -- 40 ms frames per kill.
void CScriptBinderObjectWrapper::net_Relcase(CScriptGameObject* object)
{
    if (m_relcase_override < 0)
        m_relcase_override = has_lua_override("net_Relcase") ? 1 : 0;
    if (!m_relcase_override)
        return; // the default is CScriptBinderObject::net_Relcase, which does nothing
    OGSR_GUARD_BINDER(luabind::call_member<void>(this, "net_Relcase", object), );
}

void CScriptBinderObjectWrapper::net_Relcase_static(CScriptBinderObject* script_binder_object, CScriptGameObject* object)
{
    script_binder_object->CScriptBinderObject::net_Relcase(object);
}