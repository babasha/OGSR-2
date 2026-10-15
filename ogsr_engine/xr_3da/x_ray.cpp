//-----------------------------------------------------------------------------
// File: x_ray.cpp
//
// Programmers:
//	Oles		- Oles Shishkovtsov
//	AlexMX		- Alexander Maksimchuk
//-----------------------------------------------------------------------------
#ifdef __SWITCH__
#include <unistd.h>
#endif
#include "stdafx.h"
#include "IGame_Level.h"
#include "IGame_Persistent.h"
#include "xr_input.h"
#include "XR_IOConsole.h"
#include "x_ray.h"
#include "std_classes.h"
#include "LightAnimLibrary.h"
#include "../xrCDB/ISpatial.h"
#include "ILoadingScreen.h"
#include "DiscordRPC.hpp"
#include "Render.h"
#include "splash.h"
#include "xr_update.h"
#include "../xrOnlineNet/NET_GNS_Transport.h" // -ticket
#include "../../3rd_party/Src/mimalloc/mimalloc/include/mimalloc.h" // -mi_guard (guarded heap sampling)
#ifdef __SWITCH__
extern "C" void xr_nx_exit_process(void); // nx/xr_nx_pad.c: ends the whole process (all threads)
#endif

#define CORE_FEATURE_SET(feature, section) Core.Features.set(xrCore::Feature::feature, READ_IF_EXISTS(pSettings, r_bool, section, #feature, false))

ENGINE_API CApplication* pApp{};
ENGINE_API bool IS_OGSR_GA{};
ENGINE_API CInifile* pGameIni{};
int max_load_stage{};

bool use_reshade{};
extern bool init_reshade();
extern void unregister_reshade();

// startup point
void InitEngine()
{
    Engine.Initialize();
    Device.Initialize();
}

void InitSettings()
{
    string_path fname;
    FS.update_path(fname, "$game_config$", "system.ltx");
    pSettings = xr_new<CInifile>(fname, TRUE);
    CHECK_OR_EXIT(!pSettings->sections().empty(), make_string("Cannot find file %s.\nReinstalling application may fix this problem.", fname));

    FS.update_path(fname, "$game_config$", "game.ltx");
    pGameIni = xr_new<CInifile>(fname, TRUE);
    CHECK_OR_EXIT(!pGameIni->sections().empty(), make_string("Cannot find file %s.\nReinstalling application may fix this problem.", fname));

    IS_OGSR_GA = strstr(READ_IF_EXISTS(pSettings, r_string, "mod_ver", "mod_ver", "nullptr"), "OGSR");

    // load custom shader params declarations

    // Simp: сюда добавлены параметры для актуальных шейдеров, в конфигах их держать не практично (потерялся параметр - и шейдер будет работать не правильно.)
    shader_exports.set_custom_params("shader_param_grayscale", {});

    shader_exports.set_custom_params("pnv_color_old", {});
    shader_exports.set_custom_params("pnv_params_old", {});

    shader_exports.set_custom_params("pnv_color", {});

    shader_exports.set_custom_params("heat_vision_steps", {});
    shader_exports.set_custom_params("heat_vision_blurring", {});
    shader_exports.set_custom_params("heat_fade_distance", {});

    shader_exports.set_custom_params("breath_size", {});
    shader_exports.set_custom_params("breath_idx", {});

    shader_exports.set_custom_params("gasmask_inertia", {});
    shader_exports.set_custom_params("device_inertia", {});

    shader_exports.set_custom_params("mark_number", {});
    shader_exports.set_custom_params("mark_color", {});

    shader_exports.set_custom_params("s3ds_param_1", {});
    shader_exports.set_custom_params("s3ds_param_2", {});
    shader_exports.set_custom_params("s3ds_param_3", {});
    shader_exports.set_custom_params("s3ds_param_4", {});
    //

    if (pSettings->section_exist("shader_params_export"))
    {
        Msg("[shader_params_export] section found!!!");

        int tb_count = pSettings->line_count("shader_params_export");
        for (int tb_idx = 0; tb_idx < tb_count; tb_idx++)
        {
            LPCSTR N, V;
            if (pSettings->r_line("shader_params_export", tb_idx, &N, &V))
            {
                if (strstr(N, "_save"))
                    continue;

                shader_exports.set_custom_params(N, Fvector4{});
            }
        }
    }

    FS.init_gamedata_unused();
}
void InitConsole()
{
    Console = xr_new<CConsole>();
    Console->Initialize();

    strcpy_s(Console->ConfigFile, "user.ltx");
    if (strstr(Core.Params, "-ltx "))
    {
        string64 c_name;
        sscanf(strstr(Core.Params, "-ltx ") + 5, "%[^ ] ", c_name);
        strcpy_s(Console->ConfigFile, c_name);
    }

    CORE_FEATURE_SET(colorize_ammo, "dragdrop");
    CORE_FEATURE_SET(colorize_untradable, "dragdrop");
    CORE_FEATURE_SET(equipped_untradable, "dragdrop");
    CORE_FEATURE_SET(select_mode_1342, "dragdrop");
    CORE_FEATURE_SET(highlight_equipped, "dragdrop");
    CORE_FEATURE_SET(af_radiation_immunity_mod, "features");
    CORE_FEATURE_SET(condition_jump_weight_mod, "features");
    CORE_FEATURE_SET(forcibly_equivalent_slots, "features");
    CORE_FEATURE_SET(slots_extend_menu, "features");
    CORE_FEATURE_SET(dynamic_sun_movement, "features");
    CORE_FEATURE_SET(wpn_bobbing, "features");
    CORE_FEATURE_SET(show_inv_item_condition, "features");
    CORE_FEATURE_SET(remove_alt_keybinding, "features");
    CORE_FEATURE_SET(binoc_firing, "features");
    CORE_FEATURE_SET(cop_style_scope_texture, "features");    
    CORE_FEATURE_SET(stop_anim_playing, "features");
    CORE_FEATURE_SET(corpses_collision, "features");
    CORE_FEATURE_SET(more_hide_weapon, "features");
    CORE_FEATURE_SET(keep_inprogress_tasks_only, "features");
    CORE_FEATURE_SET(show_dialog_numbers, "features");
    CORE_FEATURE_SET(objects_radioactive, "features");
    CORE_FEATURE_SET(af_zero_condition, "features");
    CORE_FEATURE_SET(af_satiety, "features");
    CORE_FEATURE_SET(af_psy_health, "features");
    CORE_FEATURE_SET(outfit_af, "features");
    CORE_FEATURE_SET(gd_master_only, "features");
    CORE_FEATURE_SET(scope_textures_autoresize, "features");
    CORE_FEATURE_SET(ogse_new_slots, "features");
    CORE_FEATURE_SET(ogse_wpn_zoom_system, "features");
    CORE_FEATURE_SET(wpn_cost_include_addons, "features");
    CORE_FEATURE_SET(hard_ammo_reload, "features");
    CORE_FEATURE_SET(engine_ammo_repacker, "features");
    CORE_FEATURE_SET(ruck_flag_preferred, "features");
    CORE_FEATURE_SET(old_outfit_slot_style, "features");
    CORE_FEATURE_SET(npc_simplified_shooting, "features");
    CORE_FEATURE_SET(use_trade_deficit_factor, "features");
    CORE_FEATURE_SET(show_objectives_ondemand, "features");
    CORE_FEATURE_SET(pickup_check_overlaped, "features");
    CORE_FEATURE_SET(actor_thirst, "features");
    CORE_FEATURE_SET(autoreload_wpn, "features");
    CORE_FEATURE_SET(no_progress_bar_animation, "features");
    CORE_FEATURE_SET(disable_dialog_break, "features");
    CORE_FEATURE_SET(busy_actor_restrictions, "features");
}

// Defined in the editor-host facade below (A2.3).
extern bool g_ed_embedded;

// Embedded in a host UI (the SDK): input must be NON-exclusive, otherwise DirectInput
// takes the mouse away from the host's own menus/panels and hides the cursor.
void InitInput() { pInput = xr_new<CInput>(!g_ed_embedded); }
void destroyInput() { xr_delete(pInput); }

void InitSound1() { CSound_manager_interface::_create(0); }

void InitSound2() { CSound_manager_interface::_create(1); }
void destroySound() { CSound_manager_interface::_destroy(); }

void destroySettings()
{
    xr_delete(pSettings);
    xr_delete(pGameIni);
}
void destroyConsole()
{
    //Console->Destroy();
    xr_delete(Console);
}
void destroyEngine()
{
    Device.Destroy();
    Engine.Destroy();
}

#ifdef __SWITCH__
extern "C" void xr_nx_display_size(unsigned* w, unsigned* h);
#endif

void execUserScript()
{
    Console->Execute("unbindall");
    // Before user.ltx: remember the stock values of the Switch preset's levers, so a later `_preset High` restores them.
    Console->Execute("_preset_defaults");

    if (FS.exist("$app_data_root$", Console->ConfigFile))
    {
        Console->ExecuteScript(Console->ConfigFile);
    }
    else
    {
        string_path default_full_name;

        FS.update_path(default_full_name, "$game_config$", "rspec_default.ltx");

        Console->ExecuteScript(default_full_name);
    }

#ifdef __ANDROID__
    // Bring-up: the phone starts on the console's preset (the safest known floor, Vulkan paths proven on a mobile GPU);
    // -keep_preset in args.txt keeps whatever user.ltx says instead. A phone preset of its own comes after the first runs.
    if (!strstr(Core.Params, "-keep_preset"))
        Console->Execute("_preset Switch");
#endif
#ifdef __SWITCH__
    // ONE codebase: the console is the PC engine on its lowest preset. Everything quality-related (the old Potato floor
    // plus what used to be hardcoded lists here: lean path, world colour pass off, bare scene, live sun cascades, R1 grass
    // and crowns, NPC LOD/occlusion) lives in gamedata/config/rspec_vk_switch.ltx and is the same file a PC picks as
    // `_preset Switch`. Applied on EVERY start, after user.ltx (a fresh console install has no user.ltx, and without it
    // the default-quality targets used ~2.4 GB of the 3.1 GB pool, 30-09). Only the platform bits below stay in code.
    Console->Execute("_preset Switch");
    // -nx_cpu: print the per-phase CPU budget (cpu_budget 2) to the log, to see where a 35-40 ms frame / 150 ms hitch goes.
    if (strstr(Core.Params, "-nx_cpu"))
        Console->Execute("cpu_budget 2");
    // -nx_ph100: physics at the PC's 100 Hz instead of the console's 50 (Physics.cpp fixed_step) -- A/B for the Kordon wrecks that
    // go NaN after a save is loaded and blow the player through the ground (01-10).
    if (strstr(Core.Params, "-nx_ph100"))
        Console->Execute("ph_frequency 100");
    // -nx_lean0: A/B against the preset's lean path (second rasterisations back: tree/NPC prepass, pre-skin pool, Hi-Z, sky).
    if (strstr(Core.Params, "-nx_lean0"))
    {
        static const char* const kNxLean0[] = {"r_tree_prepass 1", "r_preskin 1", "r_tonemap_lean 0", "r_hzb_cull 1", "r_tree_hzb 1", "r_sky_proc 1"};
        for (const char* c : kNxLean0)
            Console->Execute(c);
    }
    // -nx_full: full static shading instead of the preset's R1 lighting (world_frag_body.glsl WORLD_FLAT).
    if (strstr(Core.Params, "-nx_full"))
        Console->Execute("r_world_flat 0");
    // -nx_nopre: the A/B for "no depth prepass at all" (the colour passes write depth, R1 had none). v70 ran statics without
    // the prepass's early-Z and they cost 5-20 ms instead of 1.4-4.7.
    if (strstr(Core.Params, "-nx_nopre"))
    {
        Console->Execute("r_z_prepass 0");
        Console->Execute("r_depth_prepass 0");
        Console->Execute("r_hzb_cull 0");    // the pyramid is built from the prepass depth -- empty then
    }
    {
        // Internal resolution 960x540 (scene targets at 540p, the tonemap upscales to the display). Handheld 720p -> 0.75,
        // docked 1080p -> 0.5 (the knob's floor). The line "[VK] render extent ... set by r_render_scale" in the log proves it.
        unsigned w = 0, h = 0;
        xr_nx_display_size(&w, &h);
        float sc = h ? 540.f / float(h) : 0.75f;
        if (sc < 0.5f) sc = 0.5f;
        string64 cmd;
        xr_sprintf(cmd, "r_render_scale %.3f", sc);
        Console->Execute(cmd);
    }
#endif
}

void Startup()
{
    InitSound1();
    execUserScript();
    InitSound2();

    // ...command line for auto start
    {
        LPCSTR pStartup = strstr(Core.Params, "-start ");
        if (pStartup)
            Console->Execute(pStartup + 1);
    }
    {
        LPCSTR pStartup = strstr(Core.Params, "-load ");
        if (pStartup)
            Console->Execute(pStartup + 1);
    }
    {
        // The join ticket the launcher obtained from authd (online/accounts).
        // Parsed BEFORE -connect below so it is in place by the time anything
        // can start a connection.
        //
        // The ticket itself is never logged: it is a bearer credential for the
        // length of its (short) lifetime, and engine logs get pasted into chat.
        LPCSTR pTicket = strstr(Core.Params, "-ticket ");
        if (pTicket)
        {
            string512 text{};
            sscanf(pTicket + xr_strlen("-ticket "), "%[^ ]", text);
            NET_GNS::SetJoinTicket(text);
            Msg("* [auth] join ticket supplied (%d characters)", xr_strlen(text));
        }
    }
    {
        // dedicated command-line entry point for the "connect" console
        // command (xr_ioc_cmd.cpp's CCC_Connect) -- "-start "/"-load " above
        // are hardcoded to the "start"/"load" console commands specifically
        // (Console->Execute gets the raw command-line text with only the
        // leading '-' stripped, so "-start connect(...)" actually runs
        // "start" with "connect(...)" as ITS argument, not the "connect"
        // command). Lets a pure network client be launched non-interactively
        // for scripted/automated testing, e.g. `-connect 127.0.0.1:28001`.
        LPCSTR pConnect = strstr(Core.Params, "-connect ");
        if (pConnect)
        {
            string256 target;
            sscanf(pConnect + xr_strlen("-connect "), "%[^ ]", target);
            string512 cmd;
            // NB: needs a space between the command name and "(...)" --
            // CConsole::ExecuteCommand's split_cmd() tokenizes on the first
            // space, not '(', same convention as "start server(...)".
            sprintf_s(cmd, "connect (%s)", target);
            Console->Execute(cmd);
        }
    }

    // Initialize APP

    Device.Create();
    LALib.OnCreate();
    pApp = xr_new<CApplication>();
    g_pGamePersistent = (IGame_Persistent*)NEW_INSTANCE(CLSID_GAME_PERSISTANT);
    g_SpatialSpace = xr_new<ISpatial_DB>();
    g_SpatialSpacePhysic = xr_new<ISpatial_DB>();

    Discord.Init();

	// Reshade
#pragma todo("Simp: нужен вообще этот решейд???")
    use_reshade = init_reshade();
    if (use_reshade)
        Msg("--[ReShade]: Loaded compatibility addon");
    else
        Msg("!![ReShade]: ReShade not installed or version too old - didn't load compatibility addon");

    // Main cycle
    Memory.mem_usage();

#if defined(__SWITCH__) || defined(__ANDROID__)
    // Horizon has no window messages: nothing ever calls OnWM_Activate, so b_is_Active stays FALSE and
    // on_idle() skips ALL rendering and Sleep(1)s instead -- a live swapchain, ~850 loops/s, black screen
    // (measured 30-09 with the main-thread sampler: message_loop -> on_idle -> Sleep).
    // Android (SDL window, no WM_ACTIVATE either): the same black screen, 14-10 on the OnePlus 12.
    Device.b_is_Active = TRUE;
#endif

    Device.Run();

#ifdef __SWITCH__
    // Quit from the main menu: the message loop has ended. A full teardown never ran on this platform (worker threads, SDL,
    // the audio shim) and may hang, which would leave the console frozen on a black screen instead of returning to the
    // homebrew menu. Flush the log and leave the process directly.
    Msg("[nx] quit requested -> leaving to the homebrew menu");
    LogFlushNow();
    fflush(stderr);
    // ⚠⚠NOT _exit(0): under hbloader that returns to the loader with every engine thread still running, and the first
    // one to wake executes unmapped code (crash report on every quit). svcExitProcess ends them all -- see
    // xr_nx_exit_process in nx/xr_nx_pad.c.
    xr_nx_exit_process();
#endif

	// Reshade
    if (use_reshade)
        unregister_reshade();

    // Destroy APP
    xr_delete(g_SpatialSpacePhysic);
    xr_delete(g_SpatialSpace);
    xr_delete(g_pGamePersistent);
    xr_delete(pApp);
    Engine.Event.Dump();

    // Destroying
    destroyInput();

    destroySettings();

    LALib.OnDestroy();

    destroyConsole();

    destroySound();

    destroyEngine();
}

#ifdef _MSC_VER

constexpr auto dwStickyKeysStructSize = sizeof(STICKYKEYS);
constexpr auto dwFilterKeysStructSize = sizeof(FILTERKEYS);
constexpr auto dwToggleKeysStructSize = sizeof(TOGGLEKEYS);

struct damn_keys_filter
{
    BOOL bScreenSaverState;

    // Sticky & Filter & Toggle keys

    STICKYKEYS StickyKeysStruct;
    FILTERKEYS FilterKeysStruct;
    TOGGLEKEYS ToggleKeysStruct;

    DWORD dwStickyKeysFlags;
    DWORD dwFilterKeysFlags;
    DWORD dwToggleKeysFlags;

    damn_keys_filter()
    {
        // Screen saver stuff

        bScreenSaverState = FALSE;

        // Saveing current state
        SystemParametersInfo(SPI_GETSCREENSAVEACTIVE, 0, (PVOID)&bScreenSaverState, 0);

        if (bScreenSaverState)
            // Disable screensaver
            SystemParametersInfo(SPI_SETSCREENSAVEACTIVE, FALSE, NULL, 0);

        dwStickyKeysFlags = 0;
        dwFilterKeysFlags = 0;
        dwToggleKeysFlags = 0;

        ZeroMemory(&StickyKeysStruct, dwStickyKeysStructSize);
        ZeroMemory(&FilterKeysStruct, dwFilterKeysStructSize);
        ZeroMemory(&ToggleKeysStruct, dwToggleKeysStructSize);

        StickyKeysStruct.cbSize = dwStickyKeysStructSize;
        FilterKeysStruct.cbSize = dwFilterKeysStructSize;
        ToggleKeysStruct.cbSize = dwToggleKeysStructSize;

        // Saving current state
        SystemParametersInfo(SPI_GETSTICKYKEYS, dwStickyKeysStructSize, (PVOID)&StickyKeysStruct, 0);
        SystemParametersInfo(SPI_GETFILTERKEYS, dwFilterKeysStructSize, (PVOID)&FilterKeysStruct, 0);
        SystemParametersInfo(SPI_GETTOGGLEKEYS, dwToggleKeysStructSize, (PVOID)&ToggleKeysStruct, 0);

        if (StickyKeysStruct.dwFlags & SKF_AVAILABLE)
        {
            // Disable StickyKeys feature
            dwStickyKeysFlags = StickyKeysStruct.dwFlags;
            StickyKeysStruct.dwFlags = 0;
            SystemParametersInfo(SPI_SETSTICKYKEYS, dwStickyKeysStructSize, (PVOID)&StickyKeysStruct, 0);
        }

        if (FilterKeysStruct.dwFlags & FKF_AVAILABLE)
        {
            // Disable FilterKeys feature
            dwFilterKeysFlags = FilterKeysStruct.dwFlags;
            FilterKeysStruct.dwFlags = 0;
            SystemParametersInfo(SPI_SETFILTERKEYS, dwFilterKeysStructSize, (PVOID)&FilterKeysStruct, 0);
        }

        if (ToggleKeysStruct.dwFlags & TKF_AVAILABLE)
        {
            // Disable FilterKeys feature
            dwToggleKeysFlags = ToggleKeysStruct.dwFlags;
            ToggleKeysStruct.dwFlags = 0;
            SystemParametersInfo(SPI_SETTOGGLEKEYS, dwToggleKeysStructSize, (PVOID)&ToggleKeysStruct, 0);
        }
    }

    ~damn_keys_filter()
    {
        if (bScreenSaverState)
            // Restoring screen saver
            SystemParametersInfo(SPI_SETSCREENSAVEACTIVE, TRUE, NULL, 0);

        if (dwStickyKeysFlags)
        {
            // Restore StickyKeys feature
            StickyKeysStruct.dwFlags = dwStickyKeysFlags;
            SystemParametersInfo(SPI_SETSTICKYKEYS, dwStickyKeysStructSize, (PVOID)&StickyKeysStruct, 0);
        }

        if (dwFilterKeysFlags)
        {
            // Restore FilterKeys feature
            FilterKeysStruct.dwFlags = dwFilterKeysFlags;
            SystemParametersInfo(SPI_SETFILTERKEYS, dwFilterKeysStructSize, (PVOID)&FilterKeysStruct, 0);
        }

        if (dwToggleKeysFlags)
        {
            // Restore FilterKeys feature
            ToggleKeysStruct.dwFlags = dwToggleKeysFlags;
            SystemParametersInfo(SPI_SETTOGGLEKEYS, dwToggleKeysStructSize, (PVOID)&ToggleKeysStruct, 0);
        }
    }
};

#else

// Windows accessibility API (SPI_GET/SETSTICKYKEYS/FILTERKEYS/TOGGLEKEYS,
// screensaver toggle) -- no single portable equivalent (desktop-environment
// / compositor specific on Linux), and purely a fullscreen-session
// nicety, not core functionality. Inert stand-in so the RAII instantiation
// below still compiles.
struct damn_keys_filter {};

#endif // _MSC_VER

int APIENTRY WinMain_impl(HINSTANCE hInstance, HINSTANCE hPrevInstance, char* lpCmdLine, int nCmdShow)
{
    HANDLE hCheckPresenceMutex = INVALID_HANDLE_VALUE;
    if (!strstr(lpCmdLine, "-multi_instances"))
    { // Check for another instance
        constexpr const char* STALKER_PRESENCE_MUTEX = "STALKER-SoC";
        hCheckPresenceMutex = OpenMutex(READ_CONTROL, FALSE, STALKER_PRESENCE_MUTEX);
        if (hCheckPresenceMutex == nullptr)
        {
            // New mutex
            hCheckPresenceMutex = CreateMutex(nullptr, FALSE, STALKER_PRESENCE_MUTEX);
            if (hCheckPresenceMutex == nullptr)
            {
                // Shit happens
                return 0;
            }
        }
        else
        {
            // Already running
            CloseHandle(hCheckPresenceMutex);
            return 0;
        }
    }

#ifdef __SWITCH__
    XRNX_TRACE("WinMain_impl: Debug._initialize...");
#endif
    Debug._initialize();
#ifdef __SWITCH__
    XRNX_TRACE("WinMain_impl: Debug ok, ShowSplash...");
#endif

    // SetThreadAffinityMask		(GetCurrentThread(),1);

    // Title window

    DisableProcessWindowsGhosting();

    ShowSplash(hInstance);

    LPCSTR fsgame_ltx_name = "-fsltx ";
    string_path fsgame = "";
    if (strstr(lpCmdLine, fsgame_ltx_name))
    {
        int sz = xr_strlen(fsgame_ltx_name);
        sscanf(strstr(lpCmdLine, fsgame_ltx_name) + sz, "%[^ ] ", fsgame);
    }

#ifdef __SWITCH__
    XRNX_TRACE("WinMain_impl: splash ok, Core._initialize...");
#endif
    Core._initialize("xray", NULL, TRUE, fsgame[0] ? fsgame : NULL);
#ifdef __SWITCH__
    XRNX_TRACE("WinMain_impl: Core._initialize returned");
#endif

    // Heap-corruption hunt: mimalloc is built with MI_GUARDED (1/4000 allocations
    // get a trailing guard page — an overrun AVs at the corrupting WRITE, not at
    // some later allocation). -mi_guard N tightens sampling to 1/N (default 32).
#ifndef __SWITCH__ // no mimalloc on Horizon (see xrCore/xrMemory.cpp)
    if (const char* mg = strstr(lpCmdLine, "-mi_guard"))
    {
        int rate = atoi(mg + sizeof("-mi_guard") - 1);
        if (rate <= 0)
            rate = 32;
        mi_option_set(mi_option_guarded_sample_rate, rate);
        // ⚠The option is read when a thread's heap is made -- the main thread's already exists (Core._initialize
        // allocates), so without this its rate stayed at the build's 1/4000 and the flag only reached later threads.
        // MIMALLOC_GUARDED_SAMPLE_RATE in the environment covers every heap from the first allocation.
        mi_heap_guarded_set_sample_rate(mi_heap_get_default(), size_t(rate), 0);
        Msg("* [mi_guard] guarded objects: 1/%d allocations get a guard page", rate);
    }
#endif

    InitSettings();

    // Adjust player & computer name for Asian
    if (pSettings->line_exist("string_table", "no_native_input"))
    {
        strcpy_s(Core.UserName, "Player");
        strcpy_s(Core.CompName, "Computer");
    }

    {
        damn_keys_filter filter;
        (void)filter;

        InitEngine();
        InitInput();
        InitConsole();

        Engine.External.Initialize();
        Console->Execute("stat_memory");
        Startup();
        Core._destroy();

        if (!strstr(lpCmdLine, "-multi_instances")) // Delete application presence mutex
            CloseHandle(hCheckPresenceMutex);
    }
    // here damn_keys_filter class instanse will be destroyed

    return 0;
}

// A build defining XR_ONLINE_SERVER supplies its own WinMain — it drives
// Ed_Init/Ed_RenderFrame (below) instead of the full game
// Startup()+menu+message loop, so it must be the exe's only entry point.
#ifndef XR_ONLINE_SERVER
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, char* lpCmdLine, int nCmdShow)
{
    gModulesLoaded = true;

    // A downloaded update is swapped in here, before anything opens a file of the game (xr_update.h). When the
    // program itself was among the files, the old code must not go on: start the new one instead.
    if (XrUpdate::ApplyPending())
    {
        XrUpdate::RequestRestart();
        XrUpdate::OnExit();
        return 0;
    }

    WinMain_impl(hInstance, hPrevInstance, lpCmdLine, nCmdShow);

    XrUpdate::OnExit(); // the menu's "restart to update": the engine is down, its files closed

    ExitFromWinMain = true;

    return 0;
}

#ifndef _WIN32
// POSIX entry point: rebuild the Windows-style command line from argv (program name
// included, like GetCommandLine()) so every strstr(Core.Params, "-foo ") parser keeps
// working unchanged, then take the same path as WinMain.
extern "C" void xr_nx_thread_selftest();
extern "C" void xr_nx_prof_init();   // nx/xr_nx_prof.cpp
extern "C" void xr_nx_start_sampler();
extern "C" void xr_nx_start_core_probes();
extern "C" void xr_nx_start_clock_watch();
extern "C" void xr_nx_gpu_clock_boost();
extern "C" void xr_nx_link_stderr_init();   // nx/xr_nx_link.c
extern "C" void xr_nx_link_start();
extern "C" void xr_nx_link_log_line(const char* line);
extern "C" void xr_nx_stderr_flush();
extern "C" int g_xr_nx_skip_glass;
extern "C" int g_xr_nx_mme_count;
extern "C" int g_xr_nx_present_mode;   // v118: nx_present.txt -> vk_swapchain.cpp ChoosePresentMode
int g_xr_nx_present_mode = 0;
extern "C" int g_xr_nx_swap_images;    // v119: nx_present.txt "img=N" -> vk_swapchain.cpp Create
int g_xr_nx_swap_images = 0;
extern "C" int g_xr_nx_tex_prebuild; // vk_texture_stream.cpp (nx_opts.txt)
extern "C" int g_xr_nx_crop;     // nfs28 device.cpp: nx_crop.txt = display crop for the whole run
extern "C" void nvkmd_switch_set_kick_entries(unsigned);
#ifdef __ANDROID__
// SDLActivity loads libmain.so and calls SDL_main: SDL_main.h renames the main() below to it (C linkage).
#include <SDL2/SDL_main.h>
#include <SDL2/SDL_hints.h>
#include <SDL2/SDL_system.h>
#include <android/log.h>
static void xr_android_log_tap(const char* line) { __android_log_write(ANDROID_LOG_INFO, "ogsr", line); }
// Bring-up: the process ending by itself (no signal, no tombstone) leaves no trace otherwise -- name the call chain.
extern std::string BuildStackTrace(const char* header); // xrCore/stacktrace_collector.cpp
static void xr_android_exit_trace(const char* how)
{
    const std::string st = BuildStackTrace("");
    fprintf(stderr, "[ogsr/android] %s, call chain:\n%s\n", how, st.c_str());
    __android_log_print(ANDROID_LOG_ERROR, "ogsr", "%s, call chain:\n%s", how, st.c_str());
}
#endif
int main(int argc, char** argv)
{
#ifdef __ANDROID__
    // SDL_main runs on SDLActivity's Java thread: ~1 MB of stack, against 4 MB on Windows (XR_3DA_Vulkan.vcxproj
    // StackReserveSize) and 8 MB for a Linux main thread. The engine overflowed it silently (the overflow kills the
    // process before any handler can run: no log, no tombstone). Run the whole engine on a thread of its own instead.
    {
        static thread_local bool s_onEngineThread = false;
        if (!s_onEngineThread)
        {
            struct Args { int argc; char** argv; int rc; } a{argc, argv, 0};
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 16u << 20);
            pthread_t th;
            const int err = pthread_create(&th, &attr, [](void* p) -> void* {
                Args* a = static_cast<Args*>(p);
                s_onEngineThread = true;
                pthread_setname_np(pthread_self(), "X-RAY main");
                a->rc = main(a->argc, a->argv);
                return nullptr;
            }, &a);
            pthread_attr_destroy(&attr);
            if (err == 0)
            {
                pthread_join(th, nullptr);
                return a.rc;
            }
            __android_log_print(ANDROID_LOG_ERROR, "ogsr", "pthread_create(16 MB) failed: %d, staying on SDLThread", err);
        }
    }
    // fsgame.ltx, the data pack and the logs live in the app's external files folder
    // (/sdcard/Android/data/<package>/files): adb push reaches it without root and the app needs no storage permission.
    if (const char* dir = SDL_AndroidGetExternalStoragePath())
        chdir(dir);
    freopen("stderr.log", "w", stderr);
    freopen("stdout.log", "w", stdout);
    setvbuf(stderr, nullptr, _IONBF, 0); // a crash must not eat the last lines
    setvbuf(stdout, nullptr, _IOLBF, 0);
    g_xr_nx_log_tap = xr_android_log_tap; // every engine log line into logcat too (adb logcat -s ogsr)
    {
        char cwd[512] = {};
        getcwd(cwd, sizeof(cwd));
        fprintf(stderr, "[ogsr/android] main: cwd=%s, fsgame.ltx %s\n", cwd, access("fsgame.ltx", F_OK) == 0 ? "PRESENT" : "MISSING");
        __android_log_print(ANDROID_LOG_INFO, "ogsr", "main: cwd=%s, fsgame.ltx %s", cwd, access("fsgame.ltx", F_OK) == 0 ? "PRESENT" : "MISSING");
    }
    // Touches are the on-screen controls' business, not a fake mouse.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    atexit([] { xr_android_exit_trace("exit()"); });
    at_quick_exit([] { xr_android_exit_trace("quick_exit()"); });
#endif
#ifdef __SWITCH__
    // hbmenu starts a NRO with argv[0] = "sdmc:/switch/<dir>/<name>.nro" and the working directory at the
    // SD root. The engine finds fsgame.ltx, gamedata and its logs relative to the working directory, so go
    // to the folder the NRO lives in (that is where the user puts their gamedata).
    if (argc > 0 && argv && argv[0])
    {
        char dir[512];
        strncpy(dir, argv[0], sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = 0;
        if (char* slash = strrchr(dir, '/'))
        {
            *slash = 0;
            if (dir[0])
                chdir(dir);
        }
        XrUpdate::SetSelfPath(argv[0]); // the manifest's "@self": this NRO, whatever the player named it
    }
    // The swap of a downloaded update, HERE and not in WinMain: from the next block on this process runs threads (sampler,
    // core probes, clock watch, debug link), and returning to hbloader with them alive -- the chain-load to the new NRO --
    // crashed the console (13-10: swap done, then "the program was closed because of an error").
    if (XrUpdate::ApplyPending())
    {
        XrUpdate::RequestRestart();
        XrUpdate::OnExit();
        return 0;
    }

    // A NRO has no console: without this, a FATAL before the engine's own log exists (missing fsgame.ltx,
    // a failed allocation) leaves NO trace anywhere. Send stdout/stderr to a file next to the NRO and write a
    // few facts about the run. (Crashes are separate: Atmosphere drops a report in /atmosphere/crash_reports.)
    {
        xr_nx_link_stderr_init();   // stderr.log + the debug link's ring (nx/xr_nx_link.c)
        freopen("stdout.log", "w", stdout);
        setvbuf(stderr, nullptr, _IONBF, 0); // a crash must not eat the last lines
        setvbuf(stdout, nullptr, _IOLBF, 0);
        char cwd[512] = {};
        getcwd(cwd, sizeof(cwd));
        fprintf(stderr, "[ogsr/switch] main: argc=%d argv0=%s cwd=%s\n", argc, (argc > 0 && argv && argv[0]) ? argv[0] : "(none)", cwd);
        fprintf(stderr, "[ogsr/switch] fsgame.ltx %s in cwd\n", access("fsgame.ltx", F_OK) == 0 ? "PRESENT" : "MISSING");
        if (void* probe = malloc(256u << 20))
        {
            fprintf(stderr, "[ogsr/switch] malloc(256 MB) ok\n");
            free(probe);
        }
        else
            fprintf(stderr, "[ogsr/switch] malloc(256 MB) FAILED -- launched in applet mode? hold R while starting the game\n");
    }
    {
        xr_nx_prof_init();   // nfsmw-1: thread registry/placement first, then every later thread is seen (nx_prof.txt, nx_cores.txt)
        xr_nx_thread_selftest();
        xr_nx_start_sampler();
        xr_nx_start_core_probes();   // v104: per-core busy% in [nx frame]
        xr_nx_gpu_clock_boost();
        xr_nx_start_clock_watch();   // v122: keep the GPU at the stock max of the current mode
        g_xr_nx_log_tap = xr_nx_link_log_line;   // engine Msg/Log lines into the debug link's ring
        g_xr_nx_stderr_flush = xr_nx_stderr_flush;   // LogFlushNow (fatal errors) writes stderr.log out at once
        xr_nx_link_start();
    }
#endif
    char* cmd = xr_posix_command_line();
    const size_t cap = 8192;
    size_t len = 0, args_at = 0; // args_at: where the arguments start (after argv[0] + space)
    for (int i = 0; i < argc; ++i)
    {
        const bool quote = strchr(argv[i], ' ') != nullptr;
        const size_t need = strlen(argv[i]) + (quote ? 2 : 0) + (i ? 1 : 0);
        if (len + need + 1 > cap)
            break;
        if (i)
            cmd[len++] = ' ';
        if (quote)
            cmd[len++] = '"';
        memcpy(cmd + len, argv[i], strlen(argv[i]));
        len += strlen(argv[i]);
        if (quote)
            cmd[len++] = '"';
        if (i == 0)
            args_at = len + 1;
    }
    cmd[len] = 0;

#ifdef __SWITCH__
    // hbmenu passes no arguments. Extra switches come from args2.txt next to the NRO (one line).
    // Sound is ON by default since 10-10: the OpenAL->audout layer (nx/xr_nx_sdl_audio.c) crashed
    // the audio thread on the first real run (renderSamples, null write) because its pause did not
    // wait for a callback in flight -- fixed there. `-nosound` in args2.txt turns it off again.
    {
        char extra[512] = {};
        if (FILE* f = fopen("args2.txt", "r"))
        {
            if (!fgets(extra, sizeof(extra), f))
                extra[0] = 0;
            fclose(f);
            for (char* e = extra + strlen(extra); e > extra && (e[-1] == '\n' || e[-1] == '\r'); --e)
                e[-1] = 0;
        }
        // no args2.txt: no extra switches (sound on)
        // Console-side A/B knob: `-nx_kick N` = flush the GPU queue every N entries (0 = only when it is full).
        // One-shot nx_*.txt files next to the NRO are read once and deleted here, so a run needs no args2.txt edit
        // (MTP cannot overwrite a file) and the next start is clean again.
        // One-shot nx_present.txt (v118): the swapchain present mode for this run -- "immediate", "relaxed" or "mailbox".
        if (FILE* t = fopen("nx_present.txt", "r"))
        {
            char pm[64] = {};
            fread(pm, 1, sizeof(pm) - 1, t);
            fclose(t);
            remove("nx_present.txt");
            g_xr_nx_present_mode = strstr(pm, "immediate") ? 1 : strstr(pm, "relaxed") ? 2 : strstr(pm, "mailbox") ? 3 : 0;
            if (const char* im = strstr(pm, "img="))
                g_xr_nx_swap_images = atoi(im + 4);
        }
        // One-shot nx_crop.txt (nfs28): this run draws the composite + UI at the render resolution and lets the display
        // compositor stretch it (nwindowSetCrop).
        if (FILE* t = fopen("nx_crop.txt", "r"))
        {
            fclose(t);
            remove("nx_crop.txt");
            g_xr_nx_crop = 1;
        }
        // nfs29: the crop is the default now -- nx_nocrop.txt (one-shot) runs without it (composite + UI at display res).
        if (FILE* t = fopen("nx_nocrop.txt", "r"))
        {
            fclose(t);
            remove("nx_nocrop.txt");
            g_xr_nx_crop = 0;
        }
        // One-shot nx_nak.txt (v124): shader compiler / pipeline-bind knobs of our NVK, e.g. "peephole=8 memlat=200 prefetch=1".
        // They must be in the environment before vkCreateInstance (NAK reads them once). Shaders compiled with other values
        // get other cache keys, so the first run after a change recompiles everything: measure the second load, not the first.
        // nfs27: the knobs are the console default now (first run 02-10: no spills, no artefacts, no crash once NAK's
        // logging stopped going through Rust std::io). nx_nak.txt below still overrides them for one run --
        // "peephole=0 memlat=32 prefetch=0" is the stock compiler, for an A/B.
        setenv("NAK_SWITCH_PEEPHOLE", "8", 0);
        setenv("NAK_SWITCH_MEM_LATENCY", "200", 0);
        setenv("VK_SWITCH_PIPELINE_PREFETCH", "true", 0);
        if (FILE* t = fopen("nx_nak.txt", "r"))
        {
            char nk[128] = {};
            fread(nk, 1, sizeof(nk) - 1, t);
            fclose(t);
            remove("nx_nak.txt");
            char v[16];
            if (const char* p = strstr(nk, "peephole="))
            {
                snprintf(v, sizeof(v), "%d", atoi(p + 9));
                setenv("NAK_SWITCH_PEEPHOLE", v, 1);
            }
            if (const char* p = strstr(nk, "memlat="))
            {
                snprintf(v, sizeof(v), "%d", atoi(p + 7));
                setenv("NAK_SWITCH_MEM_LATENCY", v, 1);
            }
            if (const char* p = strstr(nk, "prefetch="))
                setenv("VK_SWITCH_PIPELINE_PREFETCH", atoi(p + 9) ? "true" : "false", 1);
            // Mesa's own disk cache (sdmc:/.mesa) duplicates vk_pipeline_cache.bin and runs up to 4 disk$ threads;
            // "mesacache=0" turns it off -- only once the auto-saved VkPipelineCache has proven to survive HOME.
            if (const char* p = strstr(nk, "mesacache="))
                if (!atoi(p + 10))
                    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
            fprintf(stderr, "[ogsr/switch] nx_nak.txt: [%s] -> peephole=%s memlat=%s prefetch=%s\n", nk,
                    getenv("NAK_SWITCH_PEEPHOLE") ? getenv("NAK_SWITCH_PEEPHOLE") : "stock",
                    getenv("NAK_SWITCH_MEM_LATENCY") ? getenv("NAK_SWITCH_MEM_LATENCY") : "stock",
                    getenv("VK_SWITCH_PIPELINE_PREFETCH") ? getenv("VK_SWITCH_PIPELINE_PREFETCH") : "off");
        }
        // One-shot nx_opts.txt (nfsmw-1): engine-side switches from the nfsmw-nx findings, e.g. "texprebuild=1".
        if (FILE* t = fopen("nx_opts.txt", "r"))
        {
            char op[128] = {};
            fread(op, 1, sizeof(op) - 1, t);
            fclose(t);
            remove("nx_opts.txt");
            if (const char* p = strstr(op, "texprebuild="))
                g_xr_nx_tex_prebuild = atoi(p + 12);   // vk_texture_stream.cpp: the IO thread creates streamed images
            // "dynubo=0": the Lighting UBO / grass generator params stay plain UBOs (vk_dyn_sets.h) -- A/B of the
            // dynamic-UBO path that puts them in the constant bank in every stage. Rides on the command line.
            if (const char* p = strstr(op, "dynubo="))
                if (!atoi(p + 7) && strlen(extra) + 13 < sizeof(extra))
                    strcat(extra, extra[0] ? " -no_dyn_ubo" : "-no_dyn_ubo");
            fprintf(stderr, "[ogsr/switch] nx_opts.txt: [%s] -> texprebuild=%d\n", op, g_xr_nx_tex_prebuild);
        }
        if (strstr(extra, "-nx_mmecount0"))
            g_xr_nx_mme_count = 0; // old IndirectCount stand-in (plain multi-draw of the CPU-side bound)
        if (strstr(extra, "-nx_noglass"))
            g_xr_nx_skip_glass = 1;
        if (const char* k = strstr(extra, "-nx_kick"))
        {
            nvkmd_switch_set_kick_entries((unsigned)atoi(k + 8));
        }
        if (extra[0] && len + 1 + strlen(extra) + 1 < cap)
        {
            cmd[len++] = ' ';
            memcpy(cmd + len, extra, strlen(extra));
            len += strlen(extra);
            cmd[len] = 0;
        }
        fprintf(stderr, "[ogsr/switch] cmdline extra=[%s]\n", extra);
    }
#endif
#ifdef __ANDROID__
    // The launcher passes no arguments: extra switches come from args.txt next to fsgame.ltx (one line).
    if (FILE* f = fopen("args.txt", "r"))
    {
        char extra[512] = {};
        if (!fgets(extra, sizeof(extra), f))
            extra[0] = 0;
        fclose(f);
        for (char* e = extra + strlen(extra); e > extra && (e[-1] == '\n' || e[-1] == '\r'); --e)
            e[-1] = 0;
        if (extra[0] && len + 1 + strlen(extra) + 1 < cap)
        {
            cmd[len++] = ' ';
            memcpy(cmd + len, extra, strlen(extra));
            len += strlen(extra);
            cmd[len] = 0;
        }
        fprintf(stderr, "[ogsr/android] args.txt: [%s]\n", extra);
    }
#endif

    // WinMain_impl gets lpCmdLine WITHOUT the program name (as on Windows).
    if (args_at > len)
        args_at = len;
#ifdef __ANDROID__
    const int rc = WinMain(nullptr, nullptr, cmd + args_at, 0);
    xr_android_exit_trace("WinMain returned");
    return rc;
#else
    return WinMain(nullptr, nullptr, cmd + args_at, 0);
#endif
}
#endif
#endif

// ============================================================================
// Editor-host facade (architecture a2) — see ed_facade.h. Boots the engine up to
// the point of rendering but does NOT run Device.Run()'s message loop, so an external
// host (the SDK) drives frames via Ed_RenderFrame(). This is the SAME sequence as
// WinMain_impl + Startup(), minus the mutex/splash niceties and the message loop.
// Additive and DLL-only in practice (built with EdDllBuild=true); the .exe game path
// through WinMain is completely untouched.
// ============================================================================
#include "ed_facade.h"

static bool s_edBooted = false;

// Anchor symbol whose address lives in THIS module — used to resolve the DLL's own
// HMODULE via GetModuleHandleEx(FROM_ADDRESS).
static void ed_module_anchor() {}

// A2.3 — embedded-viewport state. Non-static: the VK renderer reads it to skip its
// window restyle/reposition, and Xr_input to pick a top-level coop window.
// (declared extern above, near InitInput)
bool g_ed_embedded = false;
static HWND s_edChildWnd = nullptr;

extern LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Create the render surface as a child of the host's panel. We make our OWN child
// window rather than rendering straight into the host HWND: the engine needs its
// WndProc on the render window, and the host keeps its panel's proc untouched.
static HWND ed_create_child_window(HWND parent, int w, int h)
{
    static bool s_classReg = false;
    const char* wndclass = "_XRAY_ED_VIEWPORT";
    HINSTANCE hInst = (HINSTANCE)GetModuleHandle(nullptr);
    if (!s_classReg)
    {
        WNDCLASS wc = {CS_OWNDC, WndProc, 0, 0, hInst, nullptr, LoadCursor(nullptr, IDC_ARROW), (HBRUSH)GetStockObject(BLACK_BRUSH), nullptr, wndclass};
        RegisterClass(&wc);
        s_classReg = true;
    }
    return CreateWindowEx(0, wndclass, "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, w, h, parent, nullptr, hInst, nullptr);
}

int Ed_Init(const char* extra_params) { return Ed_InitEx(nullptr, 0, 0, extra_params); }

int Ed_InitEx(void* parent_hwnd, int width, int height, const char* extra_params)
{
    if (s_edBooted)
        return 1;

    gModulesLoaded = true;
#ifdef __SWITCH__
    XRNX_TRACE("WinMain_impl: Debug._initialize...");
#endif
    Debug._initialize();
#ifdef __SWITCH__
    XRNX_TRACE("WinMain_impl: Debug ok, ShowSplash...");
#endif

    // Resolve THIS DLL's own directory (…\bin_x64\) and hand it to Core as the
    // ApplicationPath, so the FS finds fsgame.ltx at fs_root (= its parent, the engine
    // folder) no matter which host process loaded us. The host exe's path is irrelevant.
    string_path dllDir = {0};
    {
        HMODULE hSelf = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&ed_module_anchor), &hSelf);
        GetModuleFileNameA(hSelf, dllDir, sizeof(dllDir));
        if (char* sl = strrchr(dllDir, '\\'))
            *(sl + 1) = 0; // strip filename, keep trailing '\\'
    }
    // Build the engine's OWN command line: argv[0] = the engine module, then the
    // facade params. The host's real command line is deliberately NOT inherited — the
    // SDK's flags belong to the SDK, and some (e.g. `-nocache`) are consumed during FS
    // init and would break the engine's archive mounting.
    string_path edParams = {0};
    strconcat(sizeof(edParams), edParams, "\"", dllDir, "xrEngine_VK.dll\" ", extra_params ? extra_params : "");

    Core._initialize("xray", nullptr, TRUE, nullptr, dllDir[0] ? dllDir : nullptr, edParams);
    Msg("[Ed] facade boot — Params=[%s]", Core.Params);

    InitSettings();

    // A2.3: hand Device a substitute HWND BEFORE InitEngine() (which runs
    // Device.Initialize(), and that only creates its own window `if (m_hWnd == nullptr)`).
    if (parent_hwnd && width > 0 && height > 0)
    {
        s_edChildWnd = ed_create_child_window((HWND)parent_hwnd, width, height);
        if (!s_edChildWnd)
        {
            Msg("!![Ed] failed to create child render window in parent %p (err %d)", parent_hwnd, GetLastError());
            return 2;
        }
        g_ed_embedded = true;
        Device.m_hWnd = s_edChildWnd;
        gGameWindow = s_edChildWnd;
        Msg("[Ed] embedded viewport: child hwnd=%p in parent=%p, %dx%d", s_edChildWnd, parent_hwnd, width, height);
    }

    InitEngine();
    InitInput();
    InitConsole();
    Engine.External.Initialize(); // AttachRender + AttachGame (object factory) — MUST precede Device.Create / NEW_INSTANCE

    // ---- Startup() body, minus Device.Run() ----
    InitSound1();
    execUserScript();
    InitSound2();

    Device.Create();
    LALib.OnCreate();
    pApp = xr_new<CApplication>();
    g_pGamePersistent = (IGame_Persistent*)NEW_INSTANCE(CLSID_GAME_PERSISTANT);
    g_SpatialSpace = xr_new<ISpatial_DB>();
    g_SpatialSpacePhysic = xr_new<ISpatial_DB>();

    // A WS_CHILD window never receives WM_ACTIVATE, so OnWM_Activate never runs and
    // b_is_Active stays FALSE — and on_idle() gates ALL rendering on it (device.cpp:315),
    // which is exactly a black viewport. An embedded editor viewport should keep drawing
    // regardless of the host's focus, so latch it active here.
    if (g_ed_embedded)
        Device.b_is_Active = TRUE;

    // ---- Device.Run() pre-loop setup (everything BEFORE message_loop) ----
    Device.dwTimeGlobal = 0;
    Device.seqAppStart.Process(rp_AppStart);
    ::Render->ClearTarget();
    if (!g_ed_embedded)
        Device.ShowMainWindow(); // embedded: the child is already WS_VISIBLE, and
                                 // SetForegroundWindow on the host's window is rude

    s_edBooted = true;
    Msg("[Ed] facade booted OK — external frame driving ready");
    return 0;
}

// ============================================================================
// Headless boot: headless boot: same body as Ed_InitEx, but the window
// it hands Device is never shown (no WS_VISIBLE, no ShowMainWindow) instead of
// being a visible child of a host panel. Reuses s_edChildWnd/g_ed_embedded so
// Ed_RenderFrame/Ed_Shutdown need no changes: g_ed_embedded's existing meaning
// ("the window isn't a normal top-level game window") already gets us
// non-exclusive DirectInput (InitInput's `!g_ed_embedded` flag) and skips
// Device.ShowMainWindow(). The one thing deliberately NOT copied from
// Ed_InitEx's embedded branch is `Device.b_is_Active = TRUE` — headless wants
// on_idle's render half (device.cpp) permanently unreachable, not forced on.
// ============================================================================
static HWND xros_create_hidden_window()
{
    static bool s_classReg = false;
    const char* wndclass = "_XRAY_SERVER_HEADLESS";
    HINSTANCE hInst = (HINSTANCE)GetModuleHandle(nullptr);
    if (!s_classReg)
    {
        WNDCLASS wc = {0, WndProc, 0, 0, hInst, nullptr, LoadCursor(nullptr, IDC_ARROW), (HBRUSH)GetStockObject(BLACK_BRUSH), nullptr, wndclass};
        RegisterClass(&wc);
        s_classReg = true;
    }
    // No WS_VISIBLE: never shown, never in the taskbar, never receives
    // WM_ACTIVATE (nothing ever brings it to the foreground) — on_idle's
    // b_is_Active gate (device.cpp) stays false for the process's lifetime.
    return CreateWindowEx(0, wndclass, "OGSR Server", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, hInst, nullptr);
}

int Ed_InitHeadless(const char* extra_params)
{
    if (s_edBooted)
        return 1;

    gModulesLoaded = true;
#ifdef __SWITCH__
    XRNX_TRACE("WinMain_impl: Debug._initialize...");
#endif
    Debug._initialize();
#ifdef __SWITCH__
    XRNX_TRACE("WinMain_impl: Debug ok, ShowSplash...");
#endif

    string_path dllDir = {0};
    {
        HMODULE hSelf = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&ed_module_anchor), &hSelf);
        GetModuleFileNameA(hSelf, dllDir, sizeof(dllDir));
        if (char* sl = strrchr(dllDir, '\\'))
            *(sl + 1) = 0;
    }
    string_path edParams = {0};
    strconcat(sizeof(edParams), edParams, "\"", dllDir, "xrEngine_Server.exe\" ", extra_params ? extra_params : "");

    // ⭐A headless server must not open the sound device, and this is the place
    // that decides it -- not each of the twenty harness scripts, which would all
    // have to remember.
    //
    // It was opening the real speakers: OpenAL enumerated the devices, took the
    // default one and brought up EFX, and then the simulated world played into
    // it. The symptom is unpleasant and does not look like a server bug at all --
    // you close the game and the machine keeps making game noises, with no window
    // anywhere to explain it.
    //
    // ⚠A runtime flag rather than an #ifdef on XR_ONLINE_SERVER: xrSound is built
    // ONCE and linked into both binaries, so a compile-time gate there would be
    // decided by whichever project built the lib -- the same trap that cost the
    // server binary during the RmlUi work.
    //
    // Sound stays constructed and merely inert (bPresent = FALSE, see
    // CSound_manager_interface::_create), which is the path -nosound has always
    // used on the client, so every call site keeps working and simply does
    // nothing. A server has no listener to be positional about anyway.
    if (!strstr(edParams, "-nosound"))
        xr_strcat(edParams, sizeof(edParams), " -nosound");

    Core._initialize("xray", nullptr, TRUE, nullptr, dllDir[0] ? dllDir : nullptr, edParams);
    Msg("[headless] headless boot — Params=[%s]", Core.Params);

    InitSettings();

    // Hand Device a substitute HWND BEFORE InitEngine() (Device.Initialize()
    // only creates its own window `if (m_hWnd == nullptr)`).
    s_edChildWnd = xros_create_hidden_window();
    if (!s_edChildWnd)
    {
        Msg("!![headless] failed to create hidden window (err %d)", GetLastError());
        return 2;
    }
    g_ed_embedded = true;
    Device.m_hWnd = s_edChildWnd;
    gGameWindow = s_edChildWnd;
    Msg("[headless] hidden window hwnd=%p", s_edChildWnd);

    InitEngine();
    InitInput();
    InitConsole();
    Engine.External.Initialize(); // AttachRender + AttachGame — MUST precede Device.Create / NEW_INSTANCE

    InitSound1();
    execUserScript();
    InitSound2();

    Device.Create();
    LALib.OnCreate();
    pApp = xr_new<CApplication>();
    g_pGamePersistent = (IGame_Persistent*)NEW_INSTANCE(CLSID_GAME_PERSISTANT);
    g_SpatialSpace = xr_new<ISpatial_DB>();
    g_SpatialSpacePhysic = xr_new<ISpatial_DB>();

    // Deliberately NOT setting Device.b_is_Active = TRUE here (unlike
    // Ed_InitEx's embedded branch) — see the comment block above.

    Device.dwTimeGlobal = 0;
    Device.seqAppStart.Process(rp_AppStart);
    ::Render->ClearTarget();
    // No Device.ShowMainWindow(): g_ed_embedded is true, and even if it
    // weren't, this window must never become visible.

    s_edBooted = true;
    Msg("[headless] headless boot OK — external frame driving ready");
    return 0;
}

void Ed_RenderFrame()
{
    if (!s_edBooted)
        return;

    // Pump the engine window's messages (Ed_Init created the window on THIS thread),
    // then render exactly one frame — the body of the engine's own message_loop.
    // When embedded, restrict the pump to OUR child window: the host has its own
    // message loop and must keep receiving (and pre-processing, e.g. through its ImGui
    // Win32 hook) everything addressed to it.
    MSG msg;
    HWND pumpWnd = g_ed_embedded ? s_edChildWnd : nullptr;
    while (PeekMessage(&msg, pumpWnd, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    Device.on_idle();
}

// ---- A2.4: host-driven camera + scene. Implemented in the VK renderer's editor
// viewport (Layers/xrRenderVulkan/CRender_Vulkan.cpp); the monolith links it in.
// XR_ONLINE_SERVER never calls any Ed_* function below (no viewport, no editor
// host) — guarded out rather than stubbed, since the VKEditor::/VK::ImGuiVK::
// symbols they call belong to xrRenderVulkan, which this target doesn't link.
#ifndef XR_ONLINE_SERVER
namespace VKEditor
{
void HostSetCamera(const Fvector& pos, const Fvector& dir, const Fvector& up, float fov_deg, float zn, float zf);
int HostAddModel(const char* name, const Fmatrix& xf);
int HostAddModelFile(const char* ogf_path, const char* logical_name, const Fmatrix& xf);
void SetTextureRoot(const char* dir); // Layers/xrRenderVulkan/vk_world_material.cpp
void HostSetModelXform(int id, const Fmatrix& xf);
void HostClearScene();
bool HostScreenRay(int x, int y, Fvector& out_start, Fvector& out_dir);
void HostSetSelection(const int* ids, int count);
} // namespace VKEditor

void Ed_SetCamera(const float* pos3, const float* dir3, const float* up3, float fov_deg, float znear, float zfar)
{
    if (!s_edBooted || !pos3 || !dir3 || !up3)
        return;

    Fvector p, d, u;
    p.set(pos3[0], pos3[1], pos3[2]);
    d.set(dir3[0], dir3[1], dir3[2]);
    u.set(up3[0], up3[1], up3[2]);
    VKEditor::HostSetCamera(p, d, u, fov_deg, znear, zfar);
}

void Ed_GetCamera(float* pos3, float* dir3, float* up3, float* fov_deg)
{
    if (!s_edBooted)
        return;

    // Straight off the Device: whoever set the camera this frame (host or the built-in
    // orbit camera), this is the basis the frame was actually rendered with.
    if (pos3)
    {
        pos3[0] = Device.vCameraPosition.x;
        pos3[1] = Device.vCameraPosition.y;
        pos3[2] = Device.vCameraPosition.z;
    }
    if (dir3)
    {
        dir3[0] = Device.vCameraDirection.x;
        dir3[1] = Device.vCameraDirection.y;
        dir3[2] = Device.vCameraDirection.z;
    }
    if (up3)
    {
        up3[0] = Device.vCameraTop.x;
        up3[1] = Device.vCameraTop.y;
        up3[2] = Device.vCameraTop.z;
    }
    if (fov_deg)
        *fov_deg = Device.fFOV;
}

int Ed_AddModel(const char* visual_name, const float* xform16)
{
    if (!s_edBooted)
        return -1;

    Fmatrix xf;
    if (xform16)
        CopyMemory(&xf, xform16, sizeof(float) * 16);
    else
        xf.identity();

    return VKEditor::HostAddModel(visual_name, xf);
}

void Ed_SetTextureRoot(const char* dir)
{
    if (s_edBooted)
        VKEditor::SetTextureRoot(dir);
}

int Ed_AddModelFile(const char* ogf_path, const char* logical_name, const float* xform16)
{
    if (!s_edBooted)
        return -1;

    Fmatrix xf;
    if (xform16)
        CopyMemory(&xf, xform16, sizeof(float) * 16);
    else
        xf.identity();

    return VKEditor::HostAddModelFile(ogf_path, logical_name, xf);
}

void Ed_SetModelXform(int id, const float* xform16)
{
    if (!s_edBooted || !xform16)
        return;

    Fmatrix xf;
    CopyMemory(&xf, xform16, sizeof(float) * 16);
    VKEditor::HostSetModelXform(id, xf);
}

void Ed_ClearScene()
{
    if (s_edBooted)
        VKEditor::HostClearScene();
}

int Ed_ScreenRay(int x, int y, float* out_start3, float* out_dir3)
{
    if (!s_edBooted || !out_start3 || !out_dir3)
        return 0;

    Fvector s, d;
    if (!VKEditor::HostScreenRay(x, y, s, d))
        return 0;

    out_start3[0] = s.x; out_start3[1] = s.y; out_start3[2] = s.z;
    out_dir3[0] = d.x; out_dir3[1] = d.y; out_dir3[2] = d.z;
    return 1;
}

void Ed_SetSelection(const int* ids, int count)
{
    if (s_edBooted)
        VKEditor::HostSetSelection(ids, count);
}

// Defined in the VK renderer's ImGui overlay (Layers/xrRenderVulkan/vk_imgui.cpp).
namespace VK { namespace ImGuiVK {
void PushEditorLog(const char* text, bool isError);
void PushEditorStats(float fps, float rfps, int verts, int tris, int dips, int lights, int totalLights);
void SetGizmo(int op, int mode, const float* snap, const Fmatrix& xform);
int GizmoResult(Fmatrix& out_xform, Fmatrix& out_delta);
bool GizmoIsUsing();
bool GizmoWantsMouse();
} }

void Ed_SetGizmo(int op, int mode, const float* snap, const float* xform16)
{
    if (!s_edBooted || !xform16)
        return;

    Fmatrix xf;
    CopyMemory(&xf, xform16, sizeof(float) * 16);
    VK::ImGuiVK::SetGizmo(op, mode, snap, xf);
}

int Ed_GizmoResult(float* out_xform16, float* out_delta16)
{
    if (!s_edBooted || !out_xform16 || !out_delta16)
        return 0;

    Fmatrix xf, d;
    const int changed = VK::ImGuiVK::GizmoResult(xf, d);
    CopyMemory(out_xform16, &xf, sizeof(float) * 16);
    CopyMemory(out_delta16, &d, sizeof(float) * 16);
    return changed;
}

int Ed_GizmoIsUsing() { return (s_edBooted && VK::ImGuiVK::GizmoIsUsing()) ? 1 : 0; }
int Ed_GizmoWantsMouse() { return (s_edBooted && VK::ImGuiVK::GizmoWantsMouse()) ? 1 : 0; }

void Ed_PushLog(const char* text, int isError)
{
    if (s_edBooted)
        VK::ImGuiVK::PushEditorLog(text, isError != 0);
}

void Ed_SetStats(float fps, float rfps, int verts, int tris, int dips, int lights, int total_lights)
{
    if (s_edBooted)
        VK::ImGuiVK::PushEditorStats(fps, rfps, verts, tris, dips, lights, total_lights);
}
#endif // XR_ONLINE_SERVER

void Ed_Resize(int width, int height)
{
    if (!s_edBooted || !g_ed_embedded || width <= 0 || height <= 0)
        return;

    SetWindowPos(s_edChildWnd, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);

    // Publish the new size before the reset. ⚠18-08 the second half of this note
    // is no longer true: Device::Reset now runs the SAME window-mode pass Create
    // does (vk_apply_window_mode), which for an embedded host takes the client
    // rect as-is — so it arrives at these very numbers on its own. Kept because
    // it is still what the editor host means, and because the renderer is free
    // to stop re-reading the window again.
    RECT cr{};
    GetClientRect(s_edChildWnd, &cr);
    extern u32 psCurrentVidMode[2];
    Device.dwWidth = u32(cr.right - cr.left);
    Device.dwHeight = u32(cr.bottom - cr.top);
    Device.fWidth_2 = float(Device.dwWidth) * 0.5f;
    Device.fHeight_2 = float(Device.dwHeight) * 0.5f;
    psCurrentVidMode[0] = Device.dwWidth;
    psCurrentVidMode[1] = Device.dwHeight;

    Device.Reset(false);
    Msg("[Ed] resized embedded viewport to %ux%u", Device.dwWidth, Device.dwHeight);
}

void Ed_Shutdown()
{
    if (!s_edBooted)
        return;
    s_edBooted = false;

    // Mirrors Startup()'s post-loop teardown.
    xr_delete(g_SpatialSpacePhysic);
    xr_delete(g_SpatialSpace);
    xr_delete(g_pGamePersistent);
    xr_delete(pApp);
    Engine.Event.Dump();
    destroyInput();
    destroySettings();
    LALib.OnDestroy();
    destroyConsole();
    destroySound();
    destroyEngine();
    Core._destroy();

    // The engine's atexit guard R_ASSERTs on ExitFromWinMain to catch abnormal exits.
    // When hosted, the engine's WinMain never runs (we boot via Ed_Init), so the guard
    // would false-fire "Unexpected application exit" on the host's clean shutdown. We
    // reached Ed_Shutdown normally — mark the exit as expected.
    ExitFromWinMain = true;

    if (s_edChildWnd)
    {
        DestroyWindow(s_edChildWnd);
        s_edChildWnd = nullptr;
        Device.m_hWnd = nullptr;
        g_ed_embedded = false;
    }
}

CApplication::CApplication() : loadingScreen(nullptr)
{
    ll_dwReference = 0;

    // events
    eQuit = Engine.Event.Handler_Attach("KERNEL:quit", this);
    eStart = Engine.Event.Handler_Attach("KERNEL:start", this);
    eDisconnect = Engine.Event.Handler_Attach("KERNEL:disconnect", this);

    // levels
    Level_Current = 0;
    Level_Scan();

    // Register us
    Device.seqFrame.Add(this, REG_PRIORITY_HIGH + 1000);

    Console->Show();
}

CApplication::~CApplication()
{
    Console->Hide();

    Device.seqFrame.Remove(this);

    // events
    Engine.Event.Handler_Detach(eDisconnect, this);
    Engine.Event.Handler_Detach(eStart, this);
    Engine.Event.Handler_Detach(eQuit, this);
}

void CApplication::OnEvent(EVENT E, u64 P1, u64 P2)
{
    if (E == eQuit)
    {
        PostQuitMessage(0);

        for (auto& Level : Levels)
        {
            xr_free(Level.folder);
        }
    }
    else if (E == eStart)
    {
        LPSTR op_server = LPSTR(P1);
        LPSTR op_client = LPSTR(P2);
        R_ASSERT(0 == g_pGameLevel);
        R_ASSERT(g_pGamePersistent);

        // An exception escaping level creation would be swallowed by the pureFrame
        // guard, leaving a half-initialized g_pGameLevel and a disconnect/reconnect
        // livelock (seen 16-07: frozen game, 1200+ per-frame Disconnects). Unrecoverable
        // either way — die loudly with the cause instead.
        try
        {
            Console->Execute("main_menu off");
            Console->Hide();

            g_pGamePersistent->PreStart(op_server);

            g_pGameLevel = (IGame_Level*)NEW_INSTANCE(CLSID_GAME_LEVEL);

            pApp->LoadBegin();
            g_pGamePersistent->Start(op_server);
            g_pGameLevel->net_Start(op_server, op_client);
            pApp->LoadEnd();
        }
        catch (const std::exception& e)
        {
            FATAL("level start threw [%s: %s] — state unrecoverable", typeid(e).name(), e.what());
        }
        catch (...)
        {
            FATAL("level start threw [non-std; lua top: %s] — state unrecoverable", g_lua_error_peek ? g_lua_error_peek() : "<no hook>");
        }
        xr_free(op_server);
        xr_free(op_client);
    }
    else if (E == eDisconnect)
    {
        if (g_pGameLevel)
        {
            Console->Execute("main_menu off");
            Console->Hide();

            // A load of a save on another level (or a level change): the teardown below took 5+ s on the Switch with the last
            // game frame frozen on screen, the load screen only coming up in eStart after it.
            const bool restart = !!Engine.Event.Peek("KERNEL:start");
            if (restart)
                LoadScreenEarly();

            // Same as eStart: a throw mid-teardown (e.g. a Lua error raised while level
            // members release luabind refs after the script-engine restart) leaves a
            // dangling half-destroyed level → per-frame Disconnect livelock. Die loudly.
            try
            {
                g_pGameLevel->net_Stop();
                xr_delete(g_pGameLevel);
            }
            catch (const std::exception& e)
            {
                FATAL("level unload threw [%s: %s] — state unrecoverable", typeid(e).name(), e.what());
            }
            catch (...)
            {
                FATAL("level unload threw [non-std; lua top: %s] — state unrecoverable", g_lua_error_peek ? g_lua_error_peek() : "<no hook>");
            }

            // Only when going back to the menu: before a start or a quit it was drawn for a frame or more over the load screen.
            if ((FALSE == Engine.Event.Peek("KERNEL:quit")) && !restart)
            {
                Console->Show();
                Console->Execute("main_menu on");
            }
        }

        g_pGamePersistent->Disconnect();
    }
}

static CTimer phase_timer;
extern ENGINE_API BOOL g_appLoaded = FALSE;
// Bumped by every load (new game, save, level change, quick load): state that belongs to the world being left and is
// not in the save -- the renderer's wet-ground accumulator -- starts over when it changes.
extern ENGINE_API u32 g_load_generation = 0;

const char* (*g_lua_error_peek)() = nullptr;

// Per-member seqFrame profiling (see pure.h). The VK profiler installs the
// hook; device.cpp arms the live cb around seqFrame.Process only.
seq_profile_cb g_seq_profile_cb   = nullptr;
seq_profile_cb g_seq_profile_hook = nullptr;

void CApplication::LoadBegin(bool quick)
{
    ll_dwReference++;
    if (1 == ll_dwReference)
    {
        g_appLoaded = FALSE;
        ++g_load_generation;
        // Wet ground is not in the save: it carried over from whatever rain the previous world had -- a save loaded in
        // clear weather came up soaked (13-10, Switch). The saved weather wets it again if it is raining.
        if (g_pGamePersistent) // its constructor makes the environment (IGame_Persistent.cpp)
            g_pGamePersistent->Environment().wetness_factor = 0.f;

        phase_timer.Start();

        load_stage = 0;
        if (quick)
            max_load_stage = 4; // при быстром сохранении меньше фаз загрузки. 4 вроде б
        else 
            max_load_stage = 16; // 17; //KRodin: пересчитал кол-во стадий, у нас их 15 при создании НИ + 1 на автопаузу
    }
}

void CApplication::LoadEnd()
{
    ll_dwReference--;
    if (0 == ll_dwReference)
    {
        Msg("* phase time: %d ms", phase_timer.GetElapsed_ms());
        Msg("* phase cmem: %d K", Memory.mem_usage() / 1024);
        Console->Execute("stat_memory");
        g_appLoaded = TRUE;
    }
}

void CApplication::SetLoadingScreen(ILoadingScreen* newScreen)
{
    if (loadingScreen)
    {
        Log("! Trying to create new loading screen, but there is already one..");
        xr_delete(newScreen);
        return;
    }

    loadingScreen = newScreen;
}

void CApplication::DestroyLoadingScreen() { xr_delete(loadingScreen); }

void CApplication::LoadDraw() const
{
    if (g_appLoaded)
        return;
    Device.dwFrame += 1;

    if (!Device.Begin())
        return;

    load_draw_internal();

    Device.End();
}

// Puts the load screen up BEFORE the old world is torn down (level objects, ALife: 15 s on the Switch for a save loaded from
// a running game, the frozen last frame looking like a hang). The load that follows (eStart / restart_simulator) keeps this
// screen: CreateLoadingScreen skips when one is up, and its LoadBegin resets the stages.
void CApplication::LoadScreenEarly()
{
    g_pGamePersistent->CreateLoadingScreen();
    if (!loadingScreen)
        return;
    g_appLoaded = FALSE;
    load_stage = 0;
    LoadDraw();
    Msg("* [load] screen up before the teardown");
}

void CApplication::LoadForceFinish() { loadingScreen->ForceFinish(); }

void CApplication::SetLoadStageTitle(pcstr _ls_title) { loadingScreen->SetStageTitle(_ls_title); }

void CApplication::LoadTitleInt() { loadingScreen->SetStageTip(); }

void CApplication::LoadStage()
{
    VERIFY(ll_dwReference);

    Msg("* phase time: %d ms", phase_timer.GetElapsed_ms());
    phase_timer.Start();
    Msg("* phase cmem: %d K", Memory.mem_usage() / 1024);


    LoadDraw();

    ++load_stage;
    // Msg("--LoadStage is [%d]", load_stage);
}

// The spatial DB's self-check (see the call below). Registered as g_spatial_verify
// in xr_ioc_cmd.cpp. Left ON: it LOOKS like the VisualGuard pattern -- a debug walk
// running every 30 frames in a shipping build -- but it measured 0 ms even on the
// frame the level's 1161 objects spawn, so there is nothing here to win.
int g_spatial_verify = 1;

// Sequential
void CApplication::OnFrame()
{
    ZoneScoped;

    // What the frame trace calls seq:CApplication is these three calls, and it bills
    // 67 ms to the frame the level's objects spawn on. Split them, and report only
    // frames that actually cost something -- the spatial self-check below runs every
    // 30 frames FOREVER, not just at load, so this is a gameplay probe too. Gating on
    // dwPrecacheFrame does not work: the expensive frame is the one where the level
    // goes live, and the countdown is not armed yet when seqFrame runs on it.
    CTimer _t; _t.Start();
    float _msEvent = 0, _msSpatial = 0, _msSound = 0;

    Engine.Event.OnFrame();
    _msEvent = _t.GetElapsed_ms_total(); _t.Start();

    {
        static u32 last_frame{0};

        if (Device.dwFrame > last_frame)
        {
            // ISpatial_DB::update does NOTHING unless `validate` is set, and what
            // validate does is walk the entire octree to check that its node and
            // object counters still add up (ISpatial_verify.cpp) -- a debug integrity
            // check running in the shipping build every 30 frames forever. MEASURED
            // (the split below): 0 ms, including the frame 1161 objects spawn on.
            // The 67 ms this function costs there is the DEFERRED EVENT QUEUE, not
            // this. Kept on; g_spatial_verify 0 turns it off if it ever grows.
            g_SpatialSpace->update(g_spatial_verify != 0);
            g_SpatialSpacePhysic->update(false);

            last_frame = Device.dwFrame + 30;
        }
    }
    _msSpatial = _t.GetElapsed_ms_total(); _t.Start();

    if (g_pGameLevel)
        g_pGameLevel->SoundEvent_Dispatch();

    _msSound = _t.GetElapsed_ms_total();
    {
        static u32 s_shown = 0;
        if (_msEvent + _msSpatial + _msSound >= 10.f && s_shown < 8) {
            ++s_shown;
            Msg("[load step]   CApplication::OnFrame frame %u: events %.0f | spatial update %.0f (verify %s) | sound dispatch %.0f ms",
                Device.dwFrame, _msEvent, _msSpatial, g_spatial_verify ? "ON" : "off", _msSound);
        }
    }
}

void CApplication::Level_Append(LPCSTR folder)
{
    string_path N1, N2, N3, N4;
    strconcat(sizeof(N1), N1, folder, "level");
    strconcat(sizeof(N2), N2, folder, "level.ltx");
    strconcat(sizeof(N3), N3, folder, "level.geom");
    strconcat(sizeof(N4), N4, folder, "level.cform");
    if (FS.exist("$game_levels$", N1) && FS.exist("$game_levels$", N2) && FS.exist("$game_levels$", N3) && FS.exist("$game_levels$", N4))
    {
        Levels.emplace_back(sLevelInfo{xr_strdup(folder)});
    }
}

void CApplication::Level_Scan()
{
    for (auto& lvl : Levels)
    {
        xr_free(lvl.folder);
    }
    Levels.clear();

    xr_vector<char*>* folder = FS.file_list_open("$game_levels$", FS_ListFolders | FS_RootOnly);
    R_ASSERT(folder && folder->size());

    for (auto& i : *folder)
        Level_Append(i);

    FS.file_list_close(folder);
}

// Taken from OpenXray/xray-16 and refactored
void generate_logo_path(string_path& path, pcstr level_name, int num = -1)
{
    strconcat(sizeof(path), path, "intro\\intro_", level_name);

   // const auto len = xr_strlen(path);
  //  if (path[len - 1] == '\\')
  //      path[len - 1] = 0;

    if (num < 0)
        return;

    string16 buff;
    xr_strcat(path, sizeof(path), "_");
    xr_strcat(path, sizeof(path), _itoa(num + 1, buff, 10));
}

// Taken from OpenXray/xray-16 and refactored
// Return true if logo exists
// Always sets the path even if logo doesn't exist
bool validate_logo_path(string_path& path, pcstr level_name, int num = -1)
{
    generate_logo_path(path, level_name, num);
    string_path temp;
    return FS.exist(temp, "$game_textures$", path, ".dds") || FS.exist(temp, "$level$", path, ".dds");
}

void CApplication::Level_Set(u32 L)
{
    if (L >= Levels.size())
        return;

    Level_Current = L;
    FS.get_path("$level$")->_set(Levels[L].folder);

    std::string temp = Levels[L].folder;
    temp.pop_back();
    const char* level_name = temp.c_str();

    static string_path path;
    path[0] = 0;
    
    int count = 0;
    while (true)
    {
        if (validate_logo_path(path, level_name, count))
            count++;
        else
            break;
    }

    if (count)
    {
        const int curr = ::Random.randI(count);
        generate_logo_path(path, level_name, curr);
    }
    else if (!validate_logo_path(path, level_name))
    {
        if (!validate_logo_path(path, "no_start_picture"))
            path[0] = 0;
    }

    if (path[0])
        loadingScreen->SetLevelLogo(path);

    loadingScreen->SetLevelText(level_name);
}

int CApplication::Level_ID(const char* name, const char* ver, const bool bSet)
{
    int result = -1;
    bool arch_res = false;
/* //Вроде б нам это не нужно. что-то от мп
    auto it = FS.m_archives.begin();
    auto it_e = FS.m_archives.end();
    for (; it != it_e; ++it)
    {
        CLocatorAPI::archive& A = *it;
        if (A.hSrcFile == NULL)
        {
            LPCSTR ln = A.header->r_string("header", "level_name");
            LPCSTR lv = A.header->r_string("header", "level_ver");
            if (0 == _stricmp(ln, name) && 0 == _stricmp(lv, ver))
            {
                FS.LoadArchive(A);
                arch_res = true;
            }
        }
    }

    if (arch_res)
        Level_Scan();
*/
    string256 buffer;
    strconcat(sizeof(buffer), buffer, name, "\\");
    for (u32 I = 0; I < Levels.size(); ++I)
    {
        if (0 == _stricmp(buffer, Levels[I].folder))
        {
            result = int(I);
            break;
        }
    }

    if (bSet && result != -1)
        Level_Set(result);

    if (arch_res)
        g_pGamePersistent->OnAssetsChanged();

    return result;
}

extern void render_reshade_effects();

void CApplication::load_draw_internal() const
{
    if (use_reshade)
        render_reshade_effects();

    // The load screen is destroyed by CLoadScreenRenderer::stop() in the middle of a render pass, and the same pass can
    // still call the (just removed) renderer's OnRender -> here. Crashed with a null write on the console (30-09, two runs).
    if (!loadingScreen)
        return;

    loadingScreen->Update(load_stage, max_load_stage);
}

bool CApplication::CheckCsCopMode()
{
#pragma todo("Simp: на будущее, активировать эту функцию когда будет надо!")
    if constexpr (true)
        return false;

    // Определять, что мы запускаем ЧН/ЗП будем по названию папки конфигов. Решение так себе, но ничего лучше пока не придумал.
    static const char* cfg_path{FS.get_path("$game_config$")->m_Path};
    static const char* cfg_dir_name{cfg_path + (strlen(cfg_path) - (strlen("configs") + 1))};
    static const bool res{!!strstr(cfg_dir_name, "configs")};

    static bool first_call{true};
    if (first_call)
    {
        Msg("~~[%s] Configs dir name: [%s]", __FUNCTION__, cfg_dir_name);
        first_call = false;
    }

    return res;
}

#pragma todo("Simp: нужно ли это? сомневаюсь.")
// Always request high performance GPU
extern "C" {
// https://docs.nvidia.com/gameworks/content/technologies/desktop/optimus.htm
_declspec(dllexport) u32 NvOptimusEnablement = 0x00000001; // NVIDIA Optimus

// https://gpuopen.com/amdpowerxpressrequesthighperformance/
_declspec(dllexport) u32 AmdPowerXpressRequestHighPerformance = 0x00000001; // PowerXpress or Hybrid Graphics
}