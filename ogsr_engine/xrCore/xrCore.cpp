// xrCore.cpp : Defines the entry point for the DLL application.
//
#include "stdafx.h"

#ifdef _MSC_VER
#include <mmsystem.h>
#include <objbase.h>
#include <winternl.h>

#pragma comment(lib, "winmm.lib")
#endif // _MSC_VER

XRCORE_API xrCore Core;

XRCORE_API task_thread_pool::task_thread_pool* TTAPI{};
#ifdef __SWITCH__
// TTAPI workers: 0x2E like the PinnedWorkers of the frame, above the 0x3B background loaders, below the main thread
// (0x2C) and the render thread (0x2D, xr_nx_prof.cpp). See task_thread_pool.hpp.
XRCORE_API int g_ttapi_prio = 0x2E;
XRCORE_API int g_ttapi_core = -1; // task_thread_pool.hpp
#endif

// indicate that we reach WinMain, and all static variables are initialized
XRCORE_API bool gModulesLoaded = false;

namespace
{
u32 init_counter = 0;

// ⚠⚠⚠ZwSetTimerResolution -- NT-сисколл, у которого нет эквивалента вообще
// ни на одной другой платформе (см. xr_platform_module.h: это не "загрузка
// модуля", это разрешение конкретного NT-имени). На Linux повышенное
// разрешение таймеров не требуется отдельного запроса -- clock_nanosleep уже
// даёт микросекундную точность. Оставлено _MSC_VER-only, не заглушка.
#ifdef _MSC_VER
NTSTATUS(__stdcall* ZwSetTimerResolution)(IN ULONG RequestedResolution, IN BOOLEAN Set, OUT PULONG ActualResolution) = (NTSTATUS(__stdcall*)(ULONG, BOOLEAN, PULONG))GetProcAddress(GetModuleHandle("ntdll.dll"), "ZwSetTimerResolution");

ULONG actualResolution;
#endif // _MSC_VER
} // namespace

void xrCore::_initialize(LPCSTR _ApplicationName, LogCallback cb, BOOL init_fs,
                         LPCSTR fs_fname, LPCSTR app_path_override, LPCSTR params_override)
{
    strcpy_s(ApplicationName, _ApplicationName);
    if (0 == init_counter)
    {
#if defined(XRCORE_STATIC) && defined(_MSC_VER)
        // https://stackoverflow.com/questions/85122/how-to-make-thread-sleep-less-than-a-millisecond-on-windows/31411628#31411628
        ZwSetTimerResolution(1, true, &actualResolution);
#endif

        // Normally the process command line. The editor-host facade overrides it: when
        // the engine is a DLL inside a foreign process, the host's flags are the HOST's
        // (e.g. the SDK's `-nocache` would wreck the engine's FS) and must not leak in.
        if (params_override && params_override[0])
            strcpy_s(Params, sizeof(Params), params_override);
        else
            strcpy_s(Params, sizeof(Params), GetCommandLine());

#ifdef _MSC_VER
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif // _MSC_VER

        if (strstr(Params, "-dbg"))
            ParamFlags.set(ParamFlag::dbg, TRUE);

        // application path — normally the host exe's directory; the editor-host
        // facade overrides it with the xrEngine_VK.dll's own directory so the FS
        // finds fsgame.ltx at fs_root (= ApplicationPath's parent) regardless of
        // which process loaded the engine.
        if (app_path_override && app_path_override[0])
        {
            strcpy_s(ApplicationPath, app_path_override);
        }
        else
        {
            string_path fn, dr, di;
            GetModuleFileName(nullptr, fn, sizeof(fn));
            _splitpath(fn, dr, di, nullptr, nullptr);
            strconcat(sizeof(ApplicationPath), ApplicationPath, dr, di);
        }

        // User/Comp Name
#ifdef __SWITCH__
        XRNX_TRACE("Core._initialize: ApplicationPath=[%s]", ApplicationPath);
#endif
        DWORD sz_user = sizeof(UserName);
        GetUserName(UserName, &sz_user);

        DWORD sz_comp = sizeof(CompName);
        GetComputerName(CompName, &sz_comp);

#ifdef __SWITCH__
        XRNX_TRACE("Core._initialize: user=[%s] comp=[%s], Memory._initialize...", UserName, CompName);
#endif
        Memory._initialize();
#ifdef __SWITCH__
        XRNX_TRACE("Core._initialize: Memory ok, _initialize_cpu...");
#endif

        _initialize_cpu();

#ifdef __SWITCH__
        XRNX_TRACE("Core._initialize: cpu ok, rtc_initialize...");
#endif
        rtc_initialize();
#ifdef __SWITCH__
        XRNX_TRACE("Core._initialize: rtc ok, creating CLocatorAPI...");
#endif

        xr_FS = std::make_unique<CLocatorAPI>();
    }
    if (init_fs)
    {
        u32 flags = 0;

        if (nullptr != strstr(Core.Params, "-file_activity"))
            flags |= CLocatorAPI::flDumpFileActivity;

#ifdef __SWITCH__
        XRNX_TRACE("Core._initialize: CLocatorAPI ok, FS._initialize(fs_fname=%s)...", fs_fname ? fs_fname : "(default)");
#endif
        FS._initialize(flags, fs_fname);
#ifdef __SWITCH__
        XRNX_TRACE("Core._initialize: FS ok");
#endif

        Msg("[OGSR Engine (%s)] build date: [" __DATE__ " " __TIME__ "]",
            GetBuildConfiguration());
        if (strlen(APPVEYOR_BUILD_VERSION))
            Log("[AppVeyor] build version: [" APPVEYOR_BUILD_VERSION
                "], repo: [" APPVEYOR_REPO_NAME "]");

#ifdef _MSC_VER
#pragma message("[" _CRT_STRINGIZE_(_MSC_FULL_VER) "]: [" _CRT_STRINGIZE(_MSC_FULL_VER) "], [" _CRT_STRINGIZE_(_MSVC_LANG) "]: [" _CRT_STRINGIZE(_MSVC_LANG) "]")
        Log("[" _CRT_STRINGIZE_(_MSC_FULL_VER) "]: [" _CRT_STRINGIZE(_MSC_FULL_VER) "], [" _CRT_STRINGIZE_(_MSVC_LANG) "]: [" _CRT_STRINGIZE(_MSVC_LANG) "]");
#endif // _MSC_VER

        Msg("Working Directory: [%s]", WorkingPath);
        Msg("CommandLine: [%s]", Core.Params);

#ifdef DEBUG
        Msg("CRT heap 0x%08x", _get_heap_handle());
        Msg("Process heap 0x%08x", GetProcessHeap());
#endif // DEBUG
    }

    SetLogCB(cb);
    init_counter++;

    u32 th_count{};
    // Check for override from command line
    const char* szSearchFor = "-max-threads";
    char* pszTemp = strstr(Params, szSearchFor);
    u32 dwOverride = 0;
    if (pszTemp && sscanf_s(pszTemp + strlen(szSearchFor), "%u", &dwOverride) &&
        dwOverride >= 1)
    {
        th_count = dwOverride;
    }
#ifdef __SWITCH__
    // Horizon gives an application 3 cores and std::thread::hardware_concurrency() reports 1 there (v75 log: "TTAPI number of
    // threads: [1]"): the one pool thread is permanently busy with the frame's second-thread task (sound, ALife switch
    // predicates, Lua GC, physics, net), so every other TTAPI user -- bone calculation (r_bones_mt), cover search -- ran
    // serialised on the main thread. Two workers + the main thread = the three application cores.
    if (!th_count)
        th_count = 2;
#endif
    TTAPI = xr_new<task_thread_pool::task_thread_pool>("TTAPI", th_count);
    TTAPI->init();
    Msg("TTAPI number of threads: [%u]", TTAPI->get_num_threads());
}

void xrCore::_destroy()
{
    --init_counter;
    if (0 == init_counter)
    {
        TTAPI->clear_task_queue();
        TTAPI->wait_for_tasks();
        xr_delete(TTAPI);

        FS._destroy();

        xr_FS.reset();

        Memory._destroy();

#ifdef _MSC_VER
        CoUninitialize();
#endif // _MSC_VER

#if defined(XRCORE_STATIC) && defined(_MSC_VER)
        ZwSetTimerResolution(actualResolution, true, &actualResolution);
#endif
    }
}

const char* xrCore::GetEngineVersion()
{
    static string256 buff;
    if (strlen(APPVEYOR_BUILD_VERSION))
        std::snprintf(buff, sizeof(buff), APPVEYOR_BUILD_VERSION " (%s) from repo: [" APPVEYOR_REPO_NAME "]", GetBuildConfiguration());
    else
        std::snprintf(buff, sizeof(buff), "[OGSR Engine %s (build: " __DATE__ " " __TIME__ ")]", GetBuildConfiguration());
    return buff;
}

constexpr const char* xrCore::GetBuildConfiguration()
{
#ifdef _DEBUG
#ifdef _M_X64
    return "x64_Dbg";
#else
    return "x86_Dbg";
#endif
#else
#ifdef _M_X64
    return "x64";
#else
    return "x86";
#endif
#endif
}

#if !defined(XRCORE_STATIC) && defined(_MSC_VER)
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD ul_reason_for_call, LPVOID lpvReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        /*
            По сути это не рекомендуемый Microsoft, но повсеместно используемый способ повышения точности
            соблюдения и измерения временных интревалов функциями Sleep, QueryPerformanceCounter,
            timeGetTime и GetTickCount.
            Функция действует на всю операционную систему в целом (!) и нет необходимости вызывать её при
            старте нового потока. Вызов timeEndPeriod специалисты Microsoft считают обязательным.
            Есть подозрения, что Windows сама устанавливает максимальную точность при старте таких
            приложений как, например, игры. Тогда есть шанс, что вызов timeBeginPeriod здесь бессмысленен.
            Недостатком данного способа является то, что он приводит к общему замедлению работы как
            текущего приложения, так и всей операционной системы.
            Ещё можно посмотреть ссылки:
            https://msdn.microsoft.com/en-us/library/vs/alm/dd757624(v=vs.85).aspx
            https://users.livejournal.com/-winnie/151099.html
            https://github.com/tebjan/TimerTool
        */
        timeBeginPeriod(1);
        break;
    case DLL_PROCESS_DETACH:
        timeEndPeriod(1);
        break;
    }
    return TRUE;
}
#endif // XRCORE_STATIC
