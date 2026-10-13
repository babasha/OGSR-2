// EngineAPI.cpp: implementation of the CEngineAPI class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "EngineAPI.h"
#include "XR_IOConsole.h"
#include "xr_ioc_cmd.h"

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CEngineAPI::CEngineAPI()
{
    pCreate = nullptr;
    pDestroy = nullptr;
}

CEngineAPI::~CEngineAPI()
{
}

//ENGINE_API bool is_enough_address_space_available()
//{
//    SYSTEM_INFO system_info;
//    GetSystemInfo(&system_info);
//
//    return (*(size_t*)&system_info.lpMaximumApplicationAddress) > 0x90000000ull;
//}

#ifdef XRGAME_STATIC
extern "C" {
DLL_Pure* xrFactory_Create(CLASS_ID clsid);
void xrFactory_Destroy(DLL_Pure* O);
}
#endif

void CEngineAPI::Initialize()
{
    // One renderer, linked in statically — no DLL probing and no "renderer"
    // console token to pick between R1..R4 any more.
    void AttachRender();
    AttachRender();

    Device.ConnectToRender();

    pCreate = &xrFactory_Create;
    pDestroy = &xrFactory_Destroy;
    void AttachGame();
    AttachGame();
}

void CEngineAPI::Destroy()
{
    pCreate = nullptr;
    pDestroy = nullptr;
    Engine.Event.Destroy();
}