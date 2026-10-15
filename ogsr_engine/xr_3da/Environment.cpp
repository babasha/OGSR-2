#include "stdafx.h"

#include "Render.h"

#include "Environment.h"
#include "xr_efflensflare.h"
#include "Rain.h"
#include "thunderbolt.h"
#include "xrHemisphere.h"
#include "perlin.h"

#include "xr_input.h"

//#include "resourcemanager.h"

#include "IGame_Level.h"

#include "../xrCore/xrCore.h"
#include "../xrCore/crc32.h" // weather cycles are addressed by name digest over the wire

#include "../Include/xrRender/EnvironmentRender.h"
#include "../Include/xrRender/LensFlareRender.h"
#include "../Include/xrRender/RainRender.h"
#include "../Include/xrRender/ThunderboltRender.h"

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
// rs_vis_distance — multiplier on the RENDER far plane (far_plane_render). Default 8,
// i.e. ~3.2 km of drawn world against the weather's own ~400 m.
// 📏Measured 25-08 from a rooftop in Pripyat: x8 costs NOTHING (78 -> 82 fps; the frame
// even got lighter, distant geometry being cheaper than near), and the horizon band of
// the image changed by a mean of 9-13/255 while the sky stayed identical at 0.2 — that
// is the haze lifting off the distant treeline, which is the whole point.
// ⚠ It does NOT reveal more world: +6 trees drawn. Past the level's own extent there is
// simply no content — that part is an asset problem, not a renderer one.
// ⚠ There is no reversed-Z in this engine (LESS_OR_EQUAL, near 0.2 m, D32F), so very
// large values will eventually z-fight. 8 was clean where it was measured; the cvar
// allows up to 20, and that is where to look first if distant surfaces start shimmering.
ENGINE_API float psVisDistance = 8.0f;
static const float MAX_NOISE_FREQ = 0.03f;

// real WEATHER->WFX transition time
#define WFX_TRANS_TIME 5.f

const float MAX_DIST_FACTOR = 0.95f;

//////////////////////////////////////////////////////////////////////////
// environment
CEnvironment::CEnvironment()
{
    USED_COP_WEATHER = FS.path_exist(fsgame::game_weathers);

    m_last_weather_shift = 0;
    bNeed_re_create_env = FALSE;
    b_wfx = false;
    Current[0] = nullptr;
    Current[1] = nullptr;
    CurrentWeather = nullptr;
    CurrentWeatherName = nullptr;
    eff_Rain = nullptr;
    eff_LensFlare = nullptr;
    eff_Thunderbolt = nullptr;
    OnDeviceCreate();

    fGameTime = 0.f;
    fTimeFactor = 12.f;

    wind_strength_factor = 0.f;
    wind_gust_factor = 0.f;
	wetness_factor = 0.f;
    wind_blast_strength = 0.f;
    wind_blast_direction.set(1.f, 0.f, 0.f);

    wind_blast_strength_start_value = 0.f;
    wind_blast_strength_stop_value = 0.f;

    wind_anim = {0.0f, 0.0f};

    // fill clouds hemi verts & faces
    const Fvector* verts;
    CloudsVerts.resize(xrHemisphereVertices(2, verts));
    CopyMemory(&CloudsVerts.front(), verts, CloudsVerts.size() * sizeof(Fvector));
    const u16* indices;
    CloudsIndices.resize(xrHemisphereIndices(2, indices));
    CopyMemory(&CloudsIndices.front(), indices, CloudsIndices.size() * sizeof(u16));

    // perlin noise
    PerlinNoise1D = xr_new<CPerlinNoise1D>(Random.randI(0, 0xFFFF));
    PerlinNoise1D->SetOctaves(2);
    PerlinNoise1D->SetAmplitude(0.66666f);

    if (USED_COP_WEATHER)
    {
        string_path file_name;
        m_ambients_config = xr_new<CInifile>(FS.update_path(file_name, fsgame::game_configs, "environment\\ambients.ltx"), TRUE, TRUE, FALSE);
        m_sound_channels_config = xr_new<CInifile>(FS.update_path(file_name, fsgame::game_configs, "environment\\sound_channels.ltx"), TRUE, TRUE, FALSE);
        m_effects_config = xr_new<CInifile>(FS.update_path(file_name, fsgame::game_configs, "environment\\effects.ltx"), TRUE, TRUE, FALSE);
        m_suns_config = xr_new<CInifile>(FS.update_path(file_name, fsgame::game_configs, "environment\\suns.ltx"), TRUE, TRUE, FALSE);
        m_thunderbolt_collections_config = xr_new<CInifile>(FS.update_path(file_name, fsgame::game_configs, "environment\\thunderbolt_collections.ltx"), TRUE, TRUE, FALSE);
        m_thunderbolts_config = xr_new<CInifile>(FS.update_path(file_name, fsgame::game_configs, "environment\\thunderbolts.ltx"), TRUE, TRUE, FALSE);

        m_static_sun_movement = FS.exist(fsgame::game_configs, "environment\\sun_positions.ltx");
        if (m_static_sun_movement)
            m_sun_pos_config = xr_new<CInifile>(FS.update_path(file_name, fsgame::game_configs, "environment\\sun_positions.ltx"), TRUE, TRUE, FALSE);

        CInifile* config = xr_new<CInifile>(FS.update_path(file_name, fsgame::game_configs, "environment\\environment.ltx"), TRUE, TRUE, FALSE);
        m_dynamic_sun_movement = READ_IF_EXISTS(config, r_bool, "environment", "dynamic_sun_movement", true);

        // params
        p_var_alt = deg2rad(config->r_float("environment", "altitude"));
        p_var_long = deg2rad(config->r_float("environment", "delta_longitude"));
        p_min_dist = _min(.95f, config->r_float("environment", "min_dist_factor"));
        p_tilt = deg2rad(config->r_float("environment", "tilt"));
        p_second_prop = config->r_float("environment", "second_propability");
        clamp(p_second_prop, 0.f, 1.f);
        p_sky_color = config->r_float("environment", "sky_color");
        p_sun_color = config->r_float("environment", "sun_color");
        p_fog_color = config->r_float("environment", "fog_color");

        xr_delete(config);
    }
    else
    {
        // params
        p_var_alt = deg2rad(pSettings->r_float("thunderbolt_common", "altitude"));
        p_var_long = deg2rad(pSettings->r_float("thunderbolt_common", "delta_longitude"));
        p_min_dist = _min(.95f, pSettings->r_float("thunderbolt_common", "min_dist_factor"));
        p_tilt = deg2rad(pSettings->r_float("thunderbolt_common", "tilt"));
        p_second_prop = pSettings->r_float("thunderbolt_common", "second_propability");
        clamp(p_second_prop, 0.f, 1.f);
        p_sky_color = pSettings->r_float("thunderbolt_common", "sky_color");
        p_sun_color = pSettings->r_float("thunderbolt_common", "sun_color");
        p_fog_color = pSettings->r_float("thunderbolt_common", "fog_color");
    }
}

CEnvironment::~CEnvironment()
{
    xr_delete(PerlinNoise1D);

    OnDeviceDestroy();

    if (m_ambients_config)
        CInifile::Destroy(m_ambients_config);

    if (m_sound_channels_config)
        CInifile::Destroy(m_sound_channels_config);

    if (m_effects_config)
        CInifile::Destroy(m_effects_config);

    if (m_suns_config)
        CInifile::Destroy(m_suns_config);

    if (m_thunderbolt_collections_config)
        CInifile::Destroy(m_thunderbolt_collections_config);

    if (m_thunderbolts_config)
        CInifile::Destroy(m_thunderbolts_config);

    if (m_sun_pos_config)
        CInifile::Destroy(m_sun_pos_config);

    destroy_mixer();
}

void CEnvironment::Invalidate()
{
    b_wfx = false;
    Current[0] = nullptr;
    Current[1] = nullptr;
    if (eff_LensFlare)
        eff_LensFlare->Invalidate();
    if (eff_Rain)
        eff_Rain->InvalidateState();
}

float CEnvironment::TimeDiff(float prev, float cur)
{
    if (prev > cur)
        return (DAY_LENGTH - prev) + cur;
    else
        return cur - prev;
}

float CEnvironment::TimeWeight(float val, float min_t, float max_t)
{
    float weight = 0.f;
    float length = TimeDiff(min_t, max_t);
    if (!fis_zero(length, EPS))
    {
        if (min_t > max_t)
        {
            if ((val >= min_t) || (val <= max_t))
                weight = TimeDiff(min_t, val) / length;
        }
        else
        {
            if ((val >= min_t) && (val <= max_t))
                weight = TimeDiff(min_t, val) / length;
        }
        clamp(weight, 0.f, 1.f);
    }
    return weight;
}

void CEnvironment::SetGameTime(float game_time, float time_factor)
{
    if (b_wfx)
    {
        // ⚠️TimeDiff reads prev > cur as "the clock wrapped past midnight" and returns
        // nearly a whole day. That is right for a wrap and catastrophic for a small
        // step BACKWARDS: it takes ~86400 seconds off the effect's countdown, the
        // mixer then ends the effect for being over, and nothing says why.
        // A single-process game never stepped back, so this never showed. A network
        // client takes its time of day from the update header, where ordinary sync
        // jitter moves it back by milliseconds constantly -- there, a storm died on
        // its first jitter and (with the server still reporting one) restarted, over
        // and over. Small step back = no elapsed time; a real wrap is a whole day
        // away from that.
        const float back = fGameTime - game_time;
        const bool jitter_back = (back > 0.f) && (back < 60.f);
        if (!jitter_back)
            wfx_time -= TimeDiff(fGameTime, game_time);
    }

    fGameTime = game_time;
    fTimeFactor = time_factor;
}

float CEnvironment::NormalizeTime(float tm)
{
    if (tm < 0.f)
        return tm + DAY_LENGTH;
    else if (tm > DAY_LENGTH)
        return tm - DAY_LENGTH;
    else
        return tm;
}

// The gate compares by VALUE, not by interned pointer: names arrive from Lua, from the
// console and from our own members, and a rejected weather change must not depend on
// which of those minted the string.
static inline bool weather_name_eq(const shared_str& a, const shared_str& b)
{
    return a.size() && b.size() && 0 == xr_strcmp(a.c_str(), b.c_str());
}

void CEnvironment::SetWeatherAuthoritative(shared_str name)
{
    // Take the lock BEFORE applying, and scope the pass to the name being applied.
    // SetWeather(forced) -> SelectEnvs -> OnChangeCurrentWeather -> Lua forecast ->
    // set_weather(its own cycle) all runs synchronously inside the call below. The old
    // boolean pass let that answer through: CurrentWeather (the cycle vector every later
    // hour-section step reads) went back to the script's cycle while Current[0]/Current[1]
    // stayed the descriptors we had just selected. The sky looked forced — and quietly
    // reverted at the next section, with nothing in the log to say so.
    // Never take the lock on a name this level does not have: SetWeather would refuse
    // it, and the hold would then refuse everything else too - a frozen sky with no
    // weather running at all. The console validates first; a server call may not.
    if (WeatherCycles.find(name) == WeatherCycles.end())
    {
        Msg("! weather: cannot lock to '%s' - no such cycle on this level", name.c_str());
        return;
    }
    m_weather_locked = true;
    m_weather_lock_name = name;
    SetWeather(name, true);
    Msg("~ weather: LOCKED to '%s' - script forecast can no longer change it", name.c_str());
}

LPCSTR CEnvironment::WeatherChannelName(EWeatherChannel ch)
{
    switch (ch)
    {
    case eWeatherRain: return "rain";
    case eWeatherFog: return "fog";
    case eWeatherWind: return "wind";
    case eWeatherThunder: return "thunder";
    default: return "?";
    }
}

void CEnvironment::SetWeatherOverride(EWeatherChannel ch, float value)
{
    if (ch < 0 || ch >= eWeatherChannelCount)
        return;

    // Only the 0..1 channels are clamped here. Wind is in cycle units and lightning
    // is a period in seconds -- clamping those to 1 would silently gut them.
    if (ch == eWeatherRain || ch == eWeatherFog)
        clamp(value, 0.f, 1.f);
    else if (value < 0.f)
        value = 0.f;

    SAuthWeatherChannel& c = m_auth_weather[ch];
    const bool was = c.held;
    const float prev = c.value;
    c.held = true;
    c.value = value;

    // Logging discipline, learned the hard way: on a streaming client this runs
    // twenty times a second, wind DRIFTS continuously (it is a lerp across the day),
    // and every Msg costs a disk flush (log.cpp). A plain "value changed" test filled
    // 1249 lines and 1.7 MB in one two-minute run -- a stall every 50 ms, not a log.
    // So: always announce taking the channel over, then only substantial moves, and
    // never more than one line per channel per few seconds.
    static u32 s_last_log[eWeatherChannelCount] = {};
    if (!was)
    {
        s_last_log[ch] = Device.dwTimeGlobal;
        Msg("~ weather: %s is now authoritative (%.2f)", WeatherChannelName(ch), value);
        return;
    }

    // Relative, because the channels live on different scales: 0.05 is a third of a
    // fog density and a rounding error in wind's hundreds.
    const bool moved = _abs(value - prev) > 0.05f * _max(1.f, _abs(value));
    if (moved && (Device.dwTimeGlobal - s_last_log[ch]) > 3000)
    {
        s_last_log[ch] = Device.dwTimeGlobal;
        Msg("~ weather: authoritative %s=%.2f", WeatherChannelName(ch), value);
    }
}

void CEnvironment::ClearWeatherOverride(EWeatherChannel ch)
{
    if (ch < 0 || ch >= eWeatherChannelCount)
        return;
    if (!m_auth_weather[ch].held)
        return;
    m_auth_weather[ch].held = false;
    m_auth_weather[ch].value = 0.f;
    Msg("~ weather: %s released to the cycle", WeatherChannelName(ch));
}

float CEnvironment::GetWeatherLive(EWeatherChannel ch) const
{
    if (ch < 0 || ch >= eWeatherChannelCount)
        return 0.f;
    if (m_auth_weather[ch].held)
        return m_auth_weather[ch].value;

    // CurrentEnv is degenerate until the first mix (level still loading).
    if (!CurrentEnv)
        return 0.f;

    switch (ch)
    {
    case eWeatherRain: return CurrentEnv->rain_density;
    case eWeatherFog: return CurrentEnv->fog_density;
    case eWeatherWind: return CurrentEnv->wind_velocity;
    case eWeatherThunder: return CurrentEnv->bolt_period;
    default: return 0.f;
    }
}

u32 CEnvironment::WeatherNameCrc(LPCSTR name)
{
    // 0 is reserved on the wire for "the host is not dictating a cycle", so a real
    // name must never digest to it. Cheap and stable: bump into 1.
    if (!name || !name[0])
        return 0;
    const u32 crc = crc32_fast(name, xr_strlen(name));
    return crc ? crc : 1u;
}

// Deliberately the CYCLE name (CurrentName), not the playing one: a weather effect
// overwrites CurrentWeatherName while it runs, and a client resolving that digest
// would look for an effect among the cycles and find nothing.
u32 CEnvironment::GetWeatherCrc() const { return WeatherNameCrc(CurrentName.c_str()); }

shared_str CEnvironment::FindWeatherByCrc(u32 crc) const
{
    if (!crc)
        return shared_str{};

    for (const auto& it : WeatherCycles)
        if (WeatherNameCrc(it.first.c_str()) == crc)
            return it.first;

    return shared_str{};
}

u32 CEnvironment::GetWeatherFXCrc() const { return b_wfx ? WeatherNameCrc(CurrentWeatherName.c_str()) : 0u; }

shared_str CEnvironment::FindWeatherFXByCrc(u32 crc) const
{
    if (!crc)
        return shared_str{};

    for (const auto& it : WeatherFXs)
        if (WeatherNameCrc(it.first.c_str()) == crc)
            return it.first;

    return shared_str{};
}

bool CEnvironment::StartWeatherFXAuthoritative(shared_str name, float time_left)
{
    // Guarded rather than trusting: SetWeatherFX asserts on an unknown name, and this
    // is reached from the network, where the name arrives as a digest resolved against
    // possibly-drifted content.
    if (WeatherFXs.find(name) == WeatherFXs.end())
    {
        Msg("! weather: cannot start effect '%s' - this level has no such effect", name.c_str());
        return false;
    }

    if (!SetWeatherFX(name))
        return false; // already playing one

    // SetWeatherFX always starts from the beginning; move the end to where the server
    // says it is so both sides stop together. Clamped to something positive: a
    // non-positive wfx_time ends the effect on the very next mix, which for a client
    // that joined a second before the storm ends is the right answer anyway.
    if (time_left > 0.f)
        wfx_time = time_left;

    Msg("~ weather: effect '%s' started from the server (%.1fs left)", name.c_str(), wfx_time);
    return true;
}

void CEnvironment::LockWeather(bool on)
{
    m_weather_locked = on;
    // Hold the cycle that is actually RUNNING. The engine re-asserts it by name from
    // inside SelectEnvs and when a weather effect ends; pinning the name here is what
    // keeps those alive while everything else is refused.
    m_weather_lock_name = on ? CurrentWeatherName : shared_str{};
    Msg("~ weather: lock %s (current '%s')", on ? "ON" : "OFF", CurrentWeatherName.c_str());
}

void CEnvironment::SetWeather(shared_str name, bool forced)
{
    //.	static BOOL bAlready = FALSE;
    //.	if(bAlready)	return;
    // Authoritative hold: the Lua forecast re-asserts itself on every weather change,
    // so without this a console/server request survives about a second. Refusals are
    // logged once per requested name — silence here is what made the frozen-sky bug
    // so hard to see. The held name always passes (that is how the holder writes, and
    // how the engine re-arms the same cycle from inside SelectEnvs); every other name
    // is refused no matter who asks or when — including the forecast answering our own
    // apply from inside it.
    if (m_weather_locked && !weather_name_eq(name, m_weather_lock_name))
    {
        static shared_str s_lastRefused;
        if (!weather_name_eq(name, s_lastRefused))
        {
            s_lastRefused = name;
            Msg("~ weather: request '%s' IGNORED (locked on '%s')", name.c_str(), CurrentWeatherName.c_str());
        }
        return;
    }
    if (!name.empty())
    {
        //.		bAlready = TRUE;
        EnvsMapIt it = WeatherCycles.find(name);
        if (it == WeatherCycles.end())
        {
            // A rejected name leaves the PREVIOUS cycle running, silently — the request
            // simply does not happen. That is indistinguishable from "the weather is
            // stuck" unless you know which names actually exist, so print them once.
            Msg("! Invalid weather name: %s", name.c_str());
            static bool s_listed = false;
            if (!s_listed)
            {
                s_listed = true;
                string4096 line;
                xr_strcpy(line, "! available weather cycles:");
                for (const auto& c : WeatherCycles)
                {
                    if (xr_strlen(line) + xr_strlen(c.first.c_str()) + 2 >= sizeof(line) - 8)
                    {
                        Msg("%s ...", line);
                        xr_strcpy(line, "!  ...");
                    }
                    xr_strcat(line, " ");
                    xr_strcat(line, c.first.c_str());
                }
                Msg("%s", line);
            }
            return;
        }
        R_ASSERT(it != WeatherCycles.end(), "Invalid weather name.", *name);
        CurrentName = it->first;
        if (forced)
        {
            Invalidate();
        }
        if (!b_wfx)
        {
            PrevWeatherName = (forced || CurrentWeatherName.empty()) ? it->first : CurrentWeatherName;
            CurrentWeatherName = it->first;

            CurrentWeather = &it->second;
        }
        if (forced)
        {
            SelectEnvs(fGameTime);
        }
    }
    else
    {
        FATAL("! Empty weather name");
    }
}

bool CEnvironment::SetWeatherFX(shared_str name)
{
    if (b_wfx)
        return false;
    if (!name.empty())
    {
        EnvsMapIt it = WeatherFXs.find(name);
        R_ASSERT(it != WeatherFXs.end(), "Invalid weather effect name.", *name);
        EnvVec* PrevWeather = CurrentWeather;
        VERIFY(PrevWeather);
        CurrentWeather = &it->second;
        CurrentWeatherName = it->first;

        float rewind_tm = WFX_TRANS_TIME * fTimeFactor;
        float start_tm = fGameTime + rewind_tm;
        float current_length;
        if (Current[0]->exec_time > Current[1]->exec_time)
        {
            current_length = (DAY_LENGTH - Current[0]->exec_time) + Current[1]->exec_time;
        }
        else
        {
            current_length = Current[1]->exec_time - Current[0]->exec_time;
        }

        std::sort(CurrentWeather->begin(), CurrentWeather->end(), sort_env_etl_pred);
        CEnvDescriptor* C0 = CurrentWeather->at(0);
        CEnvDescriptor* C1 = CurrentWeather->at(1);
        CEnvDescriptor* CE = CurrentWeather->at(CurrentWeather->size() - 2);
        CEnvDescriptor* CT = CurrentWeather->at(CurrentWeather->size() - 1);
        C0->copy(*Current[0]);
        C0->exec_time = NormalizeTime(fGameTime - ((rewind_tm / (Current[1]->exec_time - fGameTime)) * current_length - rewind_tm));
        C1->copy(*Current[1]);
        C1->exec_time = NormalizeTime(start_tm);
        for (EnvIt t_it = CurrentWeather->begin() + 2; t_it != CurrentWeather->end() - 1; ++t_it)
        {
            (*t_it)->exec_time = NormalizeTime(start_tm + (*t_it)->exec_time_loaded);
        }
        SelectEnvs(PrevWeather, wfx_end_desc[0], wfx_end_desc[1], CE->exec_time);
        CT->copy(*wfx_end_desc[0]);
        CT->exec_time = NormalizeTime(CE->exec_time + rewind_tm);
        wfx_time = TimeDiff(fGameTime, CT->exec_time);
        b_wfx = true;

        // sort wfx envs
        std::sort(CurrentWeather->begin(), CurrentWeather->end(), sort_env_pred);

        Current[0] = C0;
        Current[1] = C1;
    }
    else
    {
        FATAL("! Empty weather effect name");
    }
    return true;
}

bool CEnvironment::SetWeatherFXFromTime(shared_str name, float time)
{
    float _fGameTime = fGameTime;
    fGameTime = NormalizeTime(fGameTime - time);
    bool res = SetWeatherFX(name);
    fGameTime = _fGameTime;
    if (!res)
        return false;
    wfx_time -= time;
    return true;
}

void CEnvironment::StopWeatherFX()
{
    VERIFY(CurrentCycleName.size());
    b_wfx = false;
    SetWeather(CurrentName, false);
    Current[0] = wfx_end_desc[0];
    Current[1] = wfx_end_desc[1];
}

IC bool lb_env_pred(const CEnvDescriptor* x, float val) { return x->exec_time < val; }

void CEnvironment::SelectEnv(EnvVec* envs, CEnvDescriptor*& e, float gt)
{
    EnvIt env = std::lower_bound(envs->begin(), envs->end(), gt, lb_env_pred);
    if (env == envs->end())
    {
        e = envs->front();
    }
    else
    {
        e = *env;
    }
    e->on_prepare();
}

void CEnvironment::SelectEnvs(EnvVec* envs, CEnvDescriptor*& e0, CEnvDescriptor*& e1, float gt)
{
    EnvIt env = std::lower_bound(envs->begin(), envs->end(), gt, lb_env_pred);
    if (env == envs->end())
    {
        e0 = *(envs->end() - 1);
        e1 = envs->front();
    }
    else
    {
        e1 = *env;
        if (env == envs->begin())
            e0 = *(envs->end() - 1);
        else
            e0 = *(env - 1);
    }

    e0->on_prepare();
    e1->on_prepare();
}

void CEnvironment::SelectEnvs(float gt)
{
    VERIFY(CurrentWeather);
    if ((Current[0] == Current[1]) && (Current[0] == nullptr))
    {
        VERIFY(!bWFX);
        // first or forced start
        SelectEnvs(CurrentWeather, Current[0], Current[1], gt);
        m_last_weather_shift = Device.dwFrame;
        if (g_pGameLevel) // null in the -vk_editor viewport (no level) — the notify is level-only
            g_pGameLevel->OnChangeCurrentWeather(Current[0]->m_identifier.c_str());
    }
    else
    {
        bool bSelect;

        if (Current[0]->exec_time > gt && Current[1]->exec_time > gt)
        {
            SetWeather(CurrentWeatherName, true);
            return;
        }
        else if (Current[0]->exec_time > Current[1]->exec_time)
        {
            // terminator
            bSelect = (gt > Current[1]->exec_time) && (gt < Current[0]->exec_time);
        }
        else
        {
            bSelect = (gt > Current[1]->exec_time);
        }

        if (bSelect)
        {
            if (Current[0])
            {
                Current[0]->on_unload();
            }

            Current[0] = Current[1];
            SelectEnv(CurrentWeather, Current[1], gt);
            m_last_weather_shift = Device.dwFrame;
            if (g_pGameLevel) // null in the -vk_editor viewport (no level)
                g_pGameLevel->OnChangeCurrentWeather(Current[0]->m_identifier.c_str());
        }
    }
}

void CEnvironment::lerp(float& current_weight)
{
    if (b_wfx && (wfx_time <= 0.f))
        StopWeatherFX();

    SelectEnvs(fGameTime);
    VERIFY(Current[0] && Current[1]);

    current_weight = TimeWeight(fGameTime, Current[0]->exec_time, Current[1]->exec_time);

    // modifiers
    CEnvModifier EM;
    EM.far_plane = 0;
    EM.fog_color.set(0, 0, 0);
    EM.fog_density = 0;
    EM.ambient.set(0, 0, 0);
    EM.sky_color.set(0, 0, 0);
    EM.hemi_color.set(0, 0, 0);
    EM.use_flags.zero();

    Fvector view = Device.vCameraPosition;
    float mpower = 0;
    for (auto& Modifier : Modifiers)
        mpower += EM.sum(Modifier, view);

    extern bool s_ScriptNoMixer;

    if (s_ScriptNoMixer)
        current_weight = 0;

    // final lerp
    CurrentEnv->lerp(this, *Current[0], *Current[1], current_weight, EM, mpower);
}

void CEnvironment::OnFrame()
{
    // The -vk_editor viewport drives the environment with NO level loaded: run as
    // long as a weather cycle is selected (Current[0]/[1]/CurrentEnv are valid).
    // Everything below uses CurrentEnv + eff_* (built at load), not the level.
    if (!g_pGameLevel && !CurrentWeather)
        return;

    ZoneScoped;

    wind_anim_old = wind_anim;

    // Limit min at 200 to avoid slow-mo at extremly low speed.
    float WindVel = std::max(CurrentEnv->wind_velocity, 200.f) * 0.001f;
    float WindDir = -CurrentEnv->wind_direction + PI_DIV_2;
    Fvector2 WDir = {_cos(WindDir), _sin(WindDir)};
    wind_anim.x += WindVel * WDir.x * Device.fTimeDelta;
    wind_anim.y += WindVel * WDir.y * Device.fTimeDelta;
    wind_anim.z += clampr(WindVel * 1.33f, 0.0f, 1.0f) * Device.fTimeDelta;

    float current_weight;
    lerp(current_weight);

    bool dyn_sun = false;

    //	Igor. Dynamic sun position.
    if (USED_COP_WEATHER)
    {
        if (m_dynamic_sun_movement)
            dyn_sun = true;
    }
    else
    {
        if (Core.Features.test(xrCore::Feature::dynamic_sun_movement))
            dyn_sun = true;
    }

    if (dyn_sun)
        calculate_dynamic_sun_dir();
    else if (m_static_sun_movement)
        calculate_config_sun_dir();

    PerlinNoise1D->SetFrequency(wind_gust_factor * MAX_NOISE_FREQ);
    wind_strength_factor = clampr(PerlinNoise1D->GetContinious(Device.fTimeGlobal) + 0.5f, 0.f, 1.f);

    shared_str l_id = (current_weight < 0.5f) ? Current[0]->lens_flare_id : Current[1]->lens_flare_id;
    eff_LensFlare->OnFrame(l_id);

    shared_str t_id = (current_weight < 0.5f) ? Current[0]->tb_id : Current[1]->tb_id;
    // Authoritative "no lightning" (sv_thunder 0). It has to be done by withholding
    // the bolt ID: the effect enables itself on the ID and reads a zero period as
    // "strike now", so zeroing the period alone would give a permanent strobe --
    // the exact opposite of what the operator asked for.
    if (HasWeatherOverride(eWeatherThunder) && GetWeatherOverride(eWeatherThunder) <= EPS_S)
        t_id = shared_str{};
    eff_Thunderbolt->OnFrame(t_id, CurrentEnv->bolt_period, CurrentEnv->bolt_duration);

    eff_Rain->OnFrame();

    m_pRender->OnFrame(*this);
}

bool CEnvironment::IsThunderboltActive() const
{
    return eff_Thunderbolt && eff_Thunderbolt->IsActive();
}

const Fvector& CEnvironment::ThunderboltFlash() const
{
    static const Fvector s_none{0.f, 0.f, 0.f};
    return (eff_Thunderbolt && eff_Thunderbolt->IsActive()) ? eff_Thunderbolt->Flash() : s_none;
}

void CEnvironment::calculate_config_sun_dir() const
{
    float current_time = fGameTime / (DAY_LENGTH / 24);
    int weather_time = iFloor(current_time);
    float s_weight = current_time - weather_time;

    float real_sun_alt, real_sun_long;
    float s_alt = sun_hp[weather_time].x;
    float s_long = sun_hp[weather_time].y;

    if (s_weight > 0)
    {
        int next_hour = weather_time == 23 ? 0 : weather_time + 1;

        float s_alt2 = sun_hp[next_hour].x;
        float s_long2 = sun_hp[next_hour].y;

        real_sun_alt = _lerp(s_alt, s_alt2, s_weight);
        real_sun_long = _lerp(s_long, s_long2, s_weight);
    }
    else
    {
        real_sun_alt = s_alt;
        real_sun_long = s_long;
    }

    R_ASSERT(_valid(real_sun_alt));
    R_ASSERT(_valid(real_sun_long));
    CurrentEnv->sun_dir.setHP(deg2rad(real_sun_alt), deg2rad(real_sun_long));
    R_ASSERT(_valid(CurrentEnv->sun_dir));
}

void CEnvironment::calculate_dynamic_sun_dir() const
{
    float g = (360.0f / 365.25f) * (180.0f + fGameTime / DAY_LENGTH);

    g = deg2rad(g);

    //	Declination
    float D = 0.396372f - 22.91327f * _cos(g) + 4.02543f * _sin(g) - 0.387205f * _cos(2 * g) + 0.051967f * _sin(2 * g) - 0.154527f * _cos(3 * g) + 0.084798f * _sin(3 * g);

    //	Now calculate the time correction for solar angle:
    float TC = 0.004297f + 0.107029f * _cos(g) - 1.837877f * _sin(g) - 0.837378f * _cos(2 * g) - 2.340475f * _sin(2 * g);

    //	IN degrees
    float Longitude = -30.4f;

    float SHA = (fGameTime / (DAY_LENGTH / 24) - 12) * 15 + Longitude + TC;

    //	Need this to correctly determine SHA sign
    if (SHA > 180)
        SHA -= 360;
    if (SHA < -180)
        SHA += 360;

    //	IN degrees
    float const Latitude = 50.27f;
    float const LatitudeR = deg2rad(Latitude);

    //	Now we can calculate the Sun Zenith Angle (SZA):
    float cosSZA = _sin(LatitudeR) * _sin(deg2rad(D)) + _cos(LatitudeR) * _cos(deg2rad(D)) * _cos(deg2rad(SHA));

    clamp(cosSZA, -1.0f, 1.0f);

    float SZA = acosf(cosSZA);
    float SEA = PI / 2 - SZA;

    //	To finish we will calculate the Azimuth Angle (AZ):
    float cosAZ = 0.f;
    float const sin_SZA = _sin(SZA);
    float const cos_Latitude = _cos(LatitudeR);
    float const sin_SZA_X_cos_Latitude = sin_SZA * cos_Latitude;
    if (!fis_zero(sin_SZA_X_cos_Latitude))
        cosAZ = (_sin(deg2rad(D)) - _sin(LatitudeR) * _cos(SZA)) / sin_SZA_X_cos_Latitude;

    clamp(cosAZ, -1.0f, 1.0f);
    float AZ = acosf(cosAZ) + PI; // AVO: sun direction fix

    const Fvector2 minAngle = Fvector2().set(deg2rad(1.0f), deg2rad(3.0f));

    if (SEA < minAngle.x)
        SEA = minAngle.x;

    float fSunBlend = (SEA - minAngle.x) / (minAngle.y - minAngle.x);
    clamp(fSunBlend, 0.0f, 1.0f);

    SEA = -SEA;

    if (SHA < 0)
        AZ = 2 * PI - AZ;

    R_ASSERT(_valid(AZ));
    R_ASSERT(_valid(SEA));
    CurrentEnv->sun_dir.setHP(AZ, SEA);
    R_ASSERT(_valid(CurrentEnv->sun_dir));

    CurrentEnv->sun_color.mul(fSunBlend);
}

void CEnvironment::create_mixer()
{
    VERIFY(!CurrentEnv);
    CurrentEnv = xr_new<CEnvDescriptorMixer>("00:00:00");
}

void CEnvironment::destroy_mixer() { xr_delete(CurrentEnv); }

SThunderboltDesc* CEnvironment::thunderbolt_description(CInifile& config, shared_str const& section)
{
    SThunderboltDesc* result = xr_new<SThunderboltDesc>();
    result->load(config, section);
    return (result);
}

SThunderboltCollection* CEnvironment::thunderbolt_collection(CInifile* pIni, CInifile* thunderbolts, LPCSTR section)
{
    SThunderboltCollection* result = xr_new<SThunderboltCollection>();
    result->load(pIni, thunderbolts, section);
    return (result);
}

SThunderboltDesc* CEnvironment::thunderbolt_description_shoc(CInifile* config, shared_str const& section)
{
    SThunderboltDesc* result = xr_new<SThunderboltDesc>();
    result->load_shoc(config, section);
    return (result);
}

SThunderboltCollection* CEnvironment::thunderbolt_collection_shoc(CInifile* pIni, LPCSTR section)
{
    SThunderboltCollection* result = xr_new<SThunderboltCollection>();
    result->load_shoc(pIni, section);
    return (result);
}

SThunderboltCollection* CEnvironment::thunderbolt_collection(xr_vector<SThunderboltCollection*>& collection, shared_str const& id)
{
    typedef xr_vector<SThunderboltCollection*> Container;
    Container::iterator i = collection.begin();
    Container::iterator e = collection.end();
    for (; i != e; ++i)
        if ((*i)->section == id)
            return (*i);

    NODEFAULT;
}

CLensFlareDescriptor* CEnvironment::add_flare(xr_vector<CLensFlareDescriptor*>& collection, shared_str const& id)
{
    for (auto* it : collection)
        if (it->section == id)
            return it;

    CLensFlareDescriptor* result = xr_new<CLensFlareDescriptor>();

    if (USED_COP_WEATHER)
        result->load(m_suns_config, id.c_str());
    else
        result->load(pSettings, id.c_str());

    collection.push_back(result);
    return result;
}

void CEnvironment::SetWeatherNext(shared_str name)
{
    // Gated by the same lock: the forecast steers the sky through Current[1] as much
    // as through the current cycle, so leaving this open would let a locked weather
    // drift into the next one anyway.
    if (m_weather_locked && !weather_name_eq(name, m_weather_lock_name))
    {
        static shared_str s_lastRefusedNext;
        if (!weather_name_eq(name, s_lastRefusedNext))
        {
            s_lastRefusedNext = name;
            Msg("~ weather: next '%s' IGNORED (locked on '%s')", name.c_str(), CurrentWeatherName.c_str());
        }
        return;
    }
    ASSERT_FMT(name.size(), "empty weather name");
    EnvsMapIt it = WeatherCycles.find(name);
    if (it == WeatherCycles.end())
    {
        Msg("! [%s]: Invalid weather name: %s", __FUNCTION__, name.c_str());
        return;
    }
    EnvVec* NextWeather = &it->second;
    SelectEnv(NextWeather, Current[1], fGameTime);
}
