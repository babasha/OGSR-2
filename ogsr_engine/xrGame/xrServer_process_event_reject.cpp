#include "StdAfx.h"
#include "xrServer.h"
#include "xrServer_Objects.h"
#include "sv_telemetry.h"

bool xrServer::Process_event_reject(NET_Packet& P, const ClientID sender, const u32 time, const u16 id_parent, const u16 id_entity, bool send_message)
{
    // Parse message
    CSE_Abstract* e_parent = game->get_entity_from_eid(id_parent);
    CSE_Abstract* e_entity = game->get_entity_from_eid(id_entity);

    //	R_ASSERT			(e_parent && e_entity);
    if (!e_parent)
    {
        MsgDbg("~ xrServer::Process_event_reject: no parent object! ID %d", id_parent);
        return false;
    }
    if (!e_entity)
    {
        MsgDbg("~ xrServer::Process_event_reject: no entity object! ID %d", id_entity);
        return false;
    }
    game->OnDetach(id_parent, id_entity);

    if (0xffff == e_entity->ID_Parent)
    {
        MsgDbg("! ERROR: can't detach independent object. entity[%s][%d], parent[%s][%d], section[%s]", e_entity->name_replace(), id_entity, e_parent->name_replace(), id_parent,
            e_entity->s_name.c_str());
        return false;
    }

    // Rebuild parentness
    if (e_entity->ID_Parent != id_parent)
    {
        // it can't be !!!

        Msg("! ERROR: e_entity->ID_Parent = [%d]  parent = [%d][%s]  entity_id = [%d]  frame = [%d]", e_entity->ID_Parent, id_parent, e_parent->name_replace(), id_entity,
            Device.dwFrame);
    }

    auto& children = e_parent->children;
    const auto child = std::find(children.begin(), children.end(), id_entity);
    if (child == children.end())
    {
        MsgDbg("! ERROR: SV: can't find children [%d] of parent [%d]", id_entity, e_parent);
        return false;
    }

    // Counterpart of the ownership event: dropping, selling, or taking
    // something out of a container. Logged before the link is cut, so the
    // record can still name who is losing it.
    SVTelemetry::OnItemMoved("drop", e_entity, e_parent, nullptr);

    e_entity->ID_Parent = 0xffff;

    children.erase(child);

    // Signal to everyone (including sender)
    if (send_message)
    {
        DWORD MODE = net_flags(TRUE, TRUE, FALSE, TRUE);
        SendBroadcast(BroadcastCID, P, MODE);
    }

    return true;
}
