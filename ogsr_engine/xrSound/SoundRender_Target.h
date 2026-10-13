#pragma once

#include <al.h>
#include "SoundRender.h"

class CSoundRender_Target
{
protected:
    CSoundRender_Emitter* m_pEmitter;
    BOOL rendering;

public:
    float priority{};
    bool bAlSoft = false;
    bool bEFX = false;

protected:
    OggVorbis_File ovf{};
    OggVorbis_File* m_ovf{&ovf}; // the decoder state in use: ovf, or one opened off-thread (adopt)
    IReader* wave;
    bool m_wave_nxa{}; // wave is an .nxa (no ovf)
    void attach();
    void dettach();
    // a file opened elsewhere (CSoundRender_TargetNX: a worker thread) becomes this target's: w + its decoder state
    // (heap, owned from now on; null for an .nxa)
    void adopt(IReader* w, OggVorbis_File* heap_ovf);

public:
    // open a source's file the way attach does, into caller-owned state (any thread: touches no target)
    static IReader* open_source(CSoundRender_Source* src, OggVorbis_File* o);

    OggVorbis_File* get_data()
    {
        if (!wave)
            attach();
        return m_ovf;
    }
    // an .nxa source not resident: its payload, in the file this target holds open (snd_nxa, the CPU path)
    const u8* get_nxa_file();

public:
    CSoundRender_Target();
    virtual ~CSoundRender_Target();

    CSoundRender_Emitter* get_emitter() { return m_pEmitter; }
    BOOL get_Rendering() { return rendering; }

    virtual BOOL _initialize() = 0;
    virtual void _destroy() = 0;
    virtual void _restart() = 0;

    virtual void start(CSoundRender_Emitter* E) = 0;
    virtual void render() = 0;
    virtual void rewind() = 0;
    virtual void stop() = 0;
    virtual void update() = 0;

    virtual void alAuxInit(ALuint slot) = 0;

    // the emitter moved on to an attached tail (CSoundRender_Emitter::set_cursor): reopen the data
    virtual void source_changed()
    {
        dettach();
        attach();
    }

    virtual void fill_parameters(CSoundRender_Core* core) = 0;
};
