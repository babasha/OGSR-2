#pragma once

#include "SoundRender_Target.h"
#include "SoundRender_CoreA.h"

class CSoundRender_TargetA : public CSoundRender_Target
{
    typedef CSoundRender_Target inherited;

    // OpenAL
    ALuint pSource;
    ALuint pBuffers[sdef_target_count]{};

    float cache_gain{};
    float cache_pitch{};

    // snd_occ_mode 1: this source's low-pass (EFX), and the HF gain it was last set to
    ALuint pFilter{};
    float cache_hf{1.f};

    ALuint buf_block{};

    // snd_start_blocks < 3: buffers not queued yet at the start -- pBuffers[sdef_target_count - m_unqueued ...],
    // filled and queued one per update()
    u32 m_unqueued{};

private:
    void fill_block(ALuint BufferID);

public:
    CSoundRender_TargetA();
    virtual ~CSoundRender_TargetA();

    virtual BOOL _initialize();
    virtual void _destroy();
    virtual void _restart();

    virtual void start(CSoundRender_Emitter* E);
    virtual void render();
    virtual void rewind();
    virtual void stop();
    virtual void update();



    virtual void fill_parameters(CSoundRender_Core* core);

    virtual void alAuxInit(ALuint slot) override;
};
