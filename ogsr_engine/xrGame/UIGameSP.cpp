#include "StdAfx.h"

#include "xr_platform_keys.h"

#include "UIGameSP.h"
#include "Actor.h"
#include "Level.h"

#include "game_cl_single.h"
#include "ui/UIPdaAux.h"
#include "xr_level_controller.h"
#include "ActorCondition.h"
#include "object_broker.h"
#include "GametaskManager.h"
#include "GameTask.h"
#include "PDA.h"

#include "ui_rml.h" // the inventory, search, trade and talk screens
#include "UIDialogHolder.h"
#include "HUDManager.h"
#include "UICursor.h"
#include "../xr_3da/CustomHUD.h"
#include "ui/UIPdaWnd.h"
#include "ui/UIMessageBox.h"

#include "Inventory.h"
#include "HudItem.h"

CUIGameSP::CUIGameSP()
{
    m_game = NULL;

    PdaMenu = xr_new<CUIPdaWnd>();
    UIChangeLevelWnd = xr_new<CChangeLevelWnd>();
}

CUIGameSP::~CUIGameSP()
{
    delete_data(PdaMenu);
    delete_data(UIChangeLevelWnd);
}

void CUIGameSP::shedule_Update(u32 dt)
{
    inherited::shedule_Update(dt);

    CActor* pActor = smart_cast<CActor*>(Level().CurrentEntity());
    if (!pActor)
        return;
    if (pActor->g_Alive())
        return;

    HideShownDialogs();
}

void CUIGameSP::HideShownDialogs()
{
    CUIDialogWnd* mir = MainInputReceiver();
    if (mir && mir == PdaMenu)
        mir->GetHolder()->StartStopMenu(mir, true);
    UIRml::ScreenClose("talk");
    UIRml::InvClose();
}

void CUIGameSP::SetClGame(game_cl_GameState* g)
{
    inherited::SetClGame(g);
    m_game = smart_cast<game_cl_Single*>(g);
    R_ASSERT(m_game);
}

extern bool attach_adjust_mode_keyb(int dik);
extern void attach_draw_adjust_mode();
extern void hud_adjust_mode_keyb(int dik);
extern void hud_draw_adjust_mode();

extern bool g_actor_allow_pda;

bool CUIGameSP::IR_OnKeyboardPress(int dik)
{
    if (inherited::IR_OnKeyboardPress(dik))
        return true;

    if (Device.Paused())
        return false;

    CActor* pActor = smart_cast<CActor*>(Level().CurrentEntity());
    if (!pActor)
        return false;
    if (pActor && !pActor->g_Alive())
        return false;

    hud_adjust_mode_keyb(dik);
    if (attach_adjust_mode_keyb(dik))
        return true;

    auto active_hud = smart_cast<CHudItem*>(pActor->inventory().ActiveItem());
    if (active_hud && active_hud->GetState() != CHudItem::eIdle && Core.Features.test(xrCore::Feature::busy_actor_restrictions))
        return false;

    auto bind = get_binded_action(dik);
    switch (bind)
    {
    case kINVENTORY:
        if (!MainInputReceiver())
        {
            auto Pda = pActor->GetPDA();
            if (!Pda || !Pda->Is3DPDA() || !psActorFlags.test(AF_3D_PDA) || !PdaMenu->IsShown())
            {
                // ⚠Closing does NOT come back here: while the document is up
                // UIRml::OnKey owns the keyboard and handles the bound key
                // itself, so this branch never sees the second press.
                UIRml::InvOpen();
                return true;
            }
        }
        break;

    case kACTIVE_JOBS:
    case kMAP:
    case kCONTACTS: {
        auto Pda = pActor->GetPDA();
        if ((!Pda || !Pda->Is3DPDA() || !psActorFlags.test(AF_3D_PDA)) && (!MainInputReceiver() || MainInputReceiver() == PdaMenu))
        {
            if (g_actor_allow_pda)
            {
                // 10-10: the PDA is an RmlUi screen (ui_rml_pda.cpp); CUIPdaWnd is only its model.
                Msg("~ [pda] 2D screen (3d pda: %s, g_3d_pda %d)", Pda ? (Pda->Is3DPDA() ? "yes" : "no") : "no pda", psActorFlags.test(AF_3D_PDA) ? 1 : 0);
                UIRml::PdaOpen(bind == kACTIVE_JOBS ? eptQuests : (bind == kMAP ? eptMap : eptContacts));
                return true;
            }
        }
    }
    break;

    case kWPN_FIRE:
    case kWPN_ZOOM: {
        auto Pda = pActor->GetPDA();
        if (Pda && Pda->Is3DPDA() && psActorFlags.test(AF_3D_PDA) && PdaMenu->IsShown() && (!MainInputReceiver() || MainInputReceiver() != PdaMenu))
        {
            Flags8 IRFlags{};
            IRFlags.set(recvItem::eCrosshair, psHUD_Flags.test(HUD_CROSSHAIR_RT));
            IRFlags.set(recvItem::eIndicators, HUD().GetUI()->GameIndicatorsShown());

            HUD().GetUI()->SetMainInputReceiver(PdaMenu, false, IRFlags);
            Pda->m_bZoomed = true;
            return true;
        }
    }
    break;

    case kSCORES: {
        SDrawStaticStruct* ss = AddCustomStatic("main_task", true);
        SGameTaskObjective* o = pActor->GameTaskManager().ActiveObjective();
        if (!o)
            ss->m_static->SetTextST("st_no_active_task");
        else
            ss->m_static->SetTextST(*(o->description));
    }
    break;
    }
    return false;
}

bool CUIGameSP::IR_OnKeyboardRelease(int dik)
{
    if (inherited::IR_OnKeyboardRelease(dik))
        return true;

    if (is_binded(kSCORES, dik))
        RemoveCustomStatic("main_task");

    return false;
}

void CUIGameSP::Render()
{
    inherited::Render();
    hud_draw_adjust_mode();
    attach_draw_adjust_mode();
}

void CUIGameSP::StartTalk() { UIRml::TalkStart(); }

void CUIGameSP::StartCarBody(CInventoryOwner* pOurInv, CInventoryOwner* pOthers)
{
    if (MainInputReceiver())
        return;
    UIRml::CarBodyOpen(pOthers, nullptr);
}
void CUIGameSP::StartCarBody(CInventoryOwner* pOurInv, IInventoryBox* pBox)
{
    if (MainInputReceiver())
        return;
    UIRml::CarBodyOpen(nullptr, pBox);
}

void CUIGameSP::ReInitShownUI()
{
    // ⭐The one place the game says "what is carried has changed" -- Lua calls it
    // through update_inventory_window(). Marks; the rebuild is a frame's job.
    UIRml::InvInvalidate();
};

extern ENGINE_API BOOL bShowPauseString;

void CUIGameSP::ChangeLevel(GameGraph::_GRAPH_ID game_vert_id, u32 level_vert_id, Fvector pos, Fvector ang, Fvector pos2, Fvector ang2, bool b)
{
    if ((!MainInputReceiver() || MainInputReceiver() != UIChangeLevelWnd) && !UIChangeLevelWnd->ShownRml())
    {
        UIChangeLevelWnd->m_game_vertex_id = game_vert_id;
        UIChangeLevelWnd->m_level_vertex_id = level_vert_id;
        UIChangeLevelWnd->m_position = pos;
        UIChangeLevelWnd->m_angles = ang;
        UIChangeLevelWnd->m_position_cancel = pos2;
        UIChangeLevelWnd->m_angles_cancel = ang2;
        UIChangeLevelWnd->m_b_position_cancel = b;
        if (!UIChangeLevelWnd->ShowRml())
            m_game->StartStopMenu(UIChangeLevelWnd, true);
    }
}

void CUIGameSP::reset_ui()
{
    inherited::reset_ui();
    PdaMenu->Reset();
    UIChangeLevelWnd->Reset();
}

// The 3D PDA came into the hands / went out of them (CPda::OnActiveItem / OnHiddenItem).
// 10-10: not the legacy window drawn over the screen any more but the RmlUi screen,
// opened once the draw animation has played; closing it puts the PDA away.
void CUIGameSP::ShowHidePda(const bool show)
{
    if (show)
        UIRml::PdaOpenWhenDrawn(PdaMenu->m_pActiveSection);
    else
    {
        UIRml::PdaCancelPending();
        if (UIRml::PdaActive())
            UIRml::ScreenClose("pda");
    }
}

CChangeLevelWnd::CChangeLevelWnd()
{
    m_messageBox = xr_new<CUIMessageBox>();
    m_messageBox->SetAutoDelete(true);
    AttachChild(m_messageBox);
    m_messageBox->Init("message_box_change_level");
    SetWndPos(m_messageBox->GetWndPos());
    m_messageBox->SetWndPos(0.0f, 0.0f);
    SetWndSize(m_messageBox->GetWndSize());
}

void CChangeLevelWnd::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
    if (pWnd == m_messageBox)
    {
        if (msg == MESSAGE_BOX_YES_CLICKED)
        {
            OnOk();
        }
        else if (msg == MESSAGE_BOX_NO_CLICKED)
        {
            OnCancel();
        }
    }
    else
        inherited::SendMessage(pWnd, msg, pData);
}

void CChangeLevelWnd::OnOk()
{
    Game().StartStopMenu(this, true);
    SendChange();
}

void CChangeLevelWnd::SendChange()
{
    NET_Packet p;
    p.w_begin(M_CHANGE_LEVEL);
    p.w(&m_game_vertex_id, sizeof(m_game_vertex_id));
    p.w(&m_level_vertex_id, sizeof(m_level_vertex_id));
    p.w_vec3(m_position);
    p.w_vec3(m_angles);

    Level().Send(p, net_flags(TRUE));
}

void CChangeLevelWnd::OnCancel()
{
    Game().StartStopMenu(this, true);
    CancelMove();
}

void CChangeLevelWnd::CancelMove()
{
    auto pGameSP = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame());
    if (auto pda = Actor()->GetPDA(); pda && pda->Is3DPDA() && psActorFlags.test(AF_3D_PDA) && pGameSP->PdaMenu->IsShown())
    {
        pGameSP->PdaMenu->SetHolder(HUD().GetUI());
        GetUICursor()->Show();
        if (pda->m_bZoomed)
            HUD().GetUI()->SetMainInputReceiver(pGameSP->PdaMenu, false);
    }

    if (m_b_position_cancel)
    {
        Actor()->MoveActor(m_position_cancel, m_angles_cancel);
    }
}

bool CChangeLevelWnd::OnKeyboard(int dik, EUIMessages keyboard_action)
{
    if (keyboard_action == WINDOW_KEY_PRESSED)
    {
        if (dik == DIK_RETURN || dik == DIK_NUMPADENTER)
        {
            OnOk();
            return true;
        }
        else if (is_binded(kQUIT, dik))
        {
            OnCancel();
            return true;
        }        
    }
    return inherited::OnKeyboard(dik, keyboard_action);
}

bool g_block_pause = false;

void CChangeLevelWnd::Show()
{
    g_block_pause = true;
    Device.Pause(TRUE, TRUE, TRUE, "CChangeLevelWnd_show");
    bShowPauseString = FALSE;
}

void CChangeLevelWnd::Hide()
{
    g_block_pause = false;
    Device.Pause(FALSE, TRUE, TRUE, "CChangeLevelWnd_hide");
}

// ⭐10-10 (the owner: "the level change comes up all the time and looks old"). Asked on RmlUi -- level_change.rml,
// the look of the other screens -- while the window stays the MODEL: the scripts write the question into its message
// box first (bind_stalker level_changer_action: "<level>: <invitation>") and wait for it to stop being shown (dsh,
// for the autosave of the transition), so it is marked shown and pauses the game exactly as before, only nothing of
// it is drawn and the answer comes from the document's buttons (native:yes / native:no, Esc and B are no).
bool CChangeLevelWnd::ShowRml()
{
    auto on_act = [this](LPCSTR act) {
        if (!m_rml)
            return;
        m_rml = false; // the close below must not answer a second time
        UIRml::ScreenClose("dialog");
        RmlDone();
        if (!xr_strcmp(act, "yes"))
            SendChange();
        else
            CancelMove();
    };
    auto on_close = [this]() {
        if (!m_rml)
            return;
        m_rml = false;
        RmlDone();
        CancelMove();
    };
    if (!UIRml::DialogOpenNative("level_change.rml", on_act, on_close))
        return false;

    // "<level>: <invitation>" -> the level as the title, the rest as the question
    xr_string text = m_messageBox->GetText() ? m_messageBox->GetText() : "";
    xr_string level;
    if (const size_t colon = text.find(": "); colon != xr_string::npos && colon < 64)
    {
        level = text.substr(0, colon);
        text.erase(0, colon + 2);
    }
    UIRml::LuaSetInner("lc-level", UIRml::LuaText(level.c_str()).c_str());
    UIRml::LuaShow("lc-level", !level.empty());
    UIRml::LuaSetInner("lc-text", UIRml::LuaText(text.c_str()).c_str());
    UIRml::DialogFocus();

    m_rml = true;
    CUIWindow::Show(true); // IsShown() is what the scripts wait on
    Show(); // the pause, as the legacy window
    Msg("~ [rml] level change: asked [%s]", level.c_str());
    return true;
}

void CChangeLevelWnd::RmlDone()
{
    CUIWindow::Show(false);
    Hide();
}
