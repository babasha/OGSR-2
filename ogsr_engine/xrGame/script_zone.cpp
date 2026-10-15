////////////////////////////////////////////////////////////////////////////
//	Module 		: script_zone.cpp
//	Created 	: 10.10.2003
//  Modified 	: 10.10.2003
//	Author		: Dmitriy Iassenev
//	Description : Script zone object
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "script_zone.h"
#include "script_game_object.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../xr_3da/xr_collide_form.h"
#include "script_callback_ex.h"
#include "game_object_space.h"

CScriptZone::CScriptZone() {}

CScriptZone::~CScriptZone() {}

void CScriptZone::reinit() { inherited::reinit(); }

BOOL CScriptZone::net_Spawn(CSE_Abstract* DC)
{
    feel_touch.clear();

    if (!inherited::net_Spawn(DC))
        return (FALSE);

    return (TRUE);
}

void CScriptZone::net_Destroy() { inherited::net_Destroy(); }

void CScriptZone::shedule_Update(u32 dt)
{
    inherited::shedule_Update(dt);

    const Fsphere& s = CFORM()->getSphere();
    Fvector P;
    XFORM().transform_tiny(P, s.P);
    feel_touch_update(P, s.R);
}

void CScriptZone::feel_touch_new(CObject* tpObject)
{
    CGameObject* l_tpGameObject = smart_cast<CGameObject*>(tpObject);
    if (!l_tpGameObject)
        return;

    callback(GameObject::eZoneEnter)(lua_game_object(), l_tpGameObject->lua_game_object());
}

void CScriptZone::feel_touch_delete(CObject* tpObject)
{
    CGameObject* l_tpGameObject = smart_cast<CGameObject*>(tpObject);

    if (!l_tpGameObject || l_tpGameObject->getDestroy())
        return;

    callback(GameObject::eZoneExit)(lua_game_object(), l_tpGameObject->lua_game_object());
}

void CScriptZone::net_Relcase(CObject* O)
{
    xr_vector<CObject*>::iterator I = std::find(feel_touch.begin(), feel_touch.end(), O);
    if (I == feel_touch.end())
        return;
    CGameObject* l_tpGameObject = smart_cast<CGameObject*>(O);
    if (l_tpGameObject)
    {
        callback(GameObject::eZoneExit)(lua_game_object(), l_tpGameObject->lua_game_object());
    }
}

BOOL CScriptZone::feel_touch_contact(CObject* O) { return (((CCF_Shape*)CFORM())->Contact(O)); }

bool CScriptZone::active_contact(u16 id) const
{
    xr_vector<CObject*>::const_iterator I = feel_touch.begin();
    xr_vector<CObject*>::const_iterator E = feel_touch.end();
    for (; I != E; ++I)
        if ((*I)->ID() == id)
            return (true);
    return (false);
}
