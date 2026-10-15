#include "StdAfx.h"
#include "UIGameCustom.h"
#include "Level.h"
#include "HUDManager.h"
#include "ui/UIStatic.h"

using namespace luabind;


void CUIGameCustom::script_register(lua_State* L)
{
    module(L)[class_<SDrawStaticStruct>("SDrawStaticStruct").def_readwrite("m_endTime", &SDrawStaticStruct::m_endTime).def("wnd", &SDrawStaticStruct::wnd),

              class_<CUIGameCustom>("CUIGameCustom")
                  .def("AddDialogToRender", &CUIGameCustom::AddDialogToRender)
                  .def("RemoveDialogToRender", &CUIGameCustom::RemoveDialogToRender)
                  .def("AddCustomMessage", (void(CUIGameCustom::*)(LPCSTR, float, float, float, CGameFont*, u16, u32 /*, LPCSTR*/)) & CUIGameCustom::AddCustomMessage)
                  .def("AddCustomMessage", (void(CUIGameCustom::*)(LPCSTR, float, float, float, CGameFont*, u16, u32 /*, LPCSTR*/, float)) & CUIGameCustom::AddCustomMessage)
                  .def("CustomMessageOut", &CUIGameCustom::CustomMessageOut)
                  .def("RemoveCustomMessage", &CUIGameCustom::RemoveCustomMessage)
                  .def("AddCustomStatic", &CUIGameCustom::AddCustomStatic)
                  .def("RemoveCustomStatic", &CUIGameCustom::RemoveCustomStatic)
                  .def("GetCustomStatic", (SDrawStaticStruct* (CUIGameCustom::*)(LPCSTR))&CUIGameCustom::GetCustomStatic),
              def("get_hud", [] { return HUD().GetUI()->UIGame(); })];
}
