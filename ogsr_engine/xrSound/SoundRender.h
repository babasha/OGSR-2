#pragma once

#include "Sound.h"

#include <atomic>

class CSoundRender_Core;
class CSoundRender_Source;
class CSoundRender_Emitter;
class CSoundRender_EmitterParams;
class CSoundRender_Target;
class CSoundRender_Environment;
class SoundEnvironment_LIB;

const u32 sdef_target_count = 3; //
const u32 sdef_target_block = 400; // ms
const u32 sdef_target_size = sdef_target_count * sdef_target_block; // ms
const float s_f_def_source_footer = 0.0f; // sec of silence after buffer data, just for rendering
const u32 sdef_env_version = 4; // current version of env-def
const u32 sdef_level_version = 1; // current version of level-def
const float s_f_def_event_pulse = 0.5f; // sec

// [snd prof]: what a sound start costs inside -- opening the ogg (FS.r_open + vorbis headers) and decoding cache lines.
// Sound thread only (under m_bLocked); reset at the top of every update.
struct SndProfCounters
{
    float open_ms{}, dec_ms{};
    u32 opens{}, lines{};
};
extern SndProfCounters g_snd_prof;
void snd_vorbis_note(const char* name, float ms);            // [snd] vorbis (snd_dbg): SoundRender_Source_loader.cpp
void snd_vorbis_report(char* out, size_t size, float secs); // prints the top and starts over

// sources created (i_create_source): cache hits, new loads and what those cost -- any thread, read by the reports
struct SndSourceCounters
{
    std::atomic<u32> hits{}, loads{}, load_us{};
};
extern SndSourceCounters g_snd_sources;
