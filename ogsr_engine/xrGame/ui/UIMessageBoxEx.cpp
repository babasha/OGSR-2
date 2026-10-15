#include "StdAfx.h"

#include "UIMessageBox.h"
#include "UIMessageBoxEx.h"
#include "../UIDialogHolder.h"
#include "../ui_rml.h"
#include "../MainMenu.h"
#include "../Level.h"

CUIMessageBoxEx::CUIMessageBoxEx()
{
    m_pMessageBox = xr_new<CUIMessageBox>();
    m_pMessageBox->SetWindowName("msg_box");
    //	m_pMessageBox->SetAutoDelete(true);
    AttachChild(m_pMessageBox);
}

CUIMessageBoxEx::~CUIMessageBoxEx() { xr_delete(m_pMessageBox); }

void CUIMessageBoxEx::Init(LPCSTR xml_template)
{
    CUIDialogWnd::Init(0, 0, 1024, 768);
    m_pMessageBox->Init(xml_template);
}

void CUIMessageBoxEx::SetText(LPCSTR text) { m_pMessageBox->SetText(text); }

LPCSTR CUIMessageBoxEx::GetText() { return m_pMessageBox->GetText(); }

void CUIMessageBoxEx::SendMessage(CUIWindow* pWnd, s16 msg, void* pData /* = NULL */)
{
    CUIWndCallback::OnEvent(pWnd, msg, pData);
    if (pWnd == m_pMessageBox)
    {
        switch (msg)
        {
        case MESSAGE_BOX_OK_CLICKED:
        case MESSAGE_BOX_YES_CLICKED:
        case MESSAGE_BOX_NO_CLICKED:
        case MESSAGE_BOX_CANCEL_CLICKED:
        case MESSAGE_BOX_QUIT_WIN_CLICKED:
        case MESSAGE_BOX_QUIT_GAME_CLICKED: GetHolder()->StartStopMenu(this, true);
        default: break;
        }

        if (GetMessageTarget())
            GetMessageTarget()->SendMessage(this, msg, pData);
    }
}

LPCSTR CUIMessageBoxEx::GetHost() { return m_pMessageBox->GetHost(); }

LPCSTR CUIMessageBoxEx::GetPassword() { return m_pMessageBox->GetPassword(); }

// ---------------------------------------------------------------- RmlUi ----
//
// 13-10-2026 (the owner: "everything that is left, onto the new UI"). The mod's scripts start these boxes from their
// windows (remkit, car trade, repair...) with start_stop_menu and answer them through MESSAGE_BOX_* callbacks, so the
// box stays exactly that in the holder's stack -- shown, the main input receiver, closed by StartStopMenu -- and only
// the face moves: msgbox.rml (the look of the other dialogs) is opened from Show, its buttons come back as if the
// legacy buttons had been pressed. Esc / B close it the way Esc closed the legacy box: no answer. Kinds with fields
// (direct IP, password) and the main menu's boxes stay legacy.
bool CUIMessageBoxEx::ShowRml()
{
    if (!g_pGameLevel || !UIRml::Ready())
        return false;
    if (MainMenu() && MainMenu()->IsActive())
        return false;
    const auto style = m_pMessageBox->GetStyle();
    const char* kind = nullptr;
    switch (style)
    {
    case CUIMessageBox::MESSAGEBOX_OK: kind = "ok"; break;
    case CUIMessageBox::MESSAGEBOX_INFO: kind = "info"; break;
    case CUIMessageBox::MESSAGEBOX_YES_NO:
    case CUIMessageBox::MESSAGEBOX_QUIT_GAME:
    case CUIMessageBox::MESSAGEBOX_QUIT_WINDOWS: kind = "yesno"; break;
    case CUIMessageBox::MESSAGEBOX_YES_NO_CANCEL: kind = "yesnocancel"; break;
    default: return false;
    }

    auto on_act = [this](LPCSTR act) { RmlAnswer(act); };
    auto on_close = [this]() {
        if (!m_rml)
            return;
        m_rml = false; // Esc / B / closed from outside: the legacy Esc, no answer
        if (IsShown() && GetHolder())
            GetHolder()->StartStopMenu(this, true);
    };
    if (!UIRml::DialogOpenNative("msgbox.rml", on_act, on_close))
        return false;

    UIRml::LuaSetClass("mb-panel", kind, true);
    const LPCSTR text = m_pMessageBox->GetText();
    UIRml::LuaSetInner("mb-text", UIRml::LuaText(text ? text : "").c_str());
    UIRml::DialogFocus();
    m_rml = true;
    Msg("~ [rml] message box: %s", kind);
    return true;
}

void CUIMessageBoxEx::RmlAnswer(LPCSTR act)
{
    if (!m_rml)
        return;
    m_rml = false; // the close below must not answer a second time
    UIRml::ScreenClose("dialog");

    s16 msg = -1;
    const auto style = m_pMessageBox->GetStyle();
    if (!xr_strcmp(act, "yes"))
        msg = style == CUIMessageBox::MESSAGEBOX_OK                ? s16(MESSAGE_BOX_OK_CLICKED)
            : style == CUIMessageBox::MESSAGEBOX_QUIT_GAME     ? s16(MESSAGE_BOX_QUIT_GAME_CLICKED)
            : style == CUIMessageBox::MESSAGEBOX_QUIT_WINDOWS  ? s16(MESSAGE_BOX_QUIT_WIN_CLICKED)
                                                               : s16(MESSAGE_BOX_YES_CLICKED);
    else if (!xr_strcmp(act, "no"))
        msg = MESSAGE_BOX_NO_CLICKED;
    else if (!xr_strcmp(act, "cancel"))
        msg = MESSAGE_BOX_CANCEL_CLICKED;

    if (msg < 0)
    {
        if (IsShown() && GetHolder())
            GetHolder()->StartStopMenu(this, true);
        return;
    }
    // as if the legacy box had sent it: this closes the box and tells the window that owns it
    SendMessage(m_pMessageBox, msg, nullptr);
}

void CUIMessageBoxEx::Show()
{
    CUIDialogWnd::Show();
    if (!m_rml)
        ShowRml();
}

void CUIMessageBoxEx::Hide()
{
    CUIDialogWnd::Hide();
    if (m_rml)
    {
        m_rml = false; // taken off the stack by somebody else: the face goes too
        UIRml::ScreenClose("dialog");
    }
}

void CUIMessageBoxEx::Draw()
{
    if (!m_rml)
        CUIDialogWnd::Draw();
}
