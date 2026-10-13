#include "StdAfx.h"
#include "HUDManager.h"
#include "PHDynamicData.h"
#include "Physics.h"
#include "Level.h"
#include "../xr_3da/x_ray.h"
#include "../xr_3da/IGame_Persistent.h"
#include "PhysicsGamePars.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_graph_registry.h" // graph().actor(): where the collision preload starts
#include "xrServer_Objects_ALife_Monsters.h"
#include "Actor_Flags.h"
#include "../xrCore/fs_load_prefetch.h"

extern pureFrame* g_pNetProcessor;

BOOL CLevel::net_Start_client(LPCSTR options) { return FALSE; }
#include "string_table.h"
bool CLevel::net_start_client1()
{
    pApp->LoadBegin();
    // name_of_server
    string64 name_of_server = "";
    //	strcpy_s(name_of_server,*m_caClientOptions);
    if (strchr(*m_caClientOptions, '/'))
        strncpy_s(name_of_server, *m_caClientOptions, strchr(*m_caClientOptions, '/') - *m_caClientOptions);

    if (strchr(name_of_server, '/'))
        *strchr(name_of_server, '/') = 0;

    // Startup client
    /*	string256					temp;
        sprintf_s						(temp,"%s %s",
                                    CStringTable().translate("st_client_connecting_to").c_str(), name_of_server);

        g_pGamePersistent->LoadTitle				(temp);*/
    return true;
}

#include "xrServer.h"
#include "../xrOnlineNet/NET_GNS_Transport.h"
#include "MainMenu.h"

bool CLevel::net_start_client2()
{
    // Server is null for a pure network client (empty server_options,
    // see net_start1()) -- create_direct_client() would otherwise register
    // the synthetic clientID=1 "single_player" on a local xrServer that was
    // never created. Only the singleplayer/host-and-client-in-one-process
    // case (Server non-null) still does this exactly as before.
    if (Server)
        Server->create_direct_client();

    connected_to_server = Connect2Server(*m_caClientOptions);

    return true;
}

bool CLevel::net_start_client3()
{
    if (connected_to_server)
    {
        LPCSTR level_name = name().c_str();
        // shared_str::c_str() returns nullptr for a never-assigned m_name
        // (shared_str's default ctor sets p_ = nullptr) -- true for a pure
        // network client the very first time this runs, since m_name is
        // otherwise only ever set by net_start1()/net_start2()'s local-host
        // path. xr_strlen(nullptr) below would crash without this.
        if (!level_name)
            level_name = "";

        if (!xr_strlen(level_name) && xr_strlen(NET_GNS::g_ConnectedLevelName))
        {
            // pure network client has no local Server, so
            // CLevel::name()/m_name (only ever set by net_start1()/
            // net_start2()'s local-host path) is still empty here. The
            // level name instead came from the server's connect-time hello
            // (see NET_GNS_Transport.h's LevelHello), already stashed by the
            // client backend's g_ClientConnect before Connect2Server()
            // returned.
            level_name = NET_GNS::g_ConnectedLevelName;
        }

        // Determine internal level-ID
        int level_id = pApp->Level_ID(level_name);
        Msg("* Client: resolved level_name=[%s], level_id=%d", level_name, level_id);
        if (level_id < 0)
        {
            Disconnect();
            pApp->LoadEnd();
            connected_to_server = FALSE;
            m_name = level_name;
            m_connect_server_err = xrServer::ErrNoLevel;
            return false;
        }
        m_name = level_name;
        // the level is certain from here: replay its previous load's reads, record this one's
        LoadPrefetch::LevelKnown(level_name);

        // bind this process's own "$level$" FS path alias
        // (CApplication::Level_Set) before Load() reads level.ltx from it.
        // pApp->Level_ID() above defaults bSet=false, and the only other
        // caller that ever does this (alife_graph_registry.cpp) runs on
        // whichever process hosts the A-Life sim -- for a pure network
        // client that's a SEPARATE process with its own FS singleton, so
        // nothing has bound its alias yet. Harmless/idempotent for the
        // local-host-fused case (net_start1() may have already set it via
        // the same level_id).
        pApp->Level_Set(level_id);

        // Load level
        R_ASSERT2(Load(level_id), "Loading failed.");
    }
    return true;
}

bool CLevel::net_start_client4()
{
    if (connected_to_server)
    {
        // Begin spawn
        g_pGamePersistent->LoadTitle("st_client_spawning");
        {
            // the collision tree behind the spawning, the player's tiles first: the stalkers coming online are around him
            const CSE_ALifeCreatureActor* A = ai().get_alife() ? ai().alife().graph().actor() : nullptr;
            ObjectSpace.PreloadStatic(A ? &A->o_Position : nullptr);
        }

        // Send physics to single or multithreaded mode
        LoadPhysicsGameParams();
        ph_world = xr_new<CPHWorld>();
        ph_world->Create();

        // Send network to single or multithreaded mode
        // *note: release version always has "mt_*" enabled
        Device.seqFrameMT.Remove(g_pNetProcessor);
        Device.seqFrame.Remove(g_pNetProcessor);
        Device.seqFrameMT.Add(g_pNetProcessor, REG_PRIORITY_HIGH + 2);

        // Network-client frame fix: mark this process as a network client (no
        // local xrServer). At the sync->gameplay transition the client applies a
        // BURST of accumulated network spawns via ProcessGameSpawns (queued to
        // seqParallel) plus physics (seqFrameMT) on the render second thread,
        // concurrently with the main-thread renderer reading the same object /
        // scene state -- a data race that deterministically hangs the client
        // right after "End of synchronization". (It only surfaced once the
        // ClientSend() null-deref was gated: before that, net_Update()'s
        // exception unwound before PumpClient() ran, so no live server messages
        // were ever processed and the client just rendered a stale sync
        // snapshot.) The flag tells the device frame loop to JOIN the second
        // thread before rendering (see CRenderDevice::on_frame), serializing the
        // frame. It is lightweight, so this costs nothing meaningful; a fused
        // single-player host keeps the concurrent MT split (Server != nullptr there).
        extern ENGINE_API bool g_net_client_st;
        g_net_client_st = (Server == nullptr);

        // A network client must keep simulating without window focus, like any
        // multiplayer game: focus loss fires seqAppDeactivate -> Device.Pause,
        // and _NetworkProcessor::OnFrame is gated on !Device.Paused() -- the
        // paused client stops pumping the socket entirely. That single fact
        // produced a whole family of 23-07 symptoms: launching client 2 stole
        // focus and "froze" client 1 (static last frame, dead net), alt-tab in
        // the supervised session paused whichever window lost focus (so the
        // players never saw each other move), and un-pausing handed physics a
        // giant dt (the 15 s frames). rsAlwaysActive keeps the device active
        // regardless of focus; fused SP keeps stock pause-on-alt-tab.
        if (g_net_client_st)
        {
            psDeviceFlags.set(rsAlwaysActive, TRUE);
            // focus may have been stolen DURING load (e.g. a second client
            // launching) -- the deactivate already paused us. Re-run the
            // activation path now that rsAlwaysActive makes it stick.
            if (!Device.b_is_Active)
                Device.OnWM_Activate(WA_ACTIVE, 0);

            // The SP main menu only closes from its own UI handlers (the "new
            // game" button runs `start` AND deactivates itself). The `connect`
            // console path never touches it, so the menu stayed up over the
            // loaded world -- its fullscreen noise background was the "static
            // screen", and an active menu holds Device.Pause, which killed
            // the whole net pump (see _NetworkProcessor). Close it here.
            if (MainMenu() && MainMenu()->IsActive())
            {
                Msg("* [net client] closing main menu left open by the connect flow");
                MainMenu()->Activate(false);
            }

            // Top-right ping counter -- only for a thin network client. Removed
            // in CLevel::net_Stop. (Defined in Level_network.cpp.)
            extern pureRender* g_pNetPingRender;
            Device.seqRender.Add(g_pNetPingRender, 4);
        }

        while (!game_configured)
        {
            ClientReceive();
            if (Server)
                Server->Update();
            NET_GNS::PumpClient();
            Sleep(5);
        }
    }
    return true;
}

bool CLevel::net_start_client5()
{
    if (connected_to_server)
    {
        // HUD

        // Textures
        HUD().Load();
        g_pGamePersistent->LoadTitle("st_loading_textures");
        Device.m_pRender->DeferredLoad(FALSE);
        Device.m_pRender->ResourcesDeferredUpload();
        LL_CheckTextures();
    }
    return true;
}

bool CLevel::net_start_client6()
{
    if (connected_to_server)
    {
        // Sync
        if (g_hud)
            g_hud->OnConnected();

        if (!psActorFlags.test(AF_KEYPRESS_ON_START))
            pApp->LoadForceFinish();
        g_pGamePersistent->LoadTitle("st_client_synchronising");
        Device.PreCache(60, true, true);
        net_start_result_total = TRUE;
    }
    else
    {
        net_start_result_total = FALSE;
    }

    pApp->LoadEnd();
    return true;
}