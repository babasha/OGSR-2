// xrServer.h: interface for the xrServer class.
//
//////////////////////////////////////////////////////////////////////

#pragma once

#include "../xr_3da/NET_Server_Trash/NET_Server.h"
#include "game_sv_base.h"
#include "id_generator.h"

class CSE_Abstract;

const u32 NET_Latency = 50; // time in (ms)

// t-defs
typedef xr_map<u16, CSE_Abstract*> xrS_entities;

class xrClientData : public IClient
{
public:
    CSE_Abstract* owner;
    BOOL net_Ready;
    BOOL net_Accepted;

    // per-entity update export to THIS client: round-robin cursor +
    // build-rate limiter (per client, because each destination excludes its
    // own controlled entity and a shared cursor would split the world between
    // clients instead of streaming all of it to each)
    u16 upd_cursor{};
    u32 upd_last_build{};

    BOOL net_PassUpdates;
    u32 net_LastMoveUpdateTime;

    // Movement validation state (xrServer::apply_net_client_state). A client
    // reports where its own player is, so "where" has to be checked against how
    // far it could have travelled since the last position we accepted -- without
    // this the reported position IS the position, which is a teleport for the
    // asking. Baseline starts from the server's own object, not from the first
    // packet, or the first thing a client says would define its own truth.
    bool mv_have_baseline{false};
    u16 mv_entity_id{u16(-1)};
    Fvector mv_last_pos{};
    u32 mv_last_time{};
    u32 mv_violations{};
    // Set when a reported position was refused. Nothing else would ever tell the
    // owner: its own entity is normally excluded from the stream it receives
    // (echo suppression), so a silent clamp would leave it walking around a world
    // that has it somewhere else, permanently.
    bool mv_needs_correction{false};
    // Seconds of sustained above-run movement still available. The engine's own
    // stamina (CActorCondition::m_fPower) is computed on the CLIENT for its own
    // actor, so a client that stops draining it sprints forever; this is the
    // server's independent budget, spent by measured speed and refilled by rest.
    // Deliberately NOT a copy of the game's formula (weight, artefacts and mod
    // config feed that one) -- it only has to be impossible to beat honestly.
    float mv_sprint_budget{-1.f}; // < 0: not initialised yet
    bool mv_sprint_spent{false}; // already reported as exhausted

    // Who this client proved to be, from the join ticket authd signed
    // (online/server/sv_ticket.cpp verifies it during the GNS handshake).
    //
    // ⚠️ 0 means NOBODY -- either this host does not require tickets, or the
    // client joined before one was demanded. Every authority check must treat 0
    // as "no rights"; it is not account number zero, and `role` is only
    // meaningful when account_id is non-zero.
    u64 account_id{};
    u8 role{}; // SV_Ticket::Role as its numeric value: 0 player, 10 mod, 100 admin

    game_PlayerState* ps;

    xrClientData();
    virtual ~xrClientData();

    virtual void Clear();
};

// main
struct svs_respawn
{
    u32 timestamp;
    u16 phantom;
};
IC bool operator<(const svs_respawn& A, const svs_respawn& B) { return A.timestamp < B.timestamp; }

class xrServer : public IPureServer
{
private:
    xrS_entities entities;
    xr_multiset<svs_respawn> q_respawn;

    u16 m_iCurUpdatePacket;
    xr_vector<NET_Packet> m_aUpdatePackets;


    struct DelayedPacket
    {
        ClientID SenderID;
        NET_Packet Packet;
        bool operator==(const DelayedPacket& other) { return SenderID == other.SenderID; }
    };

    xrCriticalSection DelayedPackestCS;
    xr_deque<DelayedPacket> m_aDelayedPackets;

    void ProceedDelayedPackets();
    void AddDelayedPacket(NET_Packet& Packet, ClientID Sender);
    u32 OnDelayedMessage(NET_Packet& P, ClientID sender); // Non-Zero means broadcasting with "flags" as returned

    void SendUpdatesToAll();

private:
    typedef CID_Generator<u32, // time identifier type
                          u8, // compressed id type
                          u16, // id type
                          u8, // block id type
                          u16, // chunk id type
                          0, // min value
                          u16(-2), // max value
                          256, // block size
                          u16(-1) // invalid id
                          >
        id_generator_type;

private:
    id_generator_type m_tID_Generator;

public:
    game_sv_GameState* game{};

    void Export_game_type(IClient* CL);
    void Perform_game_export();
    BOOL PerformRP(CSE_Abstract* E);

    IC void clear_ids() { m_tID_Generator = id_generator_type(); }
    IC u16 PerformIDgen(u16 ID) { return (m_tID_Generator.tfGetID(ID)); }
    IC void FreeID(u16 ID, u32 time) { return (m_tID_Generator.vfFreeID(ID, time)); }

    void Perform_connect_spawn(CSE_Abstract* E, xrClientData* to, NET_Packet& P);
    void Perform_spawn_net_actor(xrClientData* CL);
    void Perform_transfer(NET_Packet& PR, NET_Packet& PT, CSE_Abstract* what, CSE_Abstract* from, CSE_Abstract* to);
    void Perform_reject(CSE_Abstract* what, CSE_Abstract* from, int delta);
    void Perform_destroy(CSE_Abstract* tpSE_Abstract, u32 mode);

    // `from_net_client` turns on the authority check in client_may_spawn. It is
    // off for every server-side caller, which is the honest default: the server
    // spawning something needs no permission from itself.
    CSE_Abstract* Process_spawn(NET_Packet& P, ClientID sender, BOOL bSpawnWithClientsMainEntityAsParent = FALSE, CSE_Abstract* tpExistedEntity = 0, bool from_net_client = false);
    void Process_update(NET_Packet& P, ClientID sender);

    // M_SPAWN as it arrives off the wire. See the long note in
    // xrServer_process_spawn.cpp: the gate this replaces discarded, in silence,
    // every spawn a real network client ever asked for.
    void Process_spawn_from_client(NET_Packet& P, ClientID sender, xrClientData* CL);
    bool client_may_spawn(xrClientData* CL, CSE_Abstract* E, CSE_Abstract* e_parent);

    // Entities a connected network client exports state for (its controlled
    // entity). While an id is in this set the in-process fused export
    // (CLevel::ClientSend) must not overwrite the server entity -- the network
    // client's updates are the source of truth for it.
    bool net_client_controls(u16 id) const { return m_net_client_entities.find(id) != m_net_client_entities.end(); }

private:
    xr_set<u16> m_net_client_entities;
    void apply_net_client_state(CSE_Abstract* E, xrClientData* CL);
    // Checks a client-reported position against the distance it could plausibly
    // have covered. Returns the position to actually use: the reported one when
    // it is within budget, a clamped one when it is not.
    Fvector validate_client_movement(xrClientData* CL, CSE_Abstract* E, const Fvector& reported, const Fvector& server_pos);

public:
    // Kill an entity on the server's own initiative, without waiting for the
    // owning client to admit it died (see sv_health.h). Does exactly what the
    // GE_DIE event path does, minus the parsing.
    void force_kill(CSE_Abstract* e_dest, u16 id_src);

    void Process_save(NET_Packet& P, ClientID sender);
    void Process_event(NET_Packet& P, ClientID sender);
    // `event_type` is only carried so the ownership ledger can tell the flows
    // apart: picking a thing off the ground, taking it out of a corpse or a
    // container, and buying it all arrive here, and a distance rule set from
    // one of them would refuse the others. It changes nothing in the handling.
    // ⚠No default for event_type on purpose: this header does not see
    // xrMessages.h, and a default of 0 would quietly file every transfer under
    // the wrong flow -- the one number the loot gate is meant to be set from.
    void Process_event_ownership(NET_Packet& P, ClientID sender, u32 time, u16 ID, BOOL bForced, u16 event_type);
    bool Process_event_reject(NET_Packet& P, const ClientID sender, const u32 time, const u16 id_parent, const u16 id_entity, bool send_message = true);
    void Process_event_destroy(NET_Packet& P, ClientID sender, u32 time, u16 ID, NET_Packet* pEPack);

    xrClientData* SelectBestClientToMigrateTo(CSE_Abstract* E, BOOL bForceAnother = FALSE);
    void SendConnectResult(IClient* CL, u8 res, u8 res1, const char* ResultStr);

    void AttachNewClient(IClient* CL);
    virtual void OnBuildVersionRespond(IClient* CL, NET_Packet& P);

    // M2: registers a newly-accepted GNS network connection as a real
    // (non-local) xrServer client, reusing the exact same new_client()/
    // GAME_EVENT_CREATE_CLIENT/AttachNewClient path the synthetic
    // "single_player" client already goes through -- see
    // create_direct_client() and xrServer_Connect.cpp.
    // account_id/role come from the verified join ticket (sv_ticket.h). The
    // defaults are the unauthenticated case: no account, no rights.
    void OnNetClientConnected(ClientID id, LPCSTR name = "net_client", u64 account_id = 0, u8 role = 0);

    // A console command an authenticated administrator sent over the wire.
    // Refuses and logs anything the sender's role does not cover.
    void Process_admin_command(NET_Packet& P, ClientID sender);

    // M2: symmetric teardown when a GNS network connection drops. Destroys the
    // per-client actor clone so every remaining client drops its remote-player
    // puppet (GE_DESTROY broadcast) instead of it lingering as a frozen ghost,
    // then removes the client bookkeeping. The ALife-simulated base actor is
    // released, not destroyed -- see the implementation for why.
    void OnNetClientDisconnected(ClientID id);

    // ---- world clock control -------------------------------------------------
    // The server owns the time of day: it drives the simulation clock and ships
    // it to every connected client in each update header, so clients render the
    // server's sky rather than a clock of their own.
    //
    // These entry points let the host process (console command, out-of-tree host
    // module) steer that clock. They only QUEUE the request: the caller may be on
    // any thread, while the clock must change on the server tick, between update
    // exports, so no client ever sees a half-applied change.
    void RequestSetGameTime(u64 game_time_ms); // absolute timestamp
    void RequestAdvanceGameTime(u32 delta_ms); // relative, forward only
    void RequestSetTimeFactor(float factor); // rate; <= 0 freezes the clock
    // Current authoritative time / rate, for a host that wants to read it back.
    u64 GetWorldTime();
    float GetWorldTimeFactor();

    // Placing the clock explicitly claims it for the host: from then on scripts
    // running in this process are refused, because a world whose time of day is
    // set by the operator must not be pulled back to the pace some script wants
    // -- and every connected client renders whatever this clock says. Released
    // again with ReleaseWorldTime() (console: sv_time_auto).
    bool WorldTimeHostOwned() const { return m_world_time_host_owned; }
    void ReleaseWorldTime() { m_world_time_host_owned = false; }

    // ---- world weather / rain control -----------------------------------------
    // Weather belongs to the world, not to the machine drawing it: whether it rains
    // and how hard has to be one answer for everybody, or two players in the same
    // street disagree about the street. So this process decides, and every client
    // renders what the update header carries (see net_Export_GameTime).
    //
    // Two levers, deliberately separate:
    //   * the CYCLE  -- which weather section runs (sky, clouds, wind, thunder and
    //     the rain curve over the day). Clients follow the server's cycle by name
    //     digest, so with the shared world clock they compute the SAME rain the
    //     server does, smoothly, without it being streamed.
    //   * the RAIN OVERRIDE -- "rain now, this hard", regardless of what hour the
    //     cycle is in. This is the one that gets shipped explicitly, because no
    //     client could derive it.
    // Same queue-and-apply-on-tick discipline as the clock above: callers may be on
    // any thread, the world changes between update exports.
    void RequestSetWeather(LPCSTR cycle_name);                  // apply a cycle authoritatively
    void RequestSetWeatherChannel(int channel, float value);    // hold one EWeatherChannel
    void RequestWeatherChannelAuto(int channel);                // that channel back to the cycle
    void RequestWeatherAllAuto();                               // all channels back to the cycle
    void RequestStartWeatherFX(LPCSTR fx_name);                 // run a weather effect (storm, emission)
    void RequestStopWeatherFX();                                // end it early
    void ReleaseWeather();                                      // cycle back to this world's scripts

    // Claimed by any explicit weather/rain placement, for the same reason the clock
    // is: a shared sky must not be pulled back by whatever a mod script wants.
    bool WeatherHostOwned() const { return m_weather_host_owned; }

    // Readback of what this world is ACTUALLY running (host- or script-decided).
    shared_str GetWorldWeather();
    u32 GetWorldWeatherCrc();
    float GetWorldWeatherChannel(int channel);   // live value -- the number clients get
    bool IsWorldWeatherChannelHeld(int channel); // true while a host override is held

private:
    bool m_weather_host_owned{false};
    struct SWorldWeatherRequest
    {
        bool has_cycle{false};
        // Fixed buffer rather than shared_str: the request is filled on the caller's
        // thread and read on the server tick, and a plain copy keeps the string
        // container out of the cross-thread path entirely.
        char cycle[64]{};
        // One slot per EWeatherChannel: 'set' holds a value, 'clear' hands it back.
        // Fixed-size arrays keep the whole request trivially copyable across threads.
        bool has_channel[4]{};
        float channel[4]{};
        bool clear_channel[4]{};
        bool release{false};
        bool has_fx{false};
        char fx[64]{};
        bool stop_fx{false};
    };
    xrCriticalSection m_world_weather_cs;
    SWorldWeatherRequest m_world_weather_req;
    void ApplyWorldWeatherRequests();

    bool m_world_time_host_owned{false};
    struct SWorldTimeRequest
    {
        bool has_time{false};
        u64 time{0};
        u32 advance_ms{0};
        bool has_factor{false};
        float factor{0.f};
    };
    xrCriticalSection m_world_time_cs;
    SWorldTimeRequest m_world_time_req;
    void ApplyWorldTimeRequests();

protected:
    virtual IClient* new_client(SClientConnectData* cl_data);

    virtual void Check_BuildVersion_Success(IClient* CL);

    void SendConnectionData(IClient* CL);

public:
    // constr / destr
    xrServer();
    virtual ~xrServer();

    // extended functionality
    virtual u32 OnMessage(NET_Packet& P, ClientID sender); // Non-Zero means broadcasting with "flags" as returned
    virtual void OnCL_Connected(IClient* CL);
    virtual void SendTo_LL(ClientID ID, void* data, u32 size, u32 dwFlags = DPNSEND_GUARANTEED, u32 dwTimeout = 0);

    virtual IClient* client_Create(); // create client info
    virtual IClient* client_Find_Get(ClientID ID); // Find earlier disconnected client
    virtual void client_Destroy(IClient* C); // destroy client info

    // utilities
    CSE_Abstract* entity_Create(LPCSTR name);
    void entity_Destroy(CSE_Abstract*& P);

    xrClientData* ID_to_client(ClientID ID, bool ScanAll = false) { return (xrClientData*)(IPureServer::ID_to_client(ID, ScanAll)); }
    CSE_Abstract* ID_to_entity(u16 ID);

    // ⭐Everything the server holds, read-only, for the census verbs.
    //
    // ID_to_entity answers "is this id here" and that is not the same question
    // as "what is here": an inventory of the world cannot be taken one guessed
    // id at a time. This map is also the only place some objects live -- an
    // entity whose can_save() is false never enters the A-Life registry -- which
    // is why SVWorld::Find consults both, and why a census must too.
    // Const, because the world is still the server's to change.
    const xrS_entities& Entities() const { return entities; }

    // main
    virtual EConnect Connect(shared_str& session_name);
    virtual void Disconnect();
    virtual void Update();

    void SLS_Default();
    void SLS_Clear();
    void SLS_Save(IWriter& fs);

    shared_str level_name(const shared_str& server_options) const;

    void create_direct_client();

public:
    xr_string ent_name_safe(u16 eid);
};
