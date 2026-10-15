#pragma once

#include "SoundRender.h"
#include "SoundRender_Environment.h"

class CSoundRender_Emitter : public CSound_emitter
{
    float starting_delay;

public:
    enum State
    {
        stStopped = 0,

        stStartingDelayed,
        stStartingLoopedDelayed,

        stStarting,
        stStartingLooped,

        stPlaying,
        stPlayingLooped,

        stSimulating,
        stSimulatingLooped,

        stFORCEDWORD = u32(-1)
    };

public:
    CSoundRender_Target* target;
    IC CSoundRender_Source* source() { return (CSoundRender_Source*)owner_data->handle; };
    ref_sound_data_ptr owner_data;

    u32 get_bytes_total() const;
    float get_length_sec() const;

    float priority_scale;
    float smooth_volume;
    float occluder_volume; // USER
    float fade_volume;
    Fvector occluder[3]{};

    // snd_occ_mode 1: transmission of the 4 paths sampled toward the source (occ_rays_refresh), smoothed into
    // occluder_volume (all frequencies) and occluder_hf (the low-pass the target applies)
    static constexpr u32 occ_paths = 4;
    float occ_path_T[occ_paths]{1.f, 1.f, 1.f, 1.f};
    u32 occ_path_next{};
    float occluder_hf{1.f};
    // snd_prop: share of the sound that comes round the obstacle (0..1, smoothed) and the point it comes from
    float prop_w{};
    Fvector prop_pos{};
    // the last refresh (occ_update re-traces on a clock, smooths in between): through-the-wall gain/highs, the
    // share that should come round, what comes round (energy at the listener), and when to look again
    float occ_through_gain{1.f}, occ_through_hf{1.f}, occ_w_target{}, occ_round_e{};
    float occ_next{};
    // the room over the source (occ_headroom) and where it was measured: the geometry does not move, the source rarely
    Fvector occ_head_P{};
    float occ_head_v{-1.f};
    // snd_hdr: this source's duck (1 = none), smoothed
    float hdr_gain{1.f};
    // where the target puts the AL source: p_source.position, or pulled toward prop_pos
    Fvector render_position() const;
    void occ_rays_refresh(u32 count);
    void occ_update(float dt); // dt < 0: a start, all paths and no smoothing
    void occ_start();
    float hdr_weight() const; // how much snd_hdr ducks this kind of sound (0..1)
    bool hdr_driver() const; // a shot / an explosion: what sets the duck

    State m_current_state;
    u32 m_stream_cursor;
    u32 m_cur_handle_cursor;
    CSound_params p_source{};

    int iPaused;
    BOOL bMoved;
    BOOL b2D;
    BOOL bStopping;
    BOOL bRewind;
    float fTimeStarted; // time of "Start"
    float fTimeToStop; // time to "Stop"
    float fTimeToPropagade;
    float fTimeToRewind; // --#SM+#--

    u32 marker;
    void i_stop();

    void set_cursor(u32 p);
    u32 get_cursor(bool b_absolute) const;
    void move_cursor(int offset);

public:
    void Event_Propagade();
    void Event_ReleaseOwner();
    BOOL isPlaying(void) { return m_current_state != stStopped; }

    virtual BOOL is_2D() { return b2D; }
    virtual void switch_to_2D();
    virtual void switch_to_3D();
    virtual void set_position(const Fvector& pos);
    virtual void set_frequency(float scale)
    {
        VERIFY(_valid(scale));
        p_source.freq = scale;
    }
    virtual void set_range(float min, float max)
    {
        VERIFY(_valid(min) && _valid(max));
        p_source.min_distance = min;
        p_source.max_distance = max;
    }
    virtual void set_volume(float vol)
    {
        if (!_valid(vol))
            vol = 0.0f;
        p_source.volume = vol;
    }
    virtual void set_priority(float p) { priority_scale = p; }
    virtual void set_time(float t); //--#SM+#--
    virtual const CSound_params* get_params() { return &p_source; }

    void fill_block(void* ptr, u32 size);
    void fill_data(u8* ptr, u32 offset, u32 size);

    float priority();
    float att();
    void start(ref_sound* _owner, BOOL _loop, float delay);
    void cancel(); // manager forces out of rendering
    void update(float dt);
    BOOL update_culling(float dt);
    void update_environment(float dt);
    void rewind();
    virtual void stop(BOOL bDeffered);
    void pause(BOOL bVal, int id);

    virtual u32 play_time();

    CSoundRender_Emitter();
    ~CSoundRender_Emitter();
};
