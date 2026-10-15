#pragma once
#include "UIGameCustom.h"
#include "ui/UIDialogWnd.h"
#include "../xr_3da/NET_Server_Trash/NET_utils.h"
#include "game_graph_space.h"

class CUIPdaWnd;
class CInventory;

class game_cl_Single;
class CChangeLevelWnd;
class CUIMessageBox;
class IInventoryBox;
class CInventoryOwner;

class CUIGameSP : public CUIGameCustom
{
private:
    game_cl_Single* m_game;
    typedef CUIGameCustom inherited;

public:
    CUIGameSP();
    virtual ~CUIGameSP();

    virtual void reset_ui();
    virtual void shedule_Update(u32 dt);
    virtual void Render();
    virtual void SetClGame(game_cl_GameState* g);
    virtual bool IR_OnKeyboardPress(int dik);
    virtual bool IR_OnKeyboardRelease(int dik);

    void StartTalk();
    void StartCarBody(CInventoryOwner* pOurInv, CInventoryOwner* pOthers);
    void StartCarBody(CInventoryOwner* pOurInv, IInventoryBox* pBox);
    virtual void ReInitShownUI();
    void ChangeLevel(GameGraph::_GRAPH_ID game_vert_id, u32 level_vert_id, Fvector pos, Fvector ang, Fvector pos2, Fvector ang2, bool b);

    virtual void HideShownDialogs();

    // The inventory, the search, the trade and the talk are RmlUi screens
    // (ui_rml_inv.cpp, reached through UIRml::); only the PDA is still a window.
    CUIPdaWnd* PdaMenu;
    CChangeLevelWnd* UIChangeLevelWnd;

    void ShowHidePda(const bool show);
};

class CChangeLevelWnd : public CUIDialogWnd
{
    CUIMessageBox* m_messageBox;
    typedef CUIDialogWnd inherited;
    void OnCancel();
    void OnOk();
    void SendChange();
    void CancelMove();
    // asked on RmlUi (level_change.rml) instead of drawn: the window only keeps the state
    bool m_rml{};
    void RmlDone();

public:
    GameGraph::_GRAPH_ID m_game_vertex_id;
    u32 m_level_vertex_id;
    Fvector m_position;
    Fvector m_angles;
    Fvector m_position_cancel;
    Fvector m_angles_cancel;
    bool m_b_position_cancel;

    CChangeLevelWnd();
    virtual ~CChangeLevelWnd(){};
    virtual void SendMessage(CUIWindow* pWnd, s16 msg, void* pData);
    virtual bool WorkInPause() const { return true; }
    virtual void Show();
    virtual void Hide();
    virtual bool OnKeyboard(int dik, EUIMessages keyboard_action);
    bool ShowRml();
    bool ShownRml() const { return m_rml; }
};