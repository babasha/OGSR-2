#pragma once

// ⭐The Switch's sound target on audren (11-10-2026, "layer B"; see SoundRender_CoreNX.h). One target = up to two
// DSP voices:
//  * DIRECT -- an .nxa source resident in the arena: the DSP plays the DSP-ADPCM payload itself (a stereo one as
//    two mono voices), nothing decoded on the CPU, nothing streamed. Loops loop on the DSP.
//  * STREAM -- everything else (.ogg, an .nxa that is not resident or has attached tails): the old path -- the
//    emitter's fill_block through the cache -- into four PCM16 blocks of this target's slot, refilled as the DSP
//    finishes them.
// Where OpenAL did the 3D, fill_parameters does: inverse-distance-clamped attenuation (AL's default model, same
// rolloff), equal-power panning to the stereo out, pitch, and the occlusion low-pass as a biquad on the voice (the
// high shelf at 5 kHz AL_LOWPASS_GAINHF means).
//
// Threads: everything runs on the sound update's thread under the core's lock -- except stop(), which the game
// calls from its own (ref_sound::stop): it only flags; commit() (the core, last thing in its update) drops the
// voices and starts what render()/rewind() asked for.
#ifdef __SWITCH__

#include "SoundRender_Target.h"

#include <atomic>
#include <mutex>

class CSoundRender_CoreNX;
class PinnedWorker;

class CSoundRender_TargetNX : public CSoundRender_Target
{
    typedef CSoundRender_Target inherited;

public:
    static constexpr u32 blocks = 4;

private:
    int m_index;
    CSoundRender_CoreNX* m_core;

    enum Mode
    {
        mStream,
        mDirect,
    };
    Mode m_mode{mStream};
    bool m_live{}; // voices initialised and playing
    bool m_play_req{}; // render()/rewind() filled what to start; commit() starts it
    std::atomic<bool> m_stop_req{}; // stop(): drop the voices at the next commit
    u32 m_nvoices{}; // voices of the current sound
    CSoundRender_Source* m_held{}; // .nxa payload in use (nxa_acquire), released at stop

    // STREAM
    u32 m_block_bytes{};
    u32 m_next{}; // block to fill next (ring of `blocks`)
    u32 m_ready{}; // blocks render() filled (0..m_ready-1), queued by commit
    // DIRECT
    u32 m_start_sample{};
    bool m_loop{};

    // fill_parameters -> commit
    float m_volume{};
    float m_pitch{1.f};
    float m_mix[8]{}; // [channel * 4 + submix buffer]: dry L, dry R, send L, send R
    float m_hf{1.f}, m_hf_set{1.f};

    // the stream's file opened off the sound thread (render -> opener() -> update adopts it); under m_open_lock
    std::mutex m_open_lock;
    u32 m_open_gen{}; // stop() bumps it: an open finishing for an older sound closes what it opened
    bool m_waiting_open{};
    bool m_opened{};
    IReader* m_opened_w{};
    OggVorbis_File* m_opened_ovf{};
    static PinnedWorker* opener();

    void fill_first_blocks();
    void fill_block_at(u32 block);
    void apply_params();

public:
    CSoundRender_TargetNX(CSoundRender_CoreNX* core, int index);
    virtual ~CSoundRender_TargetNX();

    virtual BOOL _initialize();
    virtual void _destroy();
    virtual void _restart();

    virtual void start(CSoundRender_Emitter* E);
    virtual void render();
    virtual void rewind();
    virtual void stop();
    virtual void update();

    virtual void fill_parameters(CSoundRender_Core* core);
    virtual void alAuxInit(ALuint) override {}

    void commit(); // sound thread, under the core's lock, before the renderer update
    bool live() const { return m_live; }
    bool direct() const { return m_mode == mDirect; }

    static void wait_openers(); // shutdown: no open in flight may outlive the targets
    static inline std::atomic<u32> s_async_opens{}, s_async_open_us{};
};

#endif // __SWITCH__
