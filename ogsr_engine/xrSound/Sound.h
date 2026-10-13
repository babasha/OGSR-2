#pragma once

#ifdef XRSOUND_STATIC
#define XRSOUND_API
#elif defined XRSOUND_EXPORTS
#define XRSOUND_API __declspec(dllexport)
#else
#define XRSOUND_API __declspec(dllimport)
#endif

#define OGG_COMMENT_VERSION 0x0003

// refs
class CObject;
class XRSOUND_API CSound_params;
class XRSOUND_API CSound_source;
class XRSOUND_API CSound_emitter;
class XRSOUND_API CSound_stream_interface;
class XRSOUND_API CSound_environment;

XRSOUND_API extern u32 psSoundModel;
XRSOUND_API extern float psSoundVEffects;
XRSOUND_API extern float psSoundVFactor;
XRSOUND_API extern float psSoundVMusic;
XRSOUND_API extern float psSoundRolloff;
XRSOUND_API extern float psSoundOcclusionScale;
XRSOUND_API extern float psSoundLinearFadeFactor; //--#SM+#--
XRSOUND_API extern Flags32 psSoundFlags;
XRSOUND_API extern int psSoundTargets;
XRSOUND_API extern int psSoundCacheSizeMB;
XRSOUND_API extern xr_token* snd_devices_token;
XRSOUND_API extern u32 snd_device_id;
XRSOUND_API extern float psSoundTimeFactor; //--#SM+#--

XRSOUND_API extern BOOL snd_enable_float_pcm;

// snd_hrtf: 0 off, 1 on, 2 let OpenAL Soft decide (default, and what the engine
// did before by passing no context attributes).
XRSOUND_API extern int snd_hrtf;

// ⭐08-10-2026 the sound upgrade (each part off = the old behaviour):
// snd_occ_mode: 0 = the old occlusion (one ray, a hit = volume x snd_occ_scale), 1 = several paths toward the
//   source, each through the materials it crosses, and the blocked part MUFFLES (low-pass to snd_occ_hf) instead of
//   only getting quieter. snd_occ_rays: paths refreshed per source per update (of 4); snd_occ_mtl: how much the
//   materials' transmission counts (0 = every surface blocks as fully as the old code thought).
XRSOUND_API extern int snd_occ_mode;
XRSOUND_API extern int snd_occ_rays;
XRSOUND_API extern float snd_occ_hf;
XRSOUND_API extern float snd_occ_mtl;
// snd_voices: sources actually mixed (the rest run on as virtual and come back when they are loud enough).
// snd_hdr: how deep a loud event (shot, explosion) ducks the rest of the mix, 0 = off; snd_hdr_release: seconds back.
XRSOUND_API extern int snd_voices;
XRSOUND_API extern float snd_hdr;
XRSOUND_API extern float snd_hdr_release;
// snd_prop: a source behind an obstacle is heard by the path around it (doors, corners) -- through the game's
// CSound_propagation (the AI map); snd_prop_radius: how far from the listener that path is searched (m).
XRSOUND_API extern int snd_prop;
XRSOUND_API extern float snd_prop_radius;
// snd_env_auto: the listener's reverb from rays around him -- 0 = the level's hand zones only (old), 1 = auto where
// no zone covers the listener, 2 = auto everywhere. snd_env_rays: rays per update (of 32).
XRSOUND_API extern int snd_env_auto;
XRSOUND_API extern int snd_env_rays;
// snd_dbg: 1 = a [snd] line every 2 s (voices, rays, cost, duck, room), 2 = also every source that is occluded
XRSOUND_API extern int snd_dbg;
// snd_prof: a [snd prof] line for every update at least this long (ms), part by part; 0 = off
XRSOUND_API extern int snd_prof;
// snd_start_blocks: 400 ms blocks a starting sound decodes at once (1..3); the rest follow one per update. 3 = old.
// 📏Switch 10-10: a start was ~24 ms of the sound thread (3 blocks decoded), 82% of its slow updates.
XRSOUND_API extern int snd_start_blocks;
// snd_nxa (11-10-2026): play an .nxa (DSP-ADPCM, the Switch pack's short sounds) where there is one next to the .ogg -- read
// when a source is first created. The console's audren backend (-snd_al = OpenAL as before) at its start: snd_nx_voices
// DSP voices, snd_nx_arena_mb of memory the DSP reads the resident .nxa from.
XRSOUND_API extern int snd_nxa;
XRSOUND_API extern int snd_nx_voices;
XRSOUND_API extern int snd_nx_arena_mb;
// snd_nx_reverb: the room reverb on the DSP (I3DL2) -- at start; it plays only with snd_efx on, like EFX
XRSOUND_API extern int snd_nx_reverb;
// snd_nx_capture <s>: the DSP's final mix for that long -> nxcap_NN.wav next to the NRO (a run on the console, heard later)
XRSOUND_API extern float snd_nx_capture;

// Flags
enum
{
    //ss_Hardware = (1ul << 1ul), // Use hardware mixing only
    ss_EAX = (1ul << 2ul), // Use EAX or EFX
    ss_UseDefaultDevice = (1ul << 3ul),
    ss_forcedword = u32(-1)
};

enum
{
    sq_DEFAULT,
    sq_NOVIRT,
    sq_LIGHT,
    sq_HIGH,
    sq_forcedword = u32(-1)
};
enum
{
    sg_Undefined = 0,
    sg_SourceType = u32(-1),
    sg_forcedword = u32(-1),
};
enum
{
    sm_Looped = (1ul << 0ul), //!< Looped
    sm_2D = (1ul << 1ul), //!< 2D mode
    sm_forcedword = u32(-1),
};
enum esound_type
{
    st_Effect = 0,
    st_Music = 1,
    st_forcedword = u32(-1),
};

class CSound_UserDataVisitor;

class CSound_UserData : public xr_resource
{
public:
    virtual ~CSound_UserData() {}
    virtual void accept(CSound_UserDataVisitor*) = 0;
    virtual void invalidate() = 0;
};
typedef resptr_core<CSound_UserData, resptr_base<CSound_UserData>> CSound_UserDataPtr;

class ref_sound_data : public xr_resource
{
public:
    //	shared_str						nm;
    CSound_source* handle; //!< Pointer to wave-source interface
    CSound_emitter* feedback; //!< Pointer to emitter, automaticaly clears on emitter-stop
    esound_type s_type;
    int g_type; //!< Sound type, usually for AI
    CObject* g_object; //!< Game object that emitts ref_sound
    CSound_UserDataPtr g_userdata;
    shared_str fn_attached[2];

    u32 dwBytesTotal;
    float fTimeTotal;

public:
    ref_sound_data();
    ref_sound_data(LPCSTR fName, esound_type sound_type, int game_type);
    virtual ~ref_sound_data();
    float get_length_sec() const { return fTimeTotal; };
};
typedef resptr_core<ref_sound_data, resptr_base<ref_sound_data>> ref_sound_data_ptr;
/*! \class ref_sound
\brief Sound source + control

The main class respresenting source/emitter interface
This class infact just hides internals and redirect calls to
specific sub-systems
*/
struct ref_sound
{
    ref_sound_data_ptr _p;

public:
    ref_sound() {}
    ~ref_sound() {}

    IC CSound_source* _handle() const { return _p ? _p->handle : NULL; }
    IC CSound_emitter* _feedback() { return _p ? _p->feedback : 0; }
    IC CObject* _g_object()
    {
        VERIFY(_p);
        return _p->g_object;
    }
    IC int _g_type()
    {
        VERIFY(_p);
        return _p->g_type;
    }
    IC esound_type _sound_type()
    {
        VERIFY(_p);
        return _p->s_type;
    }
    IC CSound_UserDataPtr _g_userdata()
    {
        VERIFY(_p);
        return _p->g_userdata;
    }

    IC void create(LPCSTR name, esound_type sound_type, int game_type);
    IC void attach_tail(LPCSTR name);

    IC void clone(const ref_sound& from, esound_type sound_type, int game_type);

    IC void destroy();

    IC void play(CObject* O, u32 flags = 0, float delay = 0.f);
    IC void play_at_pos(CObject* O, const Fvector& pos, u32 flags = 0, float delay = 0.f);
    IC void play_no_feedback(CObject* O, u32 flags = 0, float delay = 0.f, Fvector* pos = 0, float* vol = 0, float* freq = 0, Fvector2* range = 0, float audible = 1.f, float audible_range = 1.f);

    IC void stop();
    IC void stop_deffered();

    IC void set_position(const Fvector& pos);
    IC void set_frequency(float freq);
    IC void set_range(float min, float max);
    IC void set_volume(float vol);
    IC void set_priority(float vol);
    IC void set_time(float t); //--#SM+#--

    IC const CSound_params* get_params();
    IC void set_params(CSound_params* p);
    IC float get_length_sec() const { return _p ? _p->get_length_sec() : 0.0f; };
};

/// definition (Sound Source)
class XRSOUND_API CSound_source
{
public:
    virtual float length_sec() const = 0;
    virtual u32 game_type() const = 0;
    virtual LPCSTR file_name() const = 0;
    virtual u16 channels_num() const = 0;
    virtual u32 bytes_total() const = 0;
};

/// definition (Sound Source)
class XRSOUND_API CSound_environment
{
public:
    shared_str name;

    u32 Environment; // source environment

    float Room; // room effect level at low frequencies
    float RoomHF; // room effect high-frequency level re. low frequency level
    float RoomRolloffFactor; // like DS3D flRolloffFactor but for room effect
    float DecayTime; // reverberation decay time at low frequencies
    float DecayHFRatio; // high-frequency to low-frequency decay time ratio
    float Reflections; // early reflections level relative to room effect
    float ReflectionsDelay; // initial reflection delay time
    float Reverb; // late reverberation level relative to room effect
    float ReverbDelay; // late reverberation delay time relative to initial reflection
    float EnvironmentSize; // environment size in meters
    float EnvironmentDiffusion; // environment diffusion
    float AirAbsorptionHF; // change in level per meter at 5 kHz
};

/// definition (Sound Params)
class XRSOUND_API CSound_params
{
public:
    Fvector position;
    float base_volume;
    float volume;
    float freq;
    float min_distance;
    float max_distance;
    float max_ai_distance;
};

/// definition (Sound Interface)
class XRSOUND_API CSound_emitter
{
public:
    virtual BOOL is_2D() = 0;
    virtual void switch_to_2D() = 0;
    virtual void switch_to_3D() = 0;
    virtual void set_position(const Fvector& pos) = 0;
    virtual void set_frequency(float freq) = 0;
    virtual void set_range(float min, float max) = 0;
    virtual void set_volume(float vol) = 0;
    virtual void set_priority(float vol) = 0;
    virtual void set_time(float t) = 0; //--#SM+#--
    virtual void stop(BOOL bDeffered) = 0;
    virtual const CSound_params* get_params() = 0;
    virtual u32 play_time() = 0;
};

/// definition (Sound Stream Interface)
class XRSOUND_API CSound_stream_interface
{
public:
};

/// definition (Sound Stream Interface)
class XRSOUND_API CSound_stats
{
public:
    u32 _rendered;
    u32 _simulated;
    u32 _cache_hits;
    u32 _cache_misses;
    u32 _events;
};

class XRSOUND_API CSound_stats_ext
{
public:
    struct SItem
    {
        shared_str name;
        CSound_params params;
        float volume;
        esound_type type;
        int game_type;
        CObject* game_object;
        struct
        {
            u32 _3D : 1;
            u32 _rendered : 1;
        };
    };
    DEFINE_VECTOR(SItem, item_vec, item_vec_it);
    item_vec items;

public:
    void clear() { items.clear(); }
    void append(const SItem& itm) { items.push_back(itm); }
};

/// definition (Sound Callback)
typedef void __stdcall sound_event(ref_sound_data_ptr S, float range);

/// The game's side of snd_prop: how a sound gets to the listener around what is in the way. Called on the sound
/// thread, under the sound manager's lock (set_propagation takes the same lock, so a level going away waits).
class XRSOUND_API CSound_propagation
{
public:
    virtual ~CSound_propagation() {}
    // once per sound update, before the sources ask
    virtual void prop_update(const Fvector& listener, float radius) = 0;
    // the walkable path from the listener to P: its length and the point near the listener it leaves by
    // (the door, the corner). false = nothing known (off the map, beyond the radius).
    virtual bool prop_query(const Fvector& P, float& path_len, Fvector& exit_point) = 0;
};

/// definition (Sound Manager Interface)
class XRSOUND_API CSound_manager_interface
{
    virtual void _initialize(int stage) = 0;
    virtual void _clear() = 0;

protected:
    friend class ref_sound_data;
    virtual void _create_data(ref_sound_data& S, LPCSTR fName, esound_type sound_type, int game_type) = 0;
    virtual void _destroy_data(ref_sound_data& S) = 0;

public:
    virtual ~CSound_manager_interface() {}

    static void _create(int stage);
    static void _destroy();

    virtual void _restart() = 0;

    virtual void create(ref_sound& S, LPCSTR fName, esound_type sound_type, int game_type) = 0;
    virtual void attach_tail(ref_sound& S, LPCSTR fName) = 0;
    virtual void clone(ref_sound& S, const ref_sound& from, esound_type sound_type, int game_type) = 0;
    virtual void destroy(ref_sound& S) = 0;
    virtual void stop_emitters() = 0;
    virtual int pause_emitters(bool val) = 0;

    virtual void play(ref_sound& S, CObject* O, u32 flags = 0, float delay = 0.f) = 0;
    virtual void play_at_pos(ref_sound& S, CObject* O, const Fvector& pos, u32 flags = 0, float delay = 0.f) = 0;
    // audible, audible_range: scale only what the player hears (base volume, min/max distance), not what the AI hears by
    virtual void play_no_feedback(ref_sound& S, CObject* O, u32 flags = 0, float delay = 0.f, Fvector* pos = 0, float* vol = 0, float* freq = 0, Fvector2* range = 0, float audible = 1.f, float audible_range = 1.f) = 0;

    virtual void set_master_volume(float f = 1.f) = 0;
    virtual void set_geometry_env(IReader* I) = 0;
    virtual void set_geometry_som(IReader* I) = 0;
    virtual void set_geometry_occ(CDB::MODEL* M) = 0;
    // per CDB::TRI::material of the occlusion model: how much sound one surface lets through (0 blocks, 1 free)
    virtual void set_occlusion_materials(const float* transmission, u32 count) = 0;
    virtual void set_propagation(CSound_propagation* P) = 0;
    virtual void set_handler(sound_event* E) = 0;

    virtual void update(const Fvector& P, const Fvector& D, const Fvector& N) = 0;
    virtual void statistic(CSound_stats* s0, CSound_stats_ext* s1) = 0;

    virtual float get_occlusion_to(const Fvector& hear_pt, const Fvector& snd_pt, float dispersion = 0.2f) = 0;

    virtual void object_relcase(CObject* obj) = 0;
    virtual const Fvector& listener_position() = 0;

    virtual CSound_environment* DbgCurrentEnv() = 0;
    virtual void DbgCurrentEnvPaused(bool v) = 0;
    virtual void DbgCurrentEnvSave() = 0;
};

class CSound_manager_interface;
extern XRSOUND_API CSound_manager_interface* Sound;

/// ********* Sound ********* (utils, accessors, helpers)
IC ref_sound_data::ref_sound_data()
{
    handle = 0;
    feedback = 0;
    g_type = 0;
    g_object = 0;
    s_type = st_Effect;
    dwBytesTotal = 0;
    fTimeTotal = 0.0f;
}
IC ref_sound_data::ref_sound_data(LPCSTR fName, esound_type sound_type, int game_type) { ::Sound->_create_data(*this, fName, sound_type, game_type); }
IC ref_sound_data::~ref_sound_data() { ::Sound->_destroy_data(*this); }

IC void ref_sound::create(LPCSTR name, esound_type sound_type, int game_type)
{
    VERIFY(!::Sound->i_locked());
    ::Sound->create(*this, name, sound_type, game_type);
}
IC void ref_sound::attach_tail(LPCSTR name)
{
    VERIFY(!::Sound->i_locked());
    ::Sound->attach_tail(*this, name);
}

IC void ref_sound::clone(const ref_sound& from, esound_type sound_type, int game_type)
{
    VERIFY(!::Sound->i_locked());
    ::Sound->clone(*this, from, sound_type, game_type);
}
IC void ref_sound::destroy()
{
    VERIFY(!::Sound->i_locked());
    ::Sound->destroy(*this);
}
IC void ref_sound::play(CObject* O, u32 flags, float d)
{
    VERIFY(!::Sound->i_locked());
    ::Sound->play(*this, O, flags, d);
}
IC void ref_sound::play_at_pos(CObject* O, const Fvector& pos, u32 flags, float d)
{
    VERIFY(!::Sound->i_locked());
    ::Sound->play_at_pos(*this, O, pos, flags, d);
}
IC void ref_sound::play_no_feedback(CObject* O, u32 flags, float d, Fvector* pos, float* vol, float* freq, Fvector2* range, float audible, float audible_range)
{
    VERIFY(!::Sound->i_locked());
    ::Sound->play_no_feedback(*this, O, flags, d, pos, vol, freq, range, audible, audible_range);
}
IC void ref_sound::set_position(const Fvector& pos)
{
    VERIFY(!::Sound->i_locked());
    if (_feedback()) // null under -nosound (VERIFY is compiled out in release)
        _feedback()->set_position(pos);
}
IC void ref_sound::set_frequency(float freq)
{
    VERIFY(!::Sound->i_locked());
    if (_feedback())
        _feedback()->set_frequency(freq);
}
IC void ref_sound::set_range(float min, float max)
{
    VERIFY(!::Sound->i_locked());
    if (_feedback())
        _feedback()->set_range(min, max);
}
IC void ref_sound::set_volume(float vol)
{
    VERIFY(!::Sound->i_locked());
    if (_feedback())
        _feedback()->set_volume(vol);
}
IC void ref_sound::set_priority(float p)
{
    VERIFY(!::Sound->i_locked());
    if (_feedback())
        _feedback()->set_priority(p);
}
IC void ref_sound::set_time(float t)
{
    VERIFY(!::Sound->i_locked());
    if (_feedback())
        _feedback()->set_time(t);
}
IC void ref_sound::stop()
{
    VERIFY(!::Sound->i_locked());
    if (_feedback())
        _feedback()->stop(FALSE);
}
IC void ref_sound::stop_deffered()
{
    VERIFY(!::Sound->i_locked());
    if (_feedback())
        _feedback()->stop(TRUE);
}
IC const CSound_params* ref_sound::get_params()
{
    VERIFY(!::Sound->i_locked());
    if (_feedback())
        return _feedback()->get_params();
    else
        return NULL;
}
IC void ref_sound::set_params(CSound_params* p)
{
    VERIFY(!::Sound->i_locked());
    if (_feedback())
    {
        _feedback()->set_position(p->position);
        _feedback()->set_frequency(p->freq);
        _feedback()->set_range(p->min_distance, p->max_distance);
        _feedback()->set_volume(p->volume);
    }
}
