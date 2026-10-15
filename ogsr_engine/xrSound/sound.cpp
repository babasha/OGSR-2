#include "stdafx.h"

#include "SoundRender_CoreA.h"
#ifdef __SWITCH__
#include "SoundRender_CoreNX.h"
#endif

XRSOUND_API xr_token* snd_devices_token = nullptr;
XRSOUND_API u32 snd_device_id = u32(-1);

CSoundRender_Core* impl = nullptr;

void CSound_manager_interface::_create(int stage)
{
    if (stage == 0)
    {
#ifdef __SWITCH__
        // the console's audio DSP (SoundRender_CoreNX.h); -snd_al = OpenAL over audout, as before 11-10-2026
        if (strstr(Core.Params, "-snd_al"))
            impl = xr_new<CSoundRender_CoreA>();
        else
            impl = xr_new<CSoundRender_CoreNX>();
#else
        impl = xr_new<CSoundRender_CoreA>();
#endif
        Sound = SoundRender = impl;

        if (strstr(Core.Params, "-nosound"))
        {
            SoundRender->bPresent = FALSE;
            return;
        }

        SoundRender->bPresent = TRUE; // make assumption that sound present
    }

    if (!SoundRender->bPresent)
        return;

    SoundRender->_initialize(stage);
}

void CSound_manager_interface::_destroy()
{
    SoundRender->_clear();
    SoundRender = nullptr;

    Sound = nullptr;

    xr_delete(impl);
}
