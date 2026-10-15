#include "StdAfx.h"
#include "UICursor.h"
#include "ui/UIStatic.h"
#include "ui_rml.h"

CUICursor::CUICursor()
{
    InitInternal();
    Device.seqRender.Add(this, 1);
}

CUICursor::~CUICursor() { Device.seqRender.Remove(this); }

void CUICursor::InitInternal()
{
    m_static = std::make_unique<CUIStatic>();
    m_static->InitTextureEx("ui\\ui_ani_cursor", "hud\\cursor");

    constexpr Frect rect{0.0f, 0.0f, 40.0f, 40.0f};
    m_static->SetOriginalRect(rect);

    Fvector2 sz{rect.rb};
    sz.x *= UI()->get_current_kx();

    sz.x *= READ_IF_EXISTS(pSettings, r_float, "ui_tweaks", "cursor_scale_x", 1.f);
    sz.y *= READ_IF_EXISTS(pSettings, r_float, "ui_tweaks", "cursor_scale_y", 1.f);

    m_static->SetWndSize(sz);
    m_static->SetStretchTexture(true);
}

void CUICursor::OnRender()
{
    static u32 last_frame{};
    if (last_frame == Device.dwFrame)
        return;
    last_frame = Device.dwFrame;

    if (!IsVisible())
        return;

    // ⚠While an RmlUi document has the pointer it draws its own arrow and gets
    // every mouse delta, so this one never moves: over the trade screen (opened
    // from the legacy talk dialog, which keeps this cursor on) it hung in place.
    if (UIRml::WantsInput())
        return;

    m_static->SetWndPos(vPos);
    m_static->Update();
    m_static->Draw();
}

Fvector2 CUICursor::GetCursorPosition() const { return vPos; }

Fvector2 CUICursor::GetCursorPositionDelta() const { return Fvector2{vPos.x - vPrevPos.x, vPos.y - vPrevPos.y}; }

void CUICursor::UpdateCursorPosition(const int _dx, const int _dy)
{
    vPrevPos = vPos;

    const u32 screen_size_x = GetSystemMetrics(SM_CXSCREEN);
    const u32 screen_size_y = GetSystemMetrics(SM_CYSCREEN);

    const bool m_b_use_win_cursor = (screen_size_y >= Device.dwHeight && screen_size_x >= Device.dwWidth);

    if (m_b_use_win_cursor)
    {
        Ivector2 pti{};

        GetCursorPos((LPPOINT)&pti);

        HWND hwnd = Device.m_hWnd;
        if (hwnd)
            ScreenToClient(hwnd, (LPPOINT)&pti);

        vPos.x = (float)pti.x * (UI_BASE_WIDTH / (float)Device.dwWidth);
        vPos.y = (float)pti.y * (UI_BASE_HEIGHT / (float)Device.dwHeight);
    }
    else
    {
        constexpr float sens = 1.0f;
        vPos.x += _dx * sens;
        vPos.y += _dy * sens;
    }

    clamp(vPos.x, 0.f, UI_BASE_WIDTH);
    clamp(vPos.y, 0.f, UI_BASE_HEIGHT);
}

void CUICursor::SetUICursorPosition(const Fvector2& pos)
{
    vPos = pos;

    const int x = iFloor(vPos.x / (UI_BASE_WIDTH / (float)Device.dwWidth));
    const int y = iFloor(vPos.y / (UI_BASE_HEIGHT / (float)Device.dwHeight));

    SetCursorPos(x, y);
}
