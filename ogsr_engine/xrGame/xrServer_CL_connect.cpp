#include "StdAfx.h"
#include "xrServer.h"
#include "xrMessages.h"
#include "HUDManager.h"
#include "xrServer_Objects.h"
#include "Level.h"
#include "../COMMON_AI/xrServer_Objects_ALife_Monsters.h"
#include "game_base.h"
#include "sv_telemetry.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_graph_registry.h"
#include "alife_schedule_registry.h"

xr_vector<u16> g_perform_spawn_ids;

// ⚠Set once a connecting client has been given a body, and reset for every
// connect in SendConnectionData. Replication walks EVERY entity, so without it
// a client that arrives while several bodies are standing free adopts each one
// it meets in turn -- ending up owning the last and claiming authority over all
// of them. That could not happen while bodies were destroyed on disconnect and
// only one was ever free; it can now.
static bool g_connect_body_given = false;

void xrServer::Perform_connect_spawn(CSE_Abstract* E, xrClientData* CL, NET_Packet& P)
{
    xr_vector<u16>::iterator it = std::find(g_perform_spawn_ids.begin(), g_perform_spawn_ids.end(), E->ID);
    if (it != g_perform_spawn_ids.end())
        return;

    g_perform_spawn_ids.push_back(E->ID);

    if (E->net_Processed)
        return;
    if (E->s_flags.is(M_SPAWN_OBJECT_PHANTOM))
        return;

    // Foreign-content placeholders (unknown config section -> inert restrictor
    // substituted by F_entity_Create) may carry no serializable state at all;
    // Spawn_Write hard-asserts on such entities ("object isn't successfully
    // saved") and would kill the server on a client connect. They are inert
    // scenery -- a network client loses nothing if they are not replicated.
    if (!pSettings->section_exist(E->s_name))
    {
        Msg("! [net] connect replication: skipping unknown-section entity id=%u '%s' (%s)", E->ID, E->name_replace(), *E->s_name);
        E->net_Processed = TRUE;
        return;
    }

    // Living Zone's pf_* placeholder family can have a BROKEN Lua STATE_Write
    // (the wrapper catches the Lua error and "continues" having written
    // nothing -- seen live with pf_crow_spawner, depends on its script state
    // at connect time). Spawn_Write would then hit the fatal "object isn't
    // successfully saved" assert and kill the server on a client connect.
    // Probe STATE_Write into a scratch packet first and skip such entities --
    // they are server-side script logic, a network client loses nothing.
    // ⚠️ The probe is deliberately NARROW (pf_* only): probing every entity
    // fires ~2k out-of-context Lua serializations per connect and correlated
    // with a LuaJIT GC heap crash on the server.
    if (0 == strncmp(*E->s_name, "pf_", 3))
    {
        NET_Packet probe;
        probe.B.count = 0;
        E->STATE_Write(probe);
        if (0 == probe.B.count)
        {
            Msg("! [net] connect replication: skipping entity id=%u '%s' (%s) - empty STATE_Write (script error?)", E->ID, E->name_replace(), *E->s_name);
            E->net_Processed = TRUE;
            return;
        }
    }

    // Connectivity order
    CSE_Abstract* Parent = ID_to_entity(E->ID_Parent);
    if (Parent)
        Perform_connect_spawn(Parent, CL, P);

    // Process
    Flags16 save = E->s_flags;
    //-------------------------------------------------
    E->s_flags.set(M_SPAWN_UPDATE, TRUE);

    // A connecting network client adopts the player-controlled entity: the
    // in-process client claimed ownership of everything when the level came up
    // (owner != 0), so the plain owner==0 handover below can never fire for a
    // later client. Replicate the actor to it flagged LOCAL+ASPLAYER so the
    // receiving side binds it as its controlled entity (Spawn_Write with
    // bLocal=FALSE strips exactly those two flags).
    // A SECOND network client must NOT adopt an actor another network client
    // already controls -- it gets that actor as a plain informed entity (a
    // remote-player puppet) and OnCL_Connected spawns it a fresh actor of its
    // own afterwards.
    xrClientData* holder = (xrClientData*)E->owner;
    const bool actor_taken_by_other_net_client = holder && !holder->flags.bLocal && holder != CL;
    if (!CL->flags.bLocal && !g_connect_body_given && smart_cast<CSE_ALifeCreatureActor*>(E) && !actor_taken_by_other_net_client)
    {
        g_connect_body_given = true;
        Msg("* [net] handing player entity id=%u '%s' (section %s, prev owner %s) to network client -- it carries %u item%s", E->ID, E->name_replace(), *E->s_name,
            E->owner ? "set" : "none", u32(E->children.size()), E->children.size() == 1 ? "" : "s");
        CL->owner = E;
        E->owner = CL; // marks it taken for the next connecting client
        // Claim authority HERE, at the handover, not on the client's first update.
        // Until this id is in the set the update export treats the entity as any
        // other and streams it back to its own owner, and the owner reads an update
        // for the thing it is driving as a correction -- so a player who moved during
        // those first milliseconds got snapped back to the spawn point for no reason.
        // Only for network clients: a fused local client must stay OUT of this set or
        // its own in-process export (CLevel::ClientSend) stops updating the server
        // entity, which is how single player works.
        m_net_client_entities.insert(E->ID);
        E->s_flags.set(M_SPAWN_OBJECT_ASPLAYER, TRUE);
        E->Spawn_Write(P, TRUE);
        E->UPDATE_Write(P);
    }
    else if (0 == E->owner)
    {
        // PROCESS NAME; Name this entity
        if (E->s_flags.is(M_SPAWN_OBJECT_ASPLAYER))
        {
            CL->owner = E;
            E->set_name_replace(*CL->name);
        }

        // Associate
        E->owner = CL;
        E->Spawn_Write(P, TRUE);
        E->UPDATE_Write(P);
    }
    else
    {
        // Just inform
        E->Spawn_Write(P, FALSE);
        E->UPDATE_Write(P);
    }
    //-----------------------------------------------------
    E->s_flags = save;
    SendTo(CL->ID, P, net_flags(TRUE, TRUE));
    E->net_Processed = TRUE;
}

void xrServer::SendConnectionData(IClient* _CL)
{
    g_perform_spawn_ids.clear();
    g_connect_body_given = false;
    xrClientData* CL = (xrClientData*)_CL;
    NET_Packet P;
    u32 mode = net_flags(TRUE, TRUE);
    // Replicate current entities on to this client
    xrS_entities::iterator I = entities.begin(), E = entities.end();
    for (; I != E; ++I)
        I->second->net_Processed = FALSE;
    for (I = entities.begin(); I != E; ++I)
        Perform_connect_spawn(I->second, CL, P);

    // Send "finished" signal
    P.w_begin(M_SV_CONFIG_FINISHED);
    SendTo(CL->ID, P, mode);
};

// Spawn a fresh actor entity for a network client that connected after the
// single existing actor was already claimed. The new actor is cloned from the
// template actor (position/visual/inventory-less state), gets a fresh id from
// the generator and goes through the regular Process_spawn flag discipline:
// the owner receives it LOCAL+ASPLAYER (binds it as its controlled entity),
// everybody else receives the stripped broadcast (remote-player puppet).
// Deliberately NOT registered in ALife (Process_spawn skips game->OnCreate for
// pre-existing entities), so CALifeGraphRegistry::m_actor keeps pointing at
// player 1 -- the ALife online/offline bubble stays centered there for now.
void xrServer::Perform_spawn_net_actor(xrClientData* CL)
{
    // template = first actor entity (the ALife-spawned id=0 one)
    CSE_ALifeCreatureActor* proto = nullptr;
    for (auto& it : entities)
        if ((proto = smart_cast<CSE_ALifeCreatureActor*>(it.second)))
            break;
    if (!proto)
    {
        Msg("! [net] Perform_spawn_net_actor: no actor template entity found, client '%s' gets no actor", *CL->name);
        return;
    }

    // clone through the wire format -- the same state a client spawn carries
    NET_Packet clone;
    proto->Spawn_Write(clone, TRUE);
    u16 msg_id;
    clone.r_begin(msg_id);
    string64 sect;
    clone.r_stringZ(sect);
    CSE_Abstract* E = entity_Create(sect);
    R_ASSERT3(E, "Can't create actor clone", sect);
    E->Spawn_Read(clone);

    E->ID = 0xffff; // fresh id from the generator
    E->ID_Parent = 0xffff;
    E->ID_Phantom = 0xffff;
    E->RespawnTime = 0;
    E->owner = nullptr;
    string64 net_name;
    sprintf_s(net_name, "net_player_%u", CL->ID.value());
    E->set_name_replace(net_name);
    E->o_Position.x += 2.f; // don't spawn inside player 1
    E->s_flags.set(M_SPAWN_OBJECT_ASPLAYER, TRUE); // Process_spawn: CL->owner = E
    E->s_flags.set(M_SPAWN_OBJECT_LOCAL, TRUE);
    E->s_flags.set(M_SPAWN_UPDATE, TRUE);

    NET_Packet dummy;
    CSE_Abstract* spawned = Process_spawn(dummy, CL->ID, FALSE, E);
    if (spawned)
    {
        // ⭐⭐⭐Enter it into ALife. Process_spawn skipped game->OnCreate for a
        // pre-existing entity, so without this the clone lives in
        // xrServer::entities and NOWHERE else -- and a body ALife does not know
        // about CANNOT OWN ANYTHING. That is not a subtlety: an item is linked to
        // its owner by CALifeSimulatorBase::register_object, which resolves the
        // parent in the ALife registry alone, and handing an item to a body that
        // is missing from it used to take the whole server down with an access
        // violation (measured 28-07-2026, see the note in register_object).
        //
        // A player who cannot be given anything cannot be equipped, restocked or
        // paid, which is the entire extraction loop -- so every player body has
        // to be a first-class ALife object, not just the first one's.
        //
        // ⚠M_SPAWN_OBJECT_ASPLAYER is cleared for the duration: graph().update()
        // repoints CALifeGraphRegistry::m_actor -- the CENTRE of the ALife
        // online/offline bubble -- at any object carrying that flag. The flag is
        // needed by Process_spawn above (it is what makes the client bind this as
        // its own entity) and must not be seen by the registration below.
        CSE_ALifeDynamicObject* dyn = smart_cast<CSE_ALifeDynamicObject*>(spawned);
        CALifeSimulator* alife = const_cast<CALifeSimulator*>(ai().get_alife());
        if (dyn && alife)
        {
            const Flags16 saved = spawned->s_flags;
            spawned->s_flags.set(M_SPAWN_OBJECT_ASPLAYER, FALSE);
            alife->register_object(dyn, true);
            spawned->s_flags = saved;

            // It is already ONLINE (Process_spawn put it in entities), so it must
            // look online to ALife too. add_online_impl does exactly these three
            // things for an object crossing that boundary: an object that is
            // online is not scheduled and does not sit on a graph vertex, and
            // leaving it in either makes the switch manager fight the client for
            // a body the client is driving.
            dyn->m_bOnline = true;
            alife->scheduled().remove(dyn);
            alife->graph().remove(dyn, dyn->m_tGraphID, false);

            // ALife owns the lifetime of a registered object, so the flag that
            // says so is now true -- and it MUST be, or entity_Destroy would free
            // the CSE while the registry still points at it.
            spawned->m_bALifeControl = true;
        }
        else
        {
            // No simulator (or not a dynamic object): keep the old discipline --
            // false means entity_Destroy frees the CSE itself, which is right
            // precisely because nothing else is tracking it.
            spawned->m_bALifeControl = false;
        }

        // Same handover claim as in Perform_connect_spawn: the client drives this
        // entity from the moment it is spawned for it, so the export must stop
        // streaming it back to its own owner right away -- an update for the entity
        // you are driving reads as a correction, and snaps you.
        m_net_client_entities.insert(spawned->ID);
        Msg("* [net] spawned fresh actor id=%u '%s' for network client '%s'%s", spawned->ID, spawned->name_replace(), *CL->name,
            spawned->m_bALifeControl ? " (registered in ALife -- it can own things)" : " (NOT in ALife -- it cannot own things)");
    }
    else
        Msg("! [net] Perform_spawn_net_actor: Process_spawn failed for client '%s'", *CL->name);
}

void xrServer::OnCL_Connected(IClient* _CL)
{
    xrClientData* CL = (xrClientData*)_CL;
    CL->net_Accepted = TRUE;

    csPlayers.Enter();

    Export_game_type(CL);
    Perform_game_export();
    SendConnectionData(CL);

    // a later network client whose connect replication did NOT hand it an
    // actor (the existing one is taken) gets a freshly spawned one
    if (!CL->flags.bLocal && !CL->owner)
        Perform_spawn_net_actor(CL);

    //
    NET_Packet P;
    P.B.count = 0;
    P.w_clientID(CL->ID);
    P.r_pos = 0;
    ClientID clientID;
    clientID.set(0);
    game->AddDelayedEvent(P, GAME_EVENT_PLAYER_CONNECTED, 0, clientID);
    SVTelemetry::OnClient("join", CL->ID.value(), CL->ps ? CL->ps->getName() : (*CL->name ? *CL->name : nullptr), CL->owner, !!CL->flags.bLocal);
    csPlayers.Leave();
    game->ProcessDelayedEvent();
}

void xrServer::OnNetClientDisconnected(ClientID id)
{
    // csPlayers is recursive on this platform (Update() holds it while calling
    // client_Destroy(), which re-enters it), so taking it here and letting the
    // helpers below re-enter is safe.
    csPlayers.Enter();

    xrClientData* CL = ID_to_client(id);
    if (!CL)
    {
        csPlayers.Leave();
        Msg("! [net] disconnect: no active client for id=%u (already gone?)", id.value());
        return;
    }

    // ⭐⭐⭐A player's body OUTLIVES the session that was driving it.
    //
    // It used to be torn down -- the ALife actor id 0 kept, every net_player_N
    // clone destroyed -- and that quietly made persistent kit impossible: a
    // player who dropped out and came back was handed a NEW, EMPTY body while
    // everything they were carrying stayed on the abandoned one. That is what
    // "the inventory is not exported on reconnect" turned out to be (28-07-2026);
    // nothing failed to export, the player was simply looking out of somebody
    // else's eyes.
    //
    // So both cases now do the same thing: release the ownership link and leave
    // the body standing. Perform_connect_spawn adopts a free body before minting
    // a new one, so the next session steps back into it with its kit intact.
    //
    // ⚠The cost, stated because it is real: a body left standing is a body other
    // clients keep seeing. For a persistent world that is arguably correct -- you
    // logged out, your stalker is still in the Zone -- but it is a behaviour
    // change, not a free win, and "how a logged-out body should look" is an open
    // question.
    //
    // Logged before the teardown below, while the client still has its entity:
    // where a session ended is part of what happened in it.
    SVTelemetry::OnClient("leave", CL->ID.value(), CL->ps ? CL->ps->getName() : (*CL->name ? *CL->name : nullptr), CL->owner, !!CL->flags.bLocal);

    CSE_Abstract* E = CL->owner;
    if (E)
    {
        // It is no longer being driven from the network: the update export must
        // go back to streaming it like any other entity, or the body would stand
        // frozen for everybody watching until somebody adopts it again.
        m_net_client_entities.erase(E->ID);

        Msg("* [net] disconnect: freeing actor id=%u '%s' from client '%s' -- it keeps its %u item%s and waits to be re-adopted", E->ID, E->name_replace(), *CL->name,
            u32(E->children.size()), E->children.size() == 1 ? "" : "s");

        if (E->owner == CL)
            E->owner = nullptr;
        CL->owner = nullptr;
    }

    csPlayers.Leave();

    // Removes net_Players bookkeeping and frees the client object. Takes
    // csPlayers itself (re-entrant, see above).
    client_Destroy(CL);
}

void xrServer::SendConnectResult(IClient* CL, u8 res, u8 res1, const char* ResultStr)
{
    NET_Packet P;
    P.w_begin(M_CLIENT_CONNECT_RESULT);
    P.w_u8(res);
    P.w_u8(res1);
    P.w_stringZ(ResultStr);

    if (SV_Client && SV_Client == CL)
        P.w_u8(1);
    else
        P.w_u8(0);
    P.w_stringZ(Level().m_caServerOptions);

    SendTo(CL->ID, P);
};

void xrServer::OnBuildVersionRespond(IClient* CL, NET_Packet& P)
{
    u16 Type;
    P.r_begin(Type);
    u64 _our = 0;
    u64 _him = P.r_u64();

#ifdef DEBUG
    Msg("_our = %d", _our);
    Msg("_him = %d", _him);
#endif // DEBUG

    if (_our != _him)
    {
        SendConnectResult(CL, 0, 0, "Data verification failed. Cheater? [3]");
    }
    else
    {
        Check_BuildVersion_Success(CL);
    }
};

void xrServer::Check_BuildVersion_Success(IClient* CL)
{
    CL->flags.bVerified = TRUE;
    SendConnectResult(CL, 1, 0, "All Ok");
};