////////////////////////////////////////////////////////////////////////////
//	Module 		: ai_stalker_events.cpp
//	Created 	: 26.02.2003
//  Modified 	: 26.02.2003
//	Author		: Dmitriy Iassenev
//	Description : Events handling for monster "Stalker"
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "ai_stalker.h"
#include "../../PDA.h"
#include "../../Inventory.h"
#include "xrMessages.h"
#include "../../ShootingObject.h"
#include "../../../xr_3da/NET_Server_Trash/NET_utils.h"
#include "Level.h"
#include "../../ai_monster_space.h"
#include "../../stalker_planner.h"
#include "../../script_game_object.h"
#include "../../stalker_decision_space.h"

using namespace StalkerSpace;
using namespace MonsterSpace;

void CAI_Stalker::OnEvent(NET_Packet& P, u16 type)
{
    inherited::OnEvent(P, type);
    CInventoryOwner::OnEvent(P, type);

    switch (type)
    {
    case GE_TRADE_BUY:
    case GE_OWNERSHIP_TAKE:
    case GE_TRANSFER_TAKE: {
        u16 id;
        P.r_u16(id);
        CObject* O = Level().Objects.net_Find(id);

        R_ASSERT(O);

        CGameObject* _O = smart_cast<CGameObject*>(O);
        if (inventory().CanTakeItem(smart_cast<CInventoryItem*>(_O)))
        { // GetScriptControl()
            O->H_SetParent(this);
            inventory().Take(_O, true, false);
            if (!inventory().ActiveItem() && GetScriptControl() && smart_cast<CShootingObject*>(O))
                CObjectHandler::set_goal(eObjectActionIdle, _O);

            on_after_take(_O);
        }
        else
        {
            //				DropItemSendMessage(O);
            NET_Packet _P;
            u_EventGen(_P, GE_OWNERSHIP_REJECT, ID());
            _P.w_u16(u16(O->ID()));
            u_EventSend(_P);
        }
        break;
    }
    case GE_TRADE_SELL:
    case GE_OWNERSHIP_REJECT:
    case GE_TRANSFER_REJECT: {
        u16 id;
        P.r_u16(id);
        CObject* O = Level().Objects.net_Find(id);

        if (!O)
        {
            Msg("! [%s] Error: No object to reject/sell [%u]", __FUNCTION__, id);
            break;
        }

        bool just_before_destroy = !P.r_eof() && P.r_u8();
        bool dont_create_shell = (type == GE_TRADE_SELL) || (type == GE_TRANSFER_REJECT) || just_before_destroy;

        O->SetTmpPreDestroy(just_before_destroy);

        if (inventory().DropItem(smart_cast<CGameObject*>(O)) && !O->getDestroy())
        {
            O->H_SetParent(0, dont_create_shell);
            feel_touch_deny(O, 2000);
        }

        break;
    }
    }
}

void CAI_Stalker::feel_touch_new(CObject* O)
{
    auto& eu = brain().evaluators();

    bool propertyItems = true;
    bool propertyFoundItemToKill = true;

    const auto ePropertyItems = eu.find(StalkerDecisionSpace::eWorldPropertyItems);

    if (ePropertyItems != eu.end())
    {
        propertyItems = (*ePropertyItems).second->evaluate();
    }

    auto ePropertyFoundItemToKill = eu.find(StalkerDecisionSpace::eWorldPropertyFoundItemToKill);
    auto ePropertyItemToKill = eu.find(StalkerDecisionSpace::eWorldPropertyItemToKill);

    if (ePropertyFoundItemToKill != eu.end() && 
        ePropertyItemToKill != eu.end())
    {
        propertyFoundItemToKill = (*ePropertyFoundItemToKill).second->evaluate() && !(*ePropertyItemToKill).second->evaluate();
    }

    if (!propertyItems && !propertyFoundItemToKill) // !(propertyFoundItemToKill && !propertyItemToKill)
        return;

    if (!g_Alive())
        return;

    if (Remote())
        return;

    if ((O->spatial.type | STYPE_VISIBLEFORAI) != O->spatial.type)
        return;

    // Now, test for game specific logical objects to minimize traffic
    CInventoryItem* I = smart_cast<CInventoryItem*>(O);

    if (!wounded() && !critically_wounded() && I && I->useful_for_NPC() && can_take(I))
    {
        NET_Packet P;
        u_EventGen(P, GE_OWNERSHIP_TAKE, ID());
        P.w_u16(u16(I->object().ID()));
        u_EventSend(P);
    }
}

void CAI_Stalker::DropItemSendMessage(CObject* O)
{
    if (!O || !O->H_Parent() || (this != O->H_Parent()))
        return;

    // We doesn't have similar weapon - pick up it
    NET_Packet P;
    u_EventGen(P, GE_OWNERSHIP_REJECT, ID());
    P.w_u16(u16(O->ID()));
    u_EventSend(P);
}

/////////////////////////
// PDA functions
/////////////////////////
void CAI_Stalker::UpdateAvailableDialogs(CPhraseDialogManager* partner)
{
    CAI_PhraseDialogManager::UpdateAvailableDialogs(partner);
}