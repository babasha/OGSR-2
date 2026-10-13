#include "stdafx.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include "SoundRender_Core.h"
#include "SoundRender_Source.h"
#include "SoundRender_Target.h"
#include "SoundRender_Adpcm.h"

//	SEEK_SET	0	File beginning
//	SEEK_CUR	1	Current file pointer position
//	SEEK_END	2	End-of-file
int ov_seek_func(void* datasource, s64 offset, int whence)
{
    switch (whence)
    {
    case SEEK_SET: ((IReader*)datasource)->seek((int)offset); break;
    case SEEK_CUR: ((IReader*)datasource)->advance((int)offset); break;
    case SEEK_END: ((IReader*)datasource)->seek((int)offset + ((IReader*)datasource)->length()); break;
    }
    return 0;
}
size_t ov_read_func(void* ptr, size_t size, size_t nmemb, void* datasource)
{
    IReader* F = (IReader*)datasource;
    size_t exist_block = _max(0ul, iFloor(F->elapsed() / (float)size));
    size_t read_block = _min(exist_block, nmemb);
    F->r(ptr, (int)(read_block * size));
    return read_block;
}
int ov_close_func(void* datasource) { return 0; }
long ov_tell_func(void* datasource) { return ((IReader*)datasource)->tell(); }

void CSoundRender_Source::decompress(u32 line, CSoundRender_Target* T)
{
    // decompression of one cache-line
    u32 line_size = SoundRender->cache.get_linesize();
    const auto t0 = std::chrono::steady_clock::now();
    const auto dest = SoundRender->cache.get_dataptr(CAT, line);

    if (m_nxa)
    {
        // DSP-ADPCM on the CPU: the resident payload, or the file the target holds open (a few microseconds a line)
        const u32 ch = m_wformat.nChannels;
        const u32 first = line * line_size / (2 * ch);
        const u32 count = line_size / (2 * ch);
        const u8* payload = SoundRender->nxa_payload_for_cpu(this, T);
        if (payload)
        {
            for (u32 c = 0; c < ch; ++c)
                nxa::decode(nxa::data(payload, c, m_nxa_stride), nxa::coefs(payload, c), m_nxa_samples, first, count, static_cast<s16*>(dest) + c, ch);
        }
        else
            memset(dest, 0, line_size);
        g_snd_prof.dec_ms += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        g_snd_prof.lines++;
        return;
    }

    OggVorbis_File* ovf = T->get_data();
    VERIFY(ovf);
    u32 buf_offs = (line * line_size) / (m_wformat.wBitsPerSample / 8) / m_wformat.nChannels;
    u32 left_file = dwBytesTotal - buf_offs;
    u32 left = (u32)_min(left_file, line_size);

    // seek
    u32 cur_pos = u32(ov_pcm_tell(ovf));
    if (cur_pos != buf_offs)
        ov_pcm_seek(ovf, buf_offs);

     // decompress
    if (m_wformat.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        i_decompress(ovf, static_cast<float*>(dest), left);
    else
        i_decompress(ovf, static_cast<char*>(dest), left);
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_snd_prof.dec_ms += ms;
    g_snd_prof.lines++;
    if (snd_dbg)
        snd_vorbis_note(pname.c_str(), ms);
}

// [snd] vorbis (snd_dbg, 12-10-2026): which sounds the CPU decodes from ogg (a Switch fight: ov_read was 29 % of the
// sound update). Sound thread only, like the decode itself; the names are the sources' own, alive while they are.
namespace
{
struct VorbisAcc
{
    const char* name; // the key
    char nm[64];      // printed: a copy, the source may be gone by the report
    float ms;
    u32 lines;
};
VorbisAcc s_vorbis[32];
int s_nvorbis = 0;
} // namespace

void snd_vorbis_note(const char* name, float ms)
{
    int k = 0;
    while (k < s_nvorbis && s_vorbis[k].name != name)
        ++k;
    if (k == s_nvorbis)
    {
        if (s_nvorbis >= 32)
            return;
        VorbisAcc& a = s_vorbis[s_nvorbis++];
        a.name = name;
        const size_t len = xr_strlen(name); // the tail: the folder says less than the file
        strncpy(a.nm, name + (len > 63 ? len - 63 : 0), 63);
        a.nm[63] = 0;
        a.ms = 0.f;
        a.lines = 0;
    }
    s_vorbis[k].ms += ms;
    ++s_vorbis[k].lines;
}

void snd_vorbis_report(char* out, size_t size, float secs)
{
    std::sort(s_vorbis, s_vorbis + s_nvorbis, [](const VorbisAcc& a, const VorbisAcc& b) { return a.ms > b.ms; });
    out[0] = 0;
    float total = 0.f;
    for (int k = 0; k < s_nvorbis; ++k)
        total += s_vorbis[k].ms;
    xr_sprintf(out, size, "%.2f ms/s:", total / secs);
    for (int k = 0; k < s_nvorbis && k < 5; ++k)
    {
        string256 one;
        xr_sprintf(one, " %s %.2f (%u)", s_vorbis[k].nm, s_vorbis[k].ms / secs, s_vorbis[k].lines);
        xr_strcat(out, size, one);
    }
    s_nvorbis = 0;
}

void CSoundRender_Source::LoadWave(LPCSTR pName)
{
    pname = pName;

    // Load file into memory and parse WAV-format
    OggVorbis_File ovf;
    constexpr ov_callbacks ovc = {ov_read_func, ov_seek_func, ov_close_func, ov_tell_func};
    IReader* wave = FS.r_open(pname.c_str());
    R_ASSERT3(wave && wave->length(), "Can't open wave file:", pname.c_str());
    ov_open_callbacks(wave, &ovf, NULL, 0, ovc);

    vorbis_info* ovi = ov_info(&ovf, -1);
    // verify
    R_ASSERT3(ovi, "Invalid source info:", pName);

#ifdef DEBUG
    if (ovi->channels == 2)
    {
        Msg("stereo sound source [%s]", pName);
    }
#endif

    ZeroMemory(&m_wformat, sizeof(WAVEFORMATEX));

    m_wformat.nSamplesPerSec = (ovi->rate);
    m_wformat.nChannels = u16(ovi->channels);

    if (SoundRender->supports_float_pcm)
    {
        m_wformat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        m_wformat.wBitsPerSample = 32;
    }
    else
    {
        m_wformat.wFormatTag = WAVE_FORMAT_PCM;
        m_wformat.wBitsPerSample = 16;
    }

    m_wformat.nBlockAlign = m_wformat.wBitsPerSample / 8 * m_wformat.nChannels;
    m_wformat.nAvgBytesPerSec = m_wformat.nSamplesPerSec * m_wformat.nBlockAlign;

    s64 pcm_total = ov_pcm_total(&ovf, -1);
    dwBytesTotal = u32(pcm_total * m_wformat.nBlockAlign);
    fTimeTotal = s_f_def_source_footer + dwBytesTotal / float(m_wformat.nAvgBytesPerSec);

    vorbis_comment* ovm = ov_comment(&ovf, -1);
    if (ovm->comments)
    {
        IReader F(ovm->user_comments[0], ovm->comment_lengths[0]);

        u32 vers{};
        if (F.elapsed() <= static_cast<int>(sizeof vers))
            Msg("! Invalid ogg-comment, file: [%s]", pName);
        else
            vers = F.r_u32();

        if (vers == 0x0001)
        {
            m_fMinDist = F.r_float();
            m_fMaxDist = F.r_float();
            m_fBaseVolume = 1.f;
            m_uGameType = F.r_u32();
            m_fMaxAIDist = m_fMaxDist;
        }
        else if (vers == 0x0002)
        {
            m_fMinDist = F.r_float();
            m_fMaxDist = F.r_float();
            m_fBaseVolume = F.r_float();
            m_uGameType = F.r_u32();
            m_fMaxAIDist = m_fMaxDist;
        }
        else if (vers == OGG_COMMENT_VERSION)
        {
            m_fMinDist = F.r_float();
            m_fMaxDist = F.r_float();
            m_fBaseVolume = F.r_float();
            m_uGameType = F.r_u32();
            m_fMaxAIDist = F.r_float();
        }
        else
        {
            Msg("! Invalid ogg-comment version, file: [%s]", pName);
        }
    }
    else
    {
        Msg("! Missing ogg-comment, file: [%s]", pName);
    }
    R_ASSERT3((m_fMaxAIDist >= 0.1f) && (m_fMaxDist >= 0.1f), "Invalid max distance.", pName);

    ov_clear(&ovf);
    FS.r_close(wave);
}

// ⭐snd_nxa: an .nxa next to the .ogg (the Switch pack's archive of short sounds) is read instead. The whole file
// comes in anyway (FS reads archive entries whole), so keeping the payload resident costs a memcpy.
void CSoundRender_Source::LoadNxa(LPCSTR pName)
{
    pname = pName;
    IReader* F = FS.r_open(pname.c_str());
    R_ASSERT3(F && F->length() >= sizeof(nxa::FileHeader), "Can't open nxa file:", pName);
    nxa::FileHeader h;
    F->r(&h, sizeof h);
    R_ASSERT3(h.magic == nxa::magic && h.version == nxa::version && h.channels >= 1 && h.channels <= nxa::max_channels, "Invalid nxa file:", pName);
    m_nxa = true;
    m_nxa_samples = h.samples;
    m_nxa_stride = h.data_stride;
    m_nxa_payload_size = nxa::payload_size(h.channels, h.samples);
    R_ASSERT3(F->length() >= sizeof h + m_nxa_payload_size && h.data_stride == nxa::stride_for(h.samples), "Truncated nxa file:", pName);

    ZeroMemory(&m_wformat, sizeof(WAVEFORMATEX));
    m_wformat.nSamplesPerSec = h.sample_rate;
    m_wformat.nChannels = u16(h.channels);
    m_wformat.wFormatTag = WAVE_FORMAT_PCM; // the CPU path decodes to PCM16 whatever supports_float_pcm says
    m_wformat.wBitsPerSample = 16;
    m_wformat.nBlockAlign = m_wformat.wBitsPerSample / 8 * m_wformat.nChannels;
    m_wformat.nAvgBytesPerSec = m_wformat.nSamplesPerSec * m_wformat.nBlockAlign;
    dwBytesTotal = h.samples * m_wformat.nBlockAlign;
    fTimeTotal = s_f_def_source_footer + dwBytesTotal / float(m_wformat.nAvgBytesPerSec);

    if (h.has_params)
    {
        m_fMinDist = h.min_dist;
        m_fMaxDist = h.max_dist;
        m_fBaseVolume = h.base_volume;
        m_uGameType = h.game_type;
        m_fMaxAIDist = h.max_ai_dist;
    }
    else
        Msg("! Missing ogg-comment, file: [%s]", pName);
    R_ASSERT3((m_fMaxAIDist >= 0.1f) && (m_fMaxDist >= 0.1f), "Invalid max distance.", pName);

    SoundRender->nxa_keep(this, static_cast<const u8*>(F->pointer()));
    FS.r_close(F);
}

namespace
{
// sounds\nxa_index.bin (SoundRender_Adpcm.h): read once, on the first source that asks; never changed after
struct NxaIndex
{
    std::mutex lock;
    std::atomic<bool> loaded{};
    std::unordered_map<std::string, nxa::IndexEntry> map;
} g_nxa_index;

const nxa::IndexEntry* nxa_index_find(const char* key)
{
    if (!g_nxa_index.loaded.load(std::memory_order_acquire))
    {
        std::scoped_lock lk(g_nxa_index.lock);
        if (!g_nxa_index.loaded.load(std::memory_order_relaxed))
        {
            string_path fn;
            FS.update_path(fn, "$game_sounds$", "nxa_index.bin");
            if (FS.exist(fn))
            {
                IReader* F = FS.r_open(fn);
                u32 hdr[3]{};
                if (F && F->length() >= sizeof hdr)
                    F->r(hdr, sizeof hdr);
                if (hdr[0] == nxa::index_magic && hdr[1] == nxa::index_version)
                {
                    g_nxa_index.map.reserve(hdr[2]);
                    for (u32 i = 0; i < hdr[2] && F->elapsed() >= 2; ++i)
                    {
                        const u16 len = F->r_u16();
                        std::string k(static_cast<const char*>(F->pointer()), len);
                        F->advance(len);
                        nxa::IndexEntry e;
                        F->r(&e, sizeof e);
                        g_nxa_index.map.emplace(std::move(k), e);
                    }
                    Msg("~ [snd] nxa index: %u sounds", u32(g_nxa_index.map.size()));
                }
                else
                    Msg("! [snd] nxa index: bad header in [%s]", fn);
                if (F)
                    FS.r_close(F);
            }
            g_nxa_index.loaded.store(true, std::memory_order_release);
        }
    }
    auto it = g_nxa_index.map.find(key);
    return it == g_nxa_index.map.end() ? nullptr : &it->second;
}
} // namespace

// the header from the index, the payload later (CSoundRender_Core::nxa_prefetch, or the first play)
void CSoundRender_Source::LoadNxaIndexed(LPCSTR pName, const nxa::IndexEntry& h)
{
    pname = pName;
    m_nxa = true;
    m_nxa_samples = h.samples;
    m_nxa_stride = h.data_stride;
    m_nxa_payload_size = nxa::payload_size(h.channels, h.samples);

    ZeroMemory(&m_wformat, sizeof(WAVEFORMATEX));
    m_wformat.nSamplesPerSec = h.sample_rate;
    m_wformat.nChannels = u16(h.channels);
    m_wformat.wFormatTag = WAVE_FORMAT_PCM;
    m_wformat.wBitsPerSample = 16;
    m_wformat.nBlockAlign = m_wformat.wBitsPerSample / 8 * m_wformat.nChannels;
    m_wformat.nAvgBytesPerSec = m_wformat.nSamplesPerSec * m_wformat.nBlockAlign;
    dwBytesTotal = h.samples * m_wformat.nBlockAlign;
    fTimeTotal = s_f_def_source_footer + dwBytesTotal / float(m_wformat.nAvgBytesPerSec);
    if (h.has_params)
    {
        m_fMinDist = h.min_dist;
        m_fMaxDist = h.max_dist;
        m_fBaseVolume = h.base_volume;
        m_uGameType = h.game_type;
        m_fMaxAIDist = h.max_ai_dist;
    }
    R_ASSERT3((m_fMaxAIDist >= 0.1f) && (m_fMaxDist >= 0.1f), "Invalid max distance.", pName);
    SoundRender->nxa_prefetch(this);
}

// a long .ogg from the index: what LoadWave would read from its header, without opening it (it is opened when it
// plays, off the sound thread on the Switch)
void CSoundRender_Source::LoadOggIndexed(LPCSTR pName, const nxa::IndexEntry& h)
{
    pname = pName;
    ZeroMemory(&m_wformat, sizeof(WAVEFORMATEX));
    m_wformat.nSamplesPerSec = h.sample_rate;
    m_wformat.nChannels = u16(h.channels);
    if (SoundRender->supports_float_pcm)
    {
        m_wformat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        m_wformat.wBitsPerSample = 32;
    }
    else
    {
        m_wformat.wFormatTag = WAVE_FORMAT_PCM;
        m_wformat.wBitsPerSample = 16;
    }
    m_wformat.nBlockAlign = m_wformat.wBitsPerSample / 8 * m_wformat.nChannels;
    m_wformat.nAvgBytesPerSec = m_wformat.nSamplesPerSec * m_wformat.nBlockAlign;
    dwBytesTotal = h.samples * m_wformat.nBlockAlign;
    fTimeTotal = s_f_def_source_footer + dwBytesTotal / float(m_wformat.nAvgBytesPerSec);
    if (h.has_params)
    {
        m_fMinDist = h.min_dist;
        m_fMaxDist = h.max_dist;
        m_fBaseVolume = h.base_volume;
        m_uGameType = h.game_type;
        m_fMaxAIDist = h.max_ai_dist;
    }
    R_ASSERT3((m_fMaxAIDist >= 0.1f) && (m_fMaxDist >= 0.1f), "Invalid max distance.", pName);
}

void CSoundRender_Source::load(LPCSTR name)
{
    string_path fn, N;
    xr_strcpy(N, name);
    _strlwr(N);
    if (strext(N))
        *strext(N) = 0;

    fname = N;

    if (snd_nxa)
    {
        strconcat(sizeof(fn), fn, N, ".nxa");
        string_path fo;
        strconcat(sizeof(fo), fo, N, ".ogg");
        // a level's own sound (levels\<name>\) wins as before; everything archived comes from the index, no file opened
        if (!FS.exist("$level$", fn) && !FS.exist("$level$", fo))
        {
            string_path key;
            xr_strcpy(key, N);
            for (char* c = key; *c; ++c)
                if (*c == '/')
                    *c = '\\';
            if (const nxa::IndexEntry* e = nxa_index_find(key))
            {
                if (e->format == 0)
                {
                    FS.update_path(fn, "$game_sounds$", fn);
                    LoadNxaIndexed(fn, *e);
                }
                else
                {
                    FS.update_path(fo, "$game_sounds$", fo);
                    LoadOggIndexed(fo, *e);
                }
                SoundRender->cache.cat_create(CAT, dwBytesTotal);
                return;
            }
            FS.update_path(fn, "$game_sounds$", fn);
        }
        if (FS.exist(fn))
        {
            LoadNxa(fn);
            SoundRender->cache.cat_create(CAT, dwBytesTotal);
            return;
        }
    }

    strconcat(sizeof(fn), fn, N, ".ogg");
    if (!FS.exist("$level$", fn))
        FS.update_path(fn, "$game_sounds$", fn);

    ASSERT_FMT_DBG(FS.exist(fn), "! Can't find sound [%s.ogg]", N);
    if (!FS.exist(fn))
        FS.update_path(fn, "$game_sounds$", "$no_sound.ogg");

    LoadWave(fn);
    SoundRender->cache.cat_create(CAT, dwBytesTotal);
}

void CSoundRender_Source::unload()
{
    if (m_nxa)
        SoundRender->nxa_drop(this);
    SoundRender->cache.cat_destroy(CAT);
    fTimeTotal = 0.0f;
    dwBytesTotal = 0;
}
