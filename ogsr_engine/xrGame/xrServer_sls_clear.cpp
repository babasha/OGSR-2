#include "StdAfx.h"
#include "game_sv_single.h"
#include "alife_simulator.h"
#include "xrServer_Objects.h"
#include "xrServer.h"
#include "xrMessages.h"
#include "ai_space.h"

void xrServer::Perform_destroy(CSE_Abstract* object, u32 mode)
{
    R_ASSERT(object);
    R_ASSERT(object->ID_Parent == 0xffff);

    while (!object->children.empty())
    {
        CSE_Abstract* child = game->get_entity_from_eid(object->children.back());
        ASSERT_FMT(child, "child registered but not found [%d] for parent id=%d", object->children.back(), object->ID);

        Perform_reject(child, object, 2 * NET_Latency);
        Perform_destroy(child, mode);
    }

    //	Msg						("SLS-CLEAR : DESTROY [%s][%s]",object->name(),object->name_replace());
    u16 object_id = object->ID;
    entity_Destroy(object);

    NET_Packet P;
    P.w_begin(M_EVENT);
    P.w_u32(Device.dwTimeGlobal - 2 * NET_Latency);
    P.w_u16(GE_DESTROY);
    P.w_u16(object_id);
    SendBroadcast(BroadcastCID, P, mode);
}

void xrServer::SLS_Clear()
{
    u32 mode = net_flags(TRUE, TRUE);
    while (!entities.empty())
    {
        bool found = false;
        xrS_entities::const_iterator I = entities.begin();
        xrS_entities::const_iterator E = entities.end();
        for (; I != E; ++I)
        {
            if ((*I).second->ID_Parent != 0xffff)
                continue;
            found = true;
            Perform_destroy((*I).second, mode);
            break;
        }
        R_ASSERT(found);
    }
}
