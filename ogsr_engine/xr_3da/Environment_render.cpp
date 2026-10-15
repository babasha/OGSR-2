#include "stdafx.h"


#include "Environment.h"
#include "Render.h"
#include "xr_efflensflare.h"
#include "Rain.h"
#include "thunderbolt.h"
#include "IGame_Level.h"

//-----------------------------------------------------------------------------
// Environment render
//-----------------------------------------------------------------------------
// BOOL bNeed_re_create_env = FALSE;
void CEnvironment::RenderSky(CBackend& cmd_list)
{
    if (nullptr == g_pGameLevel)
        return;

    m_pRender->RenderSky(cmd_list , * this);
}

void CEnvironment::RenderClouds(CBackend& cmd_list)
{
    if (nullptr == g_pGameLevel)
        return;

    // draw clouds
    if (fis_zero(CurrentEnv->clouds_color.w, EPS_L))
        return;

    m_pRender->RenderClouds(cmd_list, *this);
}

void CEnvironment::OnDeviceCreate()
{
    m_pRender->OnDeviceCreate();

    // weathers
    {
        EnvsMapIt _I, _E;
        _I = WeatherCycles.begin();
        _E = WeatherCycles.end();
        for (; _I != _E; ++_I)
            for (auto& it : _I->second)
                it->on_device_create();
    }
    // effects
    {
        EnvsMapIt _I, _E;
        _I = WeatherFXs.begin();
        _E = WeatherFXs.end();
        for (; _I != _E; ++_I)
            for (auto& it : _I->second)
                it->on_device_create();
    }

    Invalidate();
    OnFrame();
}

void CEnvironment::OnDeviceDestroy()
{
    m_pRender->OnDeviceDestroy();

    // weathers
    {
        EnvsMapIt _I, _E;
        _I = WeatherCycles.begin();
        _E = WeatherCycles.end();
        for (; _I != _E; ++_I)
            for (auto& it : _I->second)
                it->on_device_destroy();
    }
    // effects
    {
        EnvsMapIt _I, _E;
        _I = WeatherFXs.begin();
        _E = WeatherFXs.end();
        for (; _I != _E; ++_I)
            for (auto& it : _I->second)
                it->on_device_destroy();
    }

    CurrentEnv->destroy();
}
