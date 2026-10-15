#pragma once

#include "UIWindow.h"

#include "../UIFrameRect.h"

class CUIStatic;

class CUIFrameWindow : public CUIWindow, public CUIMultiTextureOwner
{
private:
    typedef CUIWindow inherited;

public:
    using CUIWindow::Draw;
    CUIFrameWindow();

    virtual void Init(LPCSTR base_name, float x, float y, float width, float height);
    virtual void Init(float x, float y, float width, float height);
    virtual void Init(LPCSTR base_name, Frect* pRect);

    virtual void InitTexture(const char* texture);
    void SetTextureColor(u32 color) { m_UIWndFrame.SetTextureColor(color); }

    virtual void SetWidth(float width);
    virtual void SetHeight(float height);

    void SetColor(u32 cl);

    virtual void Draw();
    virtual void Update();

    //текст заголовка
    CUIStatic* UITitleText;
    CUIStatic* GetTitleStatic() { return UITitleText; };
    void SetVisiblePart(CUIFrameRect::EFramePart p, BOOL b) { m_UIWndFrame.SetVisiblePart(p, b); };

protected:
    CUIFrameRect m_UIWndFrame;

private:
};
