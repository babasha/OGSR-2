////////////////////////////////////////////////////////////////////////////
//	Module 		: xrGame.cpp
//	Created 	: 07.01.2001
//  Modified 	: 27.05.2004
//	Author		: Aleksandr Maksimchuk and Oles' Shyshkovtsov
//	Description : Defines the entry point for the DLL application.
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "object_factory.h"
#include "ui/xrUIXmlParser.h"
#include "xr_level_controller.h"


extern "C" {
DLL_API DLL_Pure* __cdecl xrFactory_Create(CLASS_ID clsid)
{
    DLL_Pure* object = object_factory().client_object(clsid);
    object->CLS_ID = clsid;
    return (object);
}

DLL_API void __cdecl xrFactory_Destroy(DLL_Pure* O) { xr_delete(O); }
};

extern void CCC_RegisterCommands();
extern float g_fTimeFactor;
extern shared_str g_active_task_id;

void AttachGame()
{
    g_fTimeFactor = pSettings->r_float("alife", "time_factor");
    g_active_task_id = "";

    // register console commands
    CCC_RegisterCommands();
    // keyboard binding
    CCC_RegisterInput();
}

