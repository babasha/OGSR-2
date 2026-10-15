#include "StdAfx.h"
#include "UI.h"
#include "../xr_3da/XR_IOConsole.h"
#include "Entity.h"
#include "HUDManager.h"
#include "UIGameSP.h"
#include "Actor.h"
#include "Level.h"
#include "game_cl_base.h"
#include "ui/UIMainIngameWnd.h"
#include "ui/UIPdaWnd.h"
#include "xr_level_controller.h"
#include "PDA.h"
#include "Actor_Flags.h"

CUI::CUI(CHUDManager* p)
{
    UIMainIngameWnd = xr_new<CUIMainIngameWnd>();
    UIMainIngameWnd->Init();

    m_Parent = p;
    pUIGame = NULL;

    ShowGameIndicators();
    ShowCrosshair();
}
//--------------------------------------------------------------------

CUI::~CUI()
{
    xr_delete(pUIGame);
    xr_delete(UIMainIngameWnd);
}

//--------------------------------------------------------------------

void CUI::Load(CUIGameCustom* pGameUI)
{
    if (pGameUI)
    {
        pGameUI->SetClGame(&Game());
        return;
    }
    pUIGame = Game().createGameUI();
    R_ASSERT(pUIGame);
}

void CUI::UnLoad()
{
    xr_delete(pUIGame);
    pUIGame = Game().createGameUI();
    R_ASSERT(pUIGame);
}
//--------------------------------------------------------------------

void CUI::UIOnFrame()
{
    CEntity* m_Actor = smart_cast<CEntity*>(Level().CurrentEntity());
    if (m_Actor)
    {
        // update windows
        if (GameIndicatorsShown() && psHUD_Flags.is(HUD_DRAW | HUD_DRAW_RT))
        {
            UIMainIngameWnd->Update();
        }
    }

    // out GAME-style depend information
    if (GameIndicatorsShown())
    {
        if (pUIGame)
            pUIGame->OnFrame();
    }
}
//--------------------------------------------------------------------
#include "Inventory.h"
#include "HudItem.h"
bool CUI::Render()
{
    if (GameIndicatorsShown() /*&& psHUD_Flags.is(HUD_DRAW | HUD_DRAW_RT)*/)
    {
        if (pUIGame)
            pUIGame->Render();
    }

    if (auto pActor = smart_cast<CActor*>(Level().CurrentEntity()))
    {
        auto item = smart_cast<CHudItem*>(pActor->inventory().ActiveItem());
        if (item && pActor->HUDview())
            item->OnDrawUI();
    }

    if (psHUD_Flags.is(HUD_DRAW | HUD_DRAW_RT))
    {
        // 13-10-2026: the news log (CUIMessagesWindow) is gone -- the news are cards of the RmlUi HUD
        if (GameIndicatorsShown())
            UIMainIngameWnd->Draw();
    }

    DoRenderDialogs();

    return false;
}

bool CUI::IR_OnMouseWheel(int direction)
{
    if (MainInputReceiver())
    {
        if (MainInputReceiver()->IR_OnMouseWheel(direction))
            return true;
    }

    if (pUIGame && pUIGame->IR_OnMouseWheel(direction))
        return true;

    if (MainInputReceiver())
        return true;

    return false;
}

//--------------------------------------------------------------------
bool CUI::IR_OnKeyboardHold(int dik)
{
    if (MainInputReceiver())
    {
        if (MainInputReceiver()->IR_OnKeyboardHold(dik))
            return true;
    }

    if (MainInputReceiver())
        return true;

    return false;
}

bool CUI::IR_OnKeyboardPress(int dik)
{
    // 06-10: the inventory key closes the PDA. On the Switch a tap of Minus is the
    // inventory and a hold the PDA; Esc is Plus, at the other end of the pad. Here, ahead
    // of the dialog: the PDA swallows the keys it is handed.
    // ⚠This mod's PDA is the 3D one, an ITEM in the hands: it is not a dialog until
    // zoomed, and P puts it away by emptying the hands (CInventory::Action
    // kACTIVE_JOBS) -- so that is what the inventory key does to it too.
    if (get_binded_action(dik) == kINVENTORY)
        if (auto sp = smart_cast<CUIGameSP*>(pUIGame); sp && sp->PdaMenu)
        {
            CActor* a = smart_cast<CActor*>(Level().CurrentEntity());
            CPda* pda = a ? a->GetPDA() : nullptr;
            const bool pda3d = pda && pda->Is3DPDA() && psActorFlags.test(AF_3D_PDA);
            if (pda3d && a->inventory().GetActiveSlot() == PDA_SLOT && a->inventory().ActiveItem())
            {
                a->inventory().Activate(NO_ACTIVE_SLOT);
                return true;
            }
        }

    if (MainInputReceiver())
    {
        if (MainInputReceiver()->IR_OnKeyboardPress(dik))
            return true;
    }

    if (UIMainIngameWnd->OnKeyboardPress(dik))
        return true;

    if (pUIGame && pUIGame->IR_OnKeyboardPress(dik))
        return true;

    if (MainInputReceiver())
        return true;

    return false;
}
//--------------------------------------------------------------------

bool CUI::IR_OnKeyboardRelease(int dik)
{
    if (MainInputReceiver())
    {
        if (MainInputReceiver()->IR_OnKeyboardRelease(dik))
            return true;
    }

    if (pUIGame && pUIGame->IR_OnKeyboardRelease(dik))
        return true;

    if (MainInputReceiver())
        return true;

    return false;
}
//--------------------------------------------------------------------

bool CUI::IR_OnMouseMove(int dx, int dy)
{
    if (MainInputReceiver())
    {
        if (MainInputReceiver()->IR_OnMouseMove(dx, dy))
            return true;
    }

    if (pUIGame && pUIGame->IR_OnMouseMove(dx, dy))
        return true;

    if (MainInputReceiver())
        return true;

    return false;
}

SDrawStaticStruct* CUI::AddInfoMessage(LPCSTR message)
{
    SDrawStaticStruct* ss = pUIGame->GetCustomStatic(message);
    if (!ss)
    {
        ss = pUIGame->AddCustomStatic(message, true);
    }
    return ss;
}

void CUI::ShowGameIndicators() { m_bShowGameIndicators = true; }

void CUI::HideGameIndicators()
{
    m_bShowGameIndicators = false;
    hud_disabled_by_user = false;
}

void CUI::ShowCrosshair() { psHUD_Flags.set(HUD_CROSSHAIR_RT, TRUE); }

void CUI::HideCrosshair() { psHUD_Flags.set(HUD_CROSSHAIR_RT, FALSE); }

bool CUI::CrosshairShown() { return !!psHUD_Flags.test(HUD_CROSSHAIR_RT); }

void CUI::OnConnected() { UIMainIngameWnd->OnConnected(); }
