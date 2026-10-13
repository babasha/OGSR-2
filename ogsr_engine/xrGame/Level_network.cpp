#include "StdAfx.h"
#include "xr_platform_time.h"
#include "xr_platform_seh.h"
#include "Level.h"
#include "Level_Bullet_Manager.h"
#include "../xrCore/load_prof.h"
#include "xrServer.h"
#include "xrMessages.h"
#include "game_cl_base.h"
#include "PHCommander.h"
#include "NET_Queue.h"
#include "MainMenu.h"
#include "../xrOnlineNet/NET_GNS_Transport.h"
#include "../xr_3da/GameFont.h"
#include "space_restriction_manager.h"
#include "ai_space.h"
#include "script_engine.h"
#include "stalker_animation_data_storage.h"
#include "client_spawn_manager.h"
#include "seniority_hierarchy_holder.h"
#include "script_vars_storage.h"
#include "LevelDebugScript.h"

constexpr int max_objects_size = 2 * 1024;
constexpr int max_objects_size_in_save = 6 * 1024;

void Remove_all_statics();

void CLevel::remove_objects()
{
    m_is_removing_objects = true;

    u32 m_base, c_base, m_lmaps, c_lmaps;
    Device.m_pRender->ResourcesGetMemoryUsage(m_base, c_base, m_lmaps, c_lmaps);

    Msg("~ ObjectResources unload...");
    Msg("~ ObjectResources - base: %d, %d K", c_base, m_base / 1024);
    Msg("~ ObjectResources - lmap: %d, %d K", c_lmaps, m_lmaps / 1024);

    Game().reset_ui();

    {
        VERIFY(Server);
        Server->SLS_Clear(); // generate GE_DESTROY for all game objects
    }

    snd_Events.clear();

    // process destroy queue 
    for (int i = 0; i < 6; ++i)
    {
        // ugly hack for checks that update is twice on frame
        // we need it since we do updates for checking network messages
        ++(Device.dwFrame);

        ClientReceive();
        ProcessGameEvents();

        Objects.Update(true);
        //Sleep(100);
    }


    BulletManager().Clear();
    ph_commander().clear();
    ph_commander_scripts().clear();

    space_restriction_manager().clear();

    ai().script_engine().collect_all_garbage();

    Remove_all_statics();

    stalker_animation_data_storage().clear();
    extern void npc_prewarm_release();   // Level_network_spawn.cpp: the models built at load, held for the table above
    npc_prewarm_release();

    VERIFY(Render);

    if (!g_prefetch)
        Render->models_Clear(TRUE);
    else
        Render->models_Clear(FALSE);

    Render->clear_static_wallmarks();

#ifdef DEBUG
    if (!client_spawn_manager().registry().empty())
        client_spawn_manager().dump();
#endif // DEBUG
    VERIFY(client_spawn_manager().registry().empty());
    client_spawn_manager().clear();

    for (int i = 0; i < 6; i++)
    {
        ++(Device.dwFrame);
        Objects.Update(true);
    }

    g_pGamePersistent->destroy_particles(false);
    ::Sound->stop_emitters();

    g_ScriptVars.clear();

    shader_exports.set_dof_params(0.f, 0.f, 0.f, 0.f);

    //u32 m_base, c_base, m_lmaps, c_lmaps;
    Device.m_pRender->ResourcesGetMemoryUsage(m_base, c_base, m_lmaps, c_lmaps);

    Msg("~ ObjectResources unload completed!");
    Msg("~ ObjectResources - base: %d, %d K", c_base, m_base / 1024);
    Msg("~ ObjectResources - lmap: %d, %d K", c_lmaps, m_lmaps / 1024);

    ai().script_engine().collect_all_garbage();

    for (auto& i : m_debug_render_queue)
    {
        xr_delete(i.second);
    }

    m_debug_render_queue.clear();

    // clean up scheduler queues
    Engine.Sheduler.Destroy();

    m_is_removing_objects = false;

    void Cleanup_R_occlusion();
    Cleanup_R_occlusion();
}

#ifdef DEBUG
extern void show_animation_stats();
#endif // DEBUG

// --- network-client outbound state export -----------------------------------
// A network client has no local xrServer (no in-process entity registry), so
// its outbound path serializes the player-controlled object through a local
// CSE mirror into the same [u16 id][u8 size][UPDATE_Write payload] stream that
// xrServer::Process_update parses on the receiving end. The mirror is created
// lazily from the object's own section and reused every send.
#include "xrServer_Objects.h"
#include "Actor.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"

static CSE_Abstract* s_net_export_mirror = nullptr;
static u16 s_net_export_mirror_id = 0xffff;

static void net_export_mirror_free()
{
    if (s_net_export_mirror)
        F_entity_Destroy(s_net_export_mirror);
    s_net_export_mirror_id = 0xffff;
}

void CLevel::net_Stop()
{
    Msg("- Disconnect");
    bReady = false;
    m_bGameConfigStarted = FALSE;
    game_configured = FALSE;

    // Tear the network layer down FIRST -- BEFORE remove_objects(). remove_objects()
    // (SLS_Clear / event drain / script GC) is the known "level unload threw" crash
    // source; if it throws, anything sequenced after it is skipped. Freeing the GNS
    // connection + the file-static CSE mirrors up here guarantees the socket is
    // closed and the mirrors freed even on that throwing path (they used to leak),
    // and shutting the transport down first stops any late server message from
    // arriving mid-unload. The mirrors are standalone CSEs, not referenced by the
    // CObjects remove_objects() destroys, so freeing them early is safe.
    NET_GNS::ClientDisconnect(); // no-op unless a real GNS client connection was installed
    net_export_mirror_free(); // network-client outbound mirror (no-op otherwise)
    {
        extern void net_import_mirrors_free(); // inbound per-entity mirrors (Level_network_messages.cpp)
        net_import_mirrors_free();
    }

    // Reset the thin-client latch so its lingering state (per-frame second-thread
    // join + watchdog + heartbeat, focus-loss no-pause, press-any-key skip) does
    // not persist into the menu or a later fused session. It self-heals on the next
    // successful net_start_client4, but a FAILED next connect never reaches that
    // reassignment, so clear it here. rsAlwaysActive is only cleared when we were
    // the one that set it (thin client), to avoid stomping a user's own config.
    {
        extern ENGINE_API bool g_net_client_st;
        extern pureRender* g_pNetPingRender; // defined below in this TU
        if (g_net_client_st)
        {
            psDeviceFlags.set(rsAlwaysActive, FALSE);
            Device.seqRender.Remove(g_pNetPingRender); // top-right ping overlay
        }
        g_net_client_st = false;
    }

    remove_objects();

    IGame_Level::net_Stop();
    IPureClient::Disconnect();

    if (Server)
    {
        Server->Disconnect();
        xr_delete(Server);
    }

    ai().script_engine().collect_all_garbage();

    Remove_all_statics();

    Memory.mem_compact();

#ifdef DEBUG
    show_animation_stats();
#endif // DEBUG
}

int g_net_export_div = 1; // net_export_div: see the loop below

void CLevel::ClientSend(bool all)
{
    // This is the FUSED single-player fast path: it copies each locally-simulated
    // object's runtime state DIRECTLY into its server-side CSE entity
    // (Server->ID_to_entity), skipping the wire entirely because in single-player
    // the client and the server share one process and one entity registry.
    //
    // A network client has no local xrServer, hence no server-entity registry to
    // copy into (Server == nullptr). This path must not run there -- it would
    // null-deref Server->ID_to_entity for the first net_Relevant object (the actor)
    // every single frame (the throw is caught+swallowed by the pureFrame
    // registrator, but it still means the client is doing nothing useful here).
    //
    // A network client's real equivalent: serialize the player-controlled
    // object's state (via a local CSE mirror + UPDATE_Write) into an M_UPDATE
    // packet and Send() it -- xrServer::Process_update parses the identical
    // [u16 id][u8 size][payload] stream on the receiving end. Everything else a
    // network client sees is spawned force-Local (cl_Process_Spawn), so a "send
    // all Local objects" loop would flood the wire with objects the server
    // already simulates itself -- only the controlled entity is exported.
    if (!Server)
    {
        if (!NET_GNS::g_ClientSend)
            return; // no transport connection installed

        // ~20 Hz is plenty for state export; per-render-frame is wasteful
        static u32 s_last_send_ms = 0;
        if (Device.dwTimeGlobal - s_last_send_ms < 50)
            return;
        s_last_send_ms = Device.dwTimeGlobal;

        CObject* O = CurrentControlEntity();
        if (!O || O->getDestroy() || !O->net_Relevant())
        {
            static bool s_warned_no_entity = false;
            if (!s_warned_no_entity)
            {
                s_warned_no_entity = true;
                Msg("* [net client] ClientSend: no controlled entity to export yet");
            }
            return;
        }

        // automated-test hook: -net_move_test drives the controlled entity in a
        // slow circle so server-side application can be verified without input
        if (strstr(Core.Params, "-net_move_test"))
        {
            if (CActor* A = smart_cast<CActor*>(O))
            {
                static Fvector s_move_center{};
                static bool s_move_init = false;
                static float s_move_ang = 0.f;
                if (!s_move_init)
                {
                    s_move_center = A->Position();
                    s_move_init = true;
                }
                s_move_ang += 0.1f;
                Fvector p = s_move_center;
                p.x += 3.f * _cos(s_move_ang);
                p.z += 3.f * _sin(s_move_ang);
                Fmatrix F = A->XFORM();
                F.c = p;
                A->XFORM().set(F);
                if (A->character_physics_support()->movement()->CharacterExist())
                {
                    A->character_physics_support()->movement()->SetPosition(p);
                    A->character_physics_support()->movement()->SetVelocity(0.f, 0.f, 0.f);
                }
            }
        }

        // (re)create the CSE mirror lazily -- entity id can change on respawn
        if (!s_net_export_mirror || s_net_export_mirror_id != O->ID())
        {
            net_export_mirror_free();
            s_net_export_mirror = F_entity_Create(*O->cNameSect());
            if (!s_net_export_mirror)
            {
                Msg("! [net client] ClientSend: F_entity_Create failed for '%s'", *O->cNameSect());
                return;
            }
            s_net_export_mirror->ID = O->ID();
            s_net_export_mirror_id = O->ID();
            Msg("* [net client] ClientSend: exporting entity id=%u section=%s", O->ID(), *O->cNameSect());
        }

        O->net_Export(s_net_export_mirror);

        NET_Packet P;
        P.w_begin(M_UPDATE);
        P.w_u16(u16(O->ID()));
        u32 position;
        P.w_chunk_open8(position);
        s_net_export_mirror->UPDATE_Write(P);
        P.w_chunk_close8(position);
        // reliable/ordered for now: tiny packets at 20 Hz, and ordering spares
        // the server from applying stale positions; revisit for real WAN links
        Send(P, net_flags(TRUE));
        return;
    }

    // ⭐net_export_div (12-10-2026, Switch). This loop ran on EVERY frame for every online object: a map lookup into
    // the server's ~1800 entities and an export each -- 14 % of the second thread in the 12-bandit Cordon fight, as
    // much as the whole sound update (sampler, s26). The stock game paced it by net_HasBandwidth (psNET_ServerUpdate,
    // ~30 Hz). Now the per-frame tick (all = false) takes every N-th object of the list, a different slice each frame
    // (by INDEX, not by id: 📏cpu25 tested the id of every object, and the cache misses of reading the skipped ones kept
    // 2/3 of the cost -- net 0.95 -> 0.65 ms). The list shifting under spawns/destroys only makes an object wait a frame
    // more or go twice. The controlled entity goes every frame. A save asks for all (all = true).
    const u32 div = all ? 1u : u32(_max(1, g_net_export_div));
    CObject* ctrl = CurrentControlEntity();
    auto export_one = [this](CObject* O) {
        if (O->net_Relevant() && !O->getDestroy())
        {
            // a network client is authoritative for entities it exports to us --
            // the fused in-process export must not overwrite them
            if (Server->net_client_controls(u16(O->ID())))
                return;
            CSE_Abstract* E = Server->ID_to_entity(O->ID());
            if (E)
            {
                O->net_Export(E);
                // the packet-based update path marks entities ready on their first
                // received update; this in-process shortcut is that same first
                // update, so mark them here too -- otherwise the per-entity update
                // export never picks them up
                E->net_Ready = TRUE;
            }
        }
    };
    const u32 n = Objects.o_count();
    for (u32 i = div > 1 ? Device.dwFrame % div : 0; i < n; i += div)
    {
        CObject* O = Objects.o_get_by_iterator(i);
        if (O != ctrl)
            export_one(O);
    }
    if (ctrl)
        export_one(ctrl);
}

#include "xrServer_Objects.h"

void CLevel::ClientSave()
{
    for (u32 i = 0; i < Objects.o_count(); i++)
    {
        CObject* O = Objects.o_get_by_iterator(i);
        CGameObject* P = smart_cast<CGameObject*>(O);
        // Msg( "save:iterating:%d:%s", P->ID(), *P->cName() );
        if (P && !P->getDestroy() && P->net_SaveRelevant())
        {
            NET_Packet Packet;

            P->net_Save(Packet); // вызов save(NET_Packet& output_packet)

            CSE_Abstract* E = Server->ID_to_entity(O->ID());

            if (E)
            {
                E->net_Ready = TRUE;
                E->load(Packet);
            }
        }
    }
}

extern float phTimefactor;

void CLevel::Send(NET_Packet& P, u32 dwFlags, u32 dwTimeout)
{
    if (Server)
    {
        // optimize the case when server located in our memory
        ClientID _clid;
        _clid.set(1);
        Server->OnMessage(P, _clid);
    }
    else if (NET_GNS::g_ClientSend)
    {
        // pure network client, no local xrServer -- send over the real
        // transport connection instead. dwFlags mirrors the DPNSEND_GUARANTEED
        // convention SendTo_LL's callers already use (the server backend maps
        // it to its reliable/unreliable send the same way).
        bool reliable = (dwFlags & DPNSEND_GUARANTEED) != 0;
        NET_GNS::g_ClientSend(P.B.data, P.B.count, reliable);
    }
}

void CLevel::net_Update()
{
    if (game_configured)
    {
        LoadProf::Scope _p("cl.send");
        ClientSend(false);
    }

    if (Server)
    {
        Server->Update();
    }

    // no-op unless an external client backend installed a
    // real connection (NET_GNS::g_PumpClient stays null everywhere else,
    // including the local/in-memory singleplayer path where Server is
    // non-null) -- see NET_GNS::PumpServer's equivalent on the server side.
    NET_GNS::PumpClient();

    // A pure network client whose server connection dropped (peer close, server
    // crash, or timeout) would otherwise sit frozen forever: the transport
    // backend can detect the drop but cannot unload the level itself, and there
    // is no server-initiated-disconnect path. Poll the backend (latches, so this
    // fires exactly once per drop) and defer a clean disconnect-to-menu -- the
    // same KERNEL:disconnect the console 'disconnect' command uses. Deferred (not
    // called inline) because net_Update runs on the second thread; the event is
    // processed on the main thread between frames.
    if (!Server && NET_GNS::ClientConnectionLost())
    {
        Msg("! [net] connection to server lost -- disconnecting to menu");
        Engine.Event.Defer("KERNEL:disconnect");
    }
}

struct _NetworkProcessor : public pureFrame
{
    // ⭐The registrar catches this member with catch(...) (pure.h), which runs
    // AFTER unwinding -- so a hardware fault in here could only ever print
    // "member #0 (_NetworkProcessor) threw [non-std]" and nothing else. That
    // anonymous line is all two frozen sessions (20-08) left behind.
    //
    // An SEH filter runs BEFORE unwinding, so the next occurrence arrives with
    // an exception code, a faulting address and a full stack -- the same
    // treatment CLevel::OnFrame already gets, and for the same reason.
    //
    // ⚠This does NOT fix the fault: the tick is still cut short. What it fixes
    // is that the fault says WHERE. Lock safety is a separate matter and lives
    // in xrServer.cpp -- every csPlayers/DelayedPackestCS/world-clock region is
    // RAII now, so an unwind through here can no longer leave the server wedged
    // on a lock it never released (that is what actually froze the sessions).
    void OnFrame_impl()
    {
        // a thin network client must keep pumping its socket even while
        // paused (pause menu, console, main-menu overlay): a deaf client
        // piles up server traffic and its world freezes -- the 23-07 "static
        // screen" family. Fused SP (local Server) keeps stock pause behavior.
        const bool thin_client = g_pGameLevel && nullptr == Level().Server;
        if (!g_pGameLevel || !(thin_client || !Device.Paused()))
            return;
        // [net prof] (with dbg_frame_prof <ms>): the second thread's net tick,
        // part by part. 📏07-10-2026 Switch v214: 10-34 ms ticks in a fight made
        // the main thread wait (wait_2nd_tail / extract) -- the worst frame, 41 ms.
        extern ENGINE_API int g_frame_prof_ms; // xr_3da/device.cpp
        extern ENGINE_API float g_2nd_net_ms;  // xr_3da/device.cpp: [nx 2nd] splits seqFrameMT into net + physics
        if (g_frame_prof_ms <= 0)
        {
            const u64 t0 = CPU::QPC();
            g_pGameLevel->net_Update();
            g_2nd_net_ms += float(1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq()));
            return;
        }
        LoadProf::Arm arm;
        const u64 t0 = CPU::QPC();
        {
            LoadProf::Scope root("net_update");
            g_pGameLevel->net_Update();
        }
        const double ms = 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq());
        static u32 s_printed = 0;
        if (ms >= 8.0 && arm.outer && s_printed < 300)
        {
            ++s_printed;
            string2048 parts;
            arm.format(parts, sizeof(parts), 0.5);
            Msg("~ [net prof] frame %u net_update %.1f ms: %s", Device.dwFrame, ms, parts);
        }
    }

    virtual void OnFrame()
    {
        XR_SEH_TRY
        {
            OnFrame_impl();
        }
        XR_SEH_EXCEPT(ExceptStackTrace("!![_NetworkProcessor::OnFrame] stack trace:\n"))
        {
            static u32 s_faults = 0;
            Msg("!![_NetworkProcessor] net_Update faulted (total %u) -- stack above; this tick was cut short.", ++s_faults);
        }
    }
} NET_processor;

pureFrame* g_pNetProcessor = &NET_processor;

// Top-right ping counter for the thin network client. CStats' font is null on
// the Vulkan renderer (only the DX path ever creates it), so -- like CConsole --
// this owns its own CGameFont and draws through the proven VK font path. It is
// added to Device.seqRender for a network-client session (net_start_client4) and
// removed in net_Stop; OnRender additionally self-gates on a live connection
// (NET_GNS::ClientPing() returns -1 when there is none), so it costs a cheap
// early-out and draws nothing outside an active network session.
struct _NetPingRender : public pureRender
{
    CGameFont* font{nullptr};
    virtual void OnRender()
    {
        const int ping = NET_GNS::ClientPing();
        if (ping < 0)
            return; // no live server connection

        if (!font)
            font = xr_new<CGameFont>("hud_font_di", CGameFont::fsDeviceIndependent);

        // colour-code the readout: green = good, yellow = marginal, red = bad
        u32 clr = 0xFF60FF60;
        if (ping >= 150)
            clr = 0xFFFF6060;
        else if (ping >= 80)
            clr = 0xFFFFFF60;

        font->SetAligment(CGameFont::alRight);
        font->SetHeightI(0.022f);
        font->SetColor(clr);
        // pixel coords, top-left origin; alRight anchors the right edge at x
        font->Out(float(Device.dwWidth) - 12.f, 10.f, "Ping: %d ms", ping);
        font->OnRender();
    }
} NET_pingRender;
pureRender* g_pNetPingRender = &NET_pingRender;

const int ConnectionTimeOut = 60000; // 1 min

BOOL CLevel::Connect2Server(LPCSTR options)
{
    NET_Packet P;
    m_bConnectResultReceived = false;
    m_bConnectResult = true;
    if (!Connect(options))
        return FALSE;
    //---------------------------------------------------------------------------
    m_bConnectResultReceived = true;
    u32 EndTime = xrPlatform::TickCount() + ConnectionTimeOut;
    while (!m_bConnectResultReceived)
    {
        ClientReceive();
        Sleep(5);
        if (Server)
            Server->Update();
        //-----------------------------------------
        u32 CurTime = xrPlatform::TickCount();
        if (CurTime > EndTime)
        {
            NET_Packet P;
            P.B.count = 0;
            P.r_pos = 0;

            P.w_u8(0);
            P.w_u8(0);
            P.w_stringZ("Data verification failed. Cheater? [1]");

            OnConnectResult(&P);
        }
        if (net_isFails_Connect())
        {
            OnConnectRejected();
            Disconnect();
            return FALSE;
        }
        //-----------------------------------------
    }
    Msg("%c client : connection %s - <%s>", m_bConnectResult ? '*' : '!', m_bConnectResult ? "accepted" : "rejected", m_sConnectResult.c_str());
    if (!m_bConnectResult)
    {
        OnConnectRejected();
        Disconnect();
        return FALSE;
    };

    net_Syncronised = TRUE;

    //---------------------------------------------------------------------------
    P.w_begin(M_CLIENT_REQUEST_CONNECTION_DATA);
    Send(P, net_flags(TRUE, TRUE, TRUE, TRUE));
    //---------------------------------------------------------------------------
    return TRUE;
};

void CLevel::OnBuildVersionChallenge()
{
    NET_Packet P;
    P.w_begin(M_CL_AUTH);
    u64 auth = 0;
    P.w_u64(auth);
    Send(P, net_flags(TRUE, TRUE, TRUE, TRUE));
};

void CLevel::OnConnectResult(NET_Packet* P)
{
    // multiple results can be sent during connection they should be "AND-ed"
    m_bConnectResultReceived = true;
    u8 result = P->r_u8();
    /*u8  res1					=*/P->r_u8(); //Оставлено для совместимости сейвов
    string128 ResultStr;
    P->r_stringZ(ResultStr);
    if (!result)
        m_bConnectResult = false;

    m_sConnectResult = ResultStr;
};

void CLevel::OnConnectRejected() { IPureClient::OnConnectRejected(); };

void CLevel::net_OnChangeSelfName(NET_Packet* P)
{
    if (!P)
        return;
    string64 NewName;
    P->r_stringZ(NewName);
    if (!strstr(*m_caClientOptions, "/name="))
    {
        string1024 tmpstr;
        strcpy_s(tmpstr, *m_caClientOptions);
        strcat_s(tmpstr, "/name=");
        strcat_s(tmpstr, NewName);
        m_caClientOptions = tmpstr;
    }
    else
    {
        string1024 tmpstr;
        strcpy_s(tmpstr, *m_caClientOptions);
        *(strstr(tmpstr, "name=") + 5) = 0;
        strcat_s(tmpstr, NewName);
        const char* ptmp = strchr(strstr(*m_caClientOptions, "name="), '/');
        if (ptmp)
            strcat_s(tmpstr, ptmp);
        m_caClientOptions = tmpstr;
    }
}
