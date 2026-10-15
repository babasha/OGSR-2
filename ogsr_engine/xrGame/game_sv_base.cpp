#include "StdAfx.h"
#include "xrServer.h"
#include "LevelGameDef.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "script_engine.h"
#include "Level.h"
#include "xrServer.h"
#include "ai_space.h"
#include "game_sv_event_queue.h"
#include "../xr_3da/XR_IOConsole.h"
#include "../xr_3da/xr_ioc_cmd.h"
#include "string_table.h"
#include "../xr_3da/IGame_Persistent.h" // the world's weather travels in the update header
#include "../xr_3da/Environment.h"
#include "sv_telemetry.h"
#include "sv_health.h"
#include "sv_npc_health.h"
#include "sv_damage_model.h"
#include "sv_world_lookup.h"
#include "shot_latency.h"
#include "sv_accuracy.h"
#include "sv_hit_trail.h"
#include "../xrCore/load_prof.h" // [net prof]: the parts of a server-side hit

BOOL net_sv_control_hit = FALSE;

// 0 = trust the client, 1 = measure and log, 2 = clamp the packet.
//
// Defaults to 1, deliberately, and for the reason sv_move_validate does: a
// bound switched to enforcing before anybody has watched it run refuses honest
// play, and the first symptom is a player reporting something that reads as a
// netcode bug. Console: sv_hit_validate.
int g_sv_hit_validate = 1;

// How far over the cap a shot may sit before it counts. The cap is arithmetic
// the server does itself, so this covers float and wire-format noise, not
// disagreement -- anything meaningfully above the cap is a claim the content
// cannot produce.
float g_sv_hit_slack = 0.02f;

// How much of a shot's power a creature the server simulates actually takes.
//
// ⭐⭐Why this exists as a SERVER dial rather than a content edit: the number
// that makes one rifle round lethal anywhere above the elbow is
// `multiply_hit_from_actor = 1.7`, declared in defines.ltx and applied by the
// mod's own Lua (xr_motivator.script) on the victim's binder. Editing it would
// mean shipping changed gamedata to every client -- which the content
// fingerprint (sv_content_check) then refuses -- and would move NPC-versus-NPC
// combat with it, because that callback does not check who fired despite its
// name. Scaling the relayed power moves exactly what was asked and nothing else,
// takes effect on every client at once, and cannot be edited by a player.
//
// 1.0 = the content as written. Console: sv_npc_damage_scale.
float g_sv_npc_damage_scale = 1.f;

// Whether this world streams its live weather values to clients, or only the
// channels a host explicitly holds. Defined with the rest of the server-side world
// control in xrServer.cpp; console: sv_weather_stream.
extern int g_sv_weather_stream;

game_PlayerState* game_sv_GameState::get_it(u32 it)
{
    xrClientData* C = (xrClientData*)m_server->client_Get(it);
    if (0 == C)
        return 0;
    else
        return C->ps;
}

game_PlayerState* game_sv_GameState::get_id(ClientID id)
{
    xrClientData* C = (xrClientData*)m_server->ID_to_client(id);
    if (0 == C)
        return NULL;
    else
        return C->ps;
}

ClientID game_sv_GameState::get_it_2_id(u32 it)
{
    xrClientData* C = (xrClientData*)m_server->client_Get(it);
    if (0 == C)
    {
        ClientID clientID;
        clientID.set(0);
        return clientID;
    }
    else
        return C->ID;
}

u32 game_sv_GameState::get_players_count() { return m_server->client_Count(); }

u16 game_sv_GameState::get_id_2_eid(ClientID id)
{
    xrClientData* C = (xrClientData*)m_server->ID_to_client(id);
    if (0 == C)
        return 0xffff;
    CSE_Abstract* E = C->owner;
    if (0 == E)
        return 0xffff;
    return E->ID;
}

void* game_sv_GameState::get_client(u16 id) // if exist
{
    CSE_Abstract* entity = get_entity_from_eid(id);
    if (entity && entity->owner && entity->owner->ps && entity->owner->ps->GameID == id)
        return entity->owner;
    //-------------------------------------------------
    u32 cnt = get_players_count();
    for (u32 it = 0; it < cnt; ++it)
    {
        xrClientData* C = (xrClientData*)m_server->client_Get(it);
        if (!C || !C->ps)
            continue;
        //		game_PlayerState*	ps	=	get_it	(it);
        if (C->ps->HasOldID(id))
            return C;
    };
    //-------------------------------------------------
    return NULL;
}

CSE_Abstract* game_sv_GameState::get_entity_from_eid(u16 id) { return m_server->ID_to_entity(id); }

void game_sv_GameState::signal_Syncronize() { sv_force_sync = TRUE; }

// Network
void game_sv_GameState::net_Export_State(NET_Packet& P, ClientID to)
{
    // Generic
    P.w_clientID(to);
    P.w_s32(m_type);
    P.w_u16(m_phase);
    P.w_s32(m_round);
    P.w_u32(m_start_time);
    P.w_u8(u8(0 & 0xff));
    P.w_u8(u8(net_sv_control_hit));
    P.w_u8(u8(0));

    // Players
    u32 p_count = 0;
    for (u32 p_it = 0; p_it < get_players_count(); ++p_it)
    {
        xrClientData* C = (xrClientData*)m_server->client_Get(p_it);
        if (!C->net_Ready || (C->ps->IsSkip() && C->ID != to))
            continue;
        p_count++;
    };

    P.w_u16(u16(p_count));
    game_PlayerState* Base = get_id(to);
    for (u32 p_it = 0; p_it < get_players_count(); ++p_it)
    {
        string64 p_name;
        xrClientData* C = (xrClientData*)m_server->client_Get(p_it);
        game_PlayerState* A = get_it(p_it);
        if (!C->net_Ready || (A->IsSkip() && C->ID != to))
            continue;

        CSE_Abstract* C_e = C->owner;
        if (0 == C_e)
            strcpy_s(p_name, "Unknown");
        else
        {
            strcpy_s(p_name, C_e->name_replace());
        }

        A->setName(p_name);
        u16 tmp_flags = A->flags__;

        if (Base == A)
            A->setFlag(GAME_PLAYER_FLAG_LOCAL);

        ClientID clientID = get_it_2_id(p_it);
        P.w_clientID(clientID);
        A->net_Export(P, TRUE);

        A->flags__ = tmp_flags;
    }

    net_Export_GameTime(P);
}

void game_sv_GameState::net_Export_Update(NET_Packet& P, ClientID id_to, ClientID id)
{
    game_PlayerState* A = get_id(id);
    if (A)
    {
        u16 bk_flags = A->flags__;
        if (id == id_to)
        {
            A->setFlag(GAME_PLAYER_FLAG_LOCAL);
        }

        P.w_clientID(id);
        A->net_Export(P);
        A->flags__ = bk_flags;
    };
};

void game_sv_GameState::net_Export_GameTime(NET_Packet& P)
{
    // Syncronize GameTime
    P.w_u64(GetGameTime());
    P.w_float(GetGameTimeFactor());
    // Syncronize EnvironmentGameTime
    P.w_u64(GetEnvironmentGameTime());
    P.w_float(GetEnvironmentGameTimeFactor());

    // Syncronize the weather this world is running -- sent as STATE, not as an event,
    // so a client that drops a packet, joins late or reconnects converges on the next
    // header instead of living under a sky of its own.
    u32 weather_crc = 0;
    u8 channel_mask = 0;
    float channel_value[eWeatherChannelCount] = {};
    u32 wfx_crc = 0;
    float wfx_left = 0.f;

    if (g_pGamePersistent)
    {
        CEnvironment& env = g_pGamePersistent->Environment();
        weather_crc = env.GetWeatherCrc();

        // A running weather effect goes with its remaining time so a client that
        // joins mid-storm starts it in progress instead of from the top.
        wfx_crc = env.GetWeatherFXCrc();
        if (wfx_crc)
        {
            channel_mask |= u8(eWeatherMask_FXPlaying);
            wfx_left = env.GetWeatherFXTimeLeft();
        }

        for (int ch = 0; ch < eWeatherChannelCount; ++ch)
        {
            // Streaming (the default): ship what this world is ACTUALLY doing, not
            // just what an operator typed. That is what makes the server the single
            // source of truth -- a weather effect started by this world's own script,
            // an environment modifier, any future mechanic that moves the weather,
            // all reach the client without needing a channel of their own.
            // Not streaming: ship only the channels a host explicitly holds, and let
            // the client mix the rest itself from the shared cycle and clock (five
            // bytes a tick instead of twenty-one).
            const bool send = g_sv_weather_stream ? true : env.HasWeatherOverride((EWeatherChannel)ch);
            if (!send)
                continue;
            channel_mask |= u8(1 << ch);
            channel_value[ch] = env.GetWeatherLive((EWeatherChannel)ch);
        }
    }

    P.w_u32(weather_crc);
    P.w_u8(channel_mask);
    for (int ch = 0; ch < eWeatherChannelCount; ++ch)
        if (channel_mask & (1 << ch))
            P.w_float(channel_value[ch]);
    if (channel_mask & eWeatherMask_FXPlaying)
    {
        P.w_u32(wfx_crc);
        P.w_float(wfx_left);
    }
};

void game_sv_GameState::OnPlayerConnect(ClientID /**id_who/**/) { signal_Syncronize(); }

void game_sv_GameState::Create(shared_str& options)
{
}

void game_sv_GameState::u_EventGen(NET_Packet& P, u16 type, u16 dest)
{
    P.w_begin(M_EVENT);
    P.w_u32(Level().timeServer()); // Device.TimerAsync());
    P.w_u16(type);
    P.w_u16(dest);
}

void game_sv_GameState::u_EventSend(NET_Packet& P, u32 dwFlags) { m_server->SendBroadcast(BroadcastCID, P, dwFlags); }

void game_sv_GameState::Update()
{
    for (u32 it = 0; it < m_server->client_Count(); ++it)
    {
        xrClientData* C = (xrClientData*)m_server->client_Get(it);
        C->ps->ping = u16(0);
    }
}

game_sv_GameState::game_sv_GameState()
{
    VERIFY(g_pGameLevel);
    m_server = Level().Server;
    m_event_queue = xr_new<GameEventQueue>();
}

game_sv_GameState::~game_sv_GameState()
{
    xr_delete(m_event_queue);
}

bool game_sv_GameState::change_level(NET_Packet& net_packet, ClientID sender) { return (true); }

void game_sv_GameState::save_game(NET_Packet& net_packet, ClientID sender) {}

bool game_sv_GameState::load_game(NET_Packet& net_packet, ClientID sender) { return (true); }

void game_sv_GameState::switch_distance(NET_Packet& net_packet, ClientID sender) {}

void game_sv_GameState::OnEvent(NET_Packet& tNetPacket, u16 type, u32 time, ClientID sender)
{
    switch (type)
    {
    case GAME_EVENT_PLAYER_CONNECTED: {
        ClientID ID;
        tNetPacket.r_clientID(ID);
        OnPlayerConnect(ID);
    }
    break;

    case GAME_EVENT_ON_HIT: {
        // Every hit passes through here, including the ones that do not kill
        // and the ones from anomalies and monsters. The stamp goes first,
        // before any gate, so the queue leg is measured for hits that end up
        // dropped too -- those are exactly the ones worth timing.
        const u32 hit_start = tNetPacket.r_tell();
        ShotLatency::OnApplied(tNetPacket);

        // ⭐⭐WHO ASKED FOR THIS HIT -- the client that sent the packet, not the
        // attacker id inside it. The two are different questions and only the
        // first one is trustworthy: the attacker id is payload a patched client
        // writes, while the sender is the transport's own answer.
        //
        // ⚠It was not available here until 30-07-2026: Process_event queued the
        // event with a default-constructed ClientID, so every hit reached this
        // switch anonymous, and "is this the server's own simulation or somebody
        // at a keyboard" -- the question that decides whether a number needs
        // capping at all -- could not be asked.
        //
        // ⚠An unknown sender counts as the server's own. That is the safe way
        // round for a diagnostic (it never invents a cheater out of a lookup
        // miss) and the unsafe way round for a gate, so it is counted out loud
        // rather than assumed away.
        const xrClientData* hit_from = m_server->ID_to_client(sender);
        const bool hit_from_network = hit_from && !hit_from->flags.bLocal;
        if (!hit_from)
        {
            static u32 s_anon = 0;
            if ((++s_anon % 101) == 1)
                Msg("! [sv_hit] hit arrived from client %u, which is not in the server's client list (%u so far) -- treating it as the server's own", sender.value(), s_anon);
        }

        tNetPacket.r_u16();
        u16 id_src = tNetPacket.r_u16();
        CSE_Abstract* e_src = get_entity_from_eid(id_src);

        // ⭐Counted BEFORE any gate, and before the drop below. A hit that the
        // server discards still happened; a hit that gets clamped still landed.
        // The question this answers -- "did they miss?" -- is decided as much by
        // the shots that produced no hit at all as by the ones that did, so the
        // two halves have to be counted on the same terms. See sv_accuracy.h.
        {
            const u32 saved = tNetPacket.r_tell();
            tNetPacket.r_seek(hit_start);
            u16 a_victim = 0xffff, a_att = 0xffff, a_wpn = 0xffff, a_bone = 0;
            Fvector a_dir;
            float a_pow = 0.f;
            tNetPacket.r_u16(a_victim);
            tNetPacket.r_u16(a_att);
            tNetPacket.r_u16(a_wpn);
            tNetPacket.r_dir(a_dir);
            tNetPacket.r_float(a_pow);
            tNetPacket.r_u16(a_bone);
            { LoadProf::Scope _lp("sv.hit.accuracy"); SVAccuracy::Hit(a_att, a_victim, a_bone, a_pow, nullptr == e_src); }
            tNetPacket.r_seek(saved);
        }

        if (!e_src) // added by andy because of Phantom does not have server entity
        {
            // Note this looks up the ONLINE entity map only, so a hit from an
            // attacker the server has offline is dropped here and reaches no
            // client at all -- measured, not assumed. Worth a line: silently
            // discarded damage is indistinguishable from damage that was
            // delivered and ignored.
            static u32 s_dropped = 0;
            if ((++s_dropped % 31) == 1)
                Msg("! [sv_hit] dropping a hit: attacker %u is not an online server entity (dropped %u so far)", id_src, s_dropped);

            // Still recorded, and recorded AS dropped. A hit nobody received is
            // history too, but a record that does not say so reads as damage
            // that landed -- the exact confusion this flag removes.
            const u32 saved = tNetPacket.r_tell();
            tNetPacket.r_seek(hit_start);
            SVTelemetry::OnHit(tNetPacket, true);
            tNetPacket.r_seek(hit_start);
            SVHitTrail::Note(tNetPacket, true, hit_from_network, sender.value());
            tNetPacket.r_seek(saved);
            ShotLatency::OnRecorded(false);
            break;
        }

        // ⭐⭐⭐The power in this packet was computed on the SHOOTER'S machine.
        // Everything downstream -- the victim's own object, the ledgers, the
        // telemetry, every other client -- applies whatever number is sitting
        // here, so this is the one place it can still be questioned, and the
        // one place a correction reaches all of them at once.
        //
        // The packet is REWRITTEN rather than merely refused: a refusal here
        // would make the shot vanish for the shooter while their client still
        // played the impact, and a hit that lands for one side only is the
        // exact class of defect this project has spent days on. Corrected, the
        // client's claim becomes a request that the server answers.
        //
        // ⭐The same rewrite is also where the server's own damage dial lives.
        // It has to be: the mod applies its multiplier from a Lua callback on
        // the victim's binder, downstream of everything the engine exposes, so
        // there is no earlier hook and no later one that reaches all clients.
        // Scaling the number in the packet is the only lever that moves the
        // whole world at once -- and it is a SERVER setting, so a player editing
        // their own gamedata cannot move it.
        if (g_sv_hit_validate || !fsimilar(g_sv_npc_damage_scale, 1.f))
        {
            const u32 saved = tNetPacket.r_tell();
            tNetPacket.r_seek(hit_start);

            u16 h_victim = 0xffff, h_att = 0xffff, h_wpn = 0xffff, h_bone = 0, h_aim = 0, h_type = 0;
            Fvector h_dir, h_in_bone;
            float h_power = 0.f, h_impulse = 0.f;
            tNetPacket.r_u16(h_victim);
            tNetPacket.r_u16(h_att);
            tNetPacket.r_u16(h_wpn);
            tNetPacket.r_dir(h_dir);
            const u32 power_at = tNetPacket.r_tell(); // where the float lives, read off the parse rather than counted by hand
            tNetPacket.r_float(h_power);
            tNetPacket.r_u16(h_bone);
            tNetPacket.r_vec3(h_in_bone);
            tNetPacket.r_float(h_impulse);
            tNetPacket.r_u16(h_aim);
            tNetPacket.r_u16(h_type);

            // ⭐⭐⭐EVERY kind of damage, not only bullets (30-07-2026).
            //
            // This branch used to read `if (eHitTypeFireWound == h_type)`, and
            // everything else -- knife, grenade, anomaly, fall -- went past
            // untouched. The census (sv_hit_types) was built to wait for the
            // first non-bullet claim from a network client before capping any
            // of it. That wait asked the wrong question: what a client HAS
            // claimed does not bound what it MAY claim, and what content
            // allows is readable now. See sv_damage_model.h for which lines
            // each producer is read from, and which sources are deliberately
            // left with no ceiling at all.
            {
                float out_power = h_power;

                CSE_Abstract* e_wpn = SVWorld::Find(h_wpn);
                const SVDamage::ShotCap cap = SVDamage::PowerCapFor(e_wpn, h_type);

                // Holding the weapon you claim to have fired. Cheap, and it
                // closes the obvious way around the cap -- name somebody's
                // rocket launcher and inherit its ceiling.
                //
                // ⚠Only for a source that IS held. An explosion and an anomaly
                // act on their own in the world and have no parent by
                // definition, so asking the question of them would answer "not
                // held" every time and exempt exactly the kinds this change was
                // made to reach.
                const bool held = e_wpn && (e_wpn->ID_Parent == h_att);
                const bool holder_ok = !cap.carried || held;

                const bool over = cap.known && holder_ok && h_power > cap.power + g_sv_hit_slack;

                if (g_sv_hit_validate && over)
                {
                    static u32 s_over = 0;
                    ++s_over;
                    // ⚠The cap is enforcement and the dial below is balance, and
                    // they are applied in that order on purpose: bound the claim
                    // to what the content can produce FIRST, then tune what the
                    // world does with it. The other order would let a cheat's
                    // inflated number survive as "scaled", which is still a
                    // cheat, only quieter.
                    if (g_sv_hit_validate >= 2)
                        out_power = cap.power;

                    // The round, when there is one, and whether it is the one
                    // actually chambered -- a cap taken over every round the
                    // weapon can take is looser, and a report that hides which
                    // it was invites the wrong argument about the number.
                    string256 from;
                    if (cap.ammo.size())
                        xr_sprintf(from, " with '%s'%s", cap.ammo.c_str(), cap.exact_ammo ? "" : " (strongest round it takes)");
                    else
                        from[0] = 0;

                    Msg("! [sv_hit] client %u claimed %.4f %s from '%s'%s (cap %.4f, %s) -- %s; victim %u bone %u; %u so far", h_att, h_power,
                        ALife::g_cafHitType2String(ALife::EHitType(h_type)), cap.weapon.c_str(), from, cap.power, cap.how, (g_sv_hit_validate >= 2) ? "CLAMPED" : "logged only",
                        h_victim, h_bone, s_over);
                }
                else if (g_sv_hit_validate && cap.known && !holder_ok)
                {
                    // Not clamped: an NPC's weapon legitimately has no parent
                    // link the moment it changes hands, and refusing those
                    // would drop honest damage. Counted so the noise floor is
                    // known before anything is built on top of it.
                    static u32 s_unheld = 0;
                    if ((++s_unheld % 31) == 1)
                        Msg("! [sv_hit] hit from weapon %u ('%s') whose parent is %u, not the claimed shooter %u (%u so far)", h_wpn, cap.weapon.c_str(), e_wpn->ID_Parent, h_att,
                            s_unheld);
                }

                // ⭐The census keeps the verdict, not just the claim: how much
                // of the wire the ceiling actually reaches is the number that
                // decides whether sv_hit_validate 2 is safe to switch on, and
                // it cannot be read off the claims alone.
                if (g_sv_hit_validate)
                    SVHitTrail::NoteCap(h_type, hit_from_network, cap.known && holder_ok, over);

                // ⚠Only for a creature the server simulates. A player's body is
                // not tuned from here: how lethal players are to each other is a
                // different question from how tough the world's inhabitants are,
                // and one dial for both would silently answer the one nobody
                // asked.
                //
                // ⚠It now scales EVERY type, where it used to move bullets
                // only. That was an artifact of living inside the fire_wound
                // branch, not a decision: a dial called "how much of a hit a
                // server creature takes" that a grenade ignores describes
                // nothing anybody asked for.
                if (!fsimilar(g_sv_npc_damage_scale, 1.f) && SVNpcHealth::IsServerCreature(h_victim))
                    out_power *= g_sv_npc_damage_scale;

                if (!fsimilar(out_power, h_power))
                    tNetPacket.w_seek(power_at, &out_power, sizeof(out_power));
            }

            tNetPacket.r_seek(saved);
        }

        // Only now is the hit going anywhere. The health ledger must charge for
        // relayed hits ONLY: a hit dropped above costs the victim nothing, so
        // charging for it would push the ledger below the truth -- and in
        // enforcing mode a ledger below the truth kills honest players. The
        // telemetry record moved down here with it, and for the same reason:
        // it used to be written above the gate, so it claimed hits the server
        // then threw away.
        {
            const u32 saved = tNetPacket.r_tell();
            tNetPacket.r_seek(hit_start);
            { LoadProf::Scope _lp("sv.hit.telemetry"); SVTelemetry::OnHit(tNetPacket, false); }
            tNetPacket.r_seek(hit_start);
            { LoadProf::Scope _lp("sv.hit.health"); SVHealth::OnHit(tNetPacket); }
            // ...and the creature ledger, for a victim no client controls. The
            // two never see the same victim: one bounds what a client claims,
            // the other IS the health. See sv_npc_health.h.
            tNetPacket.r_seek(hit_start);
            { LoadProf::Scope _lp("sv.hit.npc_health"); SVNpcHealth::OnHit(tNetPacket); }
            // ⭐The tape, written AFTER the rewrite above so it holds the power
            // that actually went out, not the one that was claimed. What was
            // claimed is already in the sv_hit line the cap prints; what was
            // carried is what a death has to be explained by.
            tNetPacket.r_seek(hit_start);
            { LoadProf::Scope _lp("sv.hit.trail"); SVHitTrail::Note(tNetPacket, false, hit_from_network, sender.value()); }
            tNetPacket.r_seek(saved);
        }
        ShotLatency::OnRecorded(true);

        { LoadProf::Scope _lp("sv.hit.broadcast"); m_server->SendBroadcast(BroadcastCID, tNetPacket, net_flags(TRUE, TRUE)); }
    }
    break;
    case GAME_EVENT_CREATE_CLIENT: {
        IClient* CL = (IClient*)m_server->ID_to_client(sender);
        if (CL == NULL)
        {
            break;
        }

        CL->flags.bConnected = TRUE;
        m_server->AttachNewClient(CL);
    }
    break;
    case GAME_EVENT_PLAYER_AUTH: {
        IClient* CL = m_server->ID_to_client(sender);
        m_server->OnBuildVersionRespond(CL, tNetPacket);
    }
    break;
    default: {
        string16 tmp;
        R_ASSERT(0, "Game Event not implemented!!!", _itoa(type, tmp, 10));
    }
    }
}

void game_sv_GameState::OnSwitchPhase(u32 old_phase, u32 new_phase)
{
    inherited::OnSwitchPhase(old_phase, new_phase);
    signal_Syncronize();
}

void game_sv_GameState::AddDelayedEvent(NET_Packet& tNetPacket, u16 type, u32 time, ClientID sender)
{
    //	OnEvent(tNetPacket,type,time,sender);
    m_event_queue->Create(tNetPacket, type, time, sender);
}

void game_sv_GameState::ProcessDelayedEvent()
{
    GameEvent* ge = NULL;
    while ((ge = m_event_queue->Retreive()) != 0)
    {
        OnEvent(ge->P, ge->type, ge->time, ge->sender);
        m_event_queue->Release();
    }
}

void game_sv_GameState::teleport_object(NET_Packet& packet, u16 id) {}

void game_sv_GameState::add_restriction(NET_Packet& packet, u16 id) {}

void game_sv_GameState::remove_restriction(NET_Packet& packet, u16 id) {}

void game_sv_GameState::remove_all_restrictions(NET_Packet& packet, u16 id) {}

shared_str game_sv_GameState::level_name(const shared_str& server_options) const
{
    string64 l_name = "";
    VERIFY(_GetItemCount(*server_options, '/'));
    return (_GetItem(*server_options, 0, l_name, '/'));
}

void game_sv_GameState::on_death(CSE_Abstract* e_dest, CSE_Abstract* e_src)
{
    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(e_dest);
    if (!creature)
        return;

    VERIFY(creature->m_killer_id == ALife::_OBJECT_ID(-1));
    creature->m_killer_id = e_src->ID;
}

