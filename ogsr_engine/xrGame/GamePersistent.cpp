#include "StdAfx.h"
#include "GamePersistent.h"
#include "../xr_3da/fmesh.h"
#include "../xr_3da/XR_IOConsole.h"
#include "../xr_3da/GameMtlLib.h"
#include "../Include/xrRender/Kinematics.h"

#include "MainMenu.h"
#include "UICursor.h"
#include "game_base_space.h"
#include "Level.h"
#include "ParticlesObject.h"
#include "Actor.h"
#include "game_base_space.h"
#include "stalker_animation_data_storage.h"
#include "stalker_velocity_holder.h"
#include "ActorEffector.h"
#include "Actor.h"
#include "ui/UILoadingScreen.h"
#include "../xr_3da/x_ray.h"
#include "string_table.h"
#include "HUDManager.h"
#include "../xr_3da/DiscordRPC.hpp"

#ifndef MASTER_GOLD
#include "CustomMonster.h"
#endif // MASTER_GOLD

#include "ai_debug.h"
#include "cl_console_watch.h"

static void* ode_alloc(size_t size) { return xr_malloc(size); }
static void* ode_realloc(void* ptr, size_t oldsize, size_t newsize) { return xr_realloc(ptr, newsize); }
static void ode_free(void* ptr, size_t size) { return xr_free(ptr); }

#include "script_engine.h"
#include "ai_space.h"
// Engine-side guards catch raw LuaJIT unwinds only as `catch(...)`; the error text
// sits on the lua stack top. This hook lets them name the script error they caught.
static const char* lua_error_peek()
{
    try
    {
        lua_State* L = ai().script_engine().lua();
        if (L && lua_gettop(L) > 0 && lua_isstring(L, -1))
            return lua_tostring(L, -1);
        return "<no lua error on stack>";
    }
    catch (...)
    {
        return "<lua peek failed>";
    }
}

CGamePersistent::CGamePersistent(void)
{
    g_lua_error_peek = &lua_error_peek;

    m_game_params.m_e_game_type = GAME_ANY;
    ambient_effect_next_time = 0;
    ambient_effect_stop_time = 0;
    ambient_particles = 0;

    ambient_effect_wind_start = 0.f;
    ambient_effect_wind_in_time = 0.f;
    ambient_effect_wind_end = 0.f;
    ambient_effect_wind_out_time = 0.f;
    ambient_effect_wind_on = false;

    m_pUI_core = NULL;
    m_pMainMenu = NULL;
    m_intro = NULL;
    m_intro_event.bind(this, &CGamePersistent::start_logo_intro);
#ifdef DEBUG
    m_frame_counter = 0;
    m_last_stats_frame = u32(-2);
#endif
    //
    dSetAllocHandler(ode_alloc);
    dSetReallocHandler(ode_realloc);
    dSetFreeHandler(ode_free);

    //
    BOOL bDemoMode = (0 != strstr(Core.Params, "-demomode "));
    if (bDemoMode)
    {
        string256 fname;
        LPCSTR name = strstr(Core.Params, "-demomode ") + 10;
        sscanf(name, "%s", fname);
        R_ASSERT2(fname[0], "Missing filename for 'demomode'");
        Msg("- playing in demo mode '%s'", fname);
        pDemoFile = FS.r_open(fname);
        Device.seqFrame.Add(this);
        eDemoStart = Engine.Event.Handler_Attach("GAME:demo", this);
        uTime2Change = 0;
    }
    else
    {
        pDemoFile = NULL;
        eDemoStart = NULL;
    }

    eQuickLoad = Engine.Event.Handler_Attach("Game:QuickLoad", this);
}

CGamePersistent::~CGamePersistent(void)
{
    FS.r_close(pDemoFile);
    Device.seqFrame.Remove(this);
    Engine.Event.Handler_Detach(eDemoStart, this);
    Engine.Event.Handler_Detach(eQuickLoad, this);
}

void CGamePersistent::RegisterModel(IRenderVisual* V)
{
    // Check types
    switch (V->getType())
    {
    case MT_SKELETON_ANIM:
    case MT_SKELETON_RIGID: {
        u16 def_idx = GMLib.GetMaterialIdx("default_object");
        R_ASSERT2(GMLib.GetMaterialByIdx(def_idx)->Flags.is(SGameMtl::flDynamic), "'default_object' - must be dynamic");
        IKinematics* K = smart_cast<IKinematics*>(V);
        VERIFY(K);
        int cnt = K->LL_BoneCount();
        for (u16 k = 0; k < cnt; k++)
        {
            CBoneData& bd = K->LL_GetData(k);
            if (*(bd.game_mtl_name))
            {
                bd.game_mtl_idx = GMLib.GetMaterialIdx(*bd.game_mtl_name);
                R_ASSERT2(GMLib.GetMaterialByIdx(bd.game_mtl_idx)->Flags.is(SGameMtl::flDynamic), "Required dynamic game material");
            }
            else
            {
                bd.game_mtl_idx = def_idx;
            }
        }
    }
    break;
    }
}

extern void clean_game_globals();
extern void init_game_globals();

void CGamePersistent::OnAppStart()
{
    // load game materials
    GMLib.Load();
    init_game_globals();
    IGame_Persistent::OnAppStart();
    m_pUI_core = xr_new<ui_core>();
    m_pMainMenu = xr_new<CMainMenu>();
}

void CGamePersistent::OnAppEnd()
{
    if (m_pMainMenu->IsActive())
        m_pMainMenu->Activate(false);

    xr_delete(m_pMainMenu);
    xr_delete(m_pUI_core);

    IGame_Persistent::OnAppEnd();

    clean_game_globals();

    GMLib.Unload();
}

void CGamePersistent::PreStart(LPCSTR op)
{
    pApp->SetLoadingScreen(xr_new<UILoadingScreen>());
    IGame_Persistent::PreStart(op);
}

void CGamePersistent::Start(LPCSTR op)
{
    IGame_Persistent::Start(op);
    m_intro_event.bind(this, &CGamePersistent::start_game_intro);
}

void CGamePersistent::Disconnect()
{
    // destroy ambient particles
    CParticlesObject::Destroy(ambient_particles);

    IGame_Persistent::Disconnect();
    // stop all played emitters
    ::Sound->stop_emitters();

    // validate and clean up spatial dbs
    g_SpatialSpace->clear();
    g_SpatialSpacePhysic->clear();

    m_game_params.m_e_game_type = GAME_ANY;
}

#include "xr_level_controller.h"

void CGamePersistent::OnGameStart()
{
    IGame_Persistent::OnGameStart();

    UpdateGameType();
}

void CGamePersistent::UpdateGameType()
{
    IGame_Persistent::UpdateGameType();
    m_game_params.m_e_game_type = GAME_SINGLE;

    ASSERT_FMT_DBG(!xr_strcmp(m_game_params.m_game_type, "single"), "!!failed to parse the name of the save, rename it and try to load again.");
}

void CGamePersistent::OnGameEnd()
{
    IGame_Persistent::OnGameEnd();

    xr_delete(g_stalker_animation_data_storage);
    xr_delete(g_stalker_velocity_holder);
}

void CGamePersistent::WeathersUpdate()
{
    if (g_pGamePersistent->Environment().USED_COP_WEATHER)
    {
        if (g_pGameLevel)
        {
            const bool bIndoor = g_pGamePersistent->IsActorInHideout();

            int data_set = (Random.randF() < (1.f - Environment().CurrentEnv->weight)) ? 0 : 1;

            CEnvDescriptor* const current_env = Environment().Current[0];
            VERIFY(current_env);

            CEnvDescriptor* const _env = Environment().Current[data_set];
            VERIFY(_env);

            CEnvAmbient* env_amb = _env->env_ambient;
            if (env_amb)
            {
                CEnvAmbient::SSndChannelVec& vec = current_env->env_ambient->get_snd_channels();
                CEnvAmbient::SSndChannelVecIt I = vec.begin();
                CEnvAmbient::SSndChannelVecIt E = vec.end();

                for (u32 idx = 0; I != E; ++I, ++idx)
                {
                    CEnvAmbient::SSndChannel& ch = **I;
                    R_ASSERT(idx < 40);
                    if (ambient_sound_next_time[idx] == 0) // first
                    {
                        ambient_sound_next_time[idx] = Device.dwTimeGlobal + ch.get_rnd_sound_first_time();
                    }
                    else if (Device.dwTimeGlobal > ambient_sound_next_time[idx])
                    {
                        ref_sound& snd = ch.get_rnd_sound();

                        Fvector pos;
                        float angle = ::Random.randF(PI_MUL_2);
                        pos.x = _cos(angle);
                        pos.y = 0;
                        pos.z = _sin(angle);
                        pos.normalize().mul(ch.get_rnd_sound_dist()).add(Device.vCameraPosition);
                        pos.y += 10.f;
                        snd.play_at_pos(0, pos);

#ifdef DEBUG
                        if (!snd._handle() && strstr(Core.Params, "-nosound"))
                            continue;
#endif // DEBUG

                        VERIFY(snd._handle());
                        u32 _length_ms = iFloor(snd.get_length_sec() * 1000.0f);
                        ambient_sound_next_time[idx] = Device.dwTimeGlobal + _length_ms + ch.get_rnd_sound_time();
                        //					Msg("- Playing ambient sound channel [%s] file[%s]",ch.m_load_section.c_str(),snd._handle()->file_name());
                    }
                }
                /*
                            if (Device.dwTimeGlobal > ambient_sound_next_time)
                            {
                                ref_sound* snd			= env_amb->get_rnd_sound();
                                ambient_sound_next_time	= Device.dwTimeGlobal + env_amb->get_rnd_sound_time();
                                if (snd)
                                {
                                    Fvector	pos;
                                    float	angle		= ::Random.randF(PI_MUL_2);
                                    pos.x				= _cos(angle);
                                    pos.y				= 0;
                                    pos.z				= _sin(angle);
                                    pos.normalize		().mul(env_amb->get_rnd_sound_dist()).add(Device.vCameraPosition);
                                    pos.y				+= 10.f;
                                    snd->play_at_pos	(0,pos);
                                }
                            }
                */
                // start effect
                if (!bIndoor && !ambient_particles && Device.dwTimeGlobal > ambient_effect_next_time)
                {
                    CEnvAmbient::SEffect* eff = env_amb->get_rnd_effect();
                    if (eff)
                    {
                        Environment().wind_gust_factor = eff->wind_gust_factor;
                        ambient_effect_next_time = Device.dwTimeGlobal + env_amb->get_rnd_effect_time();
                        ambient_effect_stop_time = Device.dwTimeGlobal + eff->life_time;
                        ambient_effect_wind_start = Device.fTimeGlobal;
                        ambient_effect_wind_in_time = Device.fTimeGlobal + eff->wind_blast_in_time;
                        ambient_effect_wind_end = Device.fTimeGlobal + eff->life_time / 1000.f;
                        ambient_effect_wind_out_time = Device.fTimeGlobal + eff->life_time / 1000.f + eff->wind_blast_out_time;
                        ambient_effect_wind_on = true;

                        ambient_particles = CParticlesObject::Create(eff->particles.c_str(), FALSE, false);
                        Fvector pos;
                        pos.add(Device.vCameraPosition, eff->offset);
                        ambient_particles->play_at_pos(pos);
                        if (eff->sound._handle())
                            eff->sound.play_at_pos(0, pos);

                        Environment().wind_blast_strength_start_value = Environment().wind_strength_factor;
                        Environment().wind_blast_strength_stop_value = eff->wind_blast_strength;

                        if (Environment().wind_blast_strength_start_value == 0.f)
                        {
                            Environment().wind_blast_start_time.set(0.f, eff->wind_blast_direction.x, eff->wind_blast_direction.y, eff->wind_blast_direction.z);
                        }
                        else
                        {
                            Environment().wind_blast_start_time.set(0.f, Environment().wind_blast_direction.x, Environment().wind_blast_direction.y,
                                                                    Environment().wind_blast_direction.z);
                        }
                        Environment().wind_blast_stop_time.set(0.f, eff->wind_blast_direction.x, eff->wind_blast_direction.y, eff->wind_blast_direction.z);
                    }
                }
                else if (!ambient_particles && Device.dwTimeGlobal > ambient_effect_next_time)
                {
                    CEnvAmbient::SEffect* eff = env_amb->get_rnd_effect();
                    if (eff)
                        ambient_effect_next_time = Device.dwTimeGlobal + env_amb->get_rnd_effect_time();
                }
            }
            if (Device.fTimeGlobal >= ambient_effect_wind_start && Device.fTimeGlobal <= ambient_effect_wind_in_time && ambient_effect_wind_on)
            {
                float delta = ambient_effect_wind_in_time - ambient_effect_wind_start;
                float t;
                if (delta != 0.f)
                {
                    float cur_in = Device.fTimeGlobal - ambient_effect_wind_start;
                    t = cur_in / delta;
                }
                else
                {
                    t = 0.f;
                }
                Environment().wind_blast_current.slerp(Environment().wind_blast_start_time, Environment().wind_blast_stop_time, t);

                Environment().wind_blast_direction.set(Environment().wind_blast_current.x, Environment().wind_blast_current.y, Environment().wind_blast_current.z);
                Environment().wind_strength_factor =
                    Environment().wind_blast_strength_start_value + t * (Environment().wind_blast_strength_stop_value - Environment().wind_blast_strength_start_value);
            }

            // stop if time exceed or indoor
            if (bIndoor || Device.dwTimeGlobal >= ambient_effect_stop_time)
            {
                if (ambient_particles)
                    ambient_particles->Stop();

                Environment().wind_gust_factor = 0.f;
            }

            if (Device.fTimeGlobal >= ambient_effect_wind_end && ambient_effect_wind_on)
            {
                Environment().wind_blast_strength_start_value = Environment().wind_strength_factor;
                Environment().wind_blast_strength_stop_value = 0.f;

                ambient_effect_wind_on = false;
            }

            if (Device.fTimeGlobal >= ambient_effect_wind_end && Device.fTimeGlobal <= ambient_effect_wind_out_time)
            {
                float delta = ambient_effect_wind_out_time - ambient_effect_wind_end;
                float t;
                if (delta != 0.f)
                {
                    float cur_in = Device.fTimeGlobal - ambient_effect_wind_end;
                    t = cur_in / delta;
                }
                else
                {
                    t = 0.f;
                }
                Environment().wind_strength_factor =
                    Environment().wind_blast_strength_start_value + t * (Environment().wind_blast_strength_stop_value - Environment().wind_blast_strength_start_value);
            }
            if (Device.fTimeGlobal > ambient_effect_wind_out_time && ambient_effect_wind_out_time != 0.f)
            {
                Environment().wind_strength_factor = 0.0;
            }

            // if particles not playing - destroy
            if (ambient_particles && !ambient_particles->IsPlaying())
                CParticlesObject::Destroy(ambient_particles);
        }
    }
    else
    {
        if (g_pGameLevel)
        {
            const bool bIndoor = g_pGamePersistent->IsActorInHideout();

            int data_set = (Random.randF() < (1.f - Environment().CurrentEnv->weight)) ? 0 : 1;

            CEnvDescriptor* const _env = Environment().Current[data_set];
            VERIFY(_env);

            CEnvAmbient* env_amb = _env->env_ambient;

            // start sound
            if (env_amb)
            {
                if (Device.dwTimeGlobal > ambient_sound_next_time_shoc)
                {
                    ref_sound* snd = env_amb->get_rnd_sound();
                    ambient_sound_next_time_shoc = Device.dwTimeGlobal + env_amb->get_rnd_sound_time();
                    if (snd)
                    {
                        Fvector pos;
                        float angle = ::Random.randF(PI_MUL_2);
                        pos.x = _cos(angle);
                        pos.y = 0;
                        pos.z = _sin(angle);
                        pos.normalize().mul(env_amb->get_rnd_sound_dist()).add(Device.vCameraPosition);
                        pos.y += 10.f;
                        snd->play_at_pos(0, pos);
                    }
                }

                // start effect
                if (!bIndoor && !ambient_particles && Device.dwTimeGlobal > ambient_effect_next_time)
                {
                    CEnvAmbient::SEffect* eff = env_amb->get_rnd_effect();
                    if (eff)
                    {
                        Environment().wind_gust_factor = eff->wind_gust_factor;
                        ambient_effect_next_time = Device.dwTimeGlobal + env_amb->get_rnd_effect_time_shoc();
                        ambient_effect_stop_time = Device.dwTimeGlobal + eff->life_time;
                        ambient_particles = CParticlesObject::Create(eff->particles.c_str(), FALSE, false);
                        Fvector pos;
                        pos.add(Device.vCameraPosition, eff->offset);
                        ambient_particles->play_at_pos(pos);
                        if (eff->sound._handle())
                            eff->sound.play_at_pos(0, pos);
                    }
                }
                else if (!ambient_particles && Device.dwTimeGlobal > ambient_effect_next_time)
                {
                    CEnvAmbient::SEffect* eff = env_amb->get_rnd_effect();
                    if (eff)
                        ambient_effect_next_time = Device.dwTimeGlobal + env_amb->get_rnd_effect_time_shoc();
                }
            }
            // stop if time exceed or indoor
            if (bIndoor || Device.dwTimeGlobal >= ambient_effect_stop_time)
            {
                if (ambient_particles)
                    ambient_particles->Stop();
                Environment().wind_gust_factor = 0.f;
            }
            // if particles not playing - destroy
            if (ambient_particles && !ambient_particles->IsPlaying())
                CParticlesObject::Destroy(ambient_particles);
        }
    }
}

#include "ui/UIGameTutorial.h"

void CGamePersistent::start_logo_intro()
{
    if (!strstr(Core.Params, "-intro"))
    {
        m_intro_event.clear();
        // PHASE B (editor-on-Vulkan): -vk_editor boots straight into the no-menu
        // editor viewport (a VK render sink drives the frame) — don't raise the menu.
        if (strstr(Core.Params, "-vk_editor")) { Console->Hide(); return; }
        Console->Show();
        Console->Execute("main_menu on");
        return;
    }

    if (Device.dwPrecacheFrame == 0)
    {
        m_intro_event.bind(this, &CGamePersistent::update_logo_intro);
        if (0 == xr_strlen(m_game_params.m_game_or_spawn) && NULL == g_pGameLevel)
        {
            VERIFY(NULL == m_intro);
            m_intro = xr_new<CUISequencer>();
            m_intro->Start("intro_logo");
            Console->Hide();
        }
    }
}
void CGamePersistent::update_logo_intro()
{
    if (m_intro && (false == m_intro->IsActive()))
    {
        m_intro_event.clear();
        xr_delete(m_intro);
        Console->Execute("main_menu on");
    }
}

void CGamePersistent::start_game_intro()
{
    if (g_pGameLevel && g_pGameLevel->bReady && Device.dwPrecacheFrame <= 2)
    {
        m_intro_event.bind(this, &CGamePersistent::update_game_intro);
        if (0 == _stricmp(m_game_params.m_new_or_load, "new"))
        {
            VERIFY(NULL == m_intro);
            m_intro = xr_new<CUISequencer>();
            m_intro->Start("intro_game");
            // Msg("Intro start: [%u]", Device.dwFrame);
        }
    }
}
void CGamePersistent::update_game_intro()
{
    if (!m_intro)
        m_intro_event.clear();
    else if (!m_intro->IsActive())
    {
        xr_delete(m_intro);
        m_intro_event.clear();
    }
}

#include "holder_custom.h"
extern CUISequencer* g_tutorial;
extern CUISequencer* g_tutorial2;
extern bool RESET_SECTORS_HACK;

void CGamePersistent::OnFrame()
{
    // ⚠The command channel ticks HERE and not in CLevel::OnFrame, where it
    // started. There is no CLevel in the main menu, so a channel that lived
    // there could only ever be used once a game was already running -- which
    // made the one thing a script most needs to do, START a game, the one
    // thing it could not do. Found 29-07-2026 trying to load a save from a
    // harness: the client sat in the menu and the harness waited for a channel
    // that was never going to open.
    ClientConsoleWatch::Tick();

    if (g_tutorial2)
    {
        g_tutorial2->Destroy();
        xr_delete(g_tutorial2);
    }

    if (g_tutorial && !g_tutorial->IsActive())
    {
        xr_delete(g_tutorial);
    }

#ifdef DEBUG
    ++m_frame_counter;
#endif
    if (!m_intro_event.empty() && !load_screen_renderer.b_registered)
        m_intro_event();

    if (Device.dwPrecacheFrame == 0 && load_screen_renderer.b_registered && !GameAutopaused)
    {
        // Thin network client: skip the "press any key" autopause gate. It
        // holds Device.Pause over a live connected world (game timer frozen,
        // puppets static) behind the loading screen's fullscreen backdrop --
        // the 23-07 "static screen" in every unattended client run. A network
        // session enters the world immediately, like any MP game; fused SP
        // keeps the stock behavior.
        extern ENGINE_API bool g_net_client_st;
        if (psActorFlags.test(AF_KEYPRESS_ON_START) && !g_net_client_st)
        {
            Device.Pause(TRUE, TRUE, TRUE, "AUTOPAUSE_START");
            pApp->LoadForceFinish();
            LoadTitle("st_press_any_key");
            GameAutopaused = true;
        }
        else
        {
            load_screen_renderer.stop();
        }

        RESET_SECTORS_HACK = true;

        Discord.Update(CStringTable().translate(Level().name()).c_str(), Level().name().c_str());
    }

    if (!m_pMainMenu->IsActive())
        m_pMainMenu->DestroyInternal(false);

    // PHASE B (editor-on-Vulkan): with -vk_editor there is no level, so the normal
    // environment tick (__super::OnFrame -> Environment().OnFrame) never runs — the
    // early return below skips it. Drive a fixed midday environment ourselves so the
    // full HDR pipeline (sky, auto-exposure, tonemap, colour-grading) has valid
    // lighting; a degenerate CurrentEnv makes the tonemap crush the whole frame to
    // black. This teaches the editor to render THROUGH the real HDR path, as in-game.
    if (strstr(Core.Params, "-vk_editor"))
    {
        CEnvironment& env = Environment();
        static bool s_envInit = false;
        if (!s_envInit)
        {
            s_envInit = true;
            if (env.WeatherCycles.empty())
                env.load(); // OnAppStart normally does this; force it if it hasn't run
            if (!env.WeatherCycles.empty())
            {
                // Prefer a clear daytime cycle; fall back to the first available.
                shared_str wname = "default_np_clear";
                if (env.WeatherCycles.find(wname) == env.WeatherCycles.end())
                    wname = env.WeatherCycles.begin()->first;
                env.SetGameTime(12.f * 3600.f, 0.f); // noon, time frozen (factor 0)
                env.SetWeather(wname, true);
                Msg("[VK][editor] environment: weather='%s' @ noon (%u cycles)", wname.c_str(), (u32)env.WeatherCycles.size());
            }
            else
                Msg("![VK][editor] no weather cycles available — sky will stay degenerate");
        }
        env.SetGameTime(12.f * 3600.f, 0.f); // keep the sun pinned at noon
        env.OnFrame();
        return;
    }

    if (!g_pGameLevel)
        return;
    if (!g_pGameLevel->bReady)
        return;

    if (Device.Paused())
    {
#ifndef MASTER_GOLD
        if (Level().CurrentViewEntity())
        {
            if (!g_actor || (g_actor->ID() != Level().CurrentViewEntity()->ID()))
            {
                CCustomMonster* custom_monster = smart_cast<CCustomMonster*>(Level().CurrentViewEntity());
                if (custom_monster) // can be spectator in multiplayer
                    custom_monster->UpdateCamera();
            }
            else
            {
                CCameraBase* C = NULL;
                if (g_actor)
                {
                    if (!Actor()->Holder())
                        C = Actor()->cam_Active();
                    else
                        C = Actor()->Holder()->Camera();

                    Actor()->Cameras().UpdateFromCamera(C);
                    Actor()->Cameras().ApplyDevice();
                }
            }
        }
#else // MASTER_GOLD
        if (g_actor)
        {
            CCameraBase* C = NULL;
            if (!Actor()->Holder())
                C = Actor()->cam_Active();
            else
                C = Actor()->Holder()->Camera();

            Actor()->Cameras().UpdateFromCamera(C);
            Actor()->Cameras().ApplyDevice();
        }
#endif // MASTER_GOLD
    }
    IGame_Persistent::OnFrame();

    if (!Device.Paused())
        Engine.Sheduler.Update();

    // update weathers ambient
    if (!Device.Paused())
        WeathersUpdate();

    if (0 != pDemoFile)
    {
        if (Device.dwTimeGlobal > uTime2Change)
        {
            // Change level + play demo
            if (pDemoFile->elapsed() < 3)
                pDemoFile->seek(0); // cycle

            // Read params
            string512 params;
            pDemoFile->r_string(params, sizeof(params));
            string256 o_server, o_client, o_demo;
            u32 o_time;
            sscanf(params, "%[^,],%[^,],%[^,],%d", o_server, o_client, o_demo, &o_time);

            // Start _new level + demo
            Engine.Event.Defer("KERNEL:disconnect");
            Engine.Event.Defer("KERNEL:start", size_t(xr_strdup(_Trim(o_server))), size_t(xr_strdup(_Trim(o_client))));
            Engine.Event.Defer("GAME:demo", size_t(xr_strdup(_Trim(o_demo))), u64(o_time));
            uTime2Change = 0xffffffff; // Block changer until Event received
        }
    }

#ifdef DEBUG
    if ((m_last_stats_frame + 1) < m_frame_counter)
        profiler().clear();
#endif
}

#include "game_sv_single.h"
#include "xrServer.h"
#ifdef __SWITCH__
extern "C" void xr_nx_io_dump(const char* tag); // cmake/nx_compat/nx_libc_shims.c: [nx io] SD read accounting
#endif

void CGamePersistent::OnEvent(EVENT E, u64 P1, u64 P2)
{
    if (E == eQuickLoad)
    {
        if (Device.Paused())
            Device.Pause(FALSE, TRUE, TRUE, "eQuickLoad");

        LPSTR saved_name = (LPSTR)(P1);

        Level().remove_objects();
        game_sv_Single* game = smart_cast<game_sv_Single*>(Level().Server->game);
        R_ASSERT(game);
        game->restart_simulator(saved_name);
        xr_free(saved_name);
        return;
    }
    else if (E == eDemoStart)
    {
        string256 cmd;
        LPCSTR demo = LPCSTR(P1);
        sprintf_s(cmd, "demo_play %s", demo);
        Console->Execute(cmd);
        xr_free(demo);
        uTime2Change = Device.TimerAsync() + u32(P2) * 1000;
    }
}

void CGamePersistent::Statistics(CGameFont* F)
{
#ifdef DEBUG
    m_last_stats_frame = m_frame_counter;
    profiler().show_stats(F, !!psAI_Flags.test(aiStats));
#endif
}

float CGamePersistent::MtlTransparent(u32 mtl_idx) { return GMLib.GetMaterialByIdx((u16)mtl_idx)->fVisTransparencyFactor; }
static BOOL bRestorePause = FALSE;
static BOOL bEntryFlag = TRUE;

void CGamePersistent::OnAppActivate()
{
    Device.Pause(FALSE, !bRestorePause, TRUE, "CGP::OnAppActivate");

    bEntryFlag = TRUE;
}

void CGamePersistent::OnAppDeactivate()
{
    if (!bEntryFlag)
        return;

    bRestorePause = Device.Paused();
    Device.Pause(TRUE, TRUE, TRUE, "CGP::OnAppDeactivate");

    bEntryFlag = FALSE;
}

bool CGamePersistent::OnRenderPPUI_query()
{
    return MainMenu()->OnRenderPPUI_query();
    // enable PP or not
}

void CGamePersistent::OnRenderPPUI_main()
{
    // always
    MainMenu()->OnRenderPPUI_main();
}

void CGamePersistent::OnRenderPPUI_PP() { MainMenu()->OnRenderPPUI_PP(); }

void CGamePersistent::LoadTitle(const char* str)
{
    // TEMP load-phase profiling: log the wall time of the phase that just ENDED
    // (between consecutive LoadTitle markers) — finds the real load bottleneck
    // across cform / geometry / alife / ai / spawn / textures. The 4 render
    // phases (rvk_loader, unnamed) come through as '(render)' in load order:
    // 1=shaders 2=geometry 3=visuals 4=sectors+foliage+gpu-build. Remove after.
    {
        static CTimer    s_phaseTimer;
        static shared_str s_prevTitle;
        static bool       s_started = false;
        if (s_started)
            Msg("[load phase] '%s' = %u ms", s_prevTitle.c_str(), s_phaseTimer.GetElapsed_ms());
#ifdef __SWITCH__
            xr_nx_io_dump(s_prevTitle.c_str()); // the SD reads of the phase that just ended
#endif
        s_prevTitle = (str && str[0]) ? str : "(render)";
        s_started   = true;
        s_phaseTimer.Start();
    }

    const char* tittle = CStringTable().translate(str).c_str();
    pApp->SetLoadStageTitle(tittle);
    pApp->LoadStage();

    Discord.Update(tittle);
}

void CGamePersistent::SetTip() 
{
    pApp->LoadTitleInt();
}

bool CGamePersistent::OnKeyboardPress(int dik)
{
    if (psActorFlags.test(AF_KEYPRESS_ON_START) && GameAutopaused)
    {
        Device.Pause(FALSE, TRUE, TRUE, "AUTOPAUSE_END");
        load_screen_renderer.stop();
        GameAutopaused = false;
        return true;
    }

    return false;
}