// xrServer.cpp: implementation of the xrServer class.
//
//////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "xrServer.h"
#include "xrMessages.h"
#include "xrServer_Objects_ALife_All.h"
#include "Level.h"
#include "game_cl_base.h"
#include "ai_space.h"
#include "../xr_3da/IGame_Persistent.h"

#include "../xr_3da/XR_IOConsole.h"
#include "ui/UIInventoryUtilities.h"

// M2: transport hook table, always compiled (no GameNetworkingSockets
// dependency here -- see NET_GNS_Transport.h). Hooks stay null unless a
// backend installs them (only XR_3DA_Server does, from server_main.cpp),
// so this file's behavior for every other exe (XR_3DA, XR_3DA_Vulkan) is
// byte-for-byte what it was before M2.
#include "../xrOnlineNet/NET_GNS_Transport.h"

// test-hook deps (XROS_NPC_WALK_TEST, see xrServer::Update)
#include "GameObject.h"
#include "sv_motion_audit.h" // the "wire" sample site -- see write_entity_update
#include "entity_alive.h"
#include "Actor.h"

// server-driven NPCs, stepped once per tick from xrServer::Update
#include "sv_npc.h"
#include "sv_anomaly.h"
#include "sv_prop.h"
#include "sv_perf.h"

// world-clock control (split_time for the log lines)
#include "date_time.h"
#include "../xrCore/load_prof.h" // [net prof]: the server tick's parts

// Radius (metres) around a client's own entity within which the world is sent to
// it; 0 = send everything, the behaviour this started from. See SendUpdatesToAll
// for why this is the anti-cheat measure that survives a modified client -- and
// why it needs despawn-on-exit before it can be turned on by default.
float g_sv_interest_radius = 0.f;

xrClientData::xrClientData() : IClient(Device.GetTimerGlobal())
{
    ps = Level().Server->game->createPlayerState();
    ps->clear();
    ps->m_online_time = Level().timeServer();

    Clear();
}

void xrClientData::Clear()
{
    owner = NULL;
    net_Ready = FALSE;
    net_Accepted = FALSE;
    net_PassUpdates = TRUE;
    // A reused record must not carry the previous occupant's identity: this
    // struct is pooled, and an admin's role surviving into the next client to
    // land on the same slot would be a privilege handed out by accident.
    account_id = 0;
    role = 0;
};

xrClientData::~xrClientData() { xr_delete(ps); }

xrServer::xrServer() : IPureServer(Device.GetTimerGlobal())
{
    m_iCurUpdatePacket = 0;
    m_aUpdatePackets.emplace_back();
    m_aDelayedPackets.clear();
}

xrServer::~xrServer()
{
    while (net_Players.size())
    {
        client_Destroy(net_Players[0]);
    }

    while (net_Players_disconnected.size())
    {
        client_Destroy(net_Players_disconnected[0]);
    }
    m_aUpdatePackets.clear();
    m_aDelayedPackets.clear();
}

//--------------------------------------------------------------------

CSE_Abstract* xrServer::ID_to_entity(u16 ID)
{
    // #pragma todo("??? to all : ID_to_entity - must be replaced to 'game->entity_from_eid()'")
    if (0xffff == ID)
        return 0;
    xrS_entities::iterator I = entities.find(ID);
    if (entities.end() != I)
        return I->second;
    else
        return 0;
}

//--------------------------------------------------------------------
IClient* xrServer::client_Create() { return xr_new<xrClientData>(); }

IClient* xrServer::client_Find_Get(ClientID ID)
{
    ip_address cAddress;

    cAddress.set("127.0.0.1");

    IClient* newCL = client_Create();
    newCL->ID = ID;

    {
        std::lock_guard<xrCriticalSection> lock(csPlayers);
        net_Players.push_back(newCL);
        net_Players.back()->server = this;
    }

    Msg("# Player not found. New player created.");
    return newCL;
};

INT g_sv_Client_Reconnect_Time = 0;

void xrServer::client_Destroy(IClient* C)
{
    std::lock_guard<xrCriticalSection> lock(csPlayers);   // RAII: a throw in here must NOT abandon the lock (see xrSyncronize.h)

    // Delete assosiated entity
    // xrClientData*	D = (xrClientData*)C;
    // CSE_Abstract* E = D->owner;
    for (u32 DI = 0; DI < net_Players_disconnected.size(); DI++)
    {
        if (net_Players_disconnected[DI] == C)
        {
            xr_delete(C);
            net_Players_disconnected.erase(net_Players_disconnected.begin() + DI);
            break;
        };
    };

    for (u32 I = 0; I < net_Players.size(); I++)
    {
        if (net_Players[I] == C)
        {
            {
                // ⚠ LOCK-DISCIPLINE HAZARD (latent, safe only while the server
                // runs single-threaded): this scan/erase of m_aDelayedPackets
                // runs under csPlayers, but AddDelayedPacket/ProceedDelayedPackets
                // guard the SAME deque with DelayedPackestCS -- two different
                // locks give zero mutual exclusion. Do NOT "fix" it by simply
                // taking DelayedPackestCS here: this path holds csPlayers, while
                // ProceedDelayedPackets holds DelayedPackestCS then takes csPlayers
                // (via OnDelayedMessage) -- adding the lock here inverts that order
                // and DEADLOCKS the moment the net path stops being serialized on
                // one worker thread. The correct fix is to consolidate the deque
                // under a single lock with one consistent order; deferred until/if
                // this genuinely goes multithreaded.
                DelayedPacket pp;
                pp.SenderID = C->ID;

                xr_deque<DelayedPacket>::iterator it;
                do
                {
                    it = std::find(m_aDelayedPackets.begin(), m_aDelayedPackets.end(), pp);
                    if (it != m_aDelayedPackets.end())
                    {
                        m_aDelayedPackets.erase(it);
                        Msg("removing packet from delayed event storage");
                    }
                    else
                        break;
                } while (true);
            }

            if (!g_sv_Client_Reconnect_Time || !C->flags.bVerified)
            {
                xr_delete(C);
            }
            else
            {
                C->dwTime_LastUpdate = Device.dwTimeGlobal;
                net_Players_disconnected.push_back(C);
                ((xrClientData*)C)->Clear();
            };
            net_Players.erase(net_Players.begin() + I);
            break;
        };
    }
}

//--------------------------------------------------------------------
int g_Dump_Update_Write = 0;

// One shot, not a stream: names the first two dozen entities the per-entity
// export skips for having no owner, and says whether the server is simulating
// them. See the call site -- this exists to answer whether a world of 1881
// entities reaching a client as 7 is a filter working or a filter broken.
int g_sv_export_census = 0;

// ⭐⭐Names the gate that stopped a LIVE entity from being exported.
//
// Reported from play 30-07-2026: the level's own mutants hunted and killed the
// player while the client's entire replicated world was seven entities against
// 1881 on the server. The per-entity export has six gates and the census only
// ever counted them; counting says how many were dropped, not whether dropping
// them was right. This says WHICH gate, for the only entities where being
// dropped is a defect -- the ones the server is actually simulating.
//
// ⚠Online is the filter, not position in the map. An earlier version printed
// the first two dozen skips and, since the map is walked from the start on
// every build, showed the same campfires at id~1000 forever and never once
// reached a creature. A cap on WHICH samples are taken is not a cap on how many
// are printed, and confusing the two cost a whole measurement.
static void census_skip(u16 id, CSE_Abstract& E, const char* gate)
{
    if (!g_sv_export_census)
        return;

    // ⚠⚠CREATURES ONLY, and this narrowing is the fourth attempt at getting the
    // sample right. A budget of 60 lines was spent, in order: on campfires,
    // then on breakable meshes -- all correctly dropped, none of them the
    // question. The question is whether a body that walks and shoots reaches
    // the client, so nothing that cannot walk or shoot belongs in the sample.
    if (nullptr == smart_cast<CSE_ALifeCreatureAbstract*>(&E))
        return;
    CObject* live = Level().Objects.net_Find(id);
    if (nullptr == live)
        return; // offline: dropping it is correct, and there are hundreds

    static u32 s_named = 0;
    if (s_named >= 60)
        return;
    ++s_named;

    // ⭐⭐BOTH POSITIONS, because the interest gate reads only one of them.
    //
    // The gate measures `o_Position` -- the CSE MIRROR -- and the mirror is
    // refreshed by CLevel::ClientSend only for objects it considers relevant.
    // A creature whose mirror is never refreshed therefore keeps the position
    // it spawned at, and no amount of walking towards the player can move it
    // back inside the radius. That would make "beyond the interest radius" a
    // true statement about a stale number rather than about the creature, and
    // the two are indistinguishable until they are printed side by side.
    const Fvector& real = live->Position();
    Msg("~ [export] LIVE CREATURE UNSENT id=%u '%s' -- stopped by '%s' | mirror (%.1f %.1f %.1f) vs REAL (%.1f %.1f %.1f) -- they differ by %.1f m", id, E.name_replace(),
        gate, VPUSH(E.o_Position), VPUSH(real), E.o_Position.distance_to(real));
}

#ifdef DEBUG
INT g_sv_SendUpdate = 0;
#endif

// ---- world clock control ----------------------------------------------------
// Requests are queued rather than applied in place: a host module may raise them
// from its own thread, and the clock must move on the server tick so that the
// update header every client receives is built from one consistent value.

void xrServer::RequestSetGameTime(u64 game_time_ms)
{
    {
        std::lock_guard<xrCriticalSection> lock(m_world_time_cs);   // RAII: an unwind here must not abandon it
        m_world_time_req.has_time = true;
        m_world_time_req.time = game_time_ms;
        m_world_time_req.advance_ms = 0; // an absolute placement supersedes a pending nudge
    }
    m_world_time_host_owned = true;
}

void xrServer::RequestAdvanceGameTime(u32 delta_ms)
{
    {
        std::lock_guard<xrCriticalSection> lock(m_world_time_cs);   // RAII: an unwind here must not abandon it
        m_world_time_req.advance_ms += delta_ms; // nudges accumulate
    }
}

void xrServer::RequestSetTimeFactor(float factor)
{
    {
        std::lock_guard<xrCriticalSection> lock(m_world_time_cs);   // RAII: an unwind here must not abandon it
        m_world_time_req.has_factor = true;
        m_world_time_req.factor = factor;
    }
    m_world_time_host_owned = true;
}

u64 xrServer::GetWorldTime() { return game ? game->GetGameTime() : u64(0); }

float xrServer::GetWorldTimeFactor() { return game ? game->GetGameTimeFactor() : 0.f; }

void xrServer::ApplyWorldTimeRequests()
{
    SWorldTimeRequest req;
    {
        std::lock_guard<xrCriticalSection> lock(m_world_time_cs);   // RAII: an unwind here must not abandon it
        req = m_world_time_req;
        m_world_time_req = SWorldTimeRequest();
    }

    if (!game || (!req.has_time && !req.advance_ms && !req.has_factor))
        return;

    if (req.has_factor)
        game->SetGameTimeFactor(req.factor);

    if (req.has_time || req.advance_ms)
    {
        u64 target = req.has_time ? req.time : game->GetGameTime();
        target += req.advance_ms;
        game->SetGameTime(target);
    }

    // This process draws the same sky as everyone else when it has a view of its
    // own (host/fused case); on a view-less host the call is a cheap no-op.
    if (g_pGamePersistent && g_pGameLevel)
        g_pGamePersistent->Environment().SetGameTime(Level().GetEnvironmentGameDayTimeSec(), game->GetEnvironmentGameTimeFactor());

    u32 year = 0, month = 0, day = 0, hours = 0, mins = 0, secs = 0, ms = 0;
    split_time(game->GetGameTime(), year, month, day, hours, mins, secs, ms);
    Msg("* [worldtime] now %02u:%02u:%02u (%u.%u.%u), rate=%.2f", hours, mins, secs, day, month, year, game->GetGameTimeFactor());
}

// ---- world weather / rain control -------------------------------------------
// Same discipline as the clock: queue on the caller's thread, apply on the server
// tick before this tick's update header is built.

// Stream this world's LIVE weather values to clients (default), or only the channels
// a host explicitly holds. Streaming is the default because it is what makes the
// server the single source of truth: anything that moves the weather here -- an
// operator command, this world's own forecast script, a weather effect, an
// environment modifier -- reaches every client without needing its own wire format.
// Turning it off is a bandwidth trade, not a correctness one: the client then mixes
// rain/fog/wind/lightning itself from the same cycle at the same shared time, which
// agrees with the server everywhere except where something local moved them.
// Console: sv_weather_stream. Read by game_sv_base.cpp's header export.
int g_sv_weather_stream = 1;

void xrServer::RequestSetWeather(LPCSTR cycle_name)
{
    if (!cycle_name || !cycle_name[0])
        return;

    {
        std::lock_guard<xrCriticalSection> lock(m_world_weather_cs);   // RAII: an unwind here must not abandon it
        m_world_weather_req.has_cycle = true;
        xr_strcpy(m_world_weather_req.cycle, sizeof(m_world_weather_req.cycle), cycle_name);
        m_world_weather_req.release = false; // an explicit placement supersedes a pending release
    }
}

// The request struct carries one slot per weather channel. Kept as a plain array so
// it stays trivially copyable across the queue; this assert is what catches a new
// channel being added to the enum without widening the queue behind it.
static_assert(eWeatherChannelCount == 4, "widen SWorldWeatherRequest's channel arrays");

void xrServer::RequestSetWeatherChannel(int channel, float value)
{
    if (channel < 0 || channel >= eWeatherChannelCount)
        return;

    {
        std::lock_guard<xrCriticalSection> lock(m_world_weather_cs);   // RAII: an unwind here must not abandon it
        m_world_weather_req.has_channel[channel] = true;
        m_world_weather_req.channel[channel] = value;
        m_world_weather_req.clear_channel[channel] = false;
    }
}

void xrServer::RequestWeatherChannelAuto(int channel)
{
    if (channel < 0 || channel >= eWeatherChannelCount)
        return;

    {
        std::lock_guard<xrCriticalSection> lock(m_world_weather_cs);   // RAII: an unwind here must not abandon it
        m_world_weather_req.clear_channel[channel] = true;
        m_world_weather_req.has_channel[channel] = false;
    }
}

void xrServer::RequestWeatherAllAuto()
{
    for (int ch = 0; ch < eWeatherChannelCount; ++ch)
        RequestWeatherChannelAuto(ch);
}

void xrServer::RequestStartWeatherFX(LPCSTR fx_name)
{
    if (!fx_name || !fx_name[0])
        return;

    {
        std::lock_guard<xrCriticalSection> lock(m_world_weather_cs);   // RAII: an unwind here must not abandon it
        m_world_weather_req.has_fx = true;
        xr_strcpy(m_world_weather_req.fx, sizeof(m_world_weather_req.fx), fx_name);
        m_world_weather_req.stop_fx = false;
    }
}

void xrServer::RequestStopWeatherFX()
{
    {
        std::lock_guard<xrCriticalSection> lock(m_world_weather_cs);   // RAII: an unwind here must not abandon it
        m_world_weather_req.stop_fx = true;
        m_world_weather_req.has_fx = false;
    }
}

void xrServer::ReleaseWeather()
{
    {
        std::lock_guard<xrCriticalSection> lock(m_world_weather_cs);   // RAII: an unwind here must not abandon it
        m_world_weather_req.release = true;
        m_world_weather_req.has_cycle = false;
    }
}

shared_str xrServer::GetWorldWeather()
{
    if (!g_pGamePersistent)
        return shared_str{};
    return g_pGamePersistent->Environment().GetWeather();
}

u32 xrServer::GetWorldWeatherCrc() { return g_pGamePersistent ? g_pGamePersistent->Environment().GetWeatherCrc() : 0u; }

float xrServer::GetWorldWeatherChannel(int channel)
{
    if (!g_pGamePersistent || channel < 0 || channel >= eWeatherChannelCount)
        return 0.f;
    return g_pGamePersistent->Environment().GetWeatherLive((EWeatherChannel)channel);
}

bool xrServer::IsWorldWeatherChannelHeld(int channel)
{
    if (!g_pGamePersistent || channel < 0 || channel >= eWeatherChannelCount)
        return false;
    return g_pGamePersistent->Environment().HasWeatherOverride((EWeatherChannel)channel);
}

void xrServer::ApplyWorldWeatherRequests()
{
    SWorldWeatherRequest req;
    {
        std::lock_guard<xrCriticalSection> lock(m_world_weather_cs);   // RAII: an unwind here must not abandon it
        req = m_world_weather_req;
        m_world_weather_req = SWorldWeatherRequest();
    }

    bool any_channel = false;
    for (int ch = 0; ch < eWeatherChannelCount; ++ch)
        any_channel = any_channel || req.has_channel[ch] || req.clear_channel[ch];

    if (!req.has_cycle && !req.release && !any_channel && !req.has_fx && !req.stop_fx)
        return;

    // A view-less host still runs the environment (IGame_Persistent::OnFrame ticks
    // it whether or not anything is drawn), which is exactly what lets this process
    // be the single source of truth for the sky. With no world loaded there is
    // nothing to place the weather ON, and dropping the request loudly beats
    // pretending it landed.
    if (!g_pGamePersistent || !g_pGameLevel)
    {
        Msg("! weather: no world loaded yet -- request ignored");
        return;
    }

    CEnvironment& env = g_pGamePersistent->Environment();

    if (req.release)
    {
        env.LockWeather(false);
        m_weather_host_owned = false;
    }

    if (req.has_cycle)
    {
        // Takes the lock as well: without it this world's own forecast script
        // re-asserts its cycle within about a second, and every client would then
        // follow the script instead of the operator.
        env.SetWeatherAuthoritative(req.cycle);
        m_weather_host_owned = true;
    }

    for (int ch = 0; ch < eWeatherChannelCount; ++ch)
    {
        if (req.clear_channel[ch])
            env.ClearWeatherOverride((EWeatherChannel)ch);

        if (req.has_channel[ch])
        {
            env.SetWeatherOverride((EWeatherChannel)ch, req.channel[ch]);
            m_weather_host_owned = true;
        }
    }

    // Guarded: StopWeatherFX restores Current[0]/[1] from the effect's saved end
    // descriptors, which only hold anything while an effect is actually running.
    if (req.stop_fx && env.IsWeatherFXPlaying())
    {
        env.StopWeatherFX();
        Msg("~ weather: effect stopped by the host");
    }

    if (req.has_fx)
    {
        if (env.IsWeatherFXPlaying())
            Msg("! weather: an effect is already running -- stop it first (sv_weather_fx_stop)");
        else if (!env.StartWeatherFXAuthoritative(req.fx, 0.f)) // 0 = play it whole
            Msg("! weather: effect '%s' could not be started", req.fx);
        else
            m_weather_host_owned = true;
    }
}

void xrServer::Update()
{
    NET_GNS::PumpServer(); // no-op unless a transport backend is installed

    NET_Packet Packet;
    std::lock_guard<xrCriticalSection> lock(csPlayers);   // RAII: a throw in here must NOT abandon the lock (see xrSyncronize.h)

    // Move the clock before the game state (and therefore this tick's update
    // header) is built, so a change reaches every client on the very next export
    // instead of one tick late.
    ApplyWorldTimeRequests();
    ApplyWorldWeatherRequests();

    // Re-anchor the clock every few minutes. It is reported as
    // "anchor + rate * elapsed-since-anchor" in single precision; on a host that
    // stays up for days the elapsed term grows until that product quantises into
    // steps big enough to see in the sky. Re-anchoring is a no-op in value.
    if (game)
    {
        static u32 s_wt_anchor = 0;
        if (Device.dwTimeGlobal - s_wt_anchor > 300000)
        {
            s_wt_anchor = Device.dwTimeGlobal;
            game->SetGameTime(game->GetGameTime());
        }
    }

    // World-clock heartbeat, only while somebody is actually connected over the
    // wire (a fused single-process session has just its own local client, and
    // does not need the log line).
    if (net_Players.size() > 1 && game)
    {
        static u32 s_wt_last = 0;
        if (Device.dwTimeGlobal - s_wt_last > 30000)
        {
            s_wt_last = Device.dwTimeGlobal;
            u32 year = 0, month = 0, day = 0, hours = 0, mins = 0, secs = 0, ms = 0;
            split_time(game->GetGameTime(), year, month, day, hours, mins, secs, ms);
            Msg("* [worldtime] sv %02u:%02u:%02u rate=%.2f owner=%s clients=%u", hours, mins, secs, game->GetGameTimeFactor(),
                m_world_time_host_owned ? "host" : "scripts", (u32)net_Players.size());
            // Same heartbeat for the sky: what the world is running, every channel a
            // client is being given, and who decided. Separate tag so a test run can
            // grep for it alone. A '*' marks a channel a host is holding, as opposed
            // to one the running cycle produced.
            Msg("* [weather] sv cycle='%s' rain=%.2f%s fog=%.2f%s wind=%.0f%s thunder=%.1fs%s stream=%d owner=%s", GetWorldWeather().c_str(),
                GetWorldWeatherChannel(eWeatherRain), IsWorldWeatherChannelHeld(eWeatherRain) ? "*" : "", GetWorldWeatherChannel(eWeatherFog),
                IsWorldWeatherChannelHeld(eWeatherFog) ? "*" : "", GetWorldWeatherChannel(eWeatherWind), IsWorldWeatherChannelHeld(eWeatherWind) ? "*" : "",
                GetWorldWeatherChannel(eWeatherThunder), IsWorldWeatherChannelHeld(eWeatherThunder) ? "*" : "", g_sv_weather_stream,
                m_weather_host_owned ? "host" : "scripts");
        }
    }

    VERIFY(verify_entities());

    { LoadProf::Scope _p("sv.delayed_packets"); ProceedDelayedPackets(); }
    // game update
    { LoadProf::Scope _p("sv.delayed_events"); game->ProcessDelayedEvent(); }
    { LoadProf::Scope _p("sv.game_update"); game->Update(); }

    // spawn queue
    u32 svT = Device.TimerAsync();
    while (!(q_respawn.empty() || (svT < q_respawn.begin()->timestamp)))
    {
        // get
        svs_respawn R = *q_respawn.begin();
        q_respawn.erase(q_respawn.begin());

        //
        CSE_Abstract* E = ID_to_entity(R.phantom);
        if (!E)
        {
            // ⚠ID_to_entity returns null for an id no longer in `entities` (see :93):
            // a phantom whose entity died between being queued and being respawned.
            // Dereferencing it took an access violation that unwound out of Update
            // with csPlayers HELD, and the next tick deadlocked the whole game on it
            // (silent freeze, nothing in the log). Drop the stale entry and name it.
            Msg("! [xrServer] respawn queue: phantom id=%u no longer exists — entry dropped", (u32)R.phantom);
            continue;
        }
        E->Spawn_Write(Packet, FALSE);
        u16 ID;
        Packet.r_begin(ID);
        R_ASSERT(M_SPAWN == ID);
        ClientID clientID;
        clientID.set(0xffff);
        Process_spawn(Packet, clientID);
    }

    // automated-test hook: XROS_NPC_WALK_TEST=1 drives the first live NPC in a
    // slow circle on the SERVER, so the server->client entity update path can
    // be verified visually on a connected network client without depending on
    // real AI deciding to move. Env-gated: never active outside the harness.
    {
        static int s_walk_test = -1;
        if (s_walk_test < 0)
        {
            const char* v = getenv("XROS_NPC_WALK_TEST");
            s_walk_test = (v && *v == '1') ? 1 : 0;
        }
        if (1 == s_walk_test && g_pGameLevel)
        {
            static u16 s_walk_id = 0xffff;
            static Fvector s_walk_center{};
            static float s_walk_ang = 0.f;
            CGameObject* GO = nullptr;
            if (s_walk_id != 0xffff)
            {
                CEntityAlive* EA = smart_cast<CEntityAlive*>(Level().Objects.net_Find(s_walk_id));
                if (EA && EA->g_Alive())
                    GO = EA;
                else
                    s_walk_id = 0xffff; // died/despawned -- pick another
            }
            if (!GO)
            {
                for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
                {
                    CObject* O = Level().Objects.o_get_by_iterator(i);
                    CEntityAlive* EA = smart_cast<CEntityAlive*>(O);
                    if (!EA || !EA->g_Alive() || smart_cast<CActor*>(O))
                        continue;
                    if (strstr(O->cNameSect().c_str(), "crow"))
                        continue; // a ground walker demos better than a circling bird
                    s_walk_id = u16(O->ID());
                    s_walk_center = O->Position();
                    s_walk_ang = 0.f;
                    GO = EA;
                    Msg("* [npc_walk_test] driving '%s' id=%u around (%.1f, %.1f, %.1f)", O->cNameSect().c_str(), O->ID(), VPUSH(s_walk_center));
                    break;
                }
            }
            if (GO)
            {
                // 2 m radius at ~0.75 rad/s -> ~1.5 m/s linear: squarely in the
                // client puppet's walk band
                s_walk_ang += Device.fTimeDelta * 0.75f;
                Fvector pos;
                pos.set(s_walk_center.x + 2.f * cosf(s_walk_ang), s_walk_center.y, s_walk_center.z + 2.f * sinf(s_walk_ang));
                Fmatrix F;
                F.rotateY(-s_walk_ang); // face roughly along the tangent
                F.c = pos;
                GO->ForceTransform(F);
                static u32 s_walk_logged = 0;
                if (++s_walk_logged % 400 == 1)
                    Msg("* [npc_walk_test] '%s' id=%u pos=(%.1f, %.1f, %.1f)", GO->cNameSect().c_str(), GO->ID(), VPUSH(pos));
            }
        }
    }

    // Server-driven NPCs: one step of every outstanding order, before the
    // updates go out so a puppet's new position ships in the same frame it
    // was decided rather than a tick behind.
    { LoadProf::Scope _p("sv.npc"); SVNpc::Update(); }

    // Anomalies an operator placed in an earlier run, put back once the world
    // is up enough to hold them. A no-op after its single attempt -- see
    // SVAnomaly::Tick for why it waits here rather than hooking level load.
    { LoadProf::Scope _p("sv.anomaly"); SVAnomaly::Tick(); }

    // ...and the furniture, for the same reason and on the same terms: one
    // attempt per world, once there is a world to put it in.
    { LoadProf::Scope _p("sv.prop"); SVProp::Tick(); }

    // ...and what those creatures are holding, which no part of the protocol
    // said before. Outside SVNpc::Update on purpose: that one returns early
    // when the console has spawned no puppets, and every creature A-Life put on
    // the map carries a weapon too.
    { LoadProf::Scope _p("sv.hands"); SVNpc::PublishHands(); }

    // This tick is also a frame, and the frame rate is the rate clients learn
    // anything -- sampled here so the two are counted in the same place.
    SVPerf::Sample();

    { LoadProf::Scope _p("sv.send_updates"); SendUpdatesToAll(); }

    if (game->sv_force_sync)
        Perform_game_export();

    VERIFY(verify_entities());
    //-----------------------------------------------------
    // Remove any of long time disconnected players
    for (u32 DI = 0; DI < net_Players_disconnected.size();)
    {
        IClient* CL = net_Players_disconnected[DI];
        if (CL->dwTime_LastUpdate + g_sv_Client_Reconnect_Time * 60000 < Device.dwTimeGlobal)
        {
            client_Destroy(CL);
            continue;
        }
        DI++;
    }
}

// ⭐One entity's slot in the per-client update tail: [u16 id][u8 size][payload].
//
// ⚠The payload is UPDATE_Write PLUS the presentation intent, and the second
// part is here rather than inside UPDATE_Write for a reason worth stating where
// somebody would go to add the next field: UPDATE_Write is also how `all.spawn`
// stores an entity, so appending to it breaks LEVEL LOAD, not the network. The
// size prefix makes this tail self-describing, so anything added here is
// skipped harmlessly by a reader that does not know about it.
static void write_entity_update(NET_Packet& P, CSE_Abstract* E)
{
    u32 position;
    P.w_u16(E->ID);
    P.w_chunk_open8(position);
    E->UPDATE_Write(P);
    if (CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(E))
    {
        // ⭐⭐THE THIRD SAMPLE SITE, AND THE ONLY ONE THAT IS THE WIRE.
        //
        // sv_motion_audit already watched "assert" (what the drive commanded)
        // and "export" (what net_Export put in the mirror). Neither is what a
        // client receives: net_Export runs at frame rate and overwrites the
        // mirror several times between builds, so the export site measures the
        // sampling of a value, not the transmission of it -- measured 6220
        // export samples against 1557 asserts in one run, with steps of 0.0000
        // where the drive simply had not moved yet.
        //
        // This fires exactly once per entity per packet that actually leaves,
        // which is the cadence and the spacing the receiver has to interpolate
        // between. If assert is a glide and wire is not, the defect is in the
        // build cadence and no receiver-side smoothing will reach it.
        //
        // ⚠Sampled AFTER UPDATE_Write, so `timestamp` is literally the value
        // that was just serialised -- not the clock as it reads now. The
        // recorder takes both of those itself; this is the third one.
        SVMotionAudit::Note(u16(creature->ID), "wire", creature->o_Position, creature->o_model, false, creature->timestamp);

        P.w_u8(creature->o_intent.flags);
        P.w_u8(creature->o_intent.slot);
        P.w_s8(creature->o_intent.v_fwd);
        P.w_s8(creature->o_intent.v_str);
        P.w_s8(creature->o_intent.turn);

        // The clips this server's animator is playing (SNetAnimState). One byte
        // when there is no animator here; a reader that predates this stops at
        // the intent and the chunk's size skips the rest.
        const SNetAnimState& A = creature->o_anim;
        P.w_u8(A.mask);
        for (u32 p = 0; p < SNetAnimState::kParts; ++p)
            if (A.has(p))
            {
                P.w_u8(A.part[p].slot);
                P.w_u16(A.part[p].idx);
                P.w_u16(A.part[p].time_ms);
                P.w_u8(A.part[p].speed_q);
            }
    }
    P.w_chunk_close8(position);
}

void xrServer::SendUpdatesToAll()
{

    for (u32 client = 0; client < net_Players.size(); ++client)
    { // for each client
        // Initialize process and check for available bandwidth
        xrClientData* Client = (xrClientData*)net_Players[client];
        if (!Client->net_Ready)
            continue;
        if (false

#ifdef DEBUG
            && !g_sv_SendUpdate
#endif
        )
            continue;

        // Send relevant entities to client
        NET_Packet Packet;
        u16 PacketType = M_UPDATE;
        Packet.w_begin(PacketType);
        // GameUpdate
        game->net_Export_Update(Packet, Client->ID, Client->ID);
        game->net_Export_GameTime(Packet);

        if (Client->flags.bLocal) // this is server client;
        {
            SendTo(Client->ID, Packet, net_flags(FALSE, TRUE));
            continue;
        }

        // Non-local clients get their packet (game-state header + entity tail)
        // at the 20 Hz build cadence ONLY. The old shape sent the header every
        // server tick: a headless server ticks far faster than a render client
        // pumps, so the stream backed up without bound and gameplay traffic
        // (M_SPAWN of the other player's actor, update tails) arrived minutes
        // stale -- the 23-07 "we can't see each other / world frozen" session.
        const bool build_entities = (Device.dwTimeGlobal - Client->upd_last_build) >= 50;
        if (!build_entities)
            continue;

        // Per-destination packet: game-state header + this client's per-entity
        // tail. The tail is PER CLIENT (was shared): each destination excludes
        // its OWN controlled entity (echo suppression) but must still receive
        // every OTHER client's one -- that's how player B sees player A move.
        NET_Packet ToSend;
        ToSend.B.count = 0;
        ToSend.w(Packet.B.data, Packet.B.count);

        if (g_Dump_Update_Write)
        {
            if (Client->ps)
                Msg("---- UPDATE_Write to %s --- ", Client->ps->getName());
            else
                Msg("---- UPDATE_Write to %s --- ", *(Client->name));
        }

        // Per-entity part. A large world does not fit a single packet, so
        // entities are streamed round-robin: write until the packet is nearly
        // full, remember where we stopped, resume next build.
        if (!entities.empty())
        {
            Client->upd_last_build = Device.dwTimeGlobal;

            NET_Packet tmpPacket;

            // diag: periodic stats on the per-entity update export (throttled)
            u32 diag_total = 0, diag_own0 = 0, diag_notready = 0, diag_phantom = 0, diag_irrel = 0, diag_clctl = 0, diag_wrote = 0;
            bool packet_full = false;
            // diag: sampled dump of the ids actually written -- to compare
            // 1:1 with the client-side tail dump when entries go missing
            static u32 s_diag_dump_builds = 0;
            // odd modulus: with two clients alternating builds an even one
            // samples only ONE client forever (parity lock)
            const bool dump_build = (++s_diag_dump_builds % 127 == 1);
            string512 dump_ids;
            u32 dump_len = 0;
            if (dump_build)
                dump_len = (u32)sprintf_s(dump_ids, "* [netupd] sv tail to cl %u:", Client->ID.value());

            // A refused position goes out FIRST, ahead of the round-robin below.
            // The owner is the only process still believing the position the
            // server threw away, and the cursor can take seconds to come round to
            // that entity in a populated world (~1800 entities, ~25 written per
            // build at 20 Hz) -- seconds of running somewhere the world does not
            // agree with is exactly what this channel exists to prevent.
            if (Client->mv_needs_correction)
            {
                Client->mv_needs_correction = false;
                if (CSE_Abstract* Own = ID_to_entity(Client->mv_entity_id))
                {
                    tmpPacket.B.count = 0;
                    write_entity_update(tmpPacket, Own);
                    ToSend.w(tmpPacket.B.data, tmpPacket.B.count);
                    Msg("* [sv_move] correcting client %u: id=%u at (%.2f, %.2f, %.2f)", Client->ID.value(), Own->ID, VPUSH(Client->mv_last_pos));
                }
            }

            // Interest: tell a client about what is NEAR it, not about the whole
            // world. Two reasons, and the second is the important one:
            //  - every client currently receives every entity, which is the
            //    O(clients x entities) shape that will not survive 100 players;
            //  - what was never sent cannot be drawn. This is the one measure
            //    against seeing through walls that still works when the program
            //    on the other end has been modified, because it removes the data
            //    rather than asking the client not to use it.
            // Zero radius keeps the old send-everything behaviour: entities that
            // fall out of interest simply stop updating (they are not despawned
            // yet), so a radius must be chosen well beyond what a player can see.
            const float interest_r = g_sv_interest_radius;
            const bool interest_on = (interest_r > 0.f) && (nullptr != Client->owner);
            const float interest_sqr = interest_r * interest_r;
            Fvector client_pos{};
            if (Client->owner)
                client_pos = Client->owner->o_Position;
            u32 diag_far = 0;

            const u32 total = (u32)entities.size();
            xrS_entities::iterator I = entities.upper_bound(Client->upd_cursor);
            if (I == entities.end())
                I = entities.begin();

            for (u32 visited = 0; visited < total && !packet_full; ++visited)
            {
                CSE_Abstract& Test = *(I->second);
                const u16 this_id = I->first;
                ++I;
                if (I == entities.end())
                    I = entities.begin();
                ++diag_total;

                if (0 == Test.owner)
                {
                    ++diag_own0;
                    // ⭐⭐WHAT ARE THESE 694? Reported from play 30-07-2026: the
                    // level's own mutants fought and killed the player while the
                    // client had never heard of them -- its whole replicated
                    // world was SEVEN entities against 1881 on the server. This
                    // bucket is the largest thing standing between the two, and
                    // "owner == 0" says nothing about whether that is correct:
                    // an offline A-Life object SHOULD be skipped, an online
                    // creature that is hunting the player should not.
                    //
                    // ⚠So print the entities rather than reason about them. The
                    // one thing that separates the two cases is whether the
                    // server has a live game object for the id -- an entity it
                    // is actually simulating -- and that is cheap to ask.
                    census_skip(this_id, Test, "owner=0");
                    continue;
                }
                if (!Test.net_Ready)
                {
                    ++diag_notready;
                    census_skip(this_id, Test, "net_Ready=0 -- the client never spawned it");
                    continue;
                }
                if (Test.s_flags.is(M_SPAWN_OBJECT_PHANTOM))
                {
                    ++diag_phantom;
                    census_skip(this_id, Test, "phantom");
                    continue; // Surely: phantom
                }
                // echo suppression: THIS client is authoritative for the
                // entities it controls -- don't send its own state back to it.
                // Other clients' controlled entities DO stream (and are always
                // fresh -- their owners push state at 20 Hz -- so they bypass
                // the Net_Relevant staleness check).
                const bool client_controlled = net_client_controls(this_id);
                bool driven_by_this_client = client_controlled && Test.owner == Client;

                // ⭐⭐⭐...and so is everything INSIDE that body. This used to stop
                // at the body itself, and what a player carries is owned by the
                // server client rather than by them, so a player's own weapon and
                // ammo were streamed back at them as corrections.
                //
                // ⚠Measured 28-07-2026 with a player at the screen, and the shape
                // of the failure is worth keeping: a weapon carries its round
                // count in TWO places -- CSE_ALifeItemWeapon::a_elapsed, which is
                // what the wire carries, and CWeaponMagazined::m_magazine, the
                // actual list of cartridges, which is not on the wire at all. An
                // echoed update writes the first and cannot write the second. The
                // weapon then believes it is full (iAmmoElapsed 31) while holding
                // nothing, so it cannot fire -- and cannot reload either, because
                // ReloadMagazine's `while (iAmmoElapsed < iMagazineSize + 1)` is
                // false on entry and loads nothing. The reload animation plays in
                // full and the magazine stays empty. The engine's own
                // VERIFY(iAmmoElapsed == m_magazine.size()) would have caught it,
                // and is compiled out of a release build.
                if (!driven_by_this_client && Test.ID_Parent != 0xffff)
                {
                    CSE_Abstract* holder = game->get_entity_from_eid(Test.ID_Parent);
                    if (holder && holder->owner == Client && net_client_controls(holder->ID))
                        driven_by_this_client = true;
                }

                if (driven_by_this_client)
                {
                    ++diag_clctl;
                    continue;
                }
                // Out of interest: skipped for EVERY entity kind, including other
                // players -- they are precisely what must not leak across the map.
                if (interest_on && Test.o_Position.distance_to_sqr(client_pos) > interest_sqr)
                {
                    ++diag_far;
                    census_skip(this_id, Test, "beyond the interest radius");
                    continue;
                }

                if (!client_controlled && !Test.Net_Relevant())
                {
                    ++diag_irrel;
                    census_skip(this_id, Test, "Net_Relevant()=false");
                    continue;
                }
                tmpPacket.B.count = 0;
                // write specific data
                {
                    write_entity_update(tmpPacket, &Test);
                    // id + size header aside, this is what the entity itself
                    // contributed -- zero means it had nothing to say
                    u32 ObjectSize = u32(tmpPacket.B.count) - sizeof(u16) - sizeof(u8);

                    if (ObjectSize == 0)
                        continue;
#ifdef DEBUG
                    if (g_Dump_Update_Write)
                        Msg("* %s : %d", Test.name(), ObjectSize);
#endif

                    if (ToSend.B.count + tmpPacket.B.count >= NET_PacketSizeLimit)
                    {
                        // packet full -- this entity was NOT written and the
                        // cursor still points before it, so the next build
                        // resumes exactly here
                        packet_full = true;
                        break;
                    }
                    ToSend.w(tmpPacket.B.data, tmpPacket.B.count);
                    ++diag_wrote;
                    Client->upd_cursor = this_id;
                    if (dump_build && dump_len + 8 < sizeof(dump_ids))
                        dump_len += (u32)sprintf_s(dump_ids + dump_len, sizeof(dump_ids) - dump_len, " %u", this_id);
                    // diag: actor entities carry the multiplayer feature -- trace their export
                    if (client_controlled)
                    {
                        static u32 s_diag_ctl = 0;
                        if (++s_diag_ctl % 63 == 1)
                            Msg("* [netupd] sv wrote controlled id=%u size=%u to cl %u", this_id, ObjectSize, Client->ID.value());
                    }
                }
            }

            if (dump_build)
                Msg("%s", dump_ids);

            // diag: throttled summary of what the update export actually produced
            static u32 s_diag_builds = 0;
            if (++s_diag_builds % 127 == 1)
                Msg("* [netupd] sv export to cl %u: far=%u total=%u own0=%u notready=%u phantom=%u irrel=%u clctl=%u wrote=%u full=%d bytes=%u (limit=%u)", Client->ID.value(), diag_far,
                    diag_total, diag_own0, diag_notready, diag_phantom, diag_irrel, diag_clctl, diag_wrote, packet_full ? 1 : 0, ToSend.B.count, (u32)NET_PacketSizeLimit);
        }

        //.#ifdef DEBUG
        if (g_Dump_Update_Write)
            Msg("----------------------- ");
        //.#endif
        if (ToSend.B.count > 2)
        {
            //.#ifdef DEBUG
            if (g_Dump_Update_Write && Client->ps != NULL)
            {
                Msg("- Server Update[%d] to Client[%s]  : %d", *((u16*)ToSend.B.data), Client->ps->getName(), ToSend.B.count);
            }
            //.#endif

            SendTo(Client->ID, ToSend, net_flags(FALSE, TRUE));
        }

    }; // for each client
#ifdef DEBUG
    g_sv_SendUpdate = 0;
#endif

    if (game->sv_force_sync)
        Perform_game_export();

    VERIFY(verify_entities());
}

xr_vector<shared_str> _tmp_log;
void console_log_cb(LPCSTR text) { _tmp_log.push_back(text); }

u32 xrServer::OnDelayedMessage(NET_Packet& P, ClientID sender) // Non-Zero means broadcasting with "flags" as returned
{
    u16 type;
    P.r_begin(type);

    std::lock_guard<xrCriticalSection> lock(csPlayers);   // RAII: a throw in here must NOT abandon the lock (see xrSyncronize.h)

    VERIFY(verify_entities());
    xrClientData* CL = ID_to_client(sender);
    R_ASSERT2(CL, make_string("packet type [%d]", type).c_str());

    switch (type)
    {
    case M_CLIENT_REQUEST_CONNECTION_DATA: {
        OnCL_Connected(CL);
    }
    break;
    }
    VERIFY(verify_entities());

    return 0;
}

extern float g_fCatchObjectTime;
u32 xrServer::OnMessage(NET_Packet& P, ClientID sender) // Non-Zero means broadcasting with "flags" as returned
{
    u16 type;
    P.r_begin(type);

    std::lock_guard<xrCriticalSection> lock(csPlayers);   // RAII: a throw in here must NOT abandon the lock (see xrSyncronize.h)

    VERIFY(verify_entities());
    xrClientData* CL = ID_to_client(sender);

    switch (type)
    {
    case M_UPDATE: {
        Process_update(P, sender); // No broadcast
        VERIFY(verify_entities());
    }
    break;
    case M_SPAWN: {
        Process_spawn_from_client(P, sender, CL);
        VERIFY(verify_entities());
    }
    break;
    case M_EVENT: {
        Process_event(P, sender);
        VERIFY(verify_entities());
    }
    break;
    case M_EVENT_PACK: {
        NET_Packet tmpP;
        while (!P.r_eof())
        {
            tmpP.B.count = P.r_u8();
            P.r(&tmpP.B.data, tmpP.B.count);

            OnMessage(tmpP, sender);
        }
    }
    break;
    case M_CLIENTREADY: {
        xrClientData* CL = ID_to_client(sender);
        if (CL)
        {
            CL->net_Ready = TRUE;
            CL->ps->DeathTime = Device.dwTimeGlobal;
            CL->ps->setName(CL->name.c_str());
        };
        game->signal_Syncronize();
        VERIFY(verify_entities());
    }
    break;
    case M_ADMIN_COMMAND: {
        Process_admin_command(P, sender);
        VERIFY(verify_entities());
    }
    break;
    case M_SWITCH_DISTANCE: {
        game->switch_distance(P, sender);
        VERIFY(verify_entities());
    }
    break;
    case M_CHANGE_LEVEL: {
        if (game->change_level(P, sender))
        {
            SendBroadcast(BroadcastCID, P, net_flags(TRUE, TRUE));
        }
        VERIFY(verify_entities());
    }
    break;
    case M_SAVE_GAME: {
        game->save_game(P, sender);
        VERIFY(verify_entities());
    }
    break;
    case M_LOAD_GAME: {
        game->load_game(P, sender);
        SendBroadcast(BroadcastCID, P, net_flags(TRUE, TRUE));
        VERIFY(verify_entities());
    }
    break;
    case M_RELOAD_GAME: {
        SendBroadcast(BroadcastCID, P, net_flags(TRUE, TRUE));
        VERIFY(verify_entities());
    }
    break;
    case M_SAVE_PACKET: {
        Process_save(P, sender);
        VERIFY(verify_entities());
    }
    break;
    case M_CLIENT_REQUEST_CONNECTION_DATA: {
        AddDelayedPacket(P, sender);
    }
    break;
    case M_CHANGE_LEVEL_GAME: {
        ClientID CID;
        CID.set(0xffffffff);
        SendBroadcast(CID, P, net_flags(TRUE, TRUE));
    }
    break;
    case M_CL_AUTH: {
        game->AddDelayedEvent(P, GAME_EVENT_PLAYER_AUTH, 0, sender);
    }
    break;
    case M_STATISTIC_UPDATE: {
        if (SV_Client)
            SendBroadcast(SV_Client->ID, P, net_flags(TRUE, TRUE));
        else
            SendBroadcast(BroadcastCID, P, net_flags(TRUE, TRUE));
    }
    break;
    case M_STATISTIC_UPDATE_RESPOND: {
        if (SV_Client)
            SendTo(SV_Client->ID, P, net_flags(TRUE, TRUE));
    }
    break;
    }

    VERIFY(verify_entities());

    return IPureServer::OnMessage(P, sender);
}

void xrServer::SendTo_LL(ClientID ID, void* data, u32 size, u32 dwFlags, u32 dwTimeout)
{
    if (SV_Client && SV_Client->ID == ID)
    {
        // optimize local traffic
        Level().OnMessage(data, size);
    }
    else
    {
        IClient* pClient = ID_to_client(ID);
        if (!pClient)
            return;

        if (NET_GNS::g_SendToClient)
        {
            bool bReliable = (dwFlags & DPNSEND_GUARANTEED) != 0;
            if (!NET_GNS::g_SendToClient(ID, data, size, bReliable))
                Msg("! xrServer::SendTo_LL: GNS send failed for ClientID=%u", ID.value());
        }
        else
        {
            FATAL(""); //Это не должно быть вызвано -- no transport backend installed (only XR_3DA_Server installs one)
        }
    }
}

//--------------------------------------------------------------------
CSE_Abstract* xrServer::entity_Create(LPCSTR name) { return F_entity_Create(name); }

void xrServer::entity_Destroy(CSE_Abstract*& P)
{
#ifdef DEBUG
    Msg("xrServer::entity_Destroy : [%d][%s][%s]", P->ID, P->name(), P->name_replace());
#endif
    R_ASSERT(P);
    entities.erase(P->ID);
    m_tID_Generator.vfFreeID(P->ID, Device.TimerAsync());

    if (P->owner && P->owner->owner == P)
        P->owner->owner = NULL;

    P->owner = NULL;
    if (!ai().get_alife() || !P->m_bALifeControl)
    {
        F_entity_Destroy(P);
    }
}

#ifdef DEBUG

static BOOL _ve_initialized = FALSE;
static BOOL _ve_use = TRUE;

bool xrServer::verify_entities() const
{
    if (!_ve_initialized)
    {
        _ve_initialized = TRUE;
        if (strstr(Core.Params, "-~ve"))
            _ve_use = FALSE;
    }
    if (!_ve_use)
        return true;

    xrS_entities::const_iterator I = entities.begin();
    xrS_entities::const_iterator E = entities.end();
    for (; I != E; ++I)
    {
        VERIFY2((*I).first != 0xffff, "SERVER : Invalid entity id as a map key - 0xffff");
        VERIFY2((*I).second, "SERVER : Null entity object in the map");
        VERIFY3((*I).first == (*I).second->ID, "SERVER : ID mismatch - map key doesn't correspond to the real entity ID", (*I).second->name_replace());
        verify_entity((*I).second);
    }
    return (true);
}

void xrServer::verify_entity(const CSE_Abstract* entity) const
{
    VERIFY(entity->m_wVersion != 0);
    if (entity->ID_Parent != 0xffff)
    {
        xrS_entities::const_iterator J = entities.find(entity->ID_Parent);
        VERIFY3(J != entities.end(), "SERVER : Cannot find parent in the map", entity->name_replace());
        VERIFY3((*J).second, "SERVER : Null entity object in the map", entity->name_replace());
        VERIFY3((*J).first == (*J).second->ID, "SERVER : ID mismatch - map key doesn't correspond to the real entity ID", (*J).second->name_replace());
        VERIFY3(std::find((*J).second->children.begin(), (*J).second->children.end(), entity->ID) != (*J).second->children.end(),
                "SERVER : Parent/Children relationship mismatch - Object has parent, but corresponding parent doesn't have children", (*J).second->name_replace());
    }

    xr_vector<u16>::const_iterator I = entity->children.begin();
    xr_vector<u16>::const_iterator E = entity->children.end();
    for (; I != E; ++I)
    {
        VERIFY3(*I != 0xffff, "SERVER : Invalid entity children id - 0xffff", entity->name_replace());
        xrS_entities::const_iterator J = entities.find(*I);
        VERIFY3(J != entities.end(), "SERVER : Cannot find children in the map", entity->name_replace());
        VERIFY3((*J).second, "SERVER : Null entity object in the map", entity->name_replace());
        VERIFY3((*J).first == (*J).second->ID, "SERVER : ID mismatch - map key doesn't correspond to the real entity ID", (*J).second->name_replace());
        VERIFY3((*J).second->ID_Parent == entity->ID, "SERVER : Parent/Children relationship mismatch - Object has children, but children doesn't have parent",
                (*J).second->name_replace());
    }
}

#endif // DEBUG

shared_str xrServer::level_name(const shared_str& server_options) const { return (game->level_name(server_options)); }

void xrServer::create_direct_client()
{
    SClientConnectData cl_data;
    cl_data.clientID.set(1);
    strcpy_s(cl_data.name, "single_player");
    cl_data.process_id = GetCurrentProcessId();

    new_client(&cl_data);
}

void xrServer::ProceedDelayedPackets()
{
    {
        std::lock_guard<xrCriticalSection> lock(DelayedPackestCS);   // RAII: an unwind here must not abandon it
        while (!m_aDelayedPackets.empty())
        {
            DelayedPacket& DPacket = *m_aDelayedPackets.begin();
            OnDelayedMessage(DPacket.Packet, DPacket.SenderID);
            //		OnMessage(DPacket.Packet, DPacket.SenderID);
            m_aDelayedPackets.pop_front();
        }
    }
};

void xrServer::AddDelayedPacket(NET_Packet& Packet, ClientID Sender)
{
    {
        std::lock_guard<xrCriticalSection> lock(DelayedPackestCS);   // RAII: an unwind here must not abandon it

        DelayedPacket* NewPacket = &(m_aDelayedPackets.emplace_back());
        NewPacket->SenderID = Sender;
        CopyMemory(&(NewPacket->Packet), &Packet, sizeof(NET_Packet));

    }
}
