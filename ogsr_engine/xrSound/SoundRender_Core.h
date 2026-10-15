#pragma once

#include <al.h>
#include <efx-presets.h>

#include "SoundRender.h"
#include "SoundRender_Environment.h"
#include "SoundRender_Cache.h"

#include <mutex>
#include <string>
#include <unordered_map>

class CSoundRender_Core : public CSound_manager_interface
{
    volatile BOOL bLocked{};

protected:
    virtual void _create_data(ref_sound_data& S, LPCSTR fName, esound_type sound_type, int game_type);
    virtual void _destroy_data(ref_sound_data& S);

protected:
    BOOL bListenerMoved{};

    bool e_currentPaused{false};

    CSoundRender_Environment e_current;
    CSoundRender_Environment* e_target{};

public:
    typedef std::pair<ref_sound_data_ptr, float> event;
    xr_vector<event> s_events;

public:
    BOOL bPresent{};
    BOOL bEAX{}; // Boolean variable to indicate presence of EAX Extension
    BOOL bDeferredEAX{};
    bool bEFX{}; // boolean variable to indicate presence of EFX Extension
    bool bNativeEnv{}; // the backend takes the room itself (i_native_env: CSoundRender_CoreNX, the DSP's I3DL2 reverb)
    BOOL bReady{};

    CTimer Timer{};
    float fTimer_Value{};
    float fTimer_Delta{};
    sound_event* Handler{};

protected:
    // Collider
    CDB::COLLIDER geom_DB{};
    CDB::MODEL* geom_SOM{};
    CDB::MODEL* geom_MODEL{};
    CDB::MODEL* geom_ENV{};

    // Containers
    xr_vector<CSoundRender_Source*> s_sources;
    std::unordered_map<std::string, CSoundRender_Source*> m_sources_map; // i_create_source: fname -> source
    std::mutex m_sources_lock;
    xr_vector<CSoundRender_Emitter*> s_emitters;
    u32 s_emitters_u{}; // emitter update marker
    xr_vector<CSoundRender_Target*> s_targets;
    xr_vector<CSoundRender_Target*> s_targets_defer;
    u32 s_targets_pu{}; // parameters update
    SoundEnvironment_LIB* s_environment{};
    xr_vector<u16> s_environment_ids;

    int m_iPauseCounter{};

public:
    bool supports_float_pcm{};

    // Cache
    CSoundRender_Cache cache{};
    u32 cache_bytes_per_line{};

protected:
    virtual void i_eax_set(const GUID* guid, u32 prop, void* val, u32 sz) = 0;
    virtual void i_eax_get(const GUID* guid, u32 prop, void* val, u32 sz) = 0;

    std::mutex m_bLocked;

public:
    CSoundRender_Core();
    virtual ~CSoundRender_Core();

    // General
    virtual void _initialize(int stage) = 0;
    virtual void _clear() = 0;
    virtual void _restart();

    // Sound interface
    void verify_refsound(ref_sound& S);
    virtual void create(ref_sound& S, LPCSTR fName, esound_type sound_type, int game_type);
    virtual void attach_tail(ref_sound& S, LPCSTR fName);

    virtual void clone(ref_sound& S, const ref_sound& from, esound_type sound_type, int game_type);
    virtual void destroy(ref_sound& S);
    virtual void stop_emitters();
    virtual int pause_emitters(bool val);

    virtual void play(ref_sound& S, CObject* O, u32 flags = 0, float delay = 0.f);
    virtual void play_at_pos(ref_sound& S, CObject* O, const Fvector& pos, u32 flags = 0, float delay = 0.f);
    virtual void play_no_feedback(ref_sound& S, CObject* O, u32 flags = 0, float delay = 0.f, Fvector* pos = 0, float* vol = 0, float* freq = 0, Fvector2* range = 0, float audible = 1.f, float audible_range = 1.f);
    virtual void set_master_volume(float f) = 0;
    virtual void set_geometry_env(IReader* I);
    virtual void set_geometry_som(IReader* I);
    virtual void set_geometry_occ(CDB::MODEL* M);
    virtual void set_occlusion_materials(const float* transmission, u32 count);
    virtual void set_propagation(CSound_propagation* P);
    virtual void set_handler(sound_event* E);

    virtual void update(const Fvector& P, const Fvector& D, const Fvector& N);
    virtual void update_events();
    virtual void statistic(CSound_stats* dest, CSound_stats_ext* ext);

    // listener
    virtual void update_listener(const Fvector& P, const Fvector& D, const Fvector& N, float dt) = 0;
    // the backend's last word in update(), still under the lock (CSoundRender_CoreNX: the voices, then the DSP)
    virtual void i_commit() {}
    // bNativeEnv: the listener's room this update (e_current), or null = snd_efx off
    virtual void i_native_env(const CSoundRender_Environment* E) {}

    // eax listener
    void i_eax_listener_set(CSound_environment* E);
    void i_eax_commit_setting();

    // efx listener
    void i_efx_listener_set(CSound_environment* E);
    bool i_efx_commit_setting();

    virtual CSound_environment* DbgCurrentEnv() override { return e_target; }
    virtual void DbgCurrentEnvPaused(bool v) override { e_currentPaused = v; }
    virtual void DbgCurrentEnvSave() override { env_save_all(); }

public:
    // ⭐snd_nxa: where an .nxa source's payload lives (CSoundRender_Source::m_nxa_payload). Base: a heap copy, kept for
    // good (the PC: OpenAL decodes it on the CPU). CSoundRender_CoreNX: the audren arena, least recently used out.
    virtual void nxa_keep(CSoundRender_Source* S, const u8* payload);
    virtual void nxa_drop(CSoundRender_Source* S);
    // the payload to decode on the CPU: resident, or the file the target holds open
    virtual const u8* nxa_payload_for_cpu(CSoundRender_Source* S, CSoundRender_Target* T);
    // a source created from the index (no payload yet): load it in the background if the backend wants it resident
    virtual void nxa_prefetch(CSoundRender_Source* S) {}

    CSoundRender_Source* i_create_source(LPCSTR name);
    void i_destroy_source(CSoundRender_Source* S);
    CSoundRender_Emitter* i_play(ref_sound* S, BOOL _loop, float delay);
    void i_start(CSoundRender_Emitter* E);
    void i_stop(CSoundRender_Emitter* E);
    void i_rewind(CSoundRender_Emitter* E);
    BOOL i_allow_play(CSoundRender_Emitter* E);

    virtual void object_relcase(CObject* obj);

    virtual float get_occlusion_to(const Fvector& hear_pt, const Fvector& snd_pt, float dispersion = 0.2f);
    float get_occlusion(Fvector& P, float R, Fvector* occ);
    // snd_occ_mode 1: what share of the sound gets from A to B through what lies between (materials, level.som)
    float occ_transmission(const Fvector& from, const Fvector& to);
    // how far up from P there is room (up to `want`, a little under a ceiling)
    float occ_headroom(const Fvector& P, float want);
    // snd_prop: the game's path around the obstacle (sound thread, under m_bLocked)
    bool prop_query(const Fvector& P, float& path_len, Fvector& exit_point);

    // [snd] counters of this update (snd_dbg)
    struct SDbg
    {
        u32 occ_rays{}, env_rays{}, prop_queries{}, prop_hits{}, prop_rebuilds{};
    } dbg{};

    // snd_hdr: 0..1 how loud the loudest shot/explosion is right now (fast attack, snd_hdr_release back)
    float hdr_level{};
    CSoundRender_Environment* get_environment_def();
    CSoundRender_Environment* get_environment(const Fvector& P);

    void env_load();
    void env_unload();
    void env_save_all() const;
    void env_apply();

protected:
    xr_vector<float> occ_mtl; // set_occlusion_materials
    CSound_propagation* m_propagation{};
    float m_prop_next{}; // fTimer_Value of the next prop_update

    // snd_env_auto: distances along 32 fixed directions around the listener, a few refreshed per update
    static constexpr u32 env_dirs = 32;
    float env_dist[env_dirs]{};
    u32 env_next{};
    float env_next_t{}; // fTimer_Value of the next rays
    bool env_valid{};
    CSoundRender_Environment e_auto;
    CSoundRender_Environment* e_zone{}; // the hand zone the listener is in (identity = none); e_target = it or e_auto
    CSoundRender_Environment e_blend; // a broad zone giving way to e_auto under a roof
    static bool env_zone_broad(const CSoundRender_Environment* E); // open_space, default, mountains...: a whole area
    float env_in{}, env_mean{}, env_roof{}, env_encl{}; // the last estimate, for [snd]
    void env_auto_update(const Fvector& P);

    void hdr_update(float dt);
    void dbg_report(float dt);
    float dbg_update_ms{}, dbg_time{}, dbg_ms_sum{}, dbg_ms_max{}, dbg_hdr_peak{};
    u32 dbg_updates{}, dbg_ducked_peak{};

protected: // EFX
    EFXEAXREVERBPROPERTIES efx_reverb{};
    ALuint effect{};
    ALuint slot{};

    bool EFXTestSupport();
    void InitAlEFXAPI();

    void release_efx_objects() const;
};

extern XRSOUND_API CSoundRender_Core* SoundRender;

// EFX filters (snd_occ_mode 1: the per-source low-pass), loaded with the rest of EFX in InitAlEFXAPI; null without EFX
namespace snd_efx
{
using GenFilters = void(AL_APIENTRY*)(ALsizei, ALuint*);
using DeleteFilters = void(AL_APIENTRY*)(ALsizei, const ALuint*);
using Filteri = void(AL_APIENTRY*)(ALuint, ALenum, ALint);
using Filterf = void(AL_APIENTRY*)(ALuint, ALenum, ALfloat);
extern GenFilters gen_filters;
extern DeleteFilters delete_filters;
extern Filteri filteri;
extern Filterf filterf;
} // namespace snd_efx
