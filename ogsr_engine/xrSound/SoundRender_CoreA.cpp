#include "stdafx.h"

#include "SoundRender_CoreA.h"
#include "SoundRender_TargetA.h"

#include <efx.h>

CSoundRender_CoreA::CSoundRender_CoreA()
{
    pDevice = nullptr;
    pDeviceList = nullptr;
    pContext = nullptr;
    eaxSet = nullptr;
    eaxGet = nullptr;
}

CSoundRender_CoreA::~CSoundRender_CoreA() {}

BOOL CSoundRender_CoreA::EAXQuerySupport(BOOL bDeferred, const GUID* guid, u32 prop, void* val, u32 sz)
{
    if (AL_NO_ERROR != eaxGet(guid, prop, 0, val, sz))
        return FALSE;
    if (AL_NO_ERROR != eaxSet(guid, (bDeferred ? DSPROPERTY_EAXLISTENER_DEFERRED : 0) | prop, 0, val, sz))
        return FALSE;
    return TRUE;
}

BOOL CSoundRender_CoreA::EAXTestSupport(BOOL bDeferred)
{
    EAXLISTENERPROPERTIES ep;
    if (!EAXQuerySupport(bDeferred, &DSPROPSETID_EAX_ListenerProperties, DSPROPERTY_EAXLISTENER_ROOM, &ep.lRoom, sizeof(LONG)))
        return FALSE;
    if (!EAXQuerySupport(bDeferred, &DSPROPSETID_EAX_ListenerProperties, DSPROPERTY_EAXLISTENER_ROOMHF, &ep.lRoomHF, sizeof(LONG)))
        return FALSE;
    if (!EAXQuerySupport(bDeferred, &DSPROPSETID_EAX_ListenerProperties, DSPROPERTY_EAXLISTENER_ROOMROLLOFFFACTOR, &ep.flRoomRolloffFactor, sizeof(float)))
        return FALSE;
    if (!EAXQuerySupport(bDeferred, &DSPROPSETID_EAX_ListenerProperties, DSPROPERTY_EAXLISTENER_DECAYTIME, &ep.flDecayTime, sizeof(float)))
        return FALSE;
    if (!EAXQuerySupport(bDeferred, &DSPROPSETID_EAX_ListenerProperties, DSPROPERTY_EAXLISTENER_DECAYHFRATIO, &ep.flDecayHFRatio, sizeof(float)))
        return FALSE;
    if (!EAXQuerySupport(bDeferred, &DSPROPSETID_EAX_ListenerProperties, DSPROPERTY_EAXLISTENER_REFLECTIONS, &ep.lReflections, sizeof(LONG)))
        return FALSE;
    if (!EAXQuerySupport(bDeferred, &DSPROPSETID_EAX_ListenerProperties, DSPROPERTY_EAXLISTENER_REFLECTIONSDELAY, &ep.flReflectionsDelay, sizeof(float)))
        return FALSE;
    if (!EAXQuerySupport(bDeferred, &DSPROPSETID_EAX_ListenerProperties, DSPROPERTY_EAXLISTENER_REVERB, &ep.lReverb, sizeof(LONG)))
        return FALSE;
    if (!EAXQuerySupport(bDeferred, &DSPROPSETID_EAX_ListenerProperties, DSPROPERTY_EAXLISTENER_REVERBDELAY, &ep.flReverbDelay, sizeof(float)))
        return FALSE;
    if (!EAXQuerySupport(bDeferred, &DSPROPSETID_EAX_ListenerProperties, DSPROPERTY_EAXLISTENER_ENVIRONMENTDIFFUSION, &ep.flEnvironmentDiffusion, sizeof(float)))
        return FALSE;
    if (!EAXQuerySupport(bDeferred, &DSPROPSETID_EAX_ListenerProperties, DSPROPERTY_EAXLISTENER_AIRABSORPTIONHF, &ep.flAirAbsorptionHF, sizeof(float)))
        return FALSE;
    if (!EAXQuerySupport(bDeferred, &DSPROPSETID_EAX_ListenerProperties, DSPROPERTY_EAXLISTENER_FLAGS, &ep.dwFlags, sizeof(DWORD)))
        return FALSE;
    return TRUE;
}

bool CSoundRender_CoreA::reopen_device(const char* deviceName) const
{
    if (alcIsExtensionPresent(pDevice, "ALC_SOFT_reopen_device"))
    {
        Msg("- snd has ALC_SOFT_reopen_device");

        typedef ALCboolean(ALC_APIENTRY * alcReopenDeviceSOFT_t)(ALCdevice*, const ALCchar*, const ALCint*);

        const alcReopenDeviceSOFT_t alcReopenDeviceSOFT = reinterpret_cast<alcReopenDeviceSOFT_t>(alcGetProcAddress(pDevice, "alcReopenDeviceSOFT"));

        if (alcReopenDeviceSOFT(pDevice, deviceName, nullptr))
        {
            return true;
        }
    }

    return false;
}

void CSoundRender_CoreA::_restart()
{
    if (!(bPresent && bReady))
        return;

    Msg("SOUND: restarting...");

    inherited::_restart();

    std::scoped_lock<std::mutex> m(m_bLocked);

    if (init_device_list())
    {
        pDeviceList->SelectBestDeviceId(/*notification_client.GetDefaultDeviceName().c_str()*/);

        R_ASSERT(snd_device_id >= 0 && snd_device_id < pDeviceList->GetNumDevices());
        const ALDeviceDesc& deviceDesc = pDeviceList->GetDeviceDesc(snd_device_id);

        // Same rule as init_context: following the system means a null name.
        if (reopen_device(psSoundFlags.test(ss_UseDefaultDevice) ? nullptr : deviceDesc.name))
        {
            const bool is_al_soft = deviceDesc.is_al_soft;

            for (u32 it = 0; it < s_targets.size(); it++)
            {
                CSoundRender_Target* T = s_targets[it];

                T->alAuxInit(AL_EFFECTSLOT_NULL);

                T->bEFX = false;
            }

            release_efx_objects();

            init_device_properties(is_al_soft);

            for (u32 it = 0; it < s_targets.size(); it++)
            {
                CSoundRender_Target* T = s_targets[it];

                if (bEFX)
                {
                    T->alAuxInit(slot);
                }

                T->bEFX = bEFX;
            }
        }
        else
        {
            Msg("! snd cannot reset device. Restart game to apply changes!");

            Msg("! snd last error %s", alcGetString(pDevice, alcGetError(pDevice)));
        }
    }
}

bool CSoundRender_CoreA::init_context(const ALDeviceDesc& deviceDesc)
{
    // ⭐⭐"Use the system default" has to mean handing OpenAL a NULL name, not a
    // name we resolved to ourselves and then opened.
    //
    // A device opened BY NAME is pinned to that hardware for the life of the
    // process. Windows' output moves to the headphones, every other program
    // follows, and the game keeps talking to the speakers -- with nothing on
    // screen to explain it and nothing in the log to find. That is the bug this
    // line fixes; snd_device_default was already the switch for it, it simply
    // did not reach as far as the open call.
    //
    // Passing null gives the question to OpenAL Soft, which is the layer that
    // actually knows what the system default is and when it changed.
    const bool follow_system = !!psSoundFlags.test(ss_UseDefaultDevice);
    const char* open_name = follow_system ? nullptr : deviceDesc.name;

    // alcOpenDevice can fail without any visible reason. Just try several times
    for (u32 i{}; i < 100; ++i)
    {
        pDevice = alcOpenDevice(open_name);
        if (pDevice != nullptr)
            break;
        else
            Sleep(1);
    }

    if (!pDevice)
    {
        FATAL("SOUND: OpenAL: Failed to create device. Error: [%s]", (LPCSTR)alGetString(alGetError()));

        bPresent = FALSE;
        return false;
    }

    // ⭐What was ACTUALLY opened, which is not always what was asked for -- and
    // is exactly the fact missing when somebody reports "the sound is coming out
    // of the wrong thing". Asking the opened device beats trusting our own list:
    // with a null name above, only OpenAL knows which one it chose.
    {
        const char* actual = alcGetString(pDevice, ALC_ALL_DEVICES_SPECIFIER);
        Msg("~~SOUND: OpenAL: playing on [%s]%s", actual ? actual : "unknown", follow_system ? " (system default, follows Windows)" : " (pinned by snd_device)");
    }

    // ⭐HRTF: the one modern audio feature this engine never asked for.
    //
    // The context was created with NO attributes at all, so OpenAL Soft was left
    // on its own defaults -- and its default is "enable HRTF only if Windows has
    // the endpoint flagged as headphones", which most headsets are not. The
    // result is that a game about listening for footsteps in the dark plays flat
    // stereo into headphones while the library shipped with the binaural
    // renderer sat there unused.
    //
    // snd_hrtf: 0 off, 1 on, 2 (default) let OpenAL decide as before.
    extern int snd_hrtf;
    ALCint attrs[3] = {0, 0, 0};
    if (snd_hrtf != 2)
    {
        attrs[0] = ALC_HRTF_SOFT;
        attrs[1] = (snd_hrtf == 1) ? ALC_TRUE : ALC_FALSE;
    }

    // Create context
    pContext = alcCreateContext(pDevice, attrs[0] ? attrs : nullptr);
    if (!pContext)
    {
        FATAL("SOUND: OpenAL: Failed to create context. Error: [%s]", (LPCSTR)alGetString(alGetError()));

        alcCloseDevice(pDevice);
        pDevice = nullptr;

        bPresent = FALSE;
        return false;
    }

    // clear errors
    alcGetError(pDevice);

    // Set active context
    AC_CHK(alcMakeContextCurrent(pContext));

    // clear errors
    alGetError();

    Msg("~[%s] OpenAL version: %s", __FUNCTION__, alGetString(AL_VERSION));

    // ⭐Asked for is not the same as got: OpenAL refuses HRTF on multichannel
    // outputs and when no dataset matches the sample rate. Report what actually
    // happened, or "I turned it on and hear no difference" has no answer.
    if (alcIsExtensionPresent(pDevice, "ALC_SOFT_HRTF"))
    {
        ALCint status = 0;
        alcGetIntegerv(pDevice, ALC_HRTF_STATUS_SOFT, 1, &status);
        const char* spec = alcGetString(pDevice, ALC_HRTF_SPECIFIER_SOFT);
        Msg("~~SOUND: HRTF %s (status %d, %s)", status == ALC_HRTF_ENABLED_SOFT ? "ENABLED" : "off", status, spec && spec[0] ? spec : "no dataset");
    }
    else
        Msg("~~SOUND: HRTF unavailable -- this OpenAL has no ALC_SOFT_HRTF");

    return true;
}

bool CSoundRender_CoreA::init_device_list()
{
    xr_delete(pDeviceList);
    pDeviceList = xr_new<ALDeviceList>();

    if (0 == pDeviceList->GetNumDevices())
    {
        xr_delete(pDeviceList);

        Msg("SOUND: OpenAL: Can't create sound device.");
        bPresent = FALSE;

        return false;
    }

    return true;
}

void CSoundRender_CoreA::init_device_properties(const bool& is_al_soft)
{
    if (is_al_soft)
    {
        bool efx = alcIsExtensionPresent(pDevice, "ALC_EXT_EFX") == AL_TRUE;

        if (efx)
        {
            InitAlEFXAPI();

            bEFX = EFXTestSupport();

            Msg("[OpenAL] EFX: %s", bEFX ? "present" : "absent");
        }
    }
    else
    {
        u32 eax{};

        if (alIsExtensionPresent("EAX5.0"))
            eax = 5;
        else if (alIsExtensionPresent("EAX4.0"))
            eax = 4;
        else if (alIsExtensionPresent("EAX3.0"))
            eax = 3;
        else if (alIsExtensionPresent("EAX2.0"))
            eax = 2;

        if (eax)
        {
            // Check for EAX extension
            bEAX = true;

            eaxSet = (EAXSet)alGetProcAddress("EAXSet");
            if (eaxSet == nullptr)
                bEAX = false;
            eaxGet = (EAXGet)alGetProcAddress("EAXGet");
            if (eaxGet == nullptr)
                bEAX = false;

            if (bEAX)
            {
                bDeferredEAX = EAXTestSupport(TRUE);
                bEAX = EAXTestSupport(FALSE);
            }

            Msg("[OpenAL] EAX 2.0 extension: %s", bEAX ? "present" : "absent");
            Msg("[OpenAL] EAX 2.0 deferred: %s", bDeferredEAX ? "present" : "absent");
        }
    }
}

#define AL_STOP_SOURCES_ON_DISCONNECT_SOFT 0x19AB  // not in public yet

void CSoundRender_CoreA::_initialize(int stage)
{
    if (stage == 0)
    {
        init_device_list();

        return;
    }

    pDeviceList->SelectBestDeviceId(/*notification_client.GetDefaultDeviceName().c_str()*/);

    R_ASSERT(snd_device_id >= 0 && snd_device_id < pDeviceList->GetNumDevices());
    const ALDeviceDesc& deviceDesc = pDeviceList->GetDeviceDesc(snd_device_id);

    if (!init_context(deviceDesc))
        return;

    // initialize listener
    A_CHK(alListener3f(AL_POSITION, 0.f, 0.f, 0.f));
    A_CHK(alListener3f(AL_VELOCITY, 0.f, 0.f, 0.f));
    constexpr Fvector orient[2] = {{0.f, 0.f, 1.f}, {0.f, 1.f, 0.f}};
    A_CHK(alListenerfv(AL_ORIENTATION, &orient[0].x));
    A_CHK(alListenerf(AL_GAIN, 1.f));

#ifdef AL_EXT_float32
    supports_float_pcm = snd_enable_float_pcm && alIsExtensionPresent("AL_EXT_FLOAT32");
#endif

    if (supports_float_pcm)
        Msg("~ xrSound has supports_float_pcm!");

    alDisable(AL_STOP_SOURCES_ON_DISCONNECT_SOFT); // not in public yet

    ALenum err = alGetError();
    if (err != AL_NO_ERROR)
    {
        Msg("!![%s] OpenAL AL_STOP_SOURCES_ON_DISCONNECT_SOFT error: %s", __FUNCTION__, alGetString(err));
    }

    const bool is_al_soft = deviceDesc.is_al_soft;
    
    init_device_properties(is_al_soft);

    inherited::_initialize(stage);

    if (stage == 1) // first initialize
    {
        // Pre-create targets
        CSoundRender_Target* T = nullptr;
        for (u32 tit = 0; tit < u32(psSoundTargets); tit++)
        {
            T = xr_new<CSoundRender_TargetA>();
            if (T->_initialize())
            {
                if (bEFX)

                {
                    T->alAuxInit(slot);
                }

                T->bAlSoft = is_al_soft;
                T->bEFX = bEFX;

                s_targets.push_back(T);
            }
            else
            {
                Msg("! SOUND: OpenAL: Max targets - [%u]", tit);
                T->_destroy();
                xr_delete(T);
                break;
            }
        }
    }

    bReady = TRUE;
    //notification_client.Start();
}

void CSoundRender_CoreA::set_master_volume(float f)
{
    if (bPresent)
    {
        A_CHK(alListenerf(AL_GAIN, f));
    }
}

void CSoundRender_CoreA::release_context()
{
    if (bEFX)
        release_efx_objects();

    // Reset the current context to NULL.
    alcMakeContextCurrent(nullptr);

    // Release the context and the device.
    alcDestroyContext(pContext);
    pContext = nullptr;
    alcCloseDevice(pDevice);
    pDevice = nullptr;
}

void CSoundRender_CoreA::_clear()
{
    //notification_client.Stop();
    bReady = FALSE;

    inherited::_clear();

    // remove targets
    CSoundRender_Target* T = nullptr;
    for (u32 tit = 0; tit < s_targets.size(); tit++)
    {
        T = s_targets[tit];
        T->_destroy();
        xr_delete(T);
    }

    release_context();

    xr_delete(pDeviceList);
}

void CSoundRender_CoreA::i_eax_set(const GUID* guid, u32 prop, void* val, u32 sz) { eaxSet(guid, prop, 0, val, sz); }
void CSoundRender_CoreA::i_eax_get(const GUID* guid, u32 prop, void* val, u32 sz) { eaxGet(guid, prop, 0, val, sz); }

void CSoundRender_CoreA::update_listener(const Fvector& P, const Fvector& D, const Fvector& N, float dt)
{
    inherited::update_listener(P, D, N, dt);

    if (!Listener.position.similar(P))
    {
        Listener.position.set(P);
        bListenerMoved = TRUE;
    }
    Listener.orientation[0].set(D.x, D.y, -D.z);
    Listener.orientation[1].set(N.x, N.y, -N.z);

    A_CHK(alListener3f(AL_POSITION, Listener.position.x, Listener.position.y, -Listener.position.z));
    A_CHK(alListener3f(AL_VELOCITY, 0.f, 0.f, 0.f));
    A_CHK(alListenerfv(AL_ORIENTATION, &Listener.orientation[0].x));
}
