#include "StdAfx.h"
#include "xrServer.h"
#include "xrServer_Objects.h"
#include "sv_telemetry.h"
#include "sv_loot.h"

void ReplaceOwnershipHeader(NET_Packet& P)
{
    //способ очень грубый, но на данный момент иного выбора нет. Заранее приношу извинения
    u16 NewType = GE_OWNERSHIP_TAKE;
    CopyMemory(&P.B.data[6], &NewType, 2);
};

void xrServer::Process_event_ownership(NET_Packet& P, ClientID sender, u32 time, u16 ID, BOOL bForced, u16 event_type)
{
    u32 MODE = net_flags(TRUE, TRUE, FALSE, TRUE);

    u16 id_parent = ID, id_entity;
    P.r_u16(id_entity);
    CSE_Abstract* e_parent = game->get_entity_from_eid(id_parent);
    CSE_Abstract* e_entity = game->get_entity_from_eid(id_entity);

#ifdef DEBUG
    Msg("sv ownership id_parent %s id_entity %s [%d]", ent_name_safe(id_parent).c_str(), ent_name_safe(id_entity).c_str(), Device.dwFrame);
#endif

    if (!e_entity)
    {
        SVLoot::OnRefused(id_entity, id_parent, SVLoot::Refusal::NoSuchItem);
        return;
    }
    R_ASSERT(/*e_entity &&*/ e_parent);
    if (0xffff != e_entity->ID_Parent)
    {
        Msg("sv !ownership (entity already has parent) new_parent %s id_parent %s id_entity %s [%d]", ent_name_safe(e_entity->ID_Parent).c_str(), ent_name_safe(id_parent).c_str(),
            ent_name_safe(id_entity).c_str(), Device.dwFrame);
        SVLoot::OnRefused(id_entity, id_parent, SVLoot::Refusal::AlreadyOwned);
        return;
    }

    xrClientData* c_parent = e_parent->owner;
    xrClientData* c_from = ID_to_client(sender);

    const bool from_server = (GetServerClient() == c_from);
    if (!from_server && (c_parent != c_from))
    {
        // trust only ServerClient or new_ownerClient
        SVLoot::OnRefused(id_entity, id_parent, SVLoot::Refusal::NotYourClient);
        return;
    }

    // ⭐The one place a plausibility check on a take can live. Off by default:
    // it measures the distance every transfer happens at and refuses nothing
    // until sv_loot_max_dist says so. See sv_loot.h for why that order.
    // ⭐⭐Refusing is COMPLETE here, and that is worth stating because it is not
    // obvious and I got it backwards once.
    //
    // Ownership in this engine is GRANTED, not asserted. A client that wants an
    // item sends the take and waits; the item enters an inventory only when the
    // server broadcasts the event back and the owner's own handler runs
    // inventory().Take (ai_stalker_events.cpp / Actor_Events.cpp). So a refusal
    // needs no correction packet: nobody ever had the thing.
    //
    // ⚠Measured 28-07-2026, by breaking it: sending GE_OWNERSHIP_REJECT back on
    // a refusal -- on the theory that the client had already taken it -- made
    // the creature try to drop an item it did not hold, and CInventory::DropItem
    // fatals on exactly that (m_pCurrentInventory is null). The server died on
    // the first refused pickup. Silence is the correct answer.
    if (!SVLoot::AllowTake(e_entity, e_parent, event_type))
        return;

    // Game allows ownership of entity
    if (game->OnTouch(id_parent, id_entity, bForced))
    {
        // ⚠Recorded HERE, before the parentage below is rebuilt: once the item
        // has an owner it reports the owner's position, and the distance the
        // whole ledger exists to measure is 0.0 for ever after.
        SVLoot::OnTaken(e_entity, e_parent, from_server, event_type);

        // Rebuild parentness
        e_entity->ID_Parent = id_parent;
        e_parent->children.push_back(id_entity);

        // Loot flow: this is the only place an item legitimately gains an
        // owner, so it covers picking up, looting a corpse, buying, and
        // putting something into a container.
        SVTelemetry::OnItemMoved("take", e_entity, nullptr, e_parent);

        if (bForced)
        {
            ReplaceOwnershipHeader(P);
        }
        // Signal to everyone (including sender)
        SendBroadcast(BroadcastCID, P, MODE);
    }
}
