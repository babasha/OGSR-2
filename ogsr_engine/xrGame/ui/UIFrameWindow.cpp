#include "StdAfx.h"
#include "UIFrameWindow.h"
#include "../HUDManager.h"
#include "UITextureMaster.h"
#include "UIXmlInit.h"
#include "UIStatic.h"

CUIFrameWindow::CUIFrameWindow()
{
    UITitleText = xr_new<CUIStatic>();
    UITitleText->SetAutoDelete(true);
    AttachChild(UITitleText);
}

void CUIFrameWindow::Init(LPCSTR base_name, float x, float y, float width, float height)
{
    Init(x, y, width, height);
    InitTexture(base_name);
}

void CUIFrameWindow::Init(float x, float y, float width, float height)
{
    CUIWindow::Init(x, y, width, height);
    m_UIWndFrame.Init(x, y, width, height);
    UITitleText->Init(0, 0, width, 50);
}

void CUIFrameWindow::Init(LPCSTR base_name, Frect* pRect) { Init(base_name, pRect->left, pRect->top, pRect->right - pRect->left, pRect->bottom - pRect->top); }

void CUIFrameWindow::InitTexture(const char* texture)
{
    m_UIWndFrame.InitTexture(texture);
    m_bTextureVisible = true;
}

void CUIFrameWindow::Draw()
{
    if (m_bTextureVisible)
    {
        Frect rect;
        GetAbsoluteRect(rect);
        Fvector2 v = rect.lt;
        m_UIWndFrame.SetWndPos(v);
        m_UIWndFrame.Draw();
    }
    inherited::Draw();
}

void CUIFrameWindow::Update()
{
    CUIWindow::Update();
    m_UIWndFrame.Update();
}

void CUIFrameWindow::SetWidth(float width)
{
    inherited::SetWidth(width);
    m_UIWndFrame.SetWidth(width);
}

void CUIFrameWindow::SetHeight(float height)
{
    inherited::SetHeight(height);
    m_UIWndFrame.SetHeight(height);
}

void CUIFrameWindow::SetColor(u32 cl) { m_UIWndFrame.SetTextureColor(cl); }

