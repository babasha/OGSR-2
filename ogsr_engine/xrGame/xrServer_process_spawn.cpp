#include "StdAfx.h"
#include "xrServer.h"
#include "HUDManager.h"
#include "xrServer_Objects.h"

#ifdef DEBUG
#include "xrServer_Objects_ALife_Items.h"
#endif

// Whether a spawn asked for by a real network client is carried out.
// Registered in console_commands.cpp; NOT savable (see the note there).
//
// ⭐Default ON, and measured before it was: with it off a thrown bolt produced
// nothing at all (13 drops across 6 throws, 01-08-2026), and with it on the same
// harness threw four bolts that flew and landed -- confirmed on screen. It stays
// a switch only so the acceptance can be turned off if a client is ever caught
// abusing it; the game does not work with it off.
int g_sv_client_spawns = 1;

namespace
{
// The section name sits first in the spawn payload. Read and put back, so the
// drop can name what it dropped -- a refusal that cannot say what it refused
// is nearly as unhelpful as no message at all.
void peek_section(NET_Packet& P, string64& out)
{
    out[0] = 0;
    const u32 saved = P.r_tell();
    if (!P.r_eof())
        P.r_stringZ(out);
    P.r_seek(saved);
}
} // namespace

// ⭐⭐⭐A stock gate that had never met a client it did not host.
//
// xrServer::OnMessage used to read
//
//     case M_SPAWN: if (CL->flags.bLocal) Process_spawn(P, sender);
//
// and in the engine this fork came from that condition is always true: there is
// exactly one client, it lives in this process, and create_direct_client() makes
// it local. Nothing was ever gated. The moment a GNS client connects the
// condition becomes FALSE for it (OnNetClientConnected sets bLocal false, and it
// must -- half the update path keys off that flag), and every spawn a player's
// client asks for is discarded. Without a word: no refusal, no counter, nothing
// on either side of the wire.
//
// What that costs is larger than it looks, because a client asks for a spawn far
// more often than "spawn" suggests. CMissile does not create the object it
// throws; it spawns a fake missile parented to the item in hand and then detaches
// it. So the drop meant a thrown bolt produced no bolt -- CBolt::Throw finds
// m_fake_missile still null and returns, silently, which is exactly the symptom
// that opened this hunt. Unloading a magazine into a fresh ammo box goes the same
// way, and so does anything else built on CLevel::spawn_item from the client.
//
// ⚠So the gate cannot simply be deleted either: "any client may ask the server to
// create any object" is a spawn-anything cheat. The rule below is the same one
// the destroy path settled on -- a client may spawn something INTO a body it
// drives, and nothing else.
void xrServer::Process_spawn_from_client(NET_Packet& P, ClientID sender, xrClientData* CL)
{
    if (CL && CL->flags.bLocal)
    {
        Process_spawn(P, sender); // the in-process client: unchanged
        return;
    }

    string64 section;
    peek_section(P, section);

    if (!g_sv_client_spawns)
    {
        // Counted rather than printed per event: this is the instrument that
        // says whether the throw asks once or asks in a loop, and a throttled
        // line cannot answer that.
        static u32 s_dropped = 0;
        static u32 s_last_report_ms = 0;
        ++s_dropped;
        if (s_dropped <= 8 || Device.dwTimeGlobal - s_last_report_ms >= 1000)
        {
            Msg("! [spawn] DROPPED M_SPAWN '%s' from client %u (bLocal=0, sv_client_spawns=0) -- %u dropped so far", section, sender.value(), s_dropped);
            s_last_report_ms = Device.dwTimeGlobal;
        }
        return;
    }

    CSE_Abstract* E = Process_spawn(P, sender, FALSE, nullptr, true);
    if (E)
        Msg("~ [spawn] client %u spawned '%s' as id=%u under parent %u", sender.value(), section, E->ID, E->ID_Parent);
}

// May this client ask for this object to be created?
//
// The honest question is not who owns the record -- the server owns every record
// -- but whose body the thing is being put inside. Walk the parent chain to its
// root and require that root to be an entity this client drives. For the bolt
// that chain is fake missile -> bolt -> actor, and the actor is the client's.
bool xrServer::client_may_spawn(xrClientData* CL, CSE_Abstract* E, CSE_Abstract* e_parent)
{
    if (!CL || !E)
        return false;

    if (!e_parent)
    {
        // Parentless: an object placed straight into the world. No client needs
        // this today (the throw path is parented), and allowing it would be the
        // spawn-anything hole. Refused loudly -- if something legitimate turns
        // out to need it, this line is how we will find out.
        Msg("! [spawn] REFUSED parentless spawn '%s' from client %u -- only spawns into a body the client drives are accepted", *E->s_name, CL->ID.value());
        return false;
    }

    CSE_Abstract* root = e_parent;
    for (int hops = 0; hops < 16 && root && root->ID_Parent != 0xffff; ++hops)
    {
        CSE_Abstract* next = game->get_entity_from_eid(root->ID_Parent);
        if (!next)
            break;
        root = next;
    }

    const bool drives_it = root && ((CL->owner && root->ID == CL->owner->ID) || net_client_controls(root->ID));
    if (!drives_it)
    {
        Msg("! [spawn] REFUSED '%s' from client %u: parent chain roots at %s[%u], which this client does not drive", *E->s_name, CL->ID.value(),
            root ? root->name_replace() : "?", root ? root->ID : 0xffff);
        return false;
    }
    return true;
}

CSE_Abstract* xrServer::Process_spawn(NET_Packet& P, ClientID sender, BOOL bSpawnWithClientsMainEntityAsParent, CSE_Abstract* tpExistedEntity, bool from_net_client)
{
    // create server entity
    xrClientData* CL = ID_to_client(sender);
    CSE_Abstract* E = tpExistedEntity;
    if (!E)
    {
        // read spawn information
        string64 s_name;
        P.r_stringZ(s_name);
        // create entity
        E = entity_Create(s_name);
        R_ASSERT3(E, "Can't create entity.", s_name);
        E->Spawn_Read(P);
        if (!((game->Type() == E->s_gameid) || (GAME_ANY == E->s_gameid)) || !E->match_configuration() || !game->OnPreCreate(E))
        {
            // Msg			("- SERVER: Entity [%s] incompatible with current game type.",*E->s_name);
            F_entity_Destroy(E);
            return NULL;
        }

        //		E->m_bALifeControl = false;
    }
    else
    {
        VERIFY(E->m_bALifeControl);
        //		E->owner			= CL;
        //		if (CL != NULL)
        //		{
        //			int x=0;
        //			x=x;
        //		};
        //		E->m_bALifeControl = true;
    }

    CSE_Abstract* e_parent = 0;
    if (E->ID_Parent != 0xffff)
    {
        e_parent = ID_to_entity(E->ID_Parent);
        if (!e_parent)
        {
            R_ASSERT(!tpExistedEntity);
            //			VERIFY3			(smart_cast<CSE_ALifeItemBolt*>(E) || smart_cast<CSE_ALifeItemGrenade*>(E),*E->s_name,E->name_replace());
            F_entity_Destroy(E);
            return NULL;
        }
    }

    // The authority check for a spawn a network client asked for. Placed here
    // because it needs the parsed entity and its parent, and before anything
    // that mutates server state -- a refusal must leave no trace but the log.
    if (from_net_client && !tpExistedEntity && !client_may_spawn(CL, E, e_parent))
    {
        F_entity_Destroy(E);
        return NULL;
    }

    // check if we can assign entity to some client
    if (0 == CL && !net_Players.empty())
    {
        CL = SelectBestClientToMigrateTo(E);
    }

    // check for respawn-capability and create phantom as needed
    if (E->RespawnTime && (0xffff == E->ID_Phantom))
    {
        // Create phantom
        CSE_Abstract* Phantom = entity_Create(*E->s_name);
        R_ASSERT(Phantom);
        Phantom->Spawn_Read(P);
        Phantom->ID = PerformIDgen(0xffff);
        Phantom->ID_Phantom = Phantom->ID; // Self-linked to avoid phantom-breeding
        Phantom->owner = NULL;
        entities.insert(mk_pair(Phantom->ID, Phantom));

        Phantom->s_flags.set(M_SPAWN_OBJECT_PHANTOM, TRUE);

        // Spawn entity
        E->ID = PerformIDgen(E->ID);
        E->ID_Phantom = Phantom->ID;
        E->owner = CL;
        entities.insert(mk_pair(E->ID, E));
    }
    else
    {
        if (E->s_flags.is(M_SPAWN_OBJECT_PHANTOM))
        {
            // Clone from Phantom
            E->ID = PerformIDgen(0xffff);
            E->owner = CL; //		= SelectBestClientToMigrateTo	(E);
            E->s_flags.set(M_SPAWN_OBJECT_PHANTOM, FALSE);
            entities.insert(mk_pair(E->ID, E));
        }
        else
        {
            // Simple spawn
            if (bSpawnWithClientsMainEntityAsParent)
            {
                R_ASSERT(CL);
                CSE_Abstract* P = CL->owner; //-V595
                R_ASSERT(P);
                E->ID_Parent = P->ID;
            }
            E->ID = PerformIDgen(E->ID);
            E->owner = CL;
            entities.insert(mk_pair(E->ID, E));
        }
    }

    // PROCESS NAME; Name this entity
    if (CL && (E->s_flags.is(M_SPAWN_OBJECT_ASPLAYER)))
    {
        CL->owner = E;
        //		E->set_name_replace	(CL->name);
    }

    // PROCESS RP;	 3D position/orientation
    PerformRP(E);
    E->s_RP = 0xFE; // Use supplied

    // Parent-Connect
    if (!tpExistedEntity)
    {
        game->OnCreate(E->ID);

        if (0xffff != E->ID_Parent)
        {
            R_ASSERT(e_parent);

            game->OnTouch(E->ID_Parent, E->ID);

            e_parent->children.push_back(E->ID);
        }
    }

    // create packet and broadcast packet to everybody
    NET_Packet Packet;
    if (CL)
    {
        // For local ONLY
        E->Spawn_Write(Packet, TRUE);
        if (E->s_flags.is(M_SPAWN_UPDATE))
            E->UPDATE_Write(Packet);
        SendTo(CL->ID, Packet, net_flags(TRUE, TRUE));

        // For everybody, except client, which contains authorative copy
        E->Spawn_Write(Packet, FALSE);
        if (E->s_flags.is(M_SPAWN_UPDATE))
            E->UPDATE_Write(Packet);
        SendBroadcast(CL->ID, Packet, net_flags(TRUE, TRUE));
    }
    else
    {
        E->Spawn_Write(Packet, FALSE);
        if (E->s_flags.is(M_SPAWN_UPDATE))
            E->UPDATE_Write(Packet);
        ClientID clientID;
        clientID.set(0);
        SendBroadcast(clientID, Packet, net_flags(TRUE, TRUE));
    }
    if (!tpExistedEntity)
    {
        game->OnPostCreate(E->ID);
    };

    // log
    // Msg		("- SERVER: Spawning '%s'(%d,%d,%d) as #%d, on '%s'", E->s_name_replace, E->g_team(), E->g_squad(), E->g_group(), E->ID, CL?CL->name:"*SERVER*");
    return E;
}

/*
void spawn_WithPhantom
void spawn_FromPhantom
void spawn_Simple
*/
