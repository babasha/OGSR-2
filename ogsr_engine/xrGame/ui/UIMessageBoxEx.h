#pragma once
#include "UIDialogWnd.h"
#include "UIWndCallback.h"
//#include "UIMessageBox.h"

class CUIMessageBox;

class CUIMessageBoxEx : public CUIDialogWnd, public CUIWndCallback
{
public:
    CUIMessageBoxEx();
    virtual ~CUIMessageBoxEx();
    void SetText(LPCSTR text);
    LPCSTR GetText();
    virtual void Init(LPCSTR xml_template);
    virtual void SendMessage(CUIWindow* pWnd, s16 msg, void* pData = NULL);

    LPCSTR GetHost();
    LPCSTR GetPassword();

    // 13-10-2026: in the game the box is asked on RmlUi (msgbox.rml); it stays in the holder's stack as the MODEL
    // (the callers' start_stop_menu and MESSAGE_BOX_* callbacks are unchanged) and draws nothing itself.
    virtual void Show();
    virtual void Hide();
    virtual void Draw();
    virtual bool NeedCursor() const { return !m_rml; }

    CUIMessageBox* m_pMessageBox;

private:
    bool ShowRml();
    void RmlAnswer(LPCSTR act);
    bool m_rml{};
};