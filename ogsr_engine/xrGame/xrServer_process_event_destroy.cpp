#include "StdAfx.h"
#include "xrServer.h"
#include "game_sv_single.h"
#include "alife_simulator.h"
#include "xrServer_Objects.h"
#include "game_base.h"
#include "game_cl_base.h"
#include "ai_space.h"
#include "alife_object_registry.h"
#include "Level.h"

xr_string xrServer::ent_name_safe(u16 eid)
{
    string1024 buff;
    CSE_Abstract* e_dest = game->get_entity_from_eid(eid);
    if (e_dest)
        sprintf(buff, "[%d][%s:%s]", eid, e_dest->name(), e_dest->name_replace());
    else
        sprintf(buff, "[%d][%s]", eid, "NOTFOUND");

    return buff;
}

void xrServer::Process_event_destroy(NET_Packet& P, ClientID sender, u32 time, u16 ID, NET_Packet* pEPack)
{
    u32 MODE = net_flags(TRUE, TRUE);

    // Parse message
    u16 id_dest = ID;
#ifdef DEBUG
    Msg("sv destroy object %s [%d]", ent_name_safe(id_dest).c_str(), Device.dwFrame);
#endif

    CSE_Abstract* e_dest = game->get_entity_from_eid(id_dest); // кто должен быть уничтожен
    if (!e_dest)
    {
#ifdef DEBUG
        Msg("!SV:ge_destroy: [%d] not found on server", id_dest);
#endif
        return;
    };

    xrClientData* c_dest = e_dest->owner; // клиент, чей юнит
    xrClientData* c_from = ID_to_client(sender); // клиент, кто прислал

    // ⭐⭐⭐Who is allowed to destroy this, and why it is a CHECK rather than an
    // assert.
    //
    // ⚠These three lines used to be R_ASSERTs, which means a packet from a
    // client could kill the server outright -- and one did, every time a player
    // finished a reload (28-07-2026). Emptying the last box makes the client
    // send GE_DESTROY for it; the box's owner field still named the SERVER's own
    // client, because the server is what spawned it into the player's inventory
    // (sv_item_give -> add_online); c_dest != c_from, and the process died.
    //
    // An assert on the contents of a network packet is a remote kill switch: any
    // client could take the server down by asking to destroy somebody else's
    // item. It refuses instead, and says so.
    //
    // ⭐The rule itself also needed widening, not just softening. A player really
    // is entitled to destroy what they are carrying -- an emptied ammo box is
    // theirs, whoever conjured it into their rucksack. So the sender is accepted
    // when it drives the BODY that holds the item, which is the same authority
    // test the weapon-state path uses (net_client_controls). Ownership of the
    // item record is server bookkeeping; who is carrying it is the real question.
    if (!c_dest || !c_from)
    {
        Msg("! [net] destroy of %s refused: %s", ent_name_safe(id_dest).c_str(), c_from ? "the entity has no owning client" : "the sender is not a known client");
        return;
    }

    bool allowed = (c_dest == c_from) || (GetServerClient() == c_from);
    if (!allowed && e_dest->ID_Parent != 0xffff)
    {
        CSE_Abstract* holder = game->get_entity_from_eid(e_dest->ID_Parent);
        if (holder && holder->owner == c_from && net_client_controls(holder->ID))
            allowed = true; // it is in a body this client is driving
    }

    if (!allowed)
    {
        Msg("! [net] destroy of %s refused: client %u neither owns it nor carries it", ent_name_safe(id_dest).c_str(), sender.value());
        return;
    }

    u16 parent_id = e_dest->ID_Parent;

    //---------------------------------------------
    NET_Packet P2, *pEventPack = pEPack;
    P2.w_begin(M_EVENT_PACK);
    //---------------------------------------------
    // check if we have children
    if (!e_dest->children.empty())
    {
        if (!pEventPack)
            pEventPack = &P2;

        while (!e_dest->children.empty())
            Process_event_destroy(P, sender, time, *e_dest->children.begin(), pEventPack);
    }

    if (0xffff == parent_id && NULL == pEventPack)
    {
        SendBroadcast(BroadcastCID, P, MODE);
    }
    else
    {
        NET_Packet tmpP;
        if (0xffff != parent_id && Process_event_reject(P, sender, time, parent_id, ID, false))
        {
            game->u_EventGen(tmpP, GE_OWNERSHIP_REJECT, parent_id);
            tmpP.w_u16(id_dest);
            tmpP.w_u8(1);

            if (!pEventPack)
                pEventPack = &P2;

            pEventPack->w_u8(u8(tmpP.B.count));
            pEventPack->w(&tmpP.B.data, tmpP.B.count);
        };

        game->u_EventGen(tmpP, GE_DESTROY, id_dest); //-V595

        pEventPack->w_u8(u8(tmpP.B.count));
        pEventPack->w(&tmpP.B.data, tmpP.B.count);
    }

    if (NULL == pEPack && NULL != pEventPack)
    {
        SendBroadcast(BroadcastCID, *pEventPack, MODE);
    }

    // Everything OK, so perform entity-destroy
    if (e_dest->m_bALifeControl && ai().get_alife())
    {
        game_sv_Single* _game = smart_cast<game_sv_Single*>(game);
        VERIFY(_game);
        if (ai().alife().objects().object(id_dest, true))
            _game->alife().release(e_dest, false);
    }

    Level().OnDestroyObject(e_dest->ID);

    entity_Destroy(e_dest);
}
