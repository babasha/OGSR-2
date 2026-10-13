#include "StdAfx.h"
#include "HUDManager.h"
#include "game_cl_base.h"
#include "Level.h"
#include "GamePersistent.h"
#include "UIGameCustom.h"
#include "script_engine.h"
#include "xr_level_controller.h"
#include "ui/UIMainIngameWnd.h"
#include "ui/UIGameTutorial.h"
#include "ui/UIMessagesWindow.h"
#include "string_table.h"
#include "date_time.h" // split_time, for the world-clock diagnostic

game_cl_GameState::game_cl_GameState()
{
    local_player = 0;
    m_game_type_name = 0;

    shedule.t_min = 5;
    shedule.t_max = 20;
    shedule_register();

    m_u16VotingEnabled = 0;
    m_bServerControlHits = true;
    m_need_to_update = true;
}

game_cl_GameState::~game_cl_GameState()
{
    PLAYERS_MAP_IT I = players.begin();
    for (; I != players.end(); ++I)
        xr_delete(I->second);
    players.clear();

    shedule_unregister(true);
}

// Applies the weather the server is running to this client's environment. Reached
// from the update header at 20 Hz, so every path here has to be idempotent and
// quiet: a value that has not changed must not log, allocate or re-select a cycle.
static void apply_server_weather(u32 weather_crc, u8 channel_mask, const float* channel_value, u32 wfx_crc, float wfx_left)
{
    if (!g_pGamePersistent || !g_pGameLevel)
        return;

    CEnvironment& env = g_pGamePersistent->Environment();

    // Weather effects first, because starting or stopping one re-selects the cycle
    // underneath it. An effect replaces the running weather, so while one plays on
    // both sides the cycle below is left alone -- re-applying it would cut the storm
    // short every tick.
    const bool server_fx = (channel_mask & eWeatherMask_FXPlaying) != 0;
    const u32 local_fx = env.GetWeatherFXCrc();
    if (server_fx && local_fx == wfx_crc)
    {
        // Already running the server's effect: keep taking its clock from the server
        // instead of counting down locally. See SetWeatherFXTimeLeft for why a local
        // countdown made the storm strobe.
        env.SetWeatherFXTimeLeft(wfx_left);
    }
    else if (server_fx)
    {
        const shared_str fx = env.FindWeatherFXByCrc(wfx_crc);
        if (fx.size())
        {
            if (local_fx)
                env.StopWeatherFX(); // a different effect is running -- swap to the server's
            env.StartWeatherFXAuthoritative(fx, wfx_left);
        }
        else
        {
            static u32 s_lastUnknownFX = 0;
            if (s_lastUnknownFX != wfx_crc)
            {
                s_lastUnknownFX = wfx_crc;
                Msg("! [weather] the server runs a weather effect this installation does not have (crc=0x%08x)", wfx_crc);
            }
        }
    }
    else if (!server_fx && local_fx)
    {
        // The server's storm ended; end ours rather than waiting for a local timer
        // that was only ever an estimate of the server's.
        env.StopWeatherFX();
        Msg("~ [weather] server's weather effect ended");
    }

    // The cycle. A zero digest means the host is not dictating one (no world loaded
    // on its side yet) -- leave whatever this level came up with alone. Skipped while
    // an effect plays: the effect IS the running weather then, and re-applying the
    // cycle under it would cut the storm short. The channels below still apply --
    // rain and wind matter most during a storm.
    if (!server_fx && weather_crc && weather_crc != env.GetWeatherCrc())
    {
        const shared_str name = env.FindWeatherByCrc(weather_crc);
        if (name.size())
        {
            Msg("~ [weather] cl following the server's cycle '%s'", name.c_str());
            // Authoritative, i.e. locked: this level's own forecast script would
            // otherwise re-assert its choice within about a second and the client
            // would drift back to a private sky.
            env.SetWeatherAuthoritative(name);
        }
        else
        {
            // The server runs a cycle this installation does not have -- content
            // drift, which the connect-time fingerprint is there to catch. Log once
            // per digest; this arrives twenty times a second.
            static u32 s_lastUnknownCrc = 0;
            if (s_lastUnknownCrc != weather_crc)
            {
                s_lastUnknownCrc = weather_crc;
                Msg("! [weather] the server runs a weather cycle this installation does not have (crc=0x%08x)", weather_crc);
            }
        }
    }

    // The channels. A bit that is set pins this client to the server's value; a bit
    // that is clear hands the channel back to the cycle, which the client mixes
    // itself from the same content at the same shared time. Both calls are
    // idempotent and only speak up on a real change (see SetWeatherOverride), which
    // matters because this runs twenty times a second.
    for (int ch = 0; ch < eWeatherChannelCount; ++ch)
    {
        if (channel_mask & (1 << ch))
            env.SetWeatherOverride((EWeatherChannel)ch, channel_value[ch]);
        else
            env.ClearWeatherOverride((EWeatherChannel)ch);
    }
}

void game_cl_GameState::net_import_GameTime(NET_Packet& P)
{
    // The four fields are ALWAYS consumed, even when this one update is meant to
    // be ignored: skipping the read left the packet cursor short of the payload
    // that follows the game-state header, so everything after it (the per-entity
    // update tail a network client parses) was read from the wrong offset.
    u64 GameTime;
    P.r_u64(GameTime);
    float TimeFactor;
    P.r_float(TimeFactor);

    u64 GameEnvironmentTime;
    P.r_u64(GameEnvironmentTime);
    float EnvironmentTimeFactor;
    P.r_float(EnvironmentTimeFactor);

    // The mask is always read, and its bits decide how many floats follow it. Walking
    // the channels in the export's order is not a style choice: get it wrong and the
    // packet cursor is off for the whole per-entity tail that comes after this header.
    u32 weather_crc = 0;
    P.r_u32(weather_crc);
    u8 channel_mask = 0;
    P.r_u8(channel_mask);
    float channel_value[eWeatherChannelCount] = {};
    for (int ch = 0; ch < eWeatherChannelCount; ++ch)
        if (channel_mask & (1 << ch))
            P.r_float(channel_value[ch]);
    u32 wfx_crc = 0;
    float wfx_left = 0.f;
    if (channel_mask & eWeatherMask_FXPlaying)
    {
        P.r_u32(wfx_crc);
        P.r_float(wfx_left);
    }

    // The sky is applied BEFORE the ignore-gate below, and only on a network
    // client. Before, because what the weather is has to be the server's answer --
    // a client opting out of the game-state update must not thereby opt out of the
    // shared world's rain. Only on a network client, because in a fused session the
    // server IS this process: re-applying its own export would take the
    // authoritative-weather lock in ordinary single player and stop the mod's
    // forecast from ever running again.
    if (!Level().Server)
        apply_server_weather(weather_crc, channel_mask, channel_value, wfx_crc, wfx_left);

    if (!m_need_to_update)
    {
        m_need_to_update = true;
        return;
    }

    Level().SetGameTimeFactor(GameTime, TimeFactor);

    // Diagnostic on the network-client path only: shows that the clock a client
    // renders is the one the host sent, not a local one. Odd modulus -- an even
    // one locks onto a single client when two of them alternate.
    if (!Level().Server)
    {
        static u32 s_time_syncs = 0;
        if (++s_time_syncs % 301 == 1)
        {
            u32 year = 0, month = 0, day = 0, hours = 0, mins = 0, secs = 0, ms = 0;
            split_time(GameTime, year, month, day, hours, mins, secs, ms);
            // Also report what the ENVIRONMENT is drawing from: that is the value
            // the sky, sun and weather blend actually read, one link further down
            // than the clock this packet carries.
            const float env_sec = g_pGamePersistent ? g_pGamePersistent->Environment().GetGameTime() : 0.f;
            Msg("* [worldtime] cl %02u:%02u:%02u rate=%.2f (from server), env=%02u:%02u", hours, mins, secs, TimeFactor, u32(env_sec / 3600.f) % 24,
                (u32(env_sec / 60.f)) % 60);
            // Same idea one link down for the sky: report the cycle and the rain the
            // renderer is actually mixing, not just what the packet said, so a run
            // can be graded on pixels-worth of state rather than on wire traffic.
            if (g_pGamePersistent)
            {
                CEnvironment& env = g_pGamePersistent->Environment();
                Msg("* [weather] cl cycle='%s' rain=%.2f fog=%.2f wind=%.0f thunder=%.1fs  mask=0x%02x (server crc=0x%08x)", env.GetWeather().c_str(),
                    env.GetWeatherLive(eWeatherRain), env.GetWeatherLive(eWeatherFog), env.GetWeatherLive(eWeatherWind), env.GetWeatherLive(eWeatherThunder),
                    channel_mask, weather_crc);
            }
        }
    }
}

void game_cl_GameState::net_import_state(NET_Packet& P)
{
    // Generic
    P.r_clientID(local_svdpnid);
    P.r_s32(m_type);

    u16 ph;
    P.r_u16(ph);

    if (Phase() != ph)
        switch_Phase(ph);

    P.r_s32(m_round);
    P.r_u32(m_start_time);
    m_u16VotingEnabled = u16(P.r_u8());
    m_bServerControlHits = !!P.r_u8();
    P.r_u8();

    // Players
    u16 p_count;
    P.r_u16(p_count);

    PLAYERS_MAP players_new;

    /*
        players.clear	();
    */
    PLAYERS_MAP_IT I;
    for (u16 p_it = 0; p_it < p_count; ++p_it)
    {
        ClientID ID;
        P.r_clientID(ID);

        game_PlayerState* IP;
        I = players.find(ID);
        if (I != players.end())
        {
            IP = I->second;
            IP->net_Import(P);
            // Msg("!![%s] Called net_import - 1", __FUNCTION__);
            players_new.insert(mk_pair(ID, IP));
            players.erase(I);
        }
        else
        {
            IP = createPlayerState();
            IP->net_Import(P);
            // Msg("!![%s] Called net_import - 2", __FUNCTION__);
            players_new.insert(mk_pair(ID, IP));
        }
        if (IP->testFlag(GAME_PLAYER_FLAG_LOCAL))
            local_player = IP;
    }

    I = players.begin();
    for (; I != players.end(); ++I)
        xr_delete(I->second);
    players.clear();

    players = players_new;

    net_import_GameTime(P);
}

void game_cl_GameState::net_import_update(NET_Packet& P)
{
    // Read
    ClientID ID;
    P.r_clientID(ID);

    // Update
    PLAYERS_MAP_IT I = players.find(ID);
    if (players.end() != I)
    {
        game_PlayerState* IP = I->second;
        IP->net_Import(P);
        // Msg("!![%s] Called net_import - 3", __FUNCTION__);
    }
    else
    {
        game_PlayerState* PS = createPlayerState();
        PS->net_Import(P);
        // Msg("!![%s] Called net_import - 4", __FUNCTION__);
        xr_delete(PS);
    }

    // Syncronize GameTime
    net_import_GameTime(P);

    // diag: throttled — how much per-entity payload arrives after the game-state part
    {
        static u32 s_diag_imports = 0;
        if (++s_diag_imports % 128 == 1)
            Msg("* [netupd] cl import: leftover=%u bytes", P.r_elapsed());
    }
}

game_PlayerState* game_cl_GameState::GetPlayerByGameID(u32 GameID)
{
    PLAYERS_MAP_IT I = players.begin();
    PLAYERS_MAP_IT E = players.end();

    for (; I != E; ++I)
    {
        game_PlayerState* P = I->second;
        if (P->GameID == GameID)
            return P;
    };
    return NULL;
};

game_PlayerState* game_cl_GameState::GetPlayerByOrderID(u32 idx)
{
    PLAYERS_MAP_IT I = players.begin();
    std::advance(I, idx);
    game_PlayerState* ps = I->second;
    return ps;
}

ClientID game_cl_GameState::GetClientIDByOrderID(u32 idx)
{
    PLAYERS_MAP_IT I = players.begin();
    std::advance(I, idx);
    return I->first;
}

float game_cl_GameState::shedule_Scale() { return 1.0f; }

void game_cl_GameState::shedule_Update(u32 dt) { ISheduled::shedule_Update(dt); }

void game_cl_GameState::StartStopMenu(CUIDialogWnd* pDialog, bool bDoHideIndicators) { HUD().GetUI()->StartStopMenu(pDialog, bDoHideIndicators); }

void game_cl_GameState::sv_EventSend(NET_Packet& P) { Level().Send(P, net_flags(TRUE, TRUE)); }

bool game_cl_GameState::IR_OnKeyboardPress(int dik)
{
    if (local_player && !local_player->IsSkip())
        return OnKeyboardPress(get_binded_action(dik));
    else
        return false;
}

bool game_cl_GameState::IR_OnKeyboardRelease(int dik)
{
    if (local_player && !local_player->IsSkip())
        return OnKeyboardRelease(get_binded_action(dik));
    else
        return false;
}

bool game_cl_GameState::IR_OnMouseMove(int dx, int dy) { return false; }
bool game_cl_GameState::IR_OnMouseWheel(int direction) { return false; }

void game_cl_GameState::u_EventGen(NET_Packet& P, u16 type, u16 dest)
{
    P.w_begin(M_EVENT);
    P.w_u32(Level().timeServer());
    P.w_u16(type);
    P.w_u16(dest);
}

void game_cl_GameState::u_EventSend(NET_Packet& P) { Level().Send(P, net_flags(TRUE, TRUE)); }

void game_cl_GameState::OnSwitchPhase(u32 old_phase, u32 new_phase)
{
    switch (old_phase)
    {
    case GAME_PHASE_INPROGRESS: {
    }
    break;
    default: {
    }
    break;
    };

    switch (new_phase)
    {
    case GAME_PHASE_INPROGRESS: {
    }
    break;
    default: {
    }
    break;
    }
}

void game_cl_GameState::SendPickUpEvent(u16 ID_who, u16 ID_what)
{
    NET_Packet P;
    u_EventGen(P, GE_OWNERSHIP_TAKE, ID_who);
    P.w_u16(ID_what);
    u_EventSend(P);
};

void game_cl_GameState::set_type_name(LPCSTR s)
{
    m_game_type_name = s;
}

void game_cl_GameState::reset_ui()
{
    auto ui = HUD().GetUI();

    auto uigame = ui->UIGame();

    uigame->reset_ui();

    auto mgw = ui->UIMainIngameWnd;

    mgw->reset_ui();

    if (auto MIR = ui->MainInputReceiver())
        ui->StartStopMenu(MIR, true);
}
