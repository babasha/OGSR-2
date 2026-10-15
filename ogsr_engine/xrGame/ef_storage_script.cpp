////////////////////////////////////////////////////////////////////////////
//	Module 		: ef_storage_script.cpp
//	Created 	: 25.03.2002
//  Modified 	: 11.10.2002
//	Author		: Dmitriy Iassenev
//	Description : Evaluation functions storage class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "ef_storage.h"
#include "ai_space.h"
#include "script_game_object.h"
#include "entity_alive.h"
#include "script_engine.h"
#include "ef_base.h"
#include "xrServer_Objects_ALife.h"

using namespace luabind;


void CEF_Storage::script_register(lua_State* L)
{
    //module(L)[def("ef_storage", &ef_storage),

    //          class_<CEF_Storage>("cef_storage")
    //              .def("evaluate", (float (*)(CEF_Storage*, LPCSTR, CScriptGameObject*))(&evaluate))
    //              .def("evaluate", (float (*)(CEF_Storage*, LPCSTR, CScriptGameObject*, CScriptGameObject*))(&evaluate))
    //              .def("evaluate", (float (*)(CEF_Storage*, LPCSTR, CScriptGameObject*, CScriptGameObject*, CScriptGameObject*))(&evaluate))
    //              .def("evaluate", (float (*)(CEF_Storage*, LPCSTR, CScriptGameObject*, CScriptGameObject*, CScriptGameObject*, CScriptGameObject*))(&evaluate))
    //              .def("evaluate", (float (*)(CEF_Storage*, LPCSTR, CSE_ALifeObject*))(&evaluate))
    //              .def("evaluate", (float (*)(CEF_Storage*, LPCSTR, CSE_ALifeObject*, CSE_ALifeObject*))(&evaluate))
    //              .def("evaluate", (float (*)(CEF_Storage*, LPCSTR, CSE_ALifeObject*, CSE_ALifeObject*, CSE_ALifeObject*))(&evaluate))
    //              .def("evaluate", (float (*)(CEF_Storage*, LPCSTR, CSE_ALifeObject*, CSE_ALifeObject*, CSE_ALifeObject*, CSE_ALifeObject*))(&evaluate))];
}