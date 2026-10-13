#pragma once

// The Switch's audio renderer (audren) behind plain types: <switch.h> and xrCore do not share one translation unit
// (u64, Event, Result...), so SoundRender_NxAudren.cpp is the only file that sees libnx and the sound core
// (SoundRender_CoreNX / TargetNX) talks to it through this. All calls from ONE thread -- the sound update's, under
// the core's lock -- except flush (any thread, it only cleans the data cache).
//
// Voices: 2 per target (a stereo .nxa is two mono ADPCM voices; a PCM16 stream is one voice of 1-2 channels),
// each with 4 wave buffers of its own. Memory the DSP reads must lie in a registered pool: the arena (resident
// .nxa payloads) and the stream slots (one per target, the PCM16 blocks), both made at init. Every voice plays into
// one submix of 4 buffers -- dry L/R, reverb send L/R -- and the DSP's I3DL2 reverb runs on the send.

#include <cstddef>
#include <cstdint>

namespace nxau
{
enum WaveState
{
    wb_free = 0,
    wb_waiting,
    wb_queued,
    wb_playing,
    wb_done,
};

struct Stats
{
    float update_ms_sum{}, update_ms_max{}; // the caller's time in update()
    float ipc_ms_sum{}, ipc_ms_max{};       // the renderer call itself (on the IPC thread when it runs)
    uint32_t updates{}, failures{}, ipc_n{};
    uint32_t dsp_drops{}; // voice drops reported by the DSP (it ran out of time) since the last call
    uint32_t reverb_state{}; // the effect's state as the renderer reports it (3 enabled, 4 disabled)
    uint64_t played{}; // source samples the DSP played (all voices) -- proof it is consuming what we queue
};

// I3DL2, the units the DSP takes: millibels, seconds, percent, Hz
struct Reverb
{
    float room_mb, room_hf_mb, decay_time, decay_hf_ratio, reflections_mb, reflections_delay, reverb_mb, reverb_delay, diffusion, density, hf_reference;
};

void set_log(void (*fn)(const char*)); // where the wrapper's messages go (Msg)
// targets = how many CSoundRender_TargetNX (2 voices each); reverb = the DSP's I3DL2 on the send buffers
bool init(int targets, size_t arena_bytes, size_t stream_slot_bytes, bool reverb);
void shutdown();
bool ready();

uint8_t* arena_base();
size_t arena_size();
uint8_t* stream_slot(int target);
void flush(const void* p, size_t n); // the DSP reads memory, not the CPU's cache: after every write it will read

inline int voice_id(int target, int sub) { return target * 2 + sub; }

// format: 0 = PCM16 (interleaved channels), 1 = DSP-ADPCM (mono; coefs = 16 x s16 inside the arena)
bool voice_init(int id, int channels, int format, int rate, const void* coefs);
void voice_drop(int id); // stops, frees its channels and forgets its wave buffers (they read as wb_free)
void voice_start(int id);
bool voice_used(int id);
// mix[c*4 + o] = channel c -> submix buffer o (0 dry L, 1 dry R, 2 send L, 3 send R)
void voice_params(int id, float volume, float pitch, const float* mix, int channels);
// one biquad (b0 b1 b2 a1 a2, a0 = 1), or off
void voice_biquad(int id, bool on, const float* coef5);
uint32_t voice_played(int id);

// wave buffer `slot` (0..3) of a voice; offsets in samples (frames)
bool wb_add(int id, int slot, const void* data, size_t bytes, int start, int end, bool loop);
int wb_state(int id, int slot);

void master_volume(float v);
bool reverb_available();
void reverb_set(const Reverb& r, bool on);
bool update(Stats& st); // the renderer update (IPC): every sound update, after the voices are set
// record the final mix (what the speakers get) for `seconds` into nxcap_NN.wav next to the NRO
bool capture(float seconds);
} // namespace nxau
