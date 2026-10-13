#pragma once

// refs
class ENGINE_API CInifile;
class ENGINE_API CEnvironment;

// refs - effects
class ENGINE_API CLensFlare;
class ENGINE_API CEffect_Rain;
class ENGINE_API CEffect_Thunderbolt;

class ENGINE_API CPerlinNoise1D;

struct SThunderboltDesc;
struct SThunderboltCollection;
class CLensFlareDescriptor;

#define DAY_LENGTH 86400.f

#include "../Include/xrRender/FactoryPtr.h"
#include "../Include/xrRender/EnvironmentRender.h"

// t-defs
class ENGINE_API CEnvModifier
{
public:
    Fvector3 position;
    float radius;
    float power;

    float far_plane;
    Fvector3 fog_color;
    float fog_density;
    Fvector3 ambient;
    Fvector3 sky_color;
    Fvector3 hemi_color;
    Flags16 use_flags;

    u8 shape_type{};
    Fobb obb{};
    Fvector shape_rotate{}, shape_halfsize{};

    bool load(IReader* fs, u32 version);

    bool loadIni(CInifile& ini, LPCSTR section);
    bool saveIni(CInifile& ini, LPCSTR section);

    float sum(CEnvModifier& _another, Fvector3& view);
};

class ENGINE_API CEnvAmbient
{
public:
    struct SEffect
    {
        shared_str m_load_section;
        u32 life_time;
        ref_sound sound;
        shared_str particles;
        Fvector offset;
        float wind_gust_factor;

        float wind_blast_in_time;
        float wind_blast_out_time;
        float wind_blast_strength;
        Fvector wind_blast_direction;

        ~SEffect() {}
    };

    DEFINE_VECTOR(SEffect*, EffectVec, EffectVecIt);
    struct SSndChannel
    {
        shared_str m_load_section;
        Fvector2 m_sound_dist;
        Ivector4 m_sound_period;

        typedef xr_vector<ref_sound> sounds_type;

        void load(CInifile& config, LPCSTR sect);
        ref_sound& get_rnd_sound() { return sounds()[Random.randI(sounds().size())]; }
        u32 get_rnd_sound_time() { return (m_sound_period.z < m_sound_period.w) ? Random.randI(m_sound_period.z, m_sound_period.w) : 0; }
        u32 get_rnd_sound_first_time() { return (m_sound_period.x < m_sound_period.y) ? Random.randI(m_sound_period.x, m_sound_period.y) : 0; }
        float get_rnd_sound_dist() { return (m_sound_dist.x < m_sound_dist.y) ? Random.randF(m_sound_dist.x, m_sound_dist.y) : 0; }
        ~SSndChannel() {}
        inline sounds_type& sounds() { return m_sounds; }

    protected:
        xr_vector<ref_sound> m_sounds;
    };
    DEFINE_VECTOR(SSndChannel*, SSndChannelVec, SSndChannelVecIt);

protected:
    shared_str m_load_section;

    EffectVec m_effects;
    Ivector2 m_effect_period{};

    SSndChannelVec m_sound_channels;
    shared_str m_ambients_config_filename;

    shared_str section;
    xr_vector<ref_sound> sounds;
    Fvector2 sound_dist{};
    Ivector2 sound_period{};
    Ivector2 effect_period{};

public:
    IC const shared_str& name() { return m_load_section.size() == 0 ? section : m_load_section; }
    IC const shared_str& get_ambients_config_filename() { return m_ambients_config_filename; }

    void load(CInifile& ambients_config, CInifile& sound_channels_config, CInifile& effects_config, const shared_str& section);
    IC SEffect* get_rnd_effect() { return m_effects.empty() ? nullptr : m_effects[Random.randI(m_effects.size())]; }
    IC u32 get_rnd_effect_time() { return Random.randI(m_effect_period.x, m_effect_period.y); }

    SEffect* create_effect(CInifile& config, LPCSTR sect);
    SSndChannel* create_sound_channel(CInifile& config, LPCSTR id);
    ~CEnvAmbient();
    void destroy();
    inline SSndChannelVec& get_snd_channels() { return m_sound_channels; }
    void load_shoc(const shared_str& section);

    IC ref_sound* get_rnd_sound() { return sounds.empty() ? nullptr : &sounds[Random.randI(sounds.size())]; }
    IC u32 get_rnd_sound_time() { return Random.randI(sound_period.x, sound_period.y); }
    IC float get_rnd_sound_dist() { return Random.randF(sound_dist.x, sound_dist.y); }
    IC u32 get_rnd_effect_time_shoc() { return Random.randI(effect_period.x, effect_period.y); }

    inline EffectVec& effects() { return m_effects; }
};

class ENGINE_API CEnvDescriptor
{
public:
    float exec_time;
    float exec_time_loaded;

    shared_str sky_texture_name;
    shared_str sky_texture_env_name;
    shared_str clouds_texture_name;

    /*
    ref_texture			sky_texture		;
    ref_texture			sky_texture_env	;
    ref_texture			clouds_texture	;
    */
    FactoryPtr<IEnvDescriptorRender> m_pDescriptor;

    Fvector4 clouds_color;
    Fvector3 sky_color;
    float sky_rotation;

    float far_plane;
    // RENDER-ONLY far plane = far_plane * rs_vis_distance. It is a separate field on
    // purpose: `far_plane` above is read by GAMEPLAY as well — the actor's visibility
    // frustum (actor_memory.cpp), the crosshair ray (HUDTarget.cpp), lightning placement
    // and lens-flare distance. Scaling that one to see further would also hand the AI a
    // 3 km view cone and make every crosshair ray 3 km long, which is not what a
    // graphics setting should do. Only the projection matrices read this.
    float far_plane_render;

    Fvector3 fog_color;
    float fog_density;
    float fog_distance;

    float rain_density;
    Fvector3 rain_color;

    float bolt_period;
    float bolt_duration;

    float wind_velocity{}, wind_velocity_old{};
    float wind_direction{}, wind_direction_old{};

    Fvector3 ambient;
    Fvector4 hemi_color; // w = R2 correction
    Fvector4 hemi_color_unmodded;
    Fvector3 sun_color;
    Fvector3 sun_dir;

    float m_fSunShaftsIntensity;
    float m_fWaterIntensity;
    float m_fTreeAmplitudeIntensity{}, m_fTreeAmplitudeIntensity_old{};

    float bloom_threshold{};
    float bloom_exposure{};
    float bloom_sky_intensity{};

    //	int					lens_flare_id;
    //	int					tb_id;
    shared_str lens_flare_id;
    shared_str tb_id;

    CEnvAmbient* env_ambient;
    void setEnvAmbient(LPCSTR sect, CEnvironment* parent);

    CEnvDescriptor(shared_str const& identifier = nullptr);

    void load(CEnvironment& environment, CInifile& config);
    void load_shoc(CEnvironment& environment, LPCSTR exec_tm, LPCSTR S);
    void load_shoc(float exec_tm, LPCSTR S, CEnvironment& environment);

    void copy(const CEnvDescriptor& src)
    {
        float tm0 = exec_time;
        float tm1 = exec_time_loaded;
        *this = src;
        exec_time = tm0;
        exec_time_loaded = tm1;
    }

    void on_device_create();
    void on_device_destroy();

    void on_prepare();
    void on_unload();

    shared_str m_identifier;
};

class ENGINE_API CEnvDescriptorMixer : public CEnvDescriptor
{
public:
    /*
    STextureList		sky_r_textures;
    STextureList		sky_r_textures_env;
    STextureList		clouds_r_textures;
    */
    FactoryPtr<IEnvDescriptorMixerRender> m_pDescriptorMixer;
    float weight;

    float fog_near;
    float fog_far;

public:
    CEnvDescriptorMixer(shared_str const& identifier);
    void lerp(CEnvironment* parent, CEnvDescriptor& A, CEnvDescriptor& B, float f, CEnvModifier& M, float m_power);
    void clear();
    void destroy();
};

// The weather values a shared world has to agree on (see the authoritative-weather
// block in CEnvironment). File scope rather than class scope because the network
// layer iterates over them to export/import the set.
enum EWeatherChannel
{
    eWeatherRain = 0, // 0..1, as the cycle's rain_density
    eWeatherFog,      // 0..1, as the cycle's fog_density (fog_near/fog_far follow it)
    eWeatherWind,     // cycle units (wind_velocity), not m/s
    eWeatherThunder,  // seconds between lightning strikes; 0 = no lightning at all
    eWeatherChannelCount
};

class ENGINE_API CEnvironment
{


public:
    DEFINE_VECTOR(CEnvAmbient*, EnvAmbVec, EnvAmbVecIt);
    DEFINE_VECTOR(CEnvDescriptor*, EnvVec, EnvIt);
    DEFINE_MAP_PRED(shared_str, EnvVec, EnvsMap, EnvsMapIt, pred_shared_str);

private:
    // clouds
    FvectorVec CloudsVerts;
    U16Vec CloudsIndices;

    float NormalizeTime(float tm);
    float TimeDiff(float prev, float cur);
    float TimeWeight(float val, float min_t, float max_t);
    void SelectEnvs(EnvVec* envs, CEnvDescriptor*& e0, CEnvDescriptor*& e1, float tm);
    void SelectEnv(EnvVec* envs, CEnvDescriptor*& e, float tm);

    void calculate_dynamic_sun_dir() const;
    void calculate_config_sun_dir() const;

    
    static bool sort_env_pred(const CEnvDescriptor* x, const CEnvDescriptor* y) { return x->exec_time < y->exec_time; }
    static bool sort_env_etl_pred(const CEnvDescriptor* x, const CEnvDescriptor* y) { return x->exec_time_loaded < y->exec_time_loaded; }

    
    FactoryPtr<IEnvironmentRender> m_pRender;

    CPerlinNoise1D* PerlinNoise1D;

    float fGameTime;

    bool b_wfx;

    CEnvDescriptor* wfx_end_desc[2];

    shared_str CurrentName;

    shared_str CurrentWeatherName;
    shared_str PrevWeatherName;

    // Authoritative-weather lock (see SetWeatherAuthoritative). The pass through the
    // gate is scoped to a NAME, not to "we are inside our own call": applying a cycle
    // runs SelectEnvs, which notifies the level, which calls the Lua forecast, which
    // answers with its OWN set_weather — synchronously, from inside our call. A boolean
    // "let the holder's write through" therefore held the door open for the exact
    // request the lock exists to refuse. Name-scoped, the engine's internal re-asserts
    // (SelectEnvs' re-arm, the WFX restore) still pass, and only they do.
    bool       m_weather_locked{};
    shared_str m_weather_lock_name;

    // Authoritative weather overrides (see SetWeatherOverride).
    struct SAuthWeatherChannel
    {
        bool held{};
        float value{};
    };
    SAuthWeatherChannel m_auth_weather[eWeatherChannelCount];

    u32 m_last_weather_shift;

    EnvsMap WeatherFXs;
    xr_vector<CEnvModifier> Modifiers;
    EnvAmbVec Ambients;

public:
    BOOL bNeed_re_create_env;

    float wind_strength_factor;
    float wind_gust_factor;
	float wetness_factor;

    Fvector3 wind_anim{}, wind_anim_old{};

    // wind blast params
    float wind_blast_strength;
    Fvector wind_blast_direction;
    Fquaternion wind_blast_start_time;
    Fquaternion wind_blast_stop_time;
    float wind_blast_strength_start_value;
    float wind_blast_strength_stop_value;
    Fquaternion wind_blast_current;

    // Environments
    CEnvDescriptorMixer* CurrentEnv{};
    CEnvDescriptor* Current[2];

    float wfx_time;

    EnvVec* CurrentWeather;

    EnvsMap WeatherCycles;

    CEffect_Rain* eff_Rain;
    CLensFlare* eff_LensFlare;
    CEffect_Thunderbolt* eff_Thunderbolt;

    bool USED_COP_WEATHER{};

    float fTimeFactor;

    void SelectEnvs(float gt);

    void UpdateAmbient();
    CEnvAmbient* AppendEnvAmb(const shared_str& sect);

    void Invalidate();

public:
    CEnvironment();

    ~CEnvironment();

    void load();
    void unload();

    void mods_load();
    void mods_unload();

    void OnFrame();

    void RenderSky(CBackend& cmd_list);
    void RenderClouds(CBackend& cmd_list);
    void RenderFlares(CBackend& cmd_list);
    void RenderLast(CBackend& cmd_list);

    bool SetWeatherFX(shared_str name);
    bool SetWeatherFXFromTime(shared_str name, float time);
    bool IsWeatherFXPlaying() const { return b_wfx; }
    // True while a thunderbolt is flashing (it transiently overwrites the env
    // sun_dir — shadow cascades must freeze their sun while this is set).
    bool IsThunderboltActive() const;
    // Flash colour a bolt is adding this frame (0 when idle). The bolt ALSO swings
    // CurrentEnv->sun_dir at the strike, which puts a duplicate sun — with god rays
    // — wherever the renderer draws "the sun". A renderer that holds the real sun
    // direction can spend this as sky light instead and get the flash without the
    // second sun. See CEffect_Thunderbolt::Flash.
    const Fvector& ThunderboltFlash() const;
    void StopWeatherFX();

    void SetWeather(shared_str name, bool forced = false);
    shared_str& GetWeather() { return CurrentWeatherName; }
    shared_str& GetPrevWeather() { return PrevWeatherName; }
    void SetWeatherNext(shared_str name);

    // ---- AUTHORITATIVE WEATHER ------------------------------------------------
    // Weather is normally owned by a Lua forecast (dsh_cop_weather), which re-asserts
    // itself on every engine weather change — so a plain SetWeather from outside is
    // undone within a second, and a broken forecast silently freezes the sky (that is
    // how rain went missing here for good). A shared world cannot live with that:
    // every client has to see the SAME sky, decided in one place.
    // Lock = the engine holds the cycle and ignores script traffic. The holder writes
    // through SetWeatherAuthoritative, which is the seam a server later drives.
    void SetWeatherAuthoritative(shared_str name);   // apply + take the lock
    void LockWeather(bool on);                       // hold whatever is current
    bool IsWeatherLocked() const { return m_weather_locked; }

    // ---- AUTHORITATIVE WEATHER CHANNELS ---------------------------------------
    // Rain, fog, wind and lightning are facts about the shared world, not local
    // preferences: they decide how far a player sees, how loud the world is and
    // whether cover works. The running cycle already describes all four per
    // time-of-day, which -- with the shared clock -- is enough for every client to
    // MIX the same weather without any of it being streamed. These overrides are
    // for the other case: the host saying "rain now, this hard", "fog in, thick",
    // independently of what hour the cycle happens to be in, and for pinning a
    // client to the server's values outright (see the streaming mode in xrServer).
    //
    // Applied INSIDE the descriptor mix (CEnvDescriptorMixer::lerp), never after
    // it: several consumed values are DERIVED from these (fog_near/fog_far from
    // fog_density, the wetness/puddle accumulator from rain_density), so an
    // override written after the mix would give rain that falls on dry ground and
    // fog whose near plane belongs to the old density.
    void SetWeatherOverride(EWeatherChannel ch, float value);
    void ClearWeatherOverride(EWeatherChannel ch);
    void ClearWeatherOverrides(); // release all four back to the cycle
    bool HasWeatherOverride(EWeatherChannel ch) const { return m_auth_weather[ch].held; }
    float GetWeatherOverride(EWeatherChannel ch) const { return m_auth_weather[ch].value; }
    // What the world is ACTUALLY doing this frame: the override where one is held,
    // the mixed cycle value otherwise. This is what a host reports and a server
    // ships, so it reads from the same place the renderer does.
    float GetWeatherLive(EWeatherChannel ch) const;
    static LPCSTR WeatherChannelName(EWeatherChannel ch);

    // Weather cycles are addressed over the wire by the CRC of their name: the
    // update header is sent 20 times a second to every client, and a 64-byte name
    // in it would be pure waste, while a 4-byte digest makes the cycle part of the
    // STATE (self-correcting on a dropped packet or a reconnect) rather than an
    // event that can be missed. Both sides run verified-identical content, so the
    // digest resolves back to a name every client has.
    static u32 WeatherNameCrc(LPCSTR name);
    u32 GetWeatherCrc() const;                  // digest of the CYCLE now running
    shared_str FindWeatherByCrc(u32 crc) const; // empty if this level has no such cycle

    // Weather effects (the scripted kind: a storm rolling through, an emission) are a
    // separate table from the cycles and they REPLACE the running weather while they
    // play -- which is why the cycle digest above is taken from the cycle name and
    // not from whatever is currently playing. Synchronising the effect as well as its
    // values is what makes a server-side storm look like a storm on the client: the
    // four streamed channels give it rain, wind and lightning, but the sky, cloud and
    // light colours come from the effect's own descriptors.
    u32 GetWeatherFXCrc() const;                  // 0 when no effect is playing
    shared_str FindWeatherFXByCrc(u32 crc) const; // empty if this level has no such effect
    const EnvsMap& GetWeatherFXs() const { return WeatherFXs; }
    float GetWeatherFXTimeLeft() const { return b_wfx ? wfx_time : 0.f; }
    // Hand the effect's remaining time back to the server's answer.
    // ⚠️Why this exists: the local countdown is wfx_time -= TimeDiff(prev, now), and
    // TimeDiff treats prev > now as "the clock wrapped past midnight" -- it returns
    // nearly a whole DAY_LENGTH. A network client's time of day is pulled straight
    // from the update header, so the tiniest step backwards (ordinary sync jitter)
    // subtracted a day from the countdown, the mixer then ended the effect for being
    // over, and the next header restarted it: a storm that strobed twice a second.
    // A client must not run its own clock for something the server is already timing.
    void SetWeatherFXTimeLeft(float seconds)
    {
        if (b_wfx && seconds > 0.f)
            wfx_time = seconds;
    }
    // Start an effect already in progress, the way a client joining mid-storm has to:
    // plays it from the beginning and then moves its end to match the server's.
    bool StartWeatherFXAuthoritative(shared_str name, float time_left);
    // The cycle last REQUESTED vs the one actually running. They diverge whenever a
    // write is refused or lands only halfway (which is precisely how the re-entrant
    // forecast bug hid: the descriptors were ours, the cycle pointer was the script's),
    // so weather_status prints both.
    const shared_str& GetWeatherRequested() const { return CurrentName; }
    const shared_str& GetWeatherLockName() const { return m_weather_lock_name; }
    const EnvsMap& GetWeatherCycles() const { return WeatherCycles; }
    void ChangeGameTime(float game_time);
    void SetGameTime(float game_time, float time_factor);
    // Seconds into the day the sky/sun/weather blend is currently drawn from --
    // one link past whatever set it, which is what makes it worth reading back.
    float GetGameTime() const { return fGameTime; }

    u32 GetWeatherLastShift() const { return m_last_weather_shift; }

    void OnDeviceCreate();
    void OnDeviceDestroy();

    CEnvDescriptor* getCurrentWeather(size_t idx) const { return CurrentWeather->at(idx); };

    CInifile* m_ambients_config{};
    CInifile* m_sound_channels_config{};
    CInifile* m_effects_config{};
    CInifile* m_suns_config{};
    CInifile* m_thunderbolt_collections_config{};
    CInifile* m_thunderbolts_config{};
    CInifile* m_sun_pos_config{};

    Fvector2 sun_hp[24];

    bool m_dynamic_sun_movement{};
    bool m_static_sun_movement{};

    void StartCalculateAsync();

    std::future<void> awaiter;
    bool async_started{};

private:
    CEnvDescriptor* create_descriptor(shared_str const& identifier, CInifile* config);
    CEnvDescriptor* create_descriptor_shoc(LPCSTR exec_tm, LPCSTR S);

    void load_weathers();
    void load_sun();
    void load_weather_effects();

    void create_mixer();
    void destroy_mixer();

    void load_level_specific_ambients();

    void lerp(float& current_weight);

public:
    SThunderboltDesc* thunderbolt_description(CInifile& config, shared_str const& section);
    SThunderboltCollection* thunderbolt_collection(CInifile* pIni, CInifile* thunderbolts, LPCSTR section);
    SThunderboltDesc* thunderbolt_description_shoc(CInifile* config, shared_str const& section);
    SThunderboltCollection* thunderbolt_collection_shoc(CInifile* pIni, LPCSTR section);
    SThunderboltCollection* thunderbolt_collection(xr_vector<SThunderboltCollection*>& collection, shared_str const& id);
    CLensFlareDescriptor* add_flare(xr_vector<CLensFlareDescriptor*>& collection, shared_str const& id);

public:
    float p_var_alt;
    float p_var_long;
    float p_min_dist;
    float p_tilt;
    float p_second_prop;
    float p_sky_color;
    float p_sun_color;
    float p_fog_color;
};

ENGINE_API extern Flags32 psEnvFlags;
ENGINE_API extern float psVisDistance;
extern ENGINE_API float ps_r_sunshafts_intensity;
