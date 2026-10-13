#include "stdafx.h"

#include "XR_IOConsole.h"
extern int g_sched_stats;   // xrSheduler.cpp — разбор планировщика
extern int g_sched_rate;    // xrSheduler.cpp — множитель периода обычной очереди, %
// Гейт Lua-прибора sigprof (ogse_signals.script) — ОТДЕЛЬНЫЙ от sched_stats.
int g_sigprof = 0;
#include "xr_ioc_cmd.h"
XRSOUND_API extern int snd_nx_rev_test; // xrSound/SoundRender_Core.cpp (not in Sound.h: a debug lever)
#include "CameraManager.h"
#include "Environment.h"
#include "xr_input.h"
#include "CustomHUD.h"
#include <regex>
#include "SkeletonMotions.h"
#include "IGame_Persistent.h"
#include "LightAnimLibrary.h"
#include "lua_vm_watch.h"
#include "cpu_budget.h"
#include "IGame_Level.h" // snd_test
#ifdef __SWITCH__
extern "C" int g_xr_malloc_slow_us; // nx/xr_nx_throw.cpp: [malloc slow] threshold in us, 0 = off
#endif

// ⭐Номинальный шаг сборщика Lua в КБ за вызов. 19-09-2026 снижен 128 -> 32:
// по развёртке шаг 32 стоит 0.35-0.43 core-ms/кадр против 1.06-1.37 у 128, а
// куча встаёт на 33 МБ против 27. Двенадцать мегабайт памяти дешевле, чем
// 0.8 core-ms на КАЖДОМ кадре, особенно там, где кадр упирается в процессор.
// Потолок кучи держит не он, а адаптация вокруг psLUA_GC_TARGET_KB.
int psLUA_GCSTEP{32};
// Целевой размер кучи Lua в КБ (консольный lua_gc_target_kb, NoSave). Вокруг
// него пляшет адаптивный шаг сборщика -- см. CLevel::script_gc, ветка gc_step.
// 40 МБ выбраны ВЫШЕ естественной полки (Кордон 27-33 МБ, Бар 23-24), чтобы
// в штатном режиме сборщик шёл номинальным шагом или накатом, а удваивался и
// восьмерился только когда куча реально поехала вверх.
int psLUA_GC_TARGET_KB{40960};
int psLUA_GCTIMEOUT{2000}, psLUA_GCTIMEOUT_MIN{2000}; // in micro seconds

// ---------------------------------------------------------------------------
// ⭐СПОСОБ СБОРКИ МУСОРА LUA. ДЕФОЛТ СМЕНЁН 19-09-2026: gc_timeout -> gc_step.
//
// 📏Прибор `cpu_budget 2` (фазы кадра в ЦИКЛАХ) при fpslock60, Кордон, ~1490
// объектов в реестре -- A/B в ОДНОМ прогоне, по 25 с на настройку:
//                          процесс      главный     фаза lua GC   куча Lua
//   gc_timeout 2000 (было)  8.43-8.50    3.68-3.72    2.12         26.1-26.6 МБ
//   gc_step 128             7.32-7.42    3.69-3.76    0.95         27.2-27.7
//   gc_step адаптивный      6.60         3.615        0.33         31.4-33.1
//   gc_default              6.88-6.95    4.13-4.21     --          26.8-27.4
// ⇒ итог вечера: вся процессорная работа кадра 8.45 -> 6.60 core-ms (-22%),
// последовательный путь не изменился (сборка живёт на втором потоке).
//
// ⚠⚠ПЕРВАЯ ВЕРСИЯ ЭТОЙ ТАБЛИЦЫ ВРАЛА: у gc_step стояло "<0.003", то есть
// "почти бесплатно". Зонд фазы был поставлен на вызывающей стороне в device.cpp,
// а тот путь работает ТОЛЬКО при gc_timeout: при gc_step/gc_default сборку зовёт
// seqParallel (Level.cpp, OnFrame), и её цена уехала в чужую фазу. Зонд перенесён
// ВНУТРЬ CLevel::script_gc. ⭐Мерить функцию, а не одну из её вызывающих сторон.
//
// ⭐ПОЧЕМУ gc_timeout БЫЛ ТАК ДОРОГ. `gc_step_timeout` (LuaJIT, lj_api.c) крутит
// `lj_gc_step` до истечения бюджета ИЛИ до конца цикла сборки, а бюджет -- не
// меньше psLUA_GCTIMEOUT_MIN = 2000 МКРОСЕКУНД. То есть движок делал ПОЛНЫЙ
// цикл сборки 26-мегабайтной кучи КАЖДЫЙ КАДР, шестьдесят раз в секунду, и
// платил 2.1 core-ms -- 25% ВСЕЙ процессорной работы кадра, больше, чем запись
// командных буферов (1.4) и планировщик (1.1) вместе.
// Инкрементальный сборщик LuaJIT на это не рассчитан: один цикл положено
// растягивать на много шагов. `gc_step` так и делает -- 128 КБ за вызов, то есть
// ~7.7 МБ/с при 60 fps. Дальше шаг снижен до 32 КБ и сделан адаптивным по
// размеру кучи (см. CLevel::script_gc): в штатном режиме 0.05-0.33 core-ms.
//
// ⚠gc_default (сам LuaJIT по порогу) даёт ЛУЧШУЮ сумму, но перекладывает сборку
// в АЛЛОКАТОР, то есть на главный поток: планировщик 1.06 -> 1.89, главный
// поток 3.5 -> 4.4. Для цели "60 fps на Switch" решает длина ПОСЛЕДОВАТЕЛЬНОГО
// пути, поэтому выбран gc_step: сборка остаётся на втором потоке и почти
// ничего не стоит.
// ⚠Читать ПАРОЙ: core-ms и КУЧУ (`[lua gc] heap ... KB` при cpu_budget >= 1).
// Сэкономить время сборки и получить рост кучи -- это не выигрыш, а отложенный
// хитч. В замере куча 26-28 МБ без тренда во всех четырёх режимах.
// ---------------------------------------------------------------------------
u32 ps_lua_gc_method{gc_step};
// lua_gc_async — hand the LuaJIT collector to a WORKER thread (the old behaviour).
// DEFAULT 0 = collect inline. LuaJIT is not thread-safe and the frame already has
// two other users of the same VM (the main frame's binders, the net tick on the
// second thread); adding the collector as a third produced a DEP EXECUTE fault at
// a data address inside gc_call_finalizer on 20-08. Kept as a cvar only so the old
// behaviour can be A/Bd — there is no configuration in which it is sound.
int ps_lua_gc_async{0};
constexpr xr_token lua_gc_method_token[]{{"gc_default", gc_default}, {"gc_step", gc_step}, {"gc_timeout", gc_timeout}, {}};

void IConsole_Command::add_to_LRU(shared_str const& arg)
{
    if (arg.size() == 0 || bEmptyArgsHandled)
    {
        return;
    }

    bool dup = (std::find(m_LRU.begin(), m_LRU.end(), arg) != m_LRU.end());
    if (!dup)
    {
        m_LRU.push_back(arg);
        if (m_LRU.size() > LRU_MAX_COUNT)
        {
            m_LRU.erase(m_LRU.begin());
        }
    }
}

void IConsole_Command::add_LRU_to_tips(vecTips& tips)
{
    vecLRU::reverse_iterator it_rb = m_LRU.rbegin();
    vecLRU::reverse_iterator it_re = m_LRU.rend();
    for (; it_rb != it_re; ++it_rb)
    {
        tips.push_back((*it_rb));
    }
}

// =======================================================

class CCC_Quit : public IConsole_Command
{
public:
    CCC_Quit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        Msg("! console 'quit' executed (KERNEL:disconnect + quit deferred)"); // diag: hunting an unexplained teardown ~+80s into automated net-client runs
        Console->Hide();
        Engine.Event.Defer("KERNEL:disconnect");
        Engine.Event.Defer("KERNEL:quit");
    }
};

//-----------------------------------------------------------------------

class CCC_DbgStrCheck : public IConsole_Command
{
public:
    CCC_DbgStrCheck(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args) { g_pStringContainer->verify(); }
};

class CCC_DbgStrDump : public IConsole_Command
{
public:
    CCC_DbgStrDump(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args) { g_pStringContainer->dump(); }
};

// lua_vm_watch_dump -- who has actually been inside the LuaJIT state and how
// often, plus the overlap count. Observation only; see xrCore/lua_vm_watch.h.
class CCC_LuaVmWatchDump : public IConsole_Command
{
public:
    CCC_LuaVmWatchDump(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args) { lua_vm_watch::dump(); }
};

class CCC_DbgLALibDump : public IConsole_Command
{
public:
    CCC_DbgLALibDump(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args) { LALib.DbgDumpInfo(); }
};

//-----------------------------------------------------------------------
class CCC_MotionsStat : public IConsole_Command
{
public:
    CCC_MotionsStat(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args) { g_pMotionsContainer->dump(); }
};

class CCC_TexturesStat : public IConsole_Command
{
public:
    CCC_TexturesStat(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        Device.DumpResourcesMemoryUsage();
        // Device.Resources->_DumpMemoryUsage();
        //	TODO: move this console commant into renderer
        // VERIFY(0);
    }
};

//-----------------------------------------------------------------------
class CCC_E_Dump : public IConsole_Command
{
public:
    CCC_E_Dump(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args) { Engine.Event.Dump(); }
};
class CCC_E_Signal : public IConsole_Command
{
public:
    CCC_E_Signal(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        char Event[128], Param[128];
        Event[0] = 0;
        Param[0] = 0;
        sscanf(args, "%[^,],%s", Event, Param);
        Engine.Event.Signal(Event, (u64)Param);
    }
};

//-----------------------------------------------------------------------
class CCC_Help : public IConsole_Command
{
public:
    CCC_Help(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        Log("- --- Command listing: start ---");
        CConsole::vecCMD_IT it;
        for (it = Console->Commands.begin(); it != Console->Commands.end(); ++it)
        {
            IConsole_Command& C = *(it->second);
            TStatus _S;
            C.Status(_S);
            TInfo _I;
            C.Info(_I);

            Msg("%-20s (%-10s) --- %s", C.Name(), _S, _I);
        }
        Log("Key: Ctrl + A         === Select all ");
        Log("Key: Ctrl + C         === Copy to clipboard ");
        Log("Key: Ctrl + V         === Paste from clipboard ");
        Log("Key: Ctrl + X         === Cut to clipboard ");
        Log("Key: Ctrl + Z         === Undo ");
        Log("Key: Ctrl + Insert    === Copy to clipboard ");
        Log("Key: Shift + Insert   === Paste from clipboard ");
        Log("Key: Shift + Delete   === Cut to clipboard ");
        Log("Key: Insert           === Toggle mode <Insert> ");
        Log("Key: Back / Delete          === Delete symbol left / right ");

        Log("Key: Up   / Down            === Prev / Next command in tips list ");
        Log("Key: Ctrl + Up / Ctrl + Down === Prev / Next executing command ");
        Log("Key: Left, Right, Home, End {+Shift/+Ctrl}       === Navigation in text ");
        Log("Key: PageUp / PageDown      === Scrolling history ");
        Log("Key: Tab  / Shift + Tab     === Next / Prev possible command from list");
        Log("Key: Enter  / NumEnter      === Execute current command ");

        Log("- --- Command listing: end ----");
    }
};

XRCORE_API void _dump_open_files(int mode);

class CCC_DumpOpenFiles : public IConsole_Command
{
public:
    CCC_DumpOpenFiles(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = FALSE; };
    virtual void Execute(LPCSTR args)
    {
        int _mode = atoi(args);
        _dump_open_files(_mode);
    }
};

//-----------------------------------------------------------------------
class CCC_SaveCFG : public IConsole_Command
{
public:
    CCC_SaveCFG(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        string_path cfg_full_name;
        xr_strcpy(cfg_full_name, (xr_strlen(args) > 0) ? args : Console->ConfigFile);

        bool b_abs_name = xr_strlen(cfg_full_name) > 2 && cfg_full_name[1] == ':';

        if (!b_abs_name)
            FS.update_path(cfg_full_name, fsgame::app_data_root, cfg_full_name);

        if (strext(cfg_full_name))
            *strext(cfg_full_name) = 0;
        xr_strcat(cfg_full_name, ".ltx");

        BOOL b_allow = TRUE;
        if (FS.exist(cfg_full_name))
            b_allow = SetFileAttributes(cfg_full_name, FILE_ATTRIBUTE_NORMAL);

        if (b_allow)
        {
            IWriter* F = FS.w_open(cfg_full_name);
            CConsole::vecCMD_IT it;
            for (it = Console->Commands.begin(); it != Console->Commands.end(); ++it)
                it->second->Save(F);
            FS.w_close(F);
            Msg("Config-file [%s] saved successfully", cfg_full_name);
        }
        else
            Msg("!Cannot store config file [%s]", cfg_full_name);
    }
};
CCC_LoadCFG::CCC_LoadCFG(LPCSTR N) : IConsole_Command(N){};

void CCC_LoadCFG::Execute(LPCSTR args)
{
    Msg("Executing config-script \"%s\"...", args);
    string_path cfg_name;

    xr_strcpy(cfg_name, args);
    if (strext(cfg_name))
        *strext(cfg_name) = 0;
    xr_strcat(cfg_name, ".ltx");

    string_path cfg_full_name;

    FS.update_path(cfg_full_name, fsgame::app_data_root, cfg_name);

    // if( NULL == FS.exist(cfg_full_name) )
    //	FS.update_path					(cfg_full_name, "$fs_root$", cfg_name);

    if (NULL == FS.exist(cfg_full_name))
        xr_strcpy(cfg_full_name, cfg_name);

    IReader* F = FS.r_open(cfg_full_name);

    string1024 str;
    if (F)
    {
        while (!F->eof())
        {
            if (F->tell() == 0 && strstr(cfg_full_name, "user.ltx"))
            {
                // Костыль от ситуации когда в редких случаях почему-то у игроков бьётся user.ltx - оказывается набит нулями, в результате чего игра не
                // запускается. Не понятно почему так происходит, поэтому сделал тут обработку такой ситуации.

                if (F->elapsed() >= sizeof(u8))
                {
                    if (F->r_u8() == 0)
                    {
                        Msg("!![%s] file [%s] broken!", __FUNCTION__, cfg_full_name);
                        FS.r_close(F);
                        FS.file_delete(cfg_full_name);
                        return;
                    }

                    F->seek(F->tell() - sizeof(u8));
                }
            }

            F->r_string(str, sizeof(str));

            // ⚠cfg_load runs a COMMAND SCRIPT, not an ini file: every line goes
            // straight to the console, so a `;` comment came back as
            // "Unknown command: ;" — one error line per comment. That is why
            // user.ltx has never had a comment in it, and why a config meant to
            // be READ (the rspec_vk_* quality presets, which are a judgement
            // about this renderer's costs and need to say so) could not explain
            // itself. Blank lines and `;` / `//` comments are now skipped.
            const char* p = str;
            while (*p == ' ' || *p == '\t')
                ++p;
            if (!*p || *p == ';' || (p[0] == '/' && p[1] == '/'))
                continue;

            if (allow(p))
                Console->Execute(p);
        }

        FS.r_close(F);
        Msg("[%s] successfully loaded.", cfg_full_name);
    }
    else
    {
        Msg("! Cannot open script file [%s]", cfg_full_name);
    }
}

//-----------------------------------------------------------------------
class CCC_Start : public IConsole_Command
{
private:
    std::string parse(const std::string& str)
    {
        static std::regex Reg("\\(([^)]+)\\)");
        std::smatch results;
        ASSERT_FMT(std::regex_search(str, results, Reg), "Failed parsing string: [%s]", str.c_str());
        return results[1].str();
    }

public:
    CCC_Start(const char* N) : IConsole_Command(N){};
    void Execute(const char* args) override
    {
        auto str = parse(args);

        Msg("Starting server...");
        Engine.Event.Defer("KERNEL:start", u64(xr_strdup(str.c_str())), u64(xr_strdup("localhost")));
    }
};

// pure network client -- connects to a remote server instead of also
// hosting a local xrServer (that's what "start server(...)"/CCC_Start
// above does). server_options is passed empty on purpose: net_start1()/
// net_start2() (Level_start.cpp) already skip creating a local xrServer when
// it's empty. op_client carries the raw "ip:port" target; IPureClient::Connect
// (xr_3da/NET_Server_Trash/NET_Client.cpp) recognizes anything other than the
// hardcoded "localhost" CCC_Start uses and routes it through
// NET_GNS::g_ClientConnect (installed by an external client backend; null
// in stock builds, so the command is a no-op there).
class CCC_Connect : public IConsole_Command
{
private:
    std::string parse(const std::string& str)
    {
        static std::regex Reg("\\(([^)]+)\\)");
        std::smatch results;
        ASSERT_FMT(std::regex_search(str, results, Reg), "Failed parsing string: [%s]", str.c_str());
        return results[1].str();
    }

public:
    CCC_Connect(const char* N) : IConsole_Command(N){};
    void Execute(const char* args) override
    {
        auto str = parse(args);

        Msg("Connecting as network client to: %s", str.c_str());
        Engine.Event.Defer("KERNEL:start", u64(xr_strdup("")), u64(xr_strdup(str.c_str())));
    }
};

class CCC_Disconnect : public IConsole_Command
{
public:
    CCC_Disconnect(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        Msg("! console 'disconnect' executed"); // diag: hunting an unexplained teardown ~+80s into automated net-client runs
        Engine.Event.Defer("KERNEL:disconnect");
    }
};

//-----------------------------------------------------------------------
class CCC_VID_Reset : public IConsole_Command
{
public:
    CCC_VID_Reset(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        if (Device.b_is_Ready)
        {
            Device.Reset();
        }
    }
};

class CCC_VidMode : public CCC_Token
{
    u32 _dummy{};

public:
    CCC_VidMode(LPCSTR N) : CCC_Token(N, &_dummy, nullptr) { bEmptyArgsHandled = FALSE; };
    virtual void Execute(LPCSTR args)
    {
        u32 _w, _h;
        int cnt = sscanf(args, "%dx%d", &_w, &_h);
        if (cnt == 2)
        {
            psCurrentVidMode[0] = _w;
            psCurrentVidMode[1] = _h;
        }
        else
        {
            Msg("! Wrong video mode [%s]", args);
            return;
        }
    }
    virtual void Status(TStatus& S) { xr_sprintf(S, sizeof(S), "%dx%d", psCurrentVidMode[0], psCurrentVidMode[1]); }
    virtual const xr_token* GetToken() override { return vid_mode_token; }
    virtual void Info(TInfo& I) { xr_strcpy(I, sizeof(I), "change screen resolution WxH"); }

    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        TStatus str, cur;
        Status(cur);

        bool res = false;
        const xr_token* tok = GetToken();
        while (tok->name && !res)
        {
            if (!xr_strcmp(tok->name, cur))
            {
                xr_sprintf(str, sizeof(str), "%s  (current)", tok->name);
                tips.push_back(str);
                res = true;
            }
            tok++;
        }
        if (!res)
        {
            tips.push_back("---  (current)");
        }
        tok = GetToken();
        while (tok->name)
        {
            tips.push_back(tok->name);
            tok++;
        }
    }
};

//-----------------------------------------------------------------------
class CCC_SND_Restart : public IConsole_Command
{
public:
    CCC_SND_Restart(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args) { Sound->_restart(); }
};

constexpr xr_token FpsLockToken[] = {
    {"nofpslock", 0},
    {"fpslock60", 60},
    {"fpslock120", 120},
    {"fpslock144", 144},
    {"fpslock240", 240},
    {nullptr, 0}};

// ⭐snd_test (08-10-2026): a known sound put where the sound upgrade has something to do, so it can be heard and
// its numbers read (snd_dbg 2) without hunting for a situation in the game.
//   snd_test wall [file]       - looped, 1.5 m behind the first wall straight ahead (up to 40 m)
//   snd_test open [dist] [file] - looped, straight ahead in the open (default 12 m)
//   snd_test at x,y,z [file]    - looped, there
//   snd_test shot [dist]        - one shot ahead (a loud event for snd_hdr)
//   snd_test stop
class CCC_SndTest : public IConsole_Command
{
    // on the heap and never freed: a ref_sound still alive when the sound manager is gone faults at exit
    ref_sound& m_loop = *xr_new<ref_sound>();

    static bool pick_file(string_path& out, LPCSTR wanted)
    {
        static const char* fallback[] = {"weapons\\ak74u_shot", "weapons\\ak74_shoot", "weapons\\pm_shot", "weapons\\walther_shoot", "ambient\\fire2"};
        string_path fn;
        if (wanted && wanted[0])
        {
            if (FS.exist(fn, "$game_sounds$", wanted, ".ogg"))
            {
                xr_strcpy(out, wanted);
                return true;
            }
            Msg("! [snd test] no sound '%s'", wanted);
            return false;
        }
        // the mod's own guns first: whatever their config shoots with
        for (const char* sect : {"wpn_ak74", "wpn_ak74u", "wpn_pm", "wpn_abakan"})
        {
            if (!pSettings->line_exist(sect, "snd_shoot"))
                continue;
            string_path f;
            _GetItem(pSettings->r_string(sect, "snd_shoot"), 0, f);
            if (FS.exist(fn, "$game_sounds$", f, ".ogg"))
            {
                xr_strcpy(out, f);
                return true;
            }
        }
        for (const char* f : fallback)
            if (FS.exist(fn, "$game_sounds$", f, ".ogg"))
            {
                xr_strcpy(out, f);
                return true;
            }
        Msg("! [snd test] none of the test sounds is there -- give a file");
        return false;
    }

    void play_loop(const Fvector& P, LPCSTR file)
    {
        string_path f;
        if (!pick_file(f, file))
            return;
        m_loop.destroy();
        m_loop.create(f, st_Effect, 0x80000000u /*SOUND_TYPE_WEAPON*/);
        m_loop.play_at_pos(nullptr, P, sm_Looped);
        Msg("[snd test] '%s' looped at [%.1f %.1f %.1f], %.1f m from the camera", f, VPUSH(P), P.distance_to(Device.vCameraPosition));
    }

public:
    CCC_SndTest(LPCSTR N) : IConsole_Command(N) {}

    void Execute(LPCSTR args) override
    {
        if (!g_pGameLevel)
        {
            Msg("! [snd test] no level");
            return;
        }
        string256 verb{}, a1{}, a2{};
        sscanf(args, "%255s %255s %255s", verb, a1, a2);
        const Fvector C = Device.vCameraPosition;
        Fvector D = Device.vCameraDirection;
        D.normalize();

        if (!xr_strcmp(verb, "stop"))
        {
            m_loop.destroy();
            Msg("[snd test] stopped");
        }
        else if (!xr_strcmp(verb, "wall") || !xr_strcmp(verb, "open"))
        {
            // ahead first, then round the compass in 16 steps (a teleport leaves the camera facing anywhere):
            // wall = the first direction with a wall 3..25 m off (the source 1.5 m behind it), open = the first with
            // nothing for dist m
            const bool wall = !xr_strcmp(verb, "wall");
            const float dist = (!wall && a1[0]) ? float(atof(a1)) : 12.f;
            Fvector flat{D.x, 0.f, D.z};
            if (flat.square_magnitude() < 1e-4f)
                flat.set(0.f, 0.f, 1.f);
            flat.normalize();
            for (int k = 0; k < 17; ++k)
            {
                Fvector dir = D;
                if (k > 0)
                {
                    const float a = float(k - 1) * PI_MUL_2 / 16.f;
                    dir.set(flat.x * _cos(a) - flat.z * _sin(a), 0.f, flat.x * _sin(a) + flat.z * _cos(a));
                }
                collide::rq_result R;
                const BOOL hit = g_pGameLevel->ObjectSpace.RayPick(C, dir, wall ? 25.f : dist, collide::rqtStatic, R, nullptr);
                if (wall && hit && R.range >= 3.f)
                {
                    Fvector P;
                    P.mad(C, dir, R.range + 1.5f);
                    Msg("[snd test] wall at %.1f m (direction %d)", R.range, k);
                    play_loop(P, a1);
                    return;
                }
                if (!wall && !hit)
                {
                    Fvector P;
                    P.mad(C, dir, dist);
                    Msg("[snd test] open for %.1f m (direction %d)", dist, k);
                    play_loop(P, a2);
                    return;
                }
            }
            Msg("! [snd test] no direction fits '%s'", verb);
        }
        else if (!xr_strcmp(verb, "side"))
        {
            // side <m>: looped, that far to the camera's right (negative: left) -- which channel gets it (audren panning)
            const float d = a1[0] ? float(atof(a1)) : 5.f;
            Fvector P;
            P.mad(C, Device.vCameraRight, d);
            play_loop(P, a2);
        }
        else if (!xr_strcmp(verb, "at"))
        {
            Fvector P;
            if (sscanf(a1, "%f,%f,%f", &P.x, &P.y, &P.z) != 3)
            {
                Msg("! [snd test] at x,y,z");
                return;
            }
            play_loop(P, a2);
        }
        else if (!xr_strcmp(verb, "shot"))
        {
            const float dist = a1[0] ? float(atof(a1)) : 8.f;
            string_path f;
            if (!pick_file(f, nullptr))
                return;
            static ref_sound& shot = *xr_new<ref_sound>();
            if (!shot._handle())
                shot.create(f, st_Effect, 0x80000000u | 0x00200000u /*SOUND_TYPE_WEAPON_SHOOTING*/);
            Fvector P;
            P.mad(C, D, dist);
            shot.play_no_feedback(nullptr, 0, 0.f, &P);
            Msg("[snd test] shot '%s' at %.1f m", f, dist);
        }
        else
            Msg("snd_test wall [file] | open [dist] [file] | at x,y,z [file] | shot [dist] | stop");
    }
};

class CCC_soundDevice : public CCC_Token
{
    typedef CCC_Token inherited;

public:
    CCC_soundDevice(LPCSTR N) : inherited(N, &snd_device_id, nullptr){};
    virtual ~CCC_soundDevice() {}

    virtual void Execute(LPCSTR args)
    {
        GetToken();
        if (!tokens)
            return;
        inherited::Execute(args);
    }

    virtual void Status(TStatus& S)
    {
        GetToken();
        if (!tokens)
            return;
        inherited::Status(S);
    }

    virtual const xr_token* GetToken() override
    {
        tokens = snd_devices_token;
        return inherited::GetToken();
    }

    virtual void Save(IWriter* F)
    {
        GetToken();
        if (!tokens)
            return;
        inherited::Save(F);
    }
};

class CCC_ExclusiveMode : public CCC_Mask
{
    using inherited = CCC_Mask;

public:
    CCC_ExclusiveMode(const char* N, Flags32* V, u32 M) : inherited(N, V, M) {}

    void Execute(const char* args) override
    {
        inherited::Execute(args);

        const bool val = GetValue();
        if (val != pInput->exclusive_mode())
            pInput->exclusive_mode(val);
    }
};

class ENGINE_API CCC_HideConsole : public IConsole_Command
{
public:
    CCC_HideConsole(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }

    virtual void Execute(LPCSTR args) { Console->Hide(); }
    virtual void Status(TStatus& S) { S[0] = 0; }
    virtual void Info(TInfo& I) { xr_sprintf(I, sizeof(I), "hide console"); }
};

class ENGINE_API CCC_ClearConsole : public IConsole_Command
{
public:
    CCC_ClearConsole(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }

    virtual void Execute(LPCSTR args)
    {
        LogFile.clear();
    }
    virtual void Status(TStatus& S) { S[0] = 0; }
    virtual void Info(TInfo& I) { xr_sprintf(I, sizeof(I), "clear console"); }
};

// cpu_off 1: benchmark switch -- the scheduler (AI think, scripts, A-Life, every shedule_Update) and the physics step do not run.
// Camera, input, the level's own per-frame update and rendering stay, so what is left is the cost of drawing. Not saved.
ENGINE_API int g_cpu_off = 0;
ENGINE_API float psHUD_FOV_def = 0.45f;
ENGINE_API float psHUD_FOV = psHUD_FOV_def;

extern int rsDVB_Size;
extern int rsDIB_Size;

extern int g_ErrorLineCount;

extern float g_fontWidthScale;
extern float g_fontHeightScale;

extern float puddles_drying;
extern float puddles_wetting;

void CCC_Register()
{
    // General
    CMD1(CCC_Help, "help");
    CMD1(CCC_Quit, "quit");
    CMD1(CCC_Start, "start");
    CMD1(CCC_Connect, "connect");
    CMD1(CCC_Disconnect, "disconnect");
    CMD1(CCC_SaveCFG, "cfg_save");
    CMD1(CCC_LoadCFG, "cfg_load");

    CMD1(CCC_MotionsStat, "stat_motions");
    CMD1(CCC_TexturesStat, "stat_textures");

#ifdef DEBUG
    CMD3(CCC_Mask, "mt_particles", &psDeviceFlags, mtParticles);

    CMD1(CCC_DbgStrCheck, "dbg_str_check");
    CMD1(CCC_DbgStrDump, "dbg_str_dump");

    CMD3(CCC_Mask, "mt_sound", &psDeviceFlags, mtSound);
    CMD3(CCC_Mask, "mt_physics", &psDeviceFlags, mtPhysics);
    CMD3(CCC_Mask, "mt_network", &psDeviceFlags, mtNetwork);

    // Events
    CMD1(CCC_E_Dump, "e_list");
    CMD1(CCC_E_Signal, "e_signal");

    CMD3(CCC_Mask, "rs_wireframe", &psDeviceFlags, rsWireframe);
    CMD3(CCC_Mask, "rs_clear_bb", &psDeviceFlags, rsClearBB);
    CMD3(CCC_Mask, "rs_occlusion", &psDeviceFlags, rsOcclusion);

    CMD3(CCC_Mask, "rs_detail", &psDeviceFlags, rsDetails);

    CMD3(CCC_Mask, "rs_render_statics", &psDeviceFlags, rsDrawStatic);
    CMD3(CCC_Mask, "rs_render_dynamics", &psDeviceFlags, rsDrawDynamic);
#endif

    // Render device states
    CMD3(CCC_Mask, "rs_always_active", &psDeviceFlags, rsAlwaysActive);
    CMD3(CCC_Token, "r_fps_lock", &g_dwFPSlimit, FpsLockToken);
    // Хвост ожидания лимитера: сколько мкс перед дедлайном дожидать спином.
    // 250 = дефолт (ждём таймером, спин только страховочный), 2000 ~ прежнее
    // поведение. NoSave: это рычаг для A/B, а не настройка.
    CMD4_NOSAVE(CCC_Integer, "r_fps_spin_us", &g_fps_spin_us, 0, 4000);

    CMD3(CCC_Mask, "rs_v_sync", &psDeviceFlags, rsVSync);
    // ⭐Back from the dead, and now honoured: off = a bordered window sized to
    // vid_mode, on = borderless over the whole monitor. Both need `vid_restart`,
    // which the options screen issues for exactly this pair.
    CMD3(CCC_Mask, "rs_fullscreen", &psDeviceFlags, rsFullscreen);
    //CMD3(CCC_Mask, "rs_refresh_60hz", &psDeviceFlags, rsRefresh60hz);
    CMD3(CCC_Mask, "rs_stats", &psDeviceFlags, rsStatistic);
    // ⚠ THE UPPER BOUND WAS 1.1, AND THAT WAS THE WALL. far_plane reaching the camera
    // projection is the weather's value TIMES this (CameraManager.cpp), so with the old
    // clamp no setting could widen the world by more than 10% — which is why the view
    // ended at ~300-400 m no matter what the renderer's LOD chain was capable of.
    // Raised to 20 so the limit becomes a real choice rather than a hidden ceiling.
    // ⚠ 1.0 is still the default: nothing changes until it is asked for. Widening it
    // also needs r_fog_dist (renderer-side fog range scale), or the extra world
    // arrives painted in flat fog colour.
    CMD4(CCC_Float, "rs_vis_distance", &psVisDistance, 0.4f, 20.0f);
    {
        // 1 = no scheduler (AI/scripts/A-Life) and no physics step: render-only benchmark. Never saved to user.ltx.
        static CCC_Integer cpu_off("cpu_off", &g_cpu_off, 0, 1);
        cpu_off.SetCanSave(false);
        Console->AddCommand(&cpu_off);
    }
    CMD3(CCC_Mask, "rs_cam_pos", &psDeviceFlags, rsCameraPos);

    CMD3(CCC_Mask, "rs_occ_draw", &psDeviceFlags, rsOcclusionDraw);

    //CMD2(CCC_Gamma, "rs_c_gamma", &ps_gamma);
    //CMD2(CCC_Gamma, "rs_c_brightness", &ps_brightness);
    //CMD2(CCC_Gamma, "rs_c_contrast", &ps_contrast);

    CMD3(CCC_Mask, "rs_hw_stats", &psDeviceFlags, rsHWInfo);

    // General video control
    CMD1(CCC_VidMode, "vid_mode");
    CMD1(CCC_VID_Reset, "vid_restart");

    // Sound
    CMD2(CCC_Float, "snd_volume_eff", &psSoundVEffects);
    CMD2(CCC_Float, "snd_volume_music", &psSoundVMusic);
    CMD1(CCC_SND_Restart, "snd_restart");
    //CMD3(CCC_Mask, "snd_acceleration", &psSoundFlags, ss_Hardware);
    CMD3(CCC_Mask, "snd_efx", &psSoundFlags, ss_EAX);
    CMD4(CCC_Integer, "snd_targets", &psSoundTargets, 128, 1024);
    CMD4(CCC_Integer, "snd_cache_size", &psSoundCacheSizeMB, 32, 128);
    {
        extern ENGINE_API int g_snd_core; // device.cpp: xrSound's update on that core (-1 = in the second-thread task)
        CMD4(CCC_Integer, "snd_core", &g_snd_core, -1, 2);
    }
#ifdef __SWITCH__
    CMD4(CCC_Integer, "nx_malloc_slow_us", &g_xr_malloc_slow_us, 0, 1000000);
    CMD4(CCC_Integer, "nx_ttapi_prio", &g_ttapi_prio, 0x2C, 0x3F); // xrCore.cpp: TTAPI workers' priority, 59 = libnx's
    CMD4_NOSAVE(CCC_Integer, "nx_ttapi_core", &g_ttapi_core, -1, 5); // task_thread_pool.hpp: TTAPI workers' core, -1 = libnx default
#endif

    // Tiled CDB collision streaming (streaming-world Stage C); cdb_tiles takes effect on level load
    {
        extern int psCDB_Tiles, psCDB_TileBudget, psCDB_TileRadius;
        CMD4(CCC_Integer, "cdb_tiles", &psCDB_Tiles, 0, 2);
        extern ENGINE_API int g_frame_prof_ms; // device.cpp [frame prof]
        CMD4(CCC_Integer, "dbg_frame_prof", &g_frame_prof_ms, 0, 1000);
        extern ENGINE_API int g_rt_prof_ms; // device.cpp [rt prof]: a render-thread job this long, by part
        CMD4(CCC_Integer, "dbg_rt_prof", &g_rt_prof_ms, 0, 1000);
        extern ENGINE_API int g_hitch_ms; // device.cpp [hitch] threshold, ms
        CMD4(CCC_Integer, "dbg_hitch_ms", &g_hitch_ms, 1, 1000);
        CMD4(CCC_Integer, "cdb_tile_budget_mb", &psCDB_TileBudget, 64, 4096);
        CMD4(CCC_Integer, "cdb_tile_radius", &psCDB_TileRadius, 64, 2000);
        extern int g_cdb_cache_read_threads;   // readers for the cache hit's 1.3 GB of verts+tris
        CMD4(CCC_Integer, "cdb_cache_read_threads", &g_cdb_cache_read_threads, 0, 16);
        extern int g_cdb_tile_preload_mb;      // a tile tree up to this size is read behind the load (0 = on demand)
        CMD4(CCC_Integer, "cdb_tile_preload_mb", &g_cdb_tile_preload_mb, 0, 4096);
        extern int ai_cover_threads;           // static-cover passes over the AI map (0 = per core, 1 = original)
        CMD4(CCC_Integer, "ai_cover_threads", &ai_cover_threads, 0, 16);
        extern int ai_cover_async;             // build the cover tree during the render phase instead of before it
        CMD4(CCC_Integer, "ai_cover_async", &ai_cover_async, 0, 1);
        extern int g_spatial_verify;           // walk the whole spatial octree every 30 frames to check its counters
        CMD4(CCC_Integer, "g_spatial_verify", &g_spatial_verify, 0, 1);
    }

    CMD3(CCC_Mask, "snd_stats", &g_stats_flags, st_sound);
    CMD3(CCC_Mask, "snd_stats_min_dist", &g_stats_flags, st_sound_min_dist);
    CMD3(CCC_Mask, "snd_stats_max_dist", &g_stats_flags, st_sound_max_dist);
    CMD3(CCC_Mask, "snd_stats_ai_dist", &g_stats_flags, st_sound_ai_dist);
    CMD3(CCC_Mask, "snd_stats_info_name", &g_stats_flags, st_sound_info_name);
    CMD3(CCC_Mask, "snd_stats_info_object", &g_stats_flags, st_sound_info_object);

    // Mouse
    CMD3(CCC_Mask, "mouse_invert", &psMouseInvert, 1);
    CMD4(CCC_Float, "mouse_sens", &psMouseSens, 0.001f, 0.6f);

    // Camera
    CMD4(CCC_Float, "cam_inert", &psCamInert, 0.0f, 0.99f);
    CMD4(CCC_Float, "cam_inert_sprint", &psSprintCamInert, 0.0f, 0.99f);
    CMD2(CCC_Float, "cam_slide_inert", &psCamSlideInert); // for 3rd person cam

    CMD1(CCC_soundDevice, "snd_device");
    CMD3(CCC_Mask, "snd_device_default", &psSoundFlags, ss_UseDefaultDevice);

    /*psSoundOcclusionScale = pSettings->r_float("sound", "occlusion_scale");
    clamp(psSoundOcclusionScale, 0.1f, 1.f);*/

    CMD4(CCC_Float, "snd_rolloff", &psSoundRolloff, 0.1f, 2.f);
    //CMD4(CCC_Float, "snd_fade_speed", &psSoundFadeSpeed, 1.f, 10.f);
    CMD4(CCC_Float, "snd_occ_scale", &psSoundOcclusionScale, 0.1f, 1.f);
    //CMD4(CCC_Float, "snd_occ_hf", &psSoundOcclusionHf, 0.f, 1.f);
    //CMD4(CCC_Float, "snd_occ_mtl", &psSoundOcclusionMtl, 0.f, 1.f);
    CMD2(CCC_Bool, "snd_enable_float_pcm", &snd_enable_float_pcm);
    // Binaural audio for headphones: 0 off, 1 on, 2 let OpenAL Soft decide.
    // Applied when the context is created, so it needs a restart to take effect.
    CMD4(CCC_Integer, "snd_hrtf", &snd_hrtf, 0, 2);
    // the 08-10-2026 sound upgrade (xrSound/Sound.h): 0 / off = the old behaviour of each part
    CMD4(CCC_Integer, "snd_occ_mode", &snd_occ_mode, 0, 1);       // occlusion: 0 one ray x volume, 1 paths + materials + low-pass
    CMD4(CCC_Integer, "snd_occ_rays", &snd_occ_rays, 1, 4);       // paths refreshed per source per update
    CMD4(CCC_Float, "snd_occ_hf", &snd_occ_hf, 0.f, 1.f);         // highs left behind a full obstacle
    CMD4(CCC_Float, "snd_occ_mtl", &snd_occ_mtl, 0.f, 1.f);       // weight of the materials' transmission
    CMD4(CCC_Integer, "snd_voices", &snd_voices, 8, 256);         // sources mixed at once, the rest virtual
    CMD4(CCC_Float, "snd_hdr", &snd_hdr, 0.f, 1.f);               // how deep a shot/explosion ducks the rest
    CMD4(CCC_Float, "snd_hdr_release", &snd_hdr_release, 0.1f, 10.f);
    CMD4(CCC_Integer, "snd_prop", &snd_prop, 0, 1);               // heard round obstacles by the AI map's paths
    CMD4(CCC_Float, "snd_prop_radius", &snd_prop_radius, 10.f, 100.f);
    CMD4(CCC_Integer, "snd_env_auto", &snd_env_auto, 0, 2);       // reverb from rays: 0 zones only, 1 where no zone, 2 always
    CMD4(CCC_Integer, "snd_env_rays", &snd_env_rays, 1, 32);
    CMD4(CCC_Integer, "snd_dbg", &snd_dbg, 0, 2);                 // [snd] line every 2 s (2: + occluded sources)
    CMD4(CCC_Integer, "snd_prof", &snd_prof, 0, 1000);            // [snd prof] an update this long (ms), by part
    CMD4(CCC_Integer, "snd_start_blocks", &snd_start_blocks, 1, 3); // blocks decoded when a sound starts, the rest later
    CMD4(CCC_Integer, "snd_nxa", &snd_nxa, 0, 1);                 // .nxa (DSP-ADPCM) over .ogg where there is one
    CMD4(CCC_Integer, "snd_nx_voices", &snd_nx_voices, 16, 256);  // Switch audren: DSP voices (at start)
    CMD4(CCC_Integer, "snd_nx_arena_mb", &snd_nx_arena_mb, 8, 256); // Switch audren: resident .nxa memory (at start)
    CMD4(CCC_Integer, "snd_nx_reverb", &snd_nx_reverb, 0, 1);     // Switch audren: the DSP's I3DL2 room reverb (at start; plays with snd_efx)
    CMD4(CCC_Float, "snd_nx_capture", &snd_nx_capture, 0.f, 60.f);   // Switch audren: record the final mix for N s -> nxcap_NN.wav
    CMD4(CCC_Integer, "snd_nx_rev_test", &snd_nx_rev_test, 0, 1); // Switch audren: 1 = the DSP reverb as a big hall (check)
    CMD1(CCC_SndTest, "snd_test");

#ifdef DEBUG
    CMD1(CCC_DumpOpenFiles, "dump_open_files");
#endif

    CMD3(CCC_ExclusiveMode, "input_exclusive_mode", &psDeviceFlags, rsExclusiveMode);

    CMD1(CCC_HideConsole, "hide");
    CMD1(CCC_ClearConsole, "clear");

    CMD4(CCC_Float, "g_font_scale_x", &g_fontWidthScale, 0.2f, 5.0f);
    CMD4(CCC_Float, "g_font_scale_y", &g_fontHeightScale, 0.2f, 5.0f);
	
	CMD4(CCC_Float, "rain_puddles_drying", &puddles_drying, 0.1f, 20.0f);
    CMD4(CCC_Float, "rain_puddles_wetting", &puddles_wetting, 0.1f, 20.0f);

    CMD2(CCC_Bool, "g_prefetch", &g_prefetch);

    //extern BOOL g_laserdotcorrection;
    //CMD2(CCC_Bool, "g_laserdotcorrection", &g_laserdotcorrection);

    // commands to control custom shader params from console (and use to user.ltx)

    // не уверен какие значения тут взять
    constexpr Fvector4 min_val{-200, -200, -200, -200}, max_val{200, 200, 200, 200};

    for (auto& [key, value] : shader_exports.customExports)
    {
        auto* xCCC_Vector3NonStrict = xr_new<CCC_Vector4NonStrict>(key.c_str(), &value, min_val, max_val);
        Console->AddCommand(xCCC_Vector3NonStrict);

        string128 save_param;
        xr_strconcat(save_param, key.c_str(), "_save");

        bool canSave = READ_IF_EXISTS(pSettings, r_bool, "shader_params_export", save_param, false);
        xCCC_Vector3NonStrict->SetCanSave(canSave);
    }

    extern float psShedulerMax;
    // ПОТОЛОК БЮДЖЕТА ProcessStep в мс (дефолт 10).
    // ⚠Нижняя граница была 3 мс, а сравнение шло в ЦЕЛЫХ мс ⇒ при 60 fps,
    // где ProcessStep стоит 0.9 мс, штатный ограничитель нагрузки не срабатывал
    // никогда. Теперь сравнение в мкс и минимум 0.1 — см. xrSheduler.cpp.
    CMD4(CCC_Float, "rs_sheduler_max", &psShedulerMax, 0.1f, 66.f);

    CMD1(CCC_DbgLALibDump, "dbg_lalib_dump");

    // ⚠⚠NoSave, и это ЧАСТЬ ПРАВКИ, а не гигиена. Квар был сохраняемым, поэтому
    // в user.ltx лежит строка "lua_gc_method gc_timeout" -- она молча пересилила
    // бы новый дефолт, и замер 19-09 (сборка мусора = 25% работы кадра) остался
    // бы на бумаге. Со NoSave строка перестаёт записываться и исчезает после
    // первого нормального выхода; один сеанс со старым значением -- цена, и она
    // видна в логе строкой "[lua gc] method".
    // Те же соображения для шага и бюджета ниже: это рычаги A/B, не настройки.
    CMD3_NOSAVE(CCC_Token, "lua_gc_method", &ps_lua_gc_method, lua_gc_method_token);
    CMD4_NOSAVE(CCC_Integer, "lua_gc_async", &ps_lua_gc_async, 0, 1); // 1 = collect on a worker (old, races); 0 = inline
    // 0 = off, 1 = track + report overlaps, 2 = also trace the first entry per thread per site
    CMD4_NOSAVE(CCC_Integer, "lua_vm_watch", &lua_vm_watch::enabled, 0, 2);
    // ⭐sched_stats — разбор планировщика по объектам с выделением доли Lua.
    // Включает и хронометраж входов в VM: без него столбец Lua был бы нулевым,
    // а прибор молча врал бы, что скрипты бесплатны.
    // NoSave: диагностика не должна залипнуть в user.ltx и мерить чужие прогоны
    // (на этом проекте r_ssao залипал так четыре раза).
    CMD4_NOSAVE(CCC_Integer, "sched_stats", &g_sched_stats, 0, 1);
    // Множитель ПЕРИОДА обычной очереди в процентах: 200 = все думают вдвое реже.
    // ⚠МЕНЯЕТ ПОВЕДЕНИЕ ИГРЫ (скорость реакции ИИ), поэтому NoSave и дефолт 100:
    // это рычаг для A/B и для слабого железа, а не настройка по умолчанию.
    CMD4_NOSAVE(CCC_Integer, "sched_rate", &g_sched_rate, 25, 1000);
    CMD4_NOSAVE(CCC_Integer, "lua_time", &lua_vm_watch::time_enabled, 0, 2);   // 2 = ещё два кадра стека вызывающей стороны
    CMD4_NOSAVE(CCC_Integer, "lua_fastpath", &lua_vm_watch::wrapper_fastpath, 0, 1);   // 0 = старый путь (всё через VM), для A/B в одном прогоне
    // Сэмплирующий профайль Lua-стека: N = раз в N тысяч инструкций VM (0 = выкл).
    // ⚠глушит JIT, пока включён; числа читать как ПРОПОРЦИИ.
    CMD4_NOSAVE(CCC_Integer, "lua_sample", &lua_vm_watch::sample_enabled, 0, 64);
    // ⚠Гейт Lua-прибора sigprof (ogse_signals.script). ОТДЕЛЬНО от sched_stats:
    // он авто-включался от него, и тогда замер мерил сам прибор — 18-09 это
    // спрятало результат правки в process_call (таблица times строилась всегда).
    CMD4_NOSAVE(CCC_Integer, "sigprof", &g_sigprof, 0, 1);
    // Цена кадра в ядро-миллисекундах по потокам (xrCore/cpu_budget.h).
    CMD4_NOSAVE(CCC_Integer, "cpu_budget", &CpuBudget::enabled, 0, 2);   // 2 = + раскладка главного потока по фазам
        CMD1(CCC_LuaVmWatchDump, "lua_vm_watch_dump");
    CMD4_NOSAVE(CCC_Integer, "lua_gcstep", &psLUA_GCSTEP, 10, 1000);
    CMD4_NOSAVE(CCC_Integer, "lua_gc_target_kb", &psLUA_GC_TARGET_KB, 8192, 262144);
    CMD4_NOSAVE(CCC_Integer, "lua_gctimeout", &psLUA_GCTIMEOUT_MIN, 1000, 16000);

    extern BOOL bLevelEnvModExport;
    CMD4(CCC_Integer, "level_env_mod_export", &bLevelEnvModExport, FALSE, TRUE);

    CMD3(CCC_Mask, "ssfx_hud_raindrops", &psDeviceFlags, rs_SSFX_HUD_RAINDROPS);
};
