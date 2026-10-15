#include "stdafx.h"

#include <chrono>

#include "SoundRender_Target.h"
#include "SoundRender_Core.h"
#include "SoundRender_Emitter.h"
#include "SoundRender_Source.h"
#include "SoundRender_Adpcm.h"

CSoundRender_Target::CSoundRender_Target(void)
{
    m_pEmitter = 0;
    rendering = FALSE;
    wave = 0;
}

CSoundRender_Target::~CSoundRender_Target(void) { VERIFY(wave == 0); }

BOOL CSoundRender_Target::_initialize()
{
    return TRUE;
}

void CSoundRender_Target::start(CSoundRender_Emitter* E)
{
    R_ASSERT(E);

    // *** Initial buffer startup ***
    // 1. Fill parameters
    // 4. Load 2 blocks of data (as much as possible)
    // 5. Deferred-play-signal (emitter-exist, rendering-false)
    m_pEmitter = E;
    rendering = FALSE;
    // attach		();
}

void CSoundRender_Target::render() { rendering = TRUE; }

void CSoundRender_Target::stop()
{
    dettach();
    m_pEmitter = NULL;
    rendering = FALSE;
}

void CSoundRender_Target::rewind() { R_ASSERT(rendering); }

void CSoundRender_Target::update() { R_ASSERT(m_pEmitter); }

void CSoundRender_Target::fill_parameters(CSoundRender_Core* core)
{
    VERIFY(m_pEmitter);
    //.	if (pEmitter->b2D){
    //.		pEmitter->set_position(SoundRender->listener_position());
    //.	}
}

extern int ov_seek_func(void* datasource, s64 offset, int whence);
extern size_t ov_read_func(void* ptr, size_t size, size_t nmemb, void* datasource);
extern int ov_close_func(void* datasource);
extern long ov_tell_func(void* datasource);

IReader* CSoundRender_Target::open_source(CSoundRender_Source* src, OggVorbis_File* o)
{
    constexpr ov_callbacks ovc = {ov_read_func, ov_seek_func, ov_close_func, ov_tell_func};
    IReader* w = FS.r_open(src->pname.c_str());
    R_ASSERT3(w && w->length(), "Can't open wave file:", src->pname.c_str());
    if (!src->m_nxa) // an .nxa needs no decoder state: the file itself is read (get_nxa_file)
        ov_open_callbacks(w, o, NULL, 0, ovc);
    return w;
}

void CSoundRender_Target::attach()
{
    VERIFY(0 == wave);
    VERIFY(m_pEmitter);
    const auto t0 = std::chrono::steady_clock::now();
    CSoundRender_Source* src = m_pEmitter->source();
    m_ovf = &ovf;
    m_wave_nxa = src->m_nxa;
    wave = open_source(src, &ovf);
    g_snd_prof.open_ms += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_snd_prof.opens++;
}

void CSoundRender_Target::adopt(IReader* w, OggVorbis_File* heap_ovf)
{
    VERIFY(0 == wave);
    wave = w;
    m_wave_nxa = heap_ovf == nullptr;
    m_ovf = heap_ovf ? heap_ovf : &ovf;
}

void CSoundRender_Target::dettach()
{
    if (wave)
    {
        if (!m_wave_nxa)
            ov_clear(m_ovf);
        if (m_ovf != &ovf)
            xr_delete(m_ovf);
        m_ovf = &ovf;
        FS.r_close(wave);
    }
}

const u8* CSoundRender_Target::get_nxa_file()
{
    if (!wave)
        attach();
    if (!wave || wave->length() < sizeof(nxa::FileHeader))
        return nullptr;
    return static_cast<const u8*>(wave->begin()) + sizeof(nxa::FileHeader);
}
