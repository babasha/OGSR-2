#pragma once

#include "SoundRender_Cache.h"

// refs
struct OggVorbis_File;
namespace nxa
{
struct IndexEntry;
}
class CSoundRender_Target;

class CSoundRender_Source : public CSound_source
{
public:
    shared_str pname;
    shared_str fname;
    cache_cat CAT;

    float fTimeTotal{};
    u32 dwBytesTotal{};

    WAVEFORMATEX m_wformat{}; //= SoundRender->wfm;

    float m_fBaseVolume;
    float m_fMinDist;
    float m_fMaxDist;
    float m_fMaxAIDist;
    u32 m_uGameType;

    // ⭐snd_nxa (11-10-2026): the source is an .nxa (DSP-ADPCM, SoundRender_Adpcm.h) -- PCM16 to the CPU path
    // (decompress decodes it, cheap), straight to the DSP on the Switch (CSoundRender_TargetNX). The payload
    // (coefficients + per-channel data) is kept resident when the core has room for it (nxa_keep); otherwise the
    // CPU path reads the file through the target, like an .ogg, and the console loads it on its first play.
    bool m_nxa{};
    u32 m_nxa_samples{}; // per channel
    u32 m_nxa_stride{}; // bytes per channel's data
    u32 m_nxa_payload_size{};
    u8* m_nxa_payload{}; // resident payload or null -- owned by the core (nxa_* below), guarded by its lock
    int m_nxa_users{}; // targets playing the resident payload (it may not be evicted meanwhile), under the core's lock
    u64 m_nxa_stamp{}; // last use, for the eviction order

private:
    void i_decompress(OggVorbis_File* ovf, char* dest, u32 size) const;
    void i_decompress(OggVorbis_File* ovf, float* dest, u32 size) const; // this overload clamps denormalized sounds

    void LoadWave(LPCSTR name);
    void LoadNxa(LPCSTR name);
    void LoadNxaIndexed(LPCSTR name, const nxa::IndexEntry& h);
    void LoadOggIndexed(LPCSTR name, const nxa::IndexEntry& h);

public:
    CSoundRender_Source();
    ~CSoundRender_Source();

    void load(LPCSTR name);
    void unload();
    void decompress(u32 line, CSoundRender_Target* T);

    virtual float length_sec() const { return fTimeTotal; }
    virtual u32 game_type() const { return m_uGameType; }
    virtual LPCSTR file_name() const { return *fname; }
    virtual float base_volume() const { return m_fBaseVolume; }
    virtual u16 channels_num() const { return m_wformat.nChannels; }
    virtual u32 bytes_total() const { return dwBytesTotal; }
};
