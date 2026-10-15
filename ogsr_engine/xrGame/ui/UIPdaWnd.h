#pragma once

#include "UIDialogWnd.h"
#include "UIPdaAux.h"
#include "../encyclopedia_article_defs.h"

// ⭐13-10-2026: the MODEL of the PDA only. What the player sees is the RmlUi screen (ui_rml_pda.cpp); the legacy view
// -- the 1024x768 frame with its seven tabs (CUIMapWnd, CUIEventsWnd, CUIDiaryWnd, CUIEncyclopediaWnd,
// CUIPdaContactsWnd, CUIActorInfoWnd, CUIStalkersRankingWnd) and the user-mark window CUIPdaSpot -- is deleted. Kept
// is what the game still talks to: the active tab (P / M / C, a tutorial step, the 3D PDA in the hands), "this part
// changed" (PdaContentsChanged -> the HUD's flashing icon, g_pda_info_state, the screen's dirty tabs) and the 3D PDA's
// joystick animation targets (PDA.cpp). It stays a CUIDialogWnd that is never shown: Lua's get_pda_wnd() hands it out
// as a window, and code asking "is the PDA up?" of it gets the honest answer, no.
class CUIPdaWnd : public CUIDialogWnd
{
private:
    typedef CUIDialogWnd inherited;

public:
    EPdaTabs m_pActiveSection{eptNoActiveTab};

    CUIPdaWnd() = default;
    virtual ~CUIPdaWnd() = default;

    void SetActiveSubdialog(EPdaTabs section);
    virtual bool StopAnyMove() { return false; }

    void PdaContentsChanged(pda_section::part type, bool = true, bool = false);
    virtual void Reset(); // also the scripts' ranking list (actor_stats.add_to_ranking)

    // the 3D PDA's joystick bone (CPda::UpdateHudAdditional)
    Fvector target_joystickrot{}, joystickrot{};
    float target_buttonpress{}, buttonpress{};

    void ResetJoystick(bool bForce)
    {
        if (bForce)
        {
            joystickrot.set(0.f, 0.f, 0.f);
            buttonpress = 0.f;
        }

        target_joystickrot.set(0.f, 0.f, 0.f);
        target_buttonpress = 0.f;
    }
};
