#include "StdAfx.h"
#include "UIPdaWnd.h"

#include "../Level.h"
#include "../Actor.h"
#include "../xrServer_Objects_ALife_Monsters.h"
#include "../HUDManager.h"
#include "UIMainIngameWnd.h"
#include "../ui_rml.h"

u32 g_pda_info_state = 0;

void CUIPdaWnd::SetActiveSubdialog(EPdaTabs section)
{
    // the tab the screen opens on; its "new" mark goes (the screen clears it again when shown, and sends the infos)
    static const std::pair<EPdaTabs, u32> seen[] = {{eptDiary, pda_section::diary},           {eptContacts, pda_section::contacts},
                                                    {eptMap, pda_section::map},               {eptEncyclopedia, pda_section::encyclopedia},
                                                    {eptActorStatistic, pda_section::statistics}, {eptRanking, pda_section::ranking},
                                                    {eptQuests, pda_section::quests}};
    for (const auto& [tab, bit] : seen)
        if (tab == section)
            g_pda_info_state &= ~bit;
    m_pActiveSection = section;
}

void CUIPdaWnd::PdaContentsChanged(pda_section::part type, bool flash, bool force_update)
{
    if (type == pda_section::contacts)
        flash = false;

    if (flash)
    {
        g_pda_info_state |= type;
        HUD().GetUI()->UIMainIngameWnd->SetFlashIconState_(CUIMainIngameWnd::efiPdaTask, true);
    }

    // the screen the player sees (ui_rml_pda.cpp) rebuilds the tab that changed
    UIRml::PdaContentsChanged(u32(type));
}

// ---------------------------------------------------------------- the ranking ----
//
// The scripts' top list (actor_stats.add_to_ranking / remove_from_ranking / get_actor_ranking), which lived with the
// legacy ranking tab (CUIStalkersRankingWnd) and is kept as it was there -- cleared when the UI is reset.
typedef xr_vector<u16> TOP_LIST;
TOP_LIST g_all_statistic_humans;

extern CSE_ALifeTraderAbstract* ch_info_get_from_id(u16 id);

static bool GreaterRankPred(const u16& h1, const u16& h2)
{
    CSE_ALifeTraderAbstract* t1 = ch_info_get_from_id(h1);
    CSE_ALifeTraderAbstract* t2 = ch_info_get_from_id(h2);
    if (t1 && t2)
        return t1->m_rank > t2->m_rank;
    else if (t1)
        return true;
    return false;
}

int get_actor_ranking()
{
    std::sort(g_all_statistic_humans.begin(), g_all_statistic_humans.end(), GreaterRankPred);
    TOP_LIST::iterator it = std::find(g_all_statistic_humans.begin(), g_all_statistic_humans.end(), Actor()->ID());
    if (it != g_all_statistic_humans.end())
        return (int)std::distance(g_all_statistic_humans.begin(), it);
    else
        return 1;
}

void remove_human_from_top_list(u16 id)
{
    TOP_LIST::iterator it = std::find(g_all_statistic_humans.begin(), g_all_statistic_humans.end(), id);
    if (it != g_all_statistic_humans.end())
        g_all_statistic_humans.erase(it);
}

void add_human_to_top_list(u16 id)
{
    CSE_ALifeTraderAbstract* t = ch_info_get_from_id(id);
    if (t)
    {
        remove_human_from_top_list(id);
        g_all_statistic_humans.push_back(id);
    }
    else
        Msg("! [%s]: id[%d] not a CSE_ALifeTraderAbstract", __FUNCTION__, id);
}

void CUIPdaWnd::Reset()
{
    inherited::Reset();
    g_all_statistic_humans.clear(); // CUIStalkersRankingWnd::Reset
}
