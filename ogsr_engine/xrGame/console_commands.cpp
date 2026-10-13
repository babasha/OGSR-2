#include "StdAfx.h"
#include "goap_stats.h"
#include "npc_perf.h"
#include "../xr_3da/XR_IOConsole.h"
#include "../xr_3da/xr_ioc_cmd.h"
#include "../xr_3da/CustomHUD.h"
#include "../xr_3da/FDemoRecord.h"
#include "../xr_3da/FDemoPlay.h"
#include "xrMessages.h"
#include "xrServer.h"
#include "Level.h"
#include "../xrOnlineNet/content_integrity.h"
#include "../xrOnlineNet/NET_GNS_Transport.h"
#include "sv_telemetry.h"
#include "sv_health.h"
#include "sv_npc.h"
#include "sv_motion_audit.h"
#include "cl_bone_audit.h"
#include "cl_anim_blendspace.h"
#include "cl_anim_ledger.h"
#include "sv_accuracy.h"
#include "sv_hit_trail.h"
#include "sv_scenario.h"
#include "sv_anomaly.h"
#include "sv_prop.h"
#include "sv_perf.h"
#include "sv_world_lookup.h"
#include "sv_damage_model.h"
#include "shot_latency.h"
#include "cl_fire_test.h"
#include "cl_bolt_test.h"
#include "cl_zone_debug.h"
#include "ai_debug_draw.h"
#include "ai_test.h"
#include "actor_aim_assist.h"
#include "perf_route.h"
#include "ai_position_query.h"
#include "ai_threat_map.h"
#include "cl_zone_edit.h"
#include "cl_prop_edit.h"
#include "ui_rml.h"
#include "ui/UITextureMaster.h" // ui_sprites -- the legacy atlas table
#include "sv_npc_health.h"
#include "sv_bone_names.h"
#include "sv_visual_armour.h"
#include "sv_revive.h"
#include "sv_npc_fight.h"
#include "sv_npc_gear.h"
#include "sv_loot.h"
#include "ai_debug.h"
#include "alife_simulator.h"
#include "alife_ext_hooks.h"
#include "game_cl_base.h"
#include "game_cl_single.h"
#include "game_sv_single.h"
#include "Hit.h"
#include "PHDestroyable.h"
#include "Actor.h"
#include "Actor_Flags.h"
#include "Inventory.h"
#include "Torch.h"
#include "CustomZone.h"
#include "script_engine.h"
#include "xrServer_Objects.h"
#include "ui/UIMainIngameWnd.h"
#include "PhysicsGamePars.h"
#include "PHWorld.h"
#include "string_table.h"
#include "ai_space.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "date_time.h"
#include "mt_config.h"
#include "alife_slow_probe.h"
#include "zone_effector.h"
#include "GameTask.h"
#include "MainMenu.h"
#include "saved_game_wrapper.h"
#include "level_graph.h"
#include "../xr_3da/GameMtlLib.h"
#include "game_level_cross_table.h"
#include "cameralook.h"
#include "ai_object_location.h"

#ifdef DEBUG
#include "PHDebug.h"
#include "ui/UIDebugFonts.h"
#include "game_graph.h"
#endif // DEBUG

#include "HUDManager.h"
#include "../xr_3da/x_ray.h"

string_path g_last_saved_game;

extern u64 g_qwStartGameTime;
extern u64 g_qwEStartGameTime;

ENGINE_API
extern float psHUD_FOV_def;
extern float psSqueezeVelocity;

extern int x_m_x;
extern int x_m_z;
#ifdef DEBUG
extern BOOL g_ShowAnimationInfo;
#endif // DEBUG
extern BOOL g_bShowHitSectors;
extern BOOL g_bDebugDumpPhysicsStep;
extern ESingleGameDifficulty g_SingleGameDifficulty;
extern BOOL g_show_wnd_rect;
extern BOOL g_show_wnd_rect2;
extern BOOL g_show_wnd_rect_text;
extern BOOL g_console_show_always;
    //-----------------------------------------------------------
extern float g_fTimeFactor;
extern BOOL g_bCopDeathAnim;
extern int g_bHudAdjustMode;
extern int psLUA_GC_STEP_MAX_US, psLUA_GC_STEPMUL, psLUA_GC_PAUSE, psLUA_GC_UDMOVE, psLUA_GC_UDCENSUS, psLUA_GC_UDSETTLE;
extern int g_bHudAdjustItemIdx;
extern float g_bHudAdjustDeltaPos;
extern float g_bHudAdjustDeltaRot;

float adj_delta_pos = 0.0005f;
float adj_delta_rot = 0.05f;
//-----------------------------------------------------------

extern BOOL g_enable_memory_debug;

Flags32 g_mt_config = {mtLevelPath | mtDetailPath | mtObjectHandler | mtSoundPlayer | mtAiVision | mtBullets | mtLUA_GC | mtLevelSounds | mtALife};

Flags32 dbg_net_Draw_Flags = {0};

#ifdef DEBUG
BOOL g_bDebugNode = FALSE;
u32 g_dwDebugNodeSource = 0;
u32 g_dwDebugNodeDest = 0;
extern BOOL g_bDrawBulletHit;
#endif
#ifdef DEBUG
extern LPSTR dbg_stalker_death_anim;
extern BOOL b_death_anim_velocity;
#endif
int g_AI_inactive_time = 0;


extern int g_dof_zoom_far;
extern int g_dof_zoom_near;

void get_files_list(xr_vector<shared_str>& files, LPCSTR dir, LPCSTR file_ext)
{
    VERIFY(dir && file_ext);
    files.clear();

    FS_Path* P = FS.get_path(dir);
    P->m_Flags.set(FS_Path::flNeedRescan, TRUE);
    FS.m_Flags.set(CLocatorAPI::flNeedCheck, TRUE);
    FS.rescan_physical_pathes();

    string_path fext;
    xr_strconcat(fext, "*", file_ext);

    FS_FileSet files_set;
    FS.file_list(files_set, dir, FS_ListFiles, fext);
    u32 len_str_ext = xr_strlen(file_ext);

    FS_FileSetIt itb = files_set.begin();
    FS_FileSetIt ite = files_set.end();

    for (; itb != ite; ++itb)
    {
        LPCSTR fn_ext = (*itb).name.c_str();
        VERIFY(xr_strlen(fn_ext) > len_str_ext);
        string_path fn;
        strncpy_s(fn, sizeof(fn), fn_ext, xr_strlen(fn_ext) - len_str_ext);
        files.push_back(fn);
    }
    FS.m_Flags.set(CLocatorAPI::flNeedCheck, FALSE);
}


class CCC_MemStats : public IConsole_Command
{
public:
    CCC_MemStats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        //Memory.mem_compact();

        u32 m_base = 0, c_base = 0, m_lmaps = 0, c_lmaps = 0;

        //	Resource check moved to m_pRender
        if (Device.m_pRender)
            Device.m_pRender->ResourcesGetMemoryUsage(m_base, c_base, m_lmaps, c_lmaps);

        Log("--------------------------------------------------------------------------------");

        SProcessMemInfo memCounters;
        GetProcessMemInfo(memCounters);
        Msg("[%I64dMB] physical memory installed, [%I64dMB] available, [%ld] percent of memory in use", 
            memCounters.TotalPhysicalMemory / (1024 * 1024),
            memCounters.FreePhysicalMemory / (1024 * 1024), 
            memCounters.MemoryLoad);

        Msg("PageFile total: [%I64dMB], free~ [%I64dMB]", 
            memCounters.TotalPageFile / (1024 * 1024), memCounters.FreePageFile / (1024 * 1024));

        //PeakWorkingSetSize
        //
        //The peak working set size, in bytes.
        //
        //WorkingSetSize
        //
        //The current working set size, in bytes.

        Msg("Engine memory usage (Working Set): [%I64dMB], peak: [%I64dMB]",
            memCounters.WorkingSetSize / (1024 * 1024), memCounters.PeakWorkingSetSize / (1024 * 1024));

        //PagefileUsage
        //
        //The Commit Charge value in bytes for this process. Commit Charge is the total amount of memory that the memory manager has committed for a running process.
        //
        //PeakPagefileUsage
        //
        //The peak value in bytes of the Commit Charge during the lifetime of this process.

        Msg("Engine memory usage (Commit Charge): [%I64dMB], peak: [%I64dMB]",
            memCounters.PagefileUsage / (1024 * 1024), memCounters.PeakPagefileUsage / (1024 * 1024));

        Log("--------------------------------------------------------------------------------");

        const size_t _process_heap = mem_usage_impl(nullptr, nullptr);
        const u32 _render = ::Render->memory_usage();
        const u32 _eco_strings = g_pStringContainer->stat_economy();
        const u32 _eco_smem = g_pSharedMemoryContainer->stat_economy();

        Msg("* [ D3D ]: textures count [%d]", (c_base + c_lmaps));
        Msg("* [ D3D ]: textures[%d K]", (m_base + m_lmaps) / 1024);
        Msg("* [x-ray]: process heap[%d K], render[%d K]", _process_heap / 1024, _render / 1024);
        Msg("* [x-ray]: economy: strings[%d K], smem[%d K]", _eco_strings / 1024, _eco_smem);

#ifdef DEBUG
        Msg("* [x-ray]: file mapping: memory[%d K], count[%d]", g_file_mapped_memory / 1024, g_file_mapped_count);
        dump_file_mappings();
#endif // DEBUG
    }
};

// console commands
class CCC_GameDifficulty : public CCC_Token
{
public:
    CCC_GameDifficulty(LPCSTR N) : CCC_Token(N, (u32*)&g_SingleGameDifficulty, difficulty_type_token){};
    virtual void Execute(LPCSTR args)
    {
        if (!Core.Features.test(xrCore::Feature::gd_master_only))
            CCC_Token::Execute(args);

        if (g_pGameLevel && Level().game)
        {
            game_cl_Single* game = smart_cast<game_cl_Single*>(Level().game);
            VERIFY(game);
            game->OnDifficultyChanged();
        }
    }
    virtual void Info(TInfo& I) { strcpy_s(I, "game difficulty"); }
};

static xr_vector<xr_token>* pLanguagesToken{};
static u32 LanguageID{};
const xr_token* GetLanguagesToken() { return &pLanguagesToken->at(LanguageID); }

class CCC_GameLanguage : public CCC_Token
{
    xr_vector<xr_token> LanguagesToken;

public:
    CCC_GameLanguage(LPCSTR N) : CCC_Token(N, &LanguageID, LanguagesToken.data())
    {
        pLanguagesToken = &LanguagesToken;

        const auto curr_lang_id = GetUserDefaultUILanguage();
        char curr_lang_name[4]{};
        const int curr_lang_len = GetLocaleInfoA(curr_lang_id, LOCALE_SISO639LANGNAME2, curr_lang_name, std::size(curr_lang_name));

        const char* str = pSettings->r_string("string_table", "language");
        for (int i{}, count = _GetItemCount(str); i < count;)
        {
            auto& tok = LanguagesToken.emplace_back();
            tok.id = i;
            string64 temp{};
            _GetItem(str, i++, temp);
            if (curr_lang_len > 0 && !_stricmp(temp, &curr_lang_name[0]))
                LanguageID = tok.id;
            tok.name = xr_strdup(temp);
        }
        LanguagesToken.emplace_back();
        tokens = LanguagesToken.data();
    };

    void Execute(LPCSTR args) override
    {
        CCC_Token::Execute(args);
        CStringTable().ReloadLanguage();

        if (IsMainMenuActive())
            MainMenu()->SetLanguageChanged(true);

        if (!g_pGameLevel)
            return;

        if (g_pGamePersistent && MainMenu() && MainMenu()->IsActive())
        {
            MainMenu()->Activate(FALSE);
            MainMenu()->Activate(TRUE);
        }

        for (u16 id = 0; id < 0xffff; id++)
        {
            auto gameObj = Level().Objects.net_Find(id);
            if (gameObj)
            {
                if (auto invItem = smart_cast<CInventoryItem*>(gameObj))
                    invItem->ReloadNames();
            }
        }
    }
};

#ifdef DEBUG
class CCC_ALifePath : public IConsole_Command
{
public:
    CCC_ALifePath(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if (!ai().get_level_graph())
            Msg("! there is no graph!");
        else
        {
            int id1 = -1, id2 = -1;
            sscanf(args, "%d %d", &id1, &id2);
            if ((-1 != id1) && (-1 != id2))
                if (_max(id1, id2) > (int)ai().game_graph().header().vertex_count() - 1)
                    Msg("! there are only %d vertexes!", ai().game_graph().header().vertex_count());
                else if (_min(id1, id2) < 0)
                    Msg("! invalid vertex number (%d)!", _min(id1, id2));
                else
                {
                    //						Sleep				(1);
                    //						CTimer				timer;
                    //						timer.Start			();
                    //						float				fValue = ai().m_tpAStar->ffFindMinimalPath(id1,id2);
                    //						Msg					("* %7.2f[%d] : %11I64u cycles (%.3f
                    //microseconds)",fValue,ai().m_tpAStar->m_tpaNodes.size(),timer.GetElapsed_ticks(),timer.GetElapsed_ms()*1000.f);
                }
            else
                Msg("! not enough parameters!");
        }
    }
};
#endif // DEBUG

class CCC_ALifeTimeFactor : public IConsole_Command
{
public:
    CCC_ALifeTimeFactor(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        float id1 = 0.0f;
        sscanf(args, "%f", &id1);
        if (id1 < EPS_L)
            Msg("Invalid time factor! (%.4f)", id1);
        else if (!g_pGameLevel || !Level().Server || !Level().Server->game)
        {
            // Was an unconditional dereference: on a machine with no world of its
            // own this crashed rather than refusing, and the rate it wants to set
            // is the host's to decide anyway.
            Msg("! al_time_factor: the world clock is owned by the server");
        }
        else
        {
            Level().Server->game->SetGameTimeFactor(id1);
        }
    }
};

class CCC_ALifeSwitchDistance : public IConsole_Command
{
public:
    CCC_ALifeSwitchDistance(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if (ai().get_alife())
        {
            float id1 = 0.0f;
            sscanf(args, "%f", &id1);
            if (id1 < 2.0f)
                Msg("Invalid online distance! (%.4f)", id1);
            else
            {
                NET_Packet P;
                P.w_begin(M_SWITCH_DISTANCE);
                P.w_float(id1);
                Level().Send(P, net_flags(TRUE, TRUE));
            }
        }
        else
            Log("!Not a single player game!");
    }
};

class CCC_ALifeProcessTime : public IConsole_Command
{
public:
    CCC_ALifeProcessTime(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if (ai().get_alife())
        {
            game_sv_Single* tpGame = smart_cast<game_sv_Single*>(Level().Server->game);
            VERIFY(tpGame);
            int id1 = 0;
            sscanf(args, "%d", &id1);
            if (id1 < 1)
                Msg("Invalid process time! (%d)", id1);
            else
                tpGame->alife().set_process_time(id1);
        }
        else
            Log("!Not a single player game!");
    }
};

class CCC_ALifeObjectsPerUpdate : public IConsole_Command
{
public:
    CCC_ALifeObjectsPerUpdate(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if (ai().get_alife())
        {
            game_sv_Single* tpGame = smart_cast<game_sv_Single*>(Level().Server->game);
            VERIFY(tpGame);
            int id1 = 0;
            sscanf(args, "%d", &id1);
            tpGame->alife().objects_per_update(id1);
        }
        else
            Log("!Not a single player game!");
    }
};

class CCC_ALifeSwitchFactor : public IConsole_Command
{
public:
    CCC_ALifeSwitchFactor(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if (ai().get_alife())
        {
            game_sv_Single* tpGame = smart_cast<game_sv_Single*>(Level().Server->game);
            VERIFY(tpGame);
            float id1 = 0;
            sscanf(args, "%f", &id1);
            clamp(id1, .1f, 1.f);
            tpGame->alife().set_switch_factor(id1);
        }
        else
            Log("!Not a single player game!");
    }
};

//#ifndef MASTER_GOLD
class CCC_TimeFactor : public IConsole_Command
{
public:
    CCC_TimeFactor(LPCSTR N) : IConsole_Command(N) {}
    virtual void Execute(LPCSTR args)
    {
        // This scales the ENGINE clock, not the world clock: it speeds up or slows
        // down everything this process simulates. A machine that reports its own
        // player's position to a host must not have that dial -- it is a speed
        // hack, and a far cheaper one than any of the lighting tricks.
        if (is_network_client())
        {
            Msg("! time_factor: not available while connected to a server");
            return;
        }

        float time_factor = (float)atof(args);
        clamp(time_factor, .001f, 1000.f);
        Device.time_factor(time_factor);
    }
};
//#endif // MASTER_GOLD

//-----------------------------------------------------------------------
class CCC_DemoRecord : public IConsole_Command
{
public:
    CCC_DemoRecord(LPCSTR N) : IConsole_Command(N) {}
    virtual void Execute(LPCSTR args)
    {
        if (!g_pGameLevel)
        { // level not loaded
            Log("Demo Record is disabled when level is not loaded.");
            return;
        }

        Console->Hide();

        if (MainMenu()->IsActive())
            MainMenu()->Activate(false);

        string_path fn_;
        strconcat(sizeof(fn_), fn_, args, ".xrdemo");
        string_path fn;
        FS.update_path(fn, "$game_saves$", fn_);

        g_pGameLevel->Cameras().AddCamEffector(xr_new<CDemoRecord>(fn));
    }
};

class CCC_DemoPlay : public IConsole_Command
{
public:
    CCC_DemoPlay(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        if (0 == g_pGameLevel)
        {
            Msg("! There are no level(s) started");
        }
        else
        {
            Console->Hide();
            string_path fn;
            u32 loops = 0;
            LPSTR comma = strchr(const_cast<LPSTR>(args), ',');
            if (comma)
            {
                loops = atoi(comma + 1);
                *comma = 0; //. :)
            }
            strconcat(sizeof(fn), fn, args, ".xrdemo");
            FS.update_path(fn, "$game_saves$", fn);
            g_pGameLevel->Cameras().AddCamEffector(xr_new<CDemoPlay>(fn, 1.0f, loops));
        }
    }
};

bool valid_file_name(LPCSTR file_name)
{
    LPCSTR I = file_name;
    LPCSTR E = file_name + xr_strlen(file_name);
    for (; I != E; ++I)
    {
        if (!strchr("/\\:*?\"<>|", *I))
            continue;

        return (false);
    };

    return (true);
}

#include "UIGameCustom.h"
#include "HUDManager.h"

class CCC_ALifeSave : public IConsole_Command
{
public:
    CCC_ALifeSave(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        if (!g_actor || !Actor()->g_Alive())
        {
            Msg("cannot make saved game because actor is dead :(");
            return;
        }

        string_path S, S1;
        S[0] = 0;
        //.		sscanf					(args ,"%s",S);
        strcpy_s(S, args);

#ifdef DEBUG
        CTimer timer;
        timer.Start();
#endif
        if (!xr_strlen(S))
        {
            strconcat(sizeof(S), S, Core.UserName, "_", "quicksave");
            NET_Packet net_packet;
            net_packet.w_begin(M_SAVE_GAME);
            net_packet.w_stringZ(S);
            net_packet.w_u8(0);
            Level().Send(net_packet, net_flags(TRUE));
        }
        else
        {
            if (!valid_file_name(S))
            {
                Msg("invalid file name");
                return;
            }

            NET_Packet net_packet;
            net_packet.w_begin(M_SAVE_GAME);
            net_packet.w_stringZ(S);
            net_packet.w_u8(1);
            Level().Send(net_packet, net_flags(TRUE));
        }
#ifdef DEBUG
        Msg("Game save overhead  : %f milliseconds", timer.GetElapsed_sec() * 1000.f);
#endif
        SDrawStaticStruct* _s = HUD().GetUI()->UIGame()->AddCustomStatic("game_saved", true);
        _s->m_endTime = Device.fTimeGlobal + 3.0f; // 3sec
        string_path save_name;
        strconcat(sizeof(save_name), save_name, *CStringTable().translate("st_game_saved"), ": ", S);
        _s->wnd()->SetText(save_name);

        strcat_s(S, ".dds");
        FS.update_path(S1, "$game_saves$", S);

#ifdef DEBUG
        timer.Start();
#endif
        MainMenu()->Screenshot(IRender_interface::SM_FOR_GAMESAVE, S1);

#ifdef DEBUG
        Msg("Screenshot overhead : %f milliseconds", timer.GetElapsed_sec() * 1000.f);
#endif
    }
};

class CCC_ALifeLoadFrom : public IConsole_Command
{
public:
    CCC_ALifeLoadFrom(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }

    void Execute(LPCSTR args) override
    {
        if (!CSavedGameWrapper::valid_saved_game(args))
        {
            Msg("! Cannot load saved game [%s]: not found or version mismatch or corrupted", args ? args : "nullptr");
            return;
        }

        if (ai().get_alife())
        {
            if (MainMenu()->IsActive())
                MainMenu()->Activate(false);

            Console->Hide();

            ::Sound->set_master_volume(0.f);

            if (Device.Paused())
                Device.Pause(FALSE, TRUE, TRUE, "CCC_ALifeLoadFrom");

            NET_Packet net_packet;
            net_packet.w_begin(M_LOAD_GAME);
            net_packet.w_stringZ(args);
            Level().Send(net_packet, net_flags(TRUE));
        }
        else
        {
            string_path command;
            xr_strconcat(command, "start server(", args, "/single/alife/load)");
            Console->Execute(command);
        }
    }

    virtual void fill_tips(vecTips& tips, u32 mode) { get_files_list(tips, "$game_saves$", SAVE_EXTENSION); }
};

class CCC_LoadLastSave : public IConsole_Command
{
public:
    CCC_LoadLastSave(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }

    virtual void Execute(LPCSTR args)
    {
        if (args && *args)
        {
            strcpy_s(g_last_saved_game, args);
            return;
        }

        if (!*g_last_saved_game)
        {
            Msg("! cannot load last saved game since it hasn't been specified");
            return;
        }

        string512 command;
        if (ai().get_alife())
        {
            strconcat(sizeof(command), command, "load ", g_last_saved_game);
            Console->Execute(command);
            return;
        }

        strconcat(sizeof(command), command, "start server(", g_last_saved_game, "/single/alife/load)");
        Console->Execute(command);
    }

    virtual void Save(IWriter* F)
    {
        if (!*g_last_saved_game)
            return;

        F->w_printf("%s %s\r\n", cName, g_last_saved_game);
    }
};

class CCC_FloatBlock : public CCC_Float
{
public:
    CCC_FloatBlock(LPCSTR N, float* V, float _min = 0, float _max = 1) : CCC_Float(N, V, _min, _max){};

    virtual void Execute(LPCSTR args) { CCC_Float::Execute(args); }
};

// ⭐Copies one file OUT of the virtual filesystem to disk.
//
// The game reads most of its content through .db archives, and there was no way
// to look at any of it: a mod's ui_mm_main.xml or one of its scripts is right
// there behind FS.r_open and unreachable to anything but the engine. That gap
// turns any "port this screen faithfully" job into guesswork off a screenshot,
// which is exactly how this command came to exist.
//
//   fs_extract $game_config$ ui\ui_mm_main.xml D:\tmp\ui_mm_main.xml
//
// ⚠Outside the DEBUG block below on purpose: the archives are only worth looking
// into on a RELEASE install, because that is where a mod's content lives.
// ⚠Three whitespace-separated arguments, and the output path may not contain
// spaces -- a half-parsed path is worse than a refused one, so anything that
// does not split into exactly three pieces is rejected with the syntax line.
class CCC_FsExtract : public IConsole_Command
{
public:
    CCC_FsExtract(LPCSTR N) : IConsole_Command(N) { bLowerCaseArgs = false; }

    virtual void Execute(LPCSTR args)
    {
        string_path alias, name, out;
        if (!args || 3 != sscanf(args, "%s %s %s", alias, name, out))
        {
            InvalidSyntax();
            return;
        }

        IReader* r = FS.r_open(alias, name);
        if (!r)
        {
            Msg("! fs_extract: '%s' not found under '%s'", name, alias);
            return;
        }

        IWriter* w = FS.w_open(out);
        if (!w)
        {
            FS.r_close(r);
            Msg("! fs_extract: cannot write '%s'", out);
            return;
        }

        const u32 size = r->length();
        w->w(r->pointer(), size);
        FS.w_close(w);
        FS.r_close(r);
        Msg("~ fs_extract: %s\\%s -> %s (%u bytes)", alias, name, out, size);
    }

    virtual void Info(TInfo& I) { strcpy_s(I, "<fs alias> <name inside it> <output file> -- copies a file out of the archives"); }
};

// ⭐The other half of fs_extract: names of ATLAS ENTRIES, which live in no file
// you can open.
//
// The legacy screens are drawn from sprites -- `ui_button_ordinary`, and the
// three state variants CUI3tButton derives from it (`_e`, `_h`, `_t`, `_d`).
// Which of those actually exist is a property of ui\textures_descr\*.xml, and
// that folder is enumerated at runtime (MainMenu.cpp) rather than listed
// anywhere. Porting a screen therefore kept coming down to guessing a name and
// finding out from a blank rectangle, which is the worst kind of feedback: it
// looks like a layout bug.
//
//   ui_sprites button_ordinary
//
// ⚠Substring, case-insensitive, and it prints the rectangle too -- a sprite that
// exists with a zero-size rect is a different failure from one that is missing,
// and on screen the two look identical.
class CCC_UiSprites : public IConsole_Command
{
public:
    CCC_UiSprites(LPCSTR N) : IConsole_Command(N) { bLowerCaseArgs = false; }

    virtual void Execute(LPCSTR args)
    {
        string_path needle;
        if (!args || 1 != sscanf(args, "%s", needle))
        {
            InvalidSyntax();
            return;
        }
        xr_strlwr(needle);

        u32 hits = 0;
        for (const auto& kv : CUITextureMaster::Textures())
        {
            string_path lower;
            xr_strcpy(lower, sizeof(lower), kv.first.c_str());
            xr_strlwr(lower);
            if (!strstr(lower, needle))
                continue;
            const Frect& r = kv.second.rect;
            Msg("~ ui_sprites: %s = %s [%.0f %.0f %.0fx%.0f]", kv.first.c_str(), kv.second.file.c_str(), r.x1, r.y1, r.width(), r.height());
            ++hits;
        }
        Msg("~ ui_sprites: %u of %u match '%s'", hits, u32(CUITextureMaster::Textures().size()), needle);
    }

    virtual void Info(TInfo& I) { strcpy_s(I, "<substring> -- lists legacy UI sprites whose name contains it"); }
};

// ⭐Presses a button in the new menu: `ui_menu_act options`, `ui_menu_act quit`,
// `ui_menu_act opt-tab:sound`. The names are the data-act values in
// main_menu.rml. Pairs with vk_shot: with these two a scripted run can walk
// every screen and photograph it, which is the only way any of this gets looked
// at without a person at the keyboard.
class CCC_UiMenuAct : public IConsole_Command
{
public:
    CCC_UiMenuAct(LPCSTR N) : IConsole_Command(N) { bLowerCaseArgs = false; }

    virtual void Execute(LPCSTR args)
    {
        if (!args || !args[0])
        {
            InvalidSyntax();
            return;
        }
        UIRml::MenuAction(args);
    }

    virtual void Info(TInfo& I) { strcpy_s(I, "<action> -- presses a button in the new main menu (data-act in main_menu.rml)"); }
};

// The position query's counters since the last reset: ai_pq_stats [reset].
class CCC_AiPqStats : public IConsole_Command
{
public:
    CCC_AiPqStats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }

    virtual void Execute(LPCSTR args)
    {
        PositionQuery::Dump();
        ThreatMap::Dump();
        if (args && strstr(args, "reset"))
        {
            PositionQuery::ResetStats();
            ThreatMap::ResetStats();
        }
    }
};

// The combat AI autotest: see ai_test.h.
class CCC_AiTest : public IConsole_Command
{
public:
    CCC_AiTest(LPCSTR N) : IConsole_Command(N) { bLowerCaseArgs = false; }

    virtual void Execute(LPCSTR args) { AITest::Command(args); }

    virtual void Info(TInfo& I) { strcpy_s(I, "siege_izba [runs=1] [count=10] [time=90] [section=..] [bearing=108] [dist=35] [keep=0] [at=x,y,z] | stop | status"); }
};

// The pad's aim assist measured without hands: see actor_aim_assist.h.
class CCC_AaTest : public IConsole_Command
{
public:
    CCC_AaTest(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }

    virtual void Execute(LPCSTR args) { AimAssist::test_command(args); }

    virtual void Info(TInfo& I) { strcpy_s(I, "snap [off=6] | track [time=4] | stop"); }
};

// The frame-rate route: see perf_route.h.
class CCC_PerfRoute : public IConsole_Command
{
public:
    CCC_PerfRoute(LPCSTR N) : IConsole_Command(N) { bLowerCaseArgs = false; }

    virtual void Execute(LPCSTR args) { PerfRoute::Command(args); }

    virtual void Info(TInfo& I) { strcpy_s(I, "list | [dwell=16] [settle=4] <smart terrain|x,y,z> ... | stop"); }
};

// The actor's torch on/off without the hotkey (an A/B arm of perf_route: "a=actor_torch:0 b=actor_torch:1").
class CCC_ActorTorch : public IConsole_Command
{
public:
    CCC_ActorTorch(LPCSTR N) : IConsole_Command(N) {}

    virtual void Execute(LPCSTR args)
    {
        CActor* A = g_pGameLevel ? Actor() : nullptr;
        auto torch = A ? smart_cast<CTorch*>(A->inventory().ItemFromSlot(TORCH_SLOT)) : nullptr;
        if (!torch)
        {
            Msg("! [actor_torch] the actor has no torch in the torch slot");
            return;
        }
        const bool on = (args && args[0]) ? atoi(args) != 0 : !torch->torch_active();
        torch->Switch(on);
        Msg("[actor_torch] %s", on ? "on" : "off");
    }

    virtual void Info(TInfo& I) { strcpy_s(I, "[0|1] (no argument = toggle)"); }
};

// The actor's active inventory slot without the hotkeys (perf_route arm: the hands/weapon on screen or not).
// 255 = hide (NO_ACTIVE_SLOT).
class CCC_ActorSlot : public IConsole_Command
{
public:
    CCC_ActorSlot(LPCSTR N) : IConsole_Command(N) {}

    virtual void Execute(LPCSTR args)
    {
        CActor* A = g_pGameLevel ? Actor() : nullptr;
        if (!A || !args || !args[0])
            return;
        const u32 slot = u32(atoi(args));
        const bool ok = A->inventory().Activate(slot, eGeneral, true);
        Msg("[actor_slot] %u -> %s (active now %u)", slot, ok ? "ok" : "refused", A->inventory().GetActiveSlot());
    }

    virtual void Info(TInfo& I) { strcpy_s(I, "<slot> (255 = hide)"); }
};

// Drives the game screens (inventory, search, trade, talk) for unattended runs:
// see UIRml::TestCmd in ui_rml_inv.cpp.
class CCC_UiTest : public IConsole_Command
{
public:
    CCC_UiTest(LPCSTR N) : IConsole_Command(N) { bLowerCaseArgs = false; }

    virtual void Execute(LPCSTR args)
    {
        if (!g_pGameLevel || !args || !args[0])
        {
            InvalidSyntax();
            return;
        }
        UIRml::TestCmd(args);
    }

    virtual void Info(TInfo& I) { strcpy_s(I, "<verb> ... -- give/open/click/drag/expect/dump on the RmlUi game screens"); }
};

// ⭐The third piece of the unattended-run kit, after ui_menu_act and vk_shot:
//
//   ui_input_mute 60
//
// deafens the RmlUi menu to keyboard and mouse for a minute, so a sweep walks
// the screens it was told to walk. Two runs in a row were derailed by a single
// click that landed while the window was taking focus -- and a stray click
// writes nothing to the log, only its consequence, so the run reads as a menu
// bug rather than as someone's hand.
//
// ⚠It expires by itself (see UIRml::InputMute); 0 clears it early. The console
// key is taken before the menu ever sees a key, so this cannot lock anybody out.
class CCC_UiInputMute : public IConsole_Command
{
public:
    CCC_UiInputMute(LPCSTR N) : IConsole_Command(N) {}

    virtual void Execute(LPCSTR args)
    {
        float seconds = 0.f;
        if (!args || 1 != sscanf(args, "%f", &seconds))
        {
            InvalidSyntax();
            return;
        }
        UIRml::InputMute(seconds);
    }

    virtual void Info(TInfo& I) { strcpy_s(I, "<seconds> -- ignores keyboard and mouse in the UI for that long (0 = off, expires by itself)"); }
};


#ifdef DEBUG

class CCC_DrawGameGraphAll : public IConsole_Command
{
public:
    CCC_DrawGameGraphAll(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }

    virtual void Execute(LPCSTR args)
    {
        if (!ai().get_level_graph())
            return;

        ai().level_graph().setup_current_level(-1);
    }
};

class CCC_DrawGameGraphCurrent : public IConsole_Command
{
public:
    CCC_DrawGameGraphCurrent(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }

    virtual void Execute(LPCSTR args)
    {
        if (!ai().get_level_graph())
            return;

        ai().level_graph().setup_current_level(ai().level_graph().level_id());
    }
};

class CCC_DrawGameGraphLevel : public IConsole_Command
{
public:
    CCC_DrawGameGraphLevel(LPCSTR N) : IConsole_Command(N) {}

    virtual void Execute(LPCSTR args)
    {
        if (!ai().get_level_graph())
            return;

        string256 S;
        S[0] = 0;
        sscanf(args, "%s", S);

        if (!*S)
        {
            ai().level_graph().setup_current_level(-1);
            return;
        }

        const GameGraph::SLevel* level = ai().game_graph().header().level(S, true);
        if (!level)
        {
            Msg("! There is no level %s in the game graph", S);
            return;
        }

        ai().level_graph().setup_current_level(level->id());
    }
};

class CCC_DumpInfos : public IConsole_Command
{
public:
    CCC_DumpInfos(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        CActor* A = smart_cast<CActor*>(Level().CurrentEntity());
        if (A)
            A->DumpInfo();
    }
    virtual void Info(TInfo& I) { strcpy_s(I, "dumps all infoportions that actor have"); }
};
#include "map_manager.h"
class CCC_DumpMap : public IConsole_Command
{
public:
    CCC_DumpMap(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args) { Level().MapManager().Dump(); }
    virtual void Info(TInfo& I) { strcpy_s(I, "dumps all currentmap locations"); }
};

#include "alife_graph_registry.h"
class CCC_DumpCreatures : public IConsole_Command
{
public:
    CCC_DumpCreatures(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        typedef CSafeMapIterator<ALife::_OBJECT_ID, CSE_ALifeDynamicObject>::_REGISTRY::const_iterator const_iterator;

        const_iterator I = ai().alife().graph().level().objects().begin();
        const_iterator E = ai().alife().graph().level().objects().end();
        for (; I != E; ++I)
        {
            CSE_ALifeCreatureAbstract* obj = smart_cast<CSE_ALifeCreatureAbstract*>(I->second);
            if (obj)
            {
                Msg("\"%s\",", obj->name_replace());
            }
        }
    }
    virtual void Info(TInfo& I) { strcpy_s(I, "dumps all creature names"); }
};

class CCC_DebugFonts : public IConsole_Command
{
public:
    CCC_DebugFonts(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
    virtual void Execute(LPCSTR args) { HUD().GetUI()->StartStopMenu(xr_new<CUIDebugFonts>(), true); }
};

class CCC_DebugNode : public IConsole_Command
{
public:
    CCC_DebugNode(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        string128 param1, param2;
        _GetItem(args, 0, param1, ' ');
        _GetItem(args, 1, param2, ' ');

        u32 value1;
        u32 value2;

        sscanf(param1, "%u", &value1);
        sscanf(param2, "%u", &value2);

        if ((value1 > 0) && (value2 > 0))
        {
            g_bDebugNode = TRUE;
            g_dwDebugNodeSource = value1;
            g_dwDebugNodeDest = value2;
        }
        else
        {
            g_bDebugNode = FALSE;
        }
    }
};

class CCC_ShowMonsterInfo : public IConsole_Command
{
public:
    CCC_ShowMonsterInfo(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        string128 param1, param2;
        _GetItem(args, 0, param1, ' ');
        _GetItem(args, 1, param2, ' ');

        CObject* obj = Level().Objects.FindObjectByName(param1);
        CBaseMonster* monster = smart_cast<CBaseMonster*>(obj);
        if (!monster)
            return;

        u32 value2;

        sscanf(param2, "%u", &value2);
        monster->set_show_debug_info(u8(value2));
    }
};
class CCC_DbgPhTrackObj : public IConsole_Command
{
public:
    CCC_DbgPhTrackObj(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args /**/)
    {
        ph_dbg_draw_mask1.set(ph_m1_DbgTrackObject, TRUE);
        PH_DBG_SetTrackObject(args);
        // CObject* O= Level().Objects.FindObjectByName(args);
        // if(O)
        //{
        //	PH_DBG_SetTrackObject(*(O->cName()));
        //	ph_dbg_draw_mask1.set(ph_m1_DbgTrackObject,TRUE);
        // }
    }

    // virtual void	Info	(TInfo& I)
    //{
    //	strcpy_s(I,"restart game fast");
    // }
};
#endif

class CCC_PHIterations : public CCC_Integer
{
public:
    CCC_PHIterations(LPCSTR N) : CCC_Integer(N, &phIterations, 15, 50){};
    virtual void Execute(LPCSTR args)
    {
        CCC_Integer::Execute(args);
        dWorldSetQuickStepNumIterations(NULL, phIterations);
    }
};

class CCC_PHGravity : public IConsole_Command
{
public:
    CCC_PHGravity(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if (!ph_world)
            return;
        ph_world->SetGravity(float(atof(args)));
    }
    virtual void Status(TStatus& S)
    {
        if (ph_world)
            sprintf_s(S, "%3.5f", ph_world->Gravity());
        else
            sprintf_s(S, "%3.5f", default_world_gravity);
        while (xr_strlen(S) && ('0' == S[xr_strlen(S) - 1]))
            S[xr_strlen(S) - 1] = 0;
    }
};

class CCC_PHFps : public IConsole_Command
{
public:
    CCC_PHFps(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        float step_count = (float)atof(args);
        clamp(step_count, 50.f, 200.f);
        CPHWorld::SetStep(1.f / step_count);
    }
    virtual void Status(TStatus& S) { sprintf_s(S, "%3.5f", 1.f / fixed_step); }
};

extern void print_help(lua_State* L);

struct CCC_LuaHelp : public IConsole_Command
{
    CCC_LuaHelp(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };

    virtual void Execute(LPCSTR args) { print_help(ai().script_engine().lua()); }
};

//#ifndef MASTER_GOLD
#include "game_graph.h"
// Кто населяет реестр ALife этого уровня. См. CALifeUpdateManager::census().
struct CCC_ALifeCensus : public IConsole_Command
{
    CCC_ALifeCensus(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
    virtual void Execute(LPCSTR)
    {
        if (!ai().get_alife()) { Msg("! al_census: no ALife (not in a level?)"); return; }
        ai().alife().census();
    }
};

struct CCC_JumpToLevel : public IConsole_Command
{
    CCC_JumpToLevel(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        if (!ai().get_alife())
        {
            Msg("! ALife simulator is needed to perform specified command!");
            return;
        }
        string256 level;
        sscanf(args, "%s", level);

        GameGraph::LEVEL_MAP::const_iterator I = ai().game_graph().header().levels().begin();
        GameGraph::LEVEL_MAP::const_iterator E = ai().game_graph().header().levels().end();
        for (; I != E; ++I)
            if (!xr_strcmp((*I).second.name(), level))
            {
                ai().alife().jump_to_level(level);
                return;
            }
        Msg("! There is no level \"%s\" in the game graph!", level);
    }

    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        if (!ai().get_alife())
        {
            Msg("! ALife simulator is needed to perform specified command!");
            return;
        }

        GameGraph::LEVEL_MAP::const_iterator itb = ai().game_graph().header().levels().begin();
        GameGraph::LEVEL_MAP::const_iterator ite = ai().game_graph().header().levels().end();
        for (; itb != ite; ++itb)
        {
            tips.push_back((*itb).second.name());
        }
    }
};

// Re-enable / disable a console command at runtime. Mods (e.g. this addon)
// disable dev commands like jump_to_level via Lua get_console():disable_command().
// `cmd_enable jump_to_level` flips it back on so it can be used for testing.
struct CCC_SetCmdEnabled : public IConsole_Command
{
    bool m_enable;
    CCC_SetCmdEnabled(LPCSTR N, bool enable) : IConsole_Command(N), m_enable(enable) {}

    virtual void Execute(LPCSTR args)
    {
        string256 name;
        name[0] = 0;
        sscanf(args, "%s", name);
        CConsole::vecCMD_IT it = Console->Commands.find(name);
        if (it == Console->Commands.end())
        {
            Msg("! No such console command: \"%s\"", name);
            return;
        }
        it->second->SetEnabled(m_enable);
        Msg("- Command \"%s\" %s", name, m_enable ? "enabled" : "disabled");
    }

    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        for (CConsole::vecCMD_IT it = Console->Commands.begin(); it != Console->Commands.end(); ++it)
            tips.push_back(it->first);
    }
};

// Force-run a console command IGNORING its bEnabled flag. cmd_enable only flips the
// flag, which the mod re-clears every tick/level-load, so `cmd_enable jump_to_level`
// races and loses. This forwards to ExecuteCommand(..., allow_disabled=true) so the
// inner command runs regardless: `cmd_force jump_to_level l10_radar`.
struct CCC_CmdForce : public IConsole_Command
{
    CCC_CmdForce(LPCSTR N) : IConsole_Command(N) { bLowerCaseArgs = false; }

    virtual void Execute(LPCSTR args)
    {
        if (!args || !args[0])
        {
            Msg("~ usage: cmd_force <command> [args]");
            return;
        }
        Console->ExecuteCommand(args, false, true);   // allow_disabled = true → bypass bEnabled
    }

    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        for (CConsole::vecCMD_IT it = Console->Commands.begin(); it != Console->Commands.end(); ++it)
            tips.push_back(it->first);
    }
};

class CCC_Spawn : public IConsole_Command
{
public:
    CCC_Spawn(LPCSTR N) : IConsole_Command(N) {}

    // g_spawn <section> [count] [distance]: with a count, a group in a ring
    // `distance` metres ahead of the camera, each on the navmesh -- for the
    // combat AI tests (a squad that has not seen the player yet, not ten
    // bodies on top of him). Without, the stock: one at the player's feet.
    void Execute(LPCSTR args)
    {
        if (!g_pGameLevel)
            return;

        string256 section{};
        int count = 0;
        float distance = 0.f;
        const int n = sscanf(args, "%255s %d %f", section, &count, &distance);
        if (n < 1 || !pSettings->section_exist(section))
        {
            Msg("! Can't find section: %s", args);
            return;
        }

        auto tpGame = smart_cast<game_sv_Single*>(Level().Server->game);
        if (!tpGame)
            return;

        if (n < 2)
        {
            tpGame->alife().spawn_item(section, Actor()->Position(), Actor()->ai_location().level_vertex_id(), Actor()->ai_location().game_vertex_id(), ALife::_OBJECT_ID(-1));
            return;
        }

        clamp(count, 1, 50);
        Fvector ahead = Device.vCameraDirection;
        ahead.y = 0.f;
        if (ahead.magnitude() < EPS)
            ahead.set(0.f, 0.f, 1.f);
        ahead.normalize();
        const Fvector center = Fvector().mad(Actor()->Position(), ahead, distance);
        const float radius = count > 1 ? 2.5f + .4f * float(count) : 0.f;

        u32 spawned = 0;
        for (int i = 0; i < count; ++i)
        {
            const float ang = PI_MUL_2 * float(i) / float(count);
            Fvector pos = center;
            pos.x += radius * _cos(ang);
            pos.z += radius * _sin(ang);

            u32 lvid = ai().level_graph().vertex_id(pos);
            if (!ai().level_graph().valid_vertex_id(lvid))
                lvid = ai().level_graph().nearest_vertex_id(pos);
            if (!ai().level_graph().valid_vertex_id(lvid))
                continue;
            pos = ai().level_graph().vertex_position(lvid);
            const GameGraph::_GRAPH_ID gvid = ai().cross_table().vertex(lvid).game_vertex_id();
            if (tpGame->alife().spawn_item(section, pos, lvid, gvid, ALife::_OBJECT_ID(-1)))
                ++spawned;
        }
        Msg("~ [g_spawn] %u of %d '%s' around (%.1f, %.1f, %.1f), %.0f m ahead", spawned, count, section, VPUSH(center), distance);
    }

    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        if (!ai().get_alife())
        {
            Msg("! ALife simulator is needed to perform specified command!");
            return;
        }

        for (const auto& it : pSettings->sections())
        {
            auto& section = it.first;

            if (pSettings->line_exist(section, "class"))
            {
                tips.push_back(section);
            }
        }

        std::sort(tips.begin(), tips.end());

        // tips.push_back((*itb).second.name());
    }
};
//#endif // MASTER_GOLD

class CCC_SpawnToInventory : public IConsole_Command
{
public:
    CCC_SpawnToInventory(LPCSTR N) : IConsole_Command(N) {}

    void Execute(LPCSTR args)
    {
        if (!g_pGameLevel)
            return;

        if (!pSettings->section_exist(args))
        {
            Msg("! Can't find section: %s", args);
            return;
        }

        if (auto tpGame = smart_cast<game_sv_Single*>(Level().Server->game))
        {
            NET_Packet packet;
            packet.w_begin(M_SPAWN);
            packet.w_stringZ(args);

            CSE_Abstract* item =
                tpGame->alife().spawn_item(args, Actor()->Position(), Actor()->ai_location().level_vertex_id(), Actor()->ai_location().game_vertex_id(), 0, false);
            item->Spawn_Write(packet, FALSE);
            tpGame->alife().server().FreeID(item->ID, 0);
            F_entity_Destroy(item);

            ClientID clientID;
            clientID.set(0xffff);

            u16 dummy;
            packet.r_begin(dummy);
            VERIFY(dummy == M_SPAWN);
            tpGame->alife().server().Process_spawn(packet, clientID);
        }
    }

    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        if (!ai().get_alife())
        {
            Msg("! ALife simulator is needed to perform specified command!");
            return;
        }

        for (const auto& it : pSettings->sections())
        {
            auto& section = it.first;

            if (pSettings->line_exist(section, "class"))
            {
                tips.push_back(section);
            }
        }

        std::sort(tips.begin(), tips.end());

        // tips.push_back((*itb).second.name());
    }
};

#include "GamePersistent.h"

class CCC_MainMenu : public IConsole_Command
{
public:
    CCC_MainMenu(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        bool bWhatToDo = TRUE;
        if (0 == xr_strlen(args))
        {
            bWhatToDo = !MainMenu()->IsActive();
        };

        if (EQ(args, "on") || EQ(args, "1"))
            bWhatToDo = TRUE;

        if (EQ(args, "off") || EQ(args, "0"))
            bWhatToDo = FALSE;

        MainMenu()->Activate(bWhatToDo);
    }
};

struct CCC_StartTimeSingle : public IConsole_Command
{
    CCC_StartTimeSingle(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        u32 year = 1, month = 1, day = 1, hours = 0, mins = 0, secs = 0, milisecs = 0;
        sscanf(args, "%d.%d.%d %d:%d:%d.%d", &year, &month, &day, &hours, &mins, &secs, &milisecs);
        year = _max(year, 1);
        month = _max(month, 1);
        day = _max(day, 1);
        g_qwStartGameTime = generate_time(year, month, day, hours, mins, secs, milisecs);

        if (!g_pGameLevel)
            return;

        if (!Level().Server)
            return;

        if (!Level().Server->game)
            return;

        Level().Server->game->SetGameTimeFactor(g_qwStartGameTime, g_fTimeFactor);
    }

    virtual void Status(TStatus& S)
    {
        u32 year = 1, month = 1, day = 1, hours = 0, mins = 0, secs = 0, milisecs = 0;
        split_time(g_qwStartGameTime, year, month, day, hours, mins, secs, milisecs);
        sprintf_s(S, "%d.%d.%d %d:%d:%d.%d", year, month, day, hours, mins, secs, milisecs);
    }
};

struct CCC_TimeFactorSingle : public CCC_Float
{
    CCC_TimeFactorSingle(LPCSTR N, float* V, float _min = 0.f, float _max = 1.f) : CCC_Float(N, V, _min, _max){};

    virtual void Execute(LPCSTR args)
    {
        CCC_Float::Execute(args);

        if (!g_pGameLevel)
            return;

        if (!Level().Server)
            return;

        if (!Level().Server->game)
            return;

        Level().Server->game->SetGameTimeFactor(g_fTimeFactor);
    }
};

// ---- world clock (server side) ----------------------------------------------
// The world clock belongs to whoever runs the simulation: it is shipped to every
// connected client in each update header, so these commands only do anything in
// the process that hosts the world. On a pure network client they say so instead
// of pretending to work -- a client-local clock would just be overwritten by the
// next update from the host.

static const u64 kWorldDayMs = u64(24) * 60 * 60 * 1000;

static xrServer* world_clock_server(LPCSTR cmd_name)
{
    if (g_pGameLevel && Level().Server && Level().Server->game)
        return Level().Server;

    Msg("! %s: this process does not host the world -- its time of day comes from the server", cmd_name);
    return nullptr;
}

static void world_clock_report(LPCSTR prefix)
{
    xrServer* SV = Level().Server;
    u32 year = 0, month = 0, day = 0, hours = 0, mins = 0, secs = 0, ms = 0;
    split_time(SV->GetWorldTime(), year, month, day, hours, mins, secs, ms);
    Msg("%s %02u:%02u:%02u  (%u.%u.%u)  rate=%.2f  owner=%s", prefix, hours, mins, secs, day, month, year, SV->GetWorldTimeFactor(),
        SV->WorldTimeHostOwned() ? "host" : "scripts");
}

struct CCC_WorldTime : public IConsole_Command
{
    CCC_WorldTime(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR /*args*/)
    {
        if (!world_clock_server("sv_time"))
            return;

        world_clock_report("- world time:");
    }
};

struct CCC_WorldTimeSet : public IConsole_Command
{
    CCC_WorldTimeSet(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        xrServer* SV = world_clock_server("sv_time_set");
        if (!SV)
            return;

        u32 year = 0, month = 0, day = 0, hours = 0, mins = 0, secs = 0;

        // Full stamp first ("y.m.d h:m[:s]"): it is the only form that can move
        // the clock backwards, and it is meant to be explicit about it.
        if (sscanf(args, "%u.%u.%u %u:%u:%u", &year, &month, &day, &hours, &mins, &secs) >= 5)
        {
            const u64 target = generate_time(_max(year, u32(1)), _max(month, u32(1)), _max(day, u32(1)), hours, mins, secs);
            if (target < SV->GetWorldTime())
                Msg("~ sv_time_set: moving the clock BACKWARDS -- timestamps already stored in the world (death times, "
                    "condition and task deadlines) were written against the old value");
            SV->RequestSetGameTime(target);
            return;
        }

        // Time of day ("h:m[:s]"): always the NEXT occurrence, so the day/night
        // cycle only ever moves forward.
        if (sscanf(args, "%u:%u:%u", &hours, &mins, &secs) >= 2)
        {
            const u64 now = SV->GetWorldTime();
            const u64 of_day = u64(hours) * 3600000 + u64(mins) * 60000 + u64(secs) * 1000;
            if (of_day >= kWorldDayMs)
            {
                Msg("! sv_time_set: %u:%02u:%02u is not a time of day", hours, mins, secs);
                return;
            }
            u64 target = now - (now % kWorldDayMs) + of_day;
            if (target <= now)
                target += kWorldDayMs; // already past today -- take tomorrow
            SV->RequestSetGameTime(target);
            return;
        }

        Msg("! sv_time_set: expected \"hh:mm[:ss]\" (next occurrence) or \"y.m.d hh:mm[:ss]\" (exact)");
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "set the world time of day: \"hh:mm[:ss]\" or \"y.m.d hh:mm[:ss]\""); }
};

struct CCC_WorldTimeAdvance : public IConsole_Command
{
    CCC_WorldTimeAdvance(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        xrServer* SV = world_clock_server("sv_time_advance");
        if (!SV)
            return;

        float minutes = 0.f;
        if (sscanf(args, "%f", &minutes) != 1 || minutes <= 0.f)
        {
            Msg("! sv_time_advance: expected a positive number of game minutes");
            return;
        }

        SV->RequestAdvanceGameTime(u32(minutes * 60000.f));
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "push the world clock forward by N game minutes"); }
};

struct CCC_WorldTimeFactor : public IConsole_Command
{
    CCC_WorldTimeFactor(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        xrServer* SV = world_clock_server("sv_time_factor");
        if (!SV)
            return;

        float factor = 0.f;
        if (sscanf(args, "%f", &factor) != 1)
        {
            world_clock_report("- world time:");
            return;
        }

        if (factor < 0.f)
        {
            Msg("! sv_time_factor: rate cannot be negative (0 freezes the clock)");
            return;
        }

        SV->RequestSetTimeFactor(factor);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "how many game seconds pass per real second (0 freezes the world clock)"); }
};

// Prints this installation's content fingerprint under a given salt (or 1 if none
// is passed). Run it on the host and on a client with the SAME salt: identical
// numbers mean identical content, and a differing list_crc vs window_crc says
// whether the file SET or the file CONTENTS drifted.
struct CCC_ContentFingerprint : public IConsole_Command
{
    CCC_ContentFingerprint(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        u64 nonce = 1;
        if (args && args[0])
            nonce = (u64)_strtoui64(args, nullptr, 0);

        ContentIntegrity::Summary s;
        if (!ContentIntegrity::Compute(nonce, s))
        {
            Msg("! sv_content_fingerprint: content could not be read");
            return;
        }

        string256 line;
        ContentIntegrity::Describe(s, line, sizeof(line));
        Msg("- content fingerprint (salt=%llu): %s", (unsigned long long)nonce, line);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "optional salt value; the same salt must be used on both sides to compare"); }
};

// The HMAC key this host verifies join tickets with -- the same 64 hex
// characters authd is given as XROS_TICKET_KEY.
//
// ⚠️ A secret typed into a console. It is never echoed back and never written
// to user.ltx (SetCanSave(false)): a key that leaks lets anyone mint themselves
// an admin ticket, and a key sitting in a settings file next to a screenshot
// folder is a key that leaks.
struct CCC_TicketKey : public IConsole_Command
{
    CCC_TicketKey(LPCSTR N) : IConsole_Command(N)
    {
        bEmptyArgsHandled = TRUE;
        SetCanSave(false);
        // The default folds arguments to lower case. Harmless for hex, but this
        // command shares its shape with sv_auth_token, where it would not be.
        bLowerCaseArgs = FALSE;
    };

    virtual void Execute(LPCSTR args)
    {
        if (!NET_GNS::g_ApplyTicketKey)
        {
            Msg("! sv_ticket_key: server only (this build cannot verify tickets)");
            return;
        }
        if (!args || !args[0])
        {
            Msg("- sv_ticket_key: give 64 hex characters (the same value as authd's XROS_TICKET_KEY)");
            return;
        }

        if (NET_GNS::g_ApplyTicketKey(args))
            Msg("* [auth] ticket key set"); // deliberately not echoing it
        else
            Msg("! sv_ticket_key: expected exactly 64 hex characters -- the previous key is unchanged");
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "64 hex characters: the ticket signing key shared with authd"); }
};

// Where authd is, as host:port. Defaults to 127.0.0.1:8081.
struct CCC_AuthUrl : public IConsole_Command
{
    CCC_AuthUrl(LPCSTR N) : IConsole_Command(N)
    {
        bEmptyArgsHandled = TRUE;
        bLowerCaseArgs = FALSE; // a hostname may be case-sensitive
    };

    virtual void Execute(LPCSTR args)
    {
        if (!NET_GNS::g_ApplyAuthEndpoint)
        {
            Msg("! sv_auth_url: server only");
            return;
        }
        if (!args || !args[0])
        {
            Msg("- sv_auth_url: give host:port of authd's internal listener");
            return;
        }

        string256 host{};
        int port = 8081;
        const char* colon = strrchr(args, ':');
        if (colon)
        {
            const size_t len = size_t(colon - args);
            if (len == 0 || len >= sizeof(host))
            {
                Msg("! sv_auth_url: expected host:port");
                return;
            }
            memcpy(host, args, len);
            host[len] = 0;
            port = atoi(colon + 1);
        }
        else
            strncpy_s(host, sizeof(host), args, _TRUNCATE);

        if (NET_GNS::g_ApplyAuthEndpoint(host, port))
            Msg("* [auth] authd at %s:%d", host, port);
        else
            Msg("! sv_auth_url: %s:%d is not a usable endpoint", host, port);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "host:port of authd's game-server API (default 127.0.0.1:8081)"); }
};

// The shared secret this host presents to authd. Same secrecy rules as the
// ticket key above.
struct CCC_AuthToken : public IConsole_Command
{
    CCC_AuthToken(LPCSTR N) : IConsole_Command(N)
    {
        bEmptyArgsHandled = TRUE;
        SetCanSave(false);
        // ⚠️ Load-bearing: a shared secret is case-sensitive, and the default
        // would silently fold it into one that authd rejects.
        bLowerCaseArgs = FALSE;
    };

    virtual void Execute(LPCSTR args)
    {
        if (!NET_GNS::g_ApplyAuthToken)
        {
            Msg("! sv_auth_token: server only");
            return;
        }
        if (!args || !args[0])
        {
            Msg("- sv_auth_token: give the value of authd's XROS_INTERNAL_TOKEN");
            return;
        }

        if (NET_GNS::g_ApplyAuthToken(args))
            Msg("* [auth] authd token set");
        else
            Msg("! sv_auth_token: rejected");
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "shared secret for authd's game-server API"); }
};

// Who ordered the administrative command currently running, or 0 for the host
// console. Set by xrServer::Process_admin_command for the duration of one
// command; read by the sv_ban/sv_unban/sv_setrole commands below so the audit
// trail in authd records an author rather than "somebody".
//
// A global rather than a parameter because IConsole_Command::Execute takes only
// its argument string -- and it is safe: console commands run on the game
// thread, one at a time.
u64 g_admin_actor_account = 0;

// The administrative verbs. Each is the same one line: hand the verb and its
// arguments to the server-side implementation (online/server/sv_admin.cpp) and
// print what came back.
//
// The work is NOT done here because it needs authd and the transport, neither of
// which xrGame.lib may depend on -- see NET_GNS::g_AdminAction. What is here is
// the console surface, identical whether the command was typed on the host or
// arrived over the wire from an admin.
struct CCC_AdminVerb : public IConsole_Command
{
    const char* verb;

    CCC_AdminVerb(LPCSTR N, const char* v) : IConsole_Command(N), verb(v)
    {
        bEmptyArgsHandled = TRUE;
        // A ban reason and a login are typed by a human and read back by one;
        // folding them to lower case would mangle both.
        bLowerCaseArgs = FALSE;
    };

    virtual void Execute(LPCSTR args)
    {
        if (!NET_GNS::g_AdminAction)
        {
            Msg("! %s: server only", cName);
            return;
        }

        string1024 reply{};
        const bool ok = NET_GNS::g_AdminAction(verb, args ? args : "", g_admin_actor_account, reply, sizeof(reply));
        // Both outcomes are printed the same way: the reply already says what
        // happened, and a caller reading the log should not have to know which
        // marker means what.
        Msg("%s %s", ok ? "-" : "!", reply);
    }
};

// Runs a console command ON THE SERVER, if this player's account is allowed to.
//
// The client does not decide anything: it sends the line, and the server checks
// the role recorded from the join ticket. A patched client can send whatever it
// likes and will be refused and logged, because the answer to "may I" lives on
// the side that would have to carry out the order.
struct CCC_Rcon : public IConsole_Command
{
    CCC_Rcon(LPCSTR N) : IConsole_Command(N)
    {
        bEmptyArgsHandled = TRUE;
        bLowerCaseArgs = FALSE; // it is somebody else's command line, passed through
    };

    virtual void Execute(LPCSTR args)
    {
        if (!args || !args[0])
        {
            Msg("- rcon: give the server console command to run, e.g. rcon sv_who");
            return;
        }
        if (!g_pGameLevel)
        {
            Msg("! rcon: not connected to a server");
            return;
        }
        if (Level().Server)
        {
            // This process IS the server -- there is nothing to send it to, and
            // running it locally is what the plain command already does.
            Msg("! rcon: this is the host; run '%s' directly", args);
            return;
        }

        NET_Packet P;
        P.w_begin(M_ADMIN_COMMAND);
        P.w_stringZ(args);
        Level().Send(P, net_flags(TRUE, TRUE));
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "a console command to run on the server (administrators only)"); }
};

// Hands the clock back to whatever scripts this world runs. Setting the time or
// the rate claims it for the host; this is the way out of that.
struct CCC_WorldTimeAuto : public IConsole_Command
{
    CCC_WorldTimeAuto(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR /*args*/)
    {
        xrServer* SV = world_clock_server("sv_time_auto");
        if (!SV)
            return;

        SV->ReleaseWorldTime();
        world_clock_report("- world time released to scripts:");
    }
};

// ---- world weather / rain (server side) --------------------------------------
// Rain is not a local preference: it decides how far a player sees and how the
// world sounds, so it is decided here and shipped to every client in the update
// header. As with the clock, on a machine that only renders someone else's world
// these commands say so rather than pretending to work.

static xrServer* world_weather_server(LPCSTR cmd_name)
{
    if (g_pGameLevel && Level().Server && g_pGamePersistent)
        return Level().Server;

    Msg("! %s: this process does not host the world -- its weather comes from the server", cmd_name);
    return nullptr;
}

static void world_weather_report(LPCSTR prefix)
{
    xrServer* SV = Level().Server;
    CEnvironment& env = g_pGamePersistent->Environment();
    extern int g_sv_weather_stream;

    Msg("%s cycle='%s'  owner=%s  stream=%s", prefix, SV->GetWorldWeather().c_str(), SV->WeatherHostOwned() ? "host" : "scripts",
        g_sv_weather_stream ? "on (clients get live values)" : "off (clients mix the cycle themselves)");

    // Every channel with its units, because the useful ranges differ per channel and
    // per level's content: rain and fog are 0..1, wind is in the cycle's own units
    // (hundreds), lightning is a period in seconds. Printing what the RUNNING cycle
    // produces is what tells an operator the scale before they type a value.
    for (int ch = 0; ch < eWeatherChannelCount; ++ch)
    {
        const EWeatherChannel c = (EWeatherChannel)ch;
        Msg("%s   %-7s = %8.2f  %s", prefix, CEnvironment::WeatherChannelName(c), SV->GetWorldWeatherChannel(ch),
            SV->IsWorldWeatherChannelHeld(ch) ? "HELD by host" : "from cycle");
    }

    // The two hour sections being blended, for the same reason weather_status prints
    // them: "the rainy cycle is running" and "it is raining right now" are different
    // claims, and only the section pair tells them apart.
    Msg("%s   sections [%s -> %s]%s%s", prefix, env.Current[0] ? env.Current[0]->m_identifier.c_str() : "?", env.Current[1] ? env.Current[1]->m_identifier.c_str() : "?",
        env.IsWeatherLocked() ? "  LOCKED" : "", env.IsWeatherFXPlaying() ? "  WFX" : "");
}

// One command class for all four channels -- they differ only in which channel they
// address and what a sensible upper bound is. Sharing the body keeps the argument
// handling, the validation message and the "not the host" refusal identical across
// them, which is what an operator needs when they are typing into a live server.
struct CCC_WorldWeatherChannel : public IConsole_Command
{
    EWeatherChannel m_channel;
    float m_max;
    LPCSTR m_units;

    CCC_WorldWeatherChannel(LPCSTR N, EWeatherChannel ch, float max_value, LPCSTR units) : IConsole_Command(N), m_channel(ch), m_max(max_value), m_units(units)
    {
        bEmptyArgsHandled = TRUE;
    }

    virtual void Execute(LPCSTR args)
    {
        xrServer* SV = world_weather_server(cName);
        if (!SV)
            return;

        // No argument: report, rather than guess at what was meant.
        float value = 0.f;
        if (!args || !args[0] || sscanf(args, "%f", &value) != 1)
        {
            world_weather_report("- world weather:");
            return;
        }

        if (value < 0.f || value > m_max)
        {
            Msg("! %s: expected 0..%g (%s); the matching *_auto command releases it back to the cycle", cName, m_max, m_units);
            return;
        }

        SV->RequestSetWeatherChannel((int)m_channel, value);
    }

    virtual void Info(TInfo& I)
    {
        string256 line;
        xr_sprintf(line, "hold this world's %s for every client: 0..%g (%s)", CEnvironment::WeatherChannelName(m_channel), m_max, m_units);
        xr_strcpy(I, line);
    }
};

struct CCC_WorldWeatherChannelAuto : public IConsole_Command
{
    EWeatherChannel m_channel;

    CCC_WorldWeatherChannelAuto(LPCSTR N, EWeatherChannel ch) : IConsole_Command(N), m_channel(ch) { bEmptyArgsHandled = TRUE; }

    virtual void Execute(LPCSTR /*args*/)
    {
        xrServer* SV = world_weather_server(cName);
        if (!SV)
            return;

        SV->RequestWeatherChannelAuto((int)m_channel);
        Msg("- %s released to the running weather cycle", CEnvironment::WeatherChannelName(m_channel));
    }
};

struct CCC_WorldWeather : public IConsole_Command
{
    CCC_WorldWeather(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR /*args*/)
    {
        if (!world_weather_server("sv_weather"))
            return;

        world_weather_report("- world weather:");
    }
};

struct CCC_WorldWeatherSet : public IConsole_Command
{
    CCC_WorldWeatherSet(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        xrServer* SV = world_weather_server("sv_weather_set");
        if (!SV)
            return;

        if (!args || !args[0])
        {
            Msg("! sv_weather_set: expected a weather cycle name -- see sv_weather_list");
            return;
        }

        // Validated here so a typo answers immediately; the request itself is applied
        // on the server tick, between update exports.
        CEnvironment& env = g_pGamePersistent->Environment();
        if (env.WeatherCycles.find(args) == env.WeatherCycles.end())
        {
            Msg("! sv_weather_set: no such cycle '%s' on this level -- see sv_weather_list", args);
            return;
        }

        SV->RequestSetWeather(args);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "weather cycle every client will follow (holds it against this world's forecast script)"); }

    void fill_tips(vecTips& tips, u32 mode) override
    {
        if (!g_pGameLevel || !g_pGamePersistent)
            return;
        for (auto& [name, cycle] : g_pGamePersistent->Environment().WeatherCycles)
            tips.push_back(name);
    }
};

struct CCC_WorldWeatherList : public IConsole_Command
{
    CCC_WorldWeatherList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR /*args*/)
    {
        if (!world_weather_server("sv_weather_list"))
            return;

        CEnvironment& env = g_pGamePersistent->Environment();
        Msg("- %u weather cycles on this level (running '%s'):", (u32)env.WeatherCycles.size(), env.GetWeather().c_str());
        for (auto& [name, cycle] : env.WeatherCycles)
            Msg("-   %s", name.c_str());
    }
};

// Hands the sky back to whatever forecast this world's scripts run. Placing a cycle
// or forcing the rain claims it for the host; this is the way out of that.
struct CCC_WorldWeatherAuto : public IConsole_Command
{
    CCC_WorldWeatherAuto(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR /*args*/)
    {
        xrServer* SV = world_weather_server("sv_weather_auto");
        if (!SV)
            return;

        SV->ReleaseWeather();
        SV->RequestWeatherAllAuto();
        Msg("- world weather released to scripts (clients still follow whatever this world runs)");
    }
};

// Weather effects: the scripted overlays (a storm rolling through, an emission sky).
// They replace the running weather for their duration and carry their own sky, cloud
// and light colours, which is why they are synchronised as an effect and not just as
// four numbers.
struct CCC_WorldWeatherFX : public IConsole_Command
{
    CCC_WorldWeatherFX(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        xrServer* SV = world_weather_server(cName);
        if (!SV)
            return;

        if (!args || !args[0])
        {
            Msg("! sv_weather_fx: expected an effect name -- see sv_weather_fx_list");
            return;
        }

        CEnvironment& env = g_pGamePersistent->Environment();
        if (env.GetWeatherFXs().find(args) == env.GetWeatherFXs().end())
        {
            Msg("! sv_weather_fx: no such effect '%s' on this level -- see sv_weather_fx_list", args);
            return;
        }

        SV->RequestStartWeatherFX(args);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "run a weather effect for every client (storm, emission); sv_weather_fx_stop ends it"); }

    void fill_tips(vecTips& tips, u32 mode) override
    {
        if (!g_pGameLevel || !g_pGamePersistent)
            return;
        for (auto& [name, fx] : g_pGamePersistent->Environment().GetWeatherFXs())
            tips.push_back(name);
    }
};

struct CCC_WorldWeatherFXStop : public IConsole_Command
{
    CCC_WorldWeatherFXStop(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR /*args*/)
    {
        xrServer* SV = world_weather_server(cName);
        if (!SV)
            return;

        SV->RequestStopWeatherFX();
    }
};

struct CCC_WorldWeatherFXList : public IConsole_Command
{
    CCC_WorldWeatherFXList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR /*args*/)
    {
        if (!world_weather_server(cName))
            return;

        CEnvironment& env = g_pGamePersistent->Environment();
        Msg("- %u weather effects on this level%s:", (u32)env.GetWeatherFXs().size(), env.IsWeatherFXPlaying() ? " (one is playing)" : "");
        for (auto& [name, fx] : env.GetWeatherFXs())
            Msg("-   %s", name.c_str());
    }
};

// Whether clients are pinned to this world's live weather values or mix the cycle
// themselves. See g_sv_weather_stream in xrServer.cpp for why it defaults to on.
struct CCC_WorldWeatherStream : public IConsole_Command
{
    CCC_WorldWeatherStream(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        extern int g_sv_weather_stream;

        if (!args || !args[0])
        {
            Msg("- sv_weather_stream is %d (%s)", g_sv_weather_stream,
                g_sv_weather_stream ? "clients render this world's live weather" : "clients mix rain/fog/wind/lightning from the shared cycle themselves");
            return;
        }

        g_sv_weather_stream = (atoi(args) != 0) ? 1 : 0;
        Msg("- sv_weather_stream = %d", g_sv_weather_stream);
        // No level needed to flip the flag (it is read when a header is built), so
        // unlike the other sv_weather commands this one works before a world loads.
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "1 = clients render the server's live weather values, 0 = clients mix the shared cycle themselves"); }
};

// Writes a census record immediately instead of waiting for the schedule, and
// says where the file is -- with no output there is no way to tell whether
// telemetry is running or silently disabled.
struct CCC_TelemetryDump : public IConsole_Command
{
    CCC_TelemetryDump(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR /*args*/)
    {
        extern int g_sv_telemetry;
        if (!g_sv_telemetry)
        {
            Msg("- sv_telemetry is 0 -- nothing is being recorded (set it to 1 first)");
            return;
        }

        SVTelemetry::CensusNow();
        if (SVTelemetry::Active())
            Msg("- telemetry census written to '%s'", SVTelemetry::FileName());
        else
            Msg("! telemetry is on but no file is open -- this process does not own a server");
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "write one telemetry census now and print the log path"); }
};

// Prints the server's health ledger next to what each client claims. This is
// the calibration instrument for sv_health_authority: the gap column is what
// has to be understood before the mode is allowed to kill anybody.
struct CCC_HealthDump : public IConsole_Command
{
    CCC_HealthDump(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR /*args*/) { SVHealth::Dump(); }

    virtual void Info(TInfo& I) { xr_strcpy(I, "print the server-side health ledger for every tracked client"); }
};

// Fault injection for the enforcement path -- see SVHealth::Drain.
struct CCC_HealthDrain : public IConsole_Command
{
    CCC_HealthDrain(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        const float amount = (args && args[0]) ? float(atof(args)) : 1.f;
        SVHealth::Drain(amount);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "take <amount> health off every tracked ledger, as if the server had seen that much damage"); }
};

// Fires a genuine GE_HIT at every tracked player -- see SVHealth::InjectHit.
// Args: <power> [hit_type], hit_type defaulting to fire wound (a bullet).
struct CCC_HitTest : public IConsole_Command
{
    CCC_HitTest(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        float power = 0.3f;
        int hit_type = int(ALife::eHitTypeFireWound);
        int bone = 0;
        float ap = 0.f;
        if (args && args[0])
            sscanf(args, "%f %d %d %f", &power, &hit_type, &bone, &ap);
        SVHealth::InjectHit(power, u16(hit_type), u16(bone), ap);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "hit every tracked player for <power> [hit_type] [bone] [ap] through the real GE_HIT channel"); }
};

// Where a server frame goes -- see sv_perf.h.
struct CCC_SvPerf : public IConsole_Command
{
    CCC_SvPerf(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { SVPerf::Dump(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "print the server's frame-time breakdown since the last print (first call arms the counters)"); }
};

struct CCC_SvPerfReset : public IConsole_Command
{
    CCC_SvPerfReset(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { SVPerf::Reset(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "start a fresh server frame-time sampling window"); }
};

// ---------------------------------------------------------------------
// How long a shot takes to become a record, and whether the record is the
// shot -- see shot_latency.h. These live on BOTH sides: the shooter and the
// recorder are different processes, and each stamps its own end.
// ---------------------------------------------------------------------
struct CCC_ShotReport : public IConsole_Command
{
    CCC_ShotReport(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { ShotLatency::Report(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "shots measured so far, and how long each leg of the journey took"); }
};

struct CCC_ShotReset : public IConsole_Command
{
    CCC_ShotReset(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { ShotLatency::Reset(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "forget every measured shot and start a new window"); }
};

struct CCC_FireTest : public IConsole_Command
{
    CCC_FireTest(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        int shots = 8;
        u32 gap = 500;
        float range = 60.f;
        Fvector p{};
        const int n = (args && args[0]) ? sscanf(args, "%d %u %f %f %f %f", &shots, &gap, &range, &p.x, &p.y, &p.z) : 0;
        if (n == 6)
            CLFire::StartAtPoint(shots, gap, p); // 12-10-2026: <shots> <gap> <range, unused> x y z -- into a fixed point
        else
            CLFire::Start(shots, gap, range);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "aim at the nearest creature and fire <shots> [gap_ms] [range_m] through the real trigger"); }
};

// dbg_water_scan <cx> <cz> <sx> <sz> <step> <water_y> (12-10-2026, Switch water test): where a water sheet is OPEN. A
// grid of rays straight down over the rectangle: a hit at the water level (a water face in the cform) or below it is
// water you can see; a hit above it is ground over a buried part of the sheet. Prints the open points, the middle of
// them and the nearest AI vertex (the shore: there are none on water) for perf_route / cl_fire_test.
struct CCC_WaterScan : public IConsole_Command
{
    CCC_WaterScan(LPCSTR N) : IConsole_Command(N) {}
    virtual void Execute(LPCSTR args)
    {
        float cx = 0, cz = 0, sx = 0, sz = 0, step = 4.f, wy = 0;
        if (!g_pGameLevel || sscanf(args, "%f %f %f %f %f %f", &cx, &cz, &sx, &sz, &step, &wy) != 6 || step < .5f)
        {
            Msg("! dbg_water_scan <cx> <cz> <sx> <sz> <step> <water_y>");
            return;
        }
        u32 open = 0, ground = 0, miss = 0, said = 0;
        Fvector sum{};
        const CDB::TRI* tris = Level().ObjectSpace.GetStaticTris();
        for (float x = cx - sx * .5f; x <= cx + sx * .5f; x += step)
            for (float z = cz - sz * .5f; z <= cz + sz * .5f; z += step)
            {
                collide::rq_result R;
                const Fvector from{x, wy + 60.f, z};
                if (!Level().ObjectSpace.RayPick(from, Fvector{0.f, -1.f, 0.f}, 120.f, collide::rqtStatic, R, nullptr))
                {
                    ++miss;
                    continue;
                }
                const float hy = from.y - R.range;
                if (hy > wy + .15f)
                {
                    ++ground;
                    continue;
                }
                ++open;
                sum.add(Fvector{x, hy, z});
                if (said < 40)
                {
                    ++said;
                    SGameMtl* M = GMLib.GetMaterialByIdx(tris[R.element].material);
                    Msg("~ [water scan] open at %.1f,%.2f,%.1f (%s)", x, hy, z, M ? M->m_Name.c_str() : "?");
                }
            }
        Msg("~ [water scan] %u open, %u ground over it, %u no hit (step %.1f m)", open, ground, miss, step);
        if (open)
        {
            sum.div(float(open));
            const u32 v = ai().level_graph().nearest_vertex_id(sum);
            const Fvector shore = ai().level_graph().valid_vertex_id(v) ? ai().level_graph().vertex_position(v) : sum;
            Msg("~ [water scan] middle of the open water %.1f,%.2f,%.1f | nearest AI vertex (shore) %.1f,%.2f,%.1f, %.1f m from it", sum.x, sum.y,
                sum.z, shore.x, shore.y, shore.z, shore.distance_to(sum));
        }
    }
};

struct CCC_FireStop : public IConsole_Command
{
    CCC_FireStop(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { CLFire::Stop(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "stop the automated fire test and let go of the trigger"); }
};

struct CCC_FireStatus : public IConsole_Command
{
    CCC_FireStatus(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { CLFire::Status(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "where the automated fire test has got to"); }
};

// The bolt equivalent -- see cl_bolt_test.h for why it is worth its own harness.
struct CCC_BoltTest : public IConsole_Command
{
    CCC_BoltTest(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        int throws = 5;
        u32 gap = 1500;
        if (args && args[0])
            sscanf(args, "%d %u", &throws, &gap);
        CLBolt::Start(throws, gap);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "draw the bolt and throw it <throws> [gap_ms] times through the real trigger"); }
};

struct CCC_BoltStop : public IConsole_Command
{
    CCC_BoltStop(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { CLBolt::Stop(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "stop the automated bolt test and let go of the trigger"); }
};

// The in-game anomaly editor. See cl_zone_edit.h -- it is a handle on top of
// sv_anomaly_* and rcon, and it commands the server rather than the world.
struct CCC_ZoneEdit : public IConsole_Command
{
    CCC_ZoneEdit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { ClZoneEdit::Toggle(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "toggle the anomaly editor: wheel sizes, LMB places, RMB picks, DEL removes"); }
};

// The first screen on the RmlUi track. See ui_rml.h.
struct CCC_ZonePicker : public IConsole_Command
{
    CCC_ZonePicker(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { UIRml::ShowZonePicker(!UIRml::ZonePickerVisible()); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "the HTML/CSS anomaly picker (RmlUi)"); }
};

struct CCC_ZoneEditSection : public IConsole_Command
{
    CCC_ZoneEditSection(LPCSTR N) : IConsole_Command(N)
    {
        bEmptyArgsHandled = TRUE;
        bLowerCaseArgs = FALSE; // a section name is content, not a keyword
    };
    virtual void Execute(LPCSTR args) { ClZoneEdit::SetSection(args); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "which zone section the editor places next"); }
};

// The in-game furniture editor. See cl_prop_edit.h -- the same handle over
// sv_prop_* that CCC_ZoneEdit is over sv_anomaly_*.
struct CCC_PropEdit : public IConsole_Command
{
    CCC_PropEdit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { ClPropEdit::Toggle(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "toggle the prop editor: LMB places, RMB picks, wheel turns, G moves, F builds a campfire"); }
};

struct CCC_PropEditModel : public IConsole_Command
{
    CCC_PropEditModel(LPCSTR N) : IConsole_Command(N)
    {
        bEmptyArgsHandled = TRUE;
        bLowerCaseArgs = FALSE; // a model path is content, not a keyword
    };
    virtual void Execute(LPCSTR args) { ClPropEdit::SetModel(args); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "which model the prop editor places next"); }
};

struct CCC_PropPicker : public IConsole_Command
{
    CCC_PropPicker(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { UIRml::ShowPropPicker(!UIRml::PropPickerVisible()); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "the HTML/CSS model picker (RmlUi)"); }
};

struct CCC_ZoneDebugDump : public IConsole_Command
{
    CCC_ZoneDebugDump(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { ClZoneDebug::Dump(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "what the zone overlay actually built: radii, power, outline and hatch counts, ground-ray hits"); }
};

struct CCC_BoltStatus : public IConsole_Command
{
    CCC_BoltStatus(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { CLBolt::Status(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "where the automated bolt test has got to, and how full the HUD blend lists are"); }
};

// ---------------------------------------------------------------------
// Server-owned creature health -- see sv_npc_health.h and sv_bone_names.h.
// ---------------------------------------------------------------------
struct CCC_NpcHealthDump : public IConsole_Command
{
    CCC_NpcHealthDump(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { SVNpcHealth::Dump(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "creatures the server keeps health for, and what is left of them"); }
};

struct CCC_NpcDamageTest : public IConsole_Command
{
    CCC_NpcDamageTest(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        string128 who = {};
        int bone = 0;
        float power = 0.625f;
        if (!args || !args[0] || sscanf(args, "%127s %f %d", who, &power, &bone) < 1)
        {
            Msg("! usage: sv_npc_damage_test <entity id|all|faction> [power] [bone]");
            return;
        }

        // ⭐"all" and a faction name, because the question this answers is
        // usually comparative -- the same bullet on the same bone against two
        // creatures wearing different models -- and a harness cannot know the
        // ids a spawn happened to hand out.
        if (who[0] >= '0' && who[0] <= '9')
        {
            SVNpcHealth::Explain(u16(atoi(who)), power, u16(bone));
            return;
        }

        xr_vector<u16> ids;
        if (0 == xr_strcmp(who, "all"))
            SVNpc::Ids(ids);
        else
            SVNpc::IdsOfFaction(who, ids);

        if (ids.empty())
        {
            Msg("! [sv_npc_health] no tracked creature matches '%s'", who);
            return;
        }
        for (u16 id : ids)
            SVNpcHealth::Explain(id, power, u16(bone));
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "print every factor that turns <power> into damage on <entity id>'s [bone]"); }
};

// sv_hit_fake <victim> <power> [bone] [type] [source id] -- a hit the server
// was never dealt.
//
// ⭐⭐A bound nobody has watched refuse anything is a bound nobody knows works.
// The honest way to test one is to produce the thing it exists to catch, and
// the only faithful way to produce it is through the SAME door the real thing
// comes in: this builds the exact M_EVENT/GE_HIT a client sends and hands it to
// xrServer::OnMessage, which is where the network delivers. Nothing about the
// validation path is bypassed or simulated -- the packet is indistinguishable
// from a patched client's, which is the point.
//
// The attacker is the player and the weapon is one the player is really
// carrying, so a claim over the cap is a claim about a REAL weapon, not a
// missing one taking some other branch.
//
// ⭐The type and the source are arguments because the caps stopped being about
// bullets (30-07-2026). A knife's `wound` and a grenade's `explosion` have
// their own ceilings out of their own ini lines, and each of them is a
// separate thing to watch refuse: sv_hit_fake 12 5 2 wound, or
// sv_hit_fake 12 50 0 explosion <grenade id>.
struct CCC_HitFake : public IConsole_Command
{
    CCC_HitFake(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        int victim = -1, bone = 2, source_id = -1;
        float power = 5.f;
        string64 type_name = "fire_wound";

        // ⭐"all" and faction names, so a harness can kill a known set without
        // knowing the ids a spawn happened to hand out. Added because a revive
        // test whose deaths depend on whether the AI felt like fighting measures
        // the AI's mood, not the revive: two runs of the same script produced
        // two deaths and then none.
        string128 who = {};
        if (args && args[0] && sscanf(args, "%127s", who) == 1 && who[0] && !(who[0] >= '0' && who[0] <= '9'))
        {
            xr_vector<u16> ids;
            if (0 == xr_strcmp(who, "all"))
                SVNpc::Ids(ids);
            else
                SVNpc::IdsOfFaction(who, ids);

            if (ids.empty())
            {
                Msg("! [sv_hit_fake] nothing matches '%s'", who);
                return;
            }

            const char* rest = args + xr_strlen(who); // the arguments after the target
            for (u16 id : ids)
            {
                string256 line;
                xr_sprintf(line, sizeof(line), "%u%s", u32(id), rest);
                Execute(line);
            }
            return;
        }

        if (!args || !args[0] || sscanf(args, "%d %f %d %63s %d", &victim, &power, &bone, type_name, &source_id) < 2)
        {
            Msg("! usage: sv_hit_fake <victim id|all|faction> <power> [bone] [type] [source id]");
            return;
        }

        const int hit_type = SVDamage::HitTypeFromName(type_name);
        if (hit_type < 0)
        {
            Msg("! [sv_hit_fake] '%s' is not a hit type -- fire_wound, wound, wound_2, strike, explosion, burn, shock, chemical_burn, radiation, telepatic", type_name);
            return;
        }

        xrServer* SV = SVWorld::Server();
        CSE_Abstract* e_vic = SVWorld::Find(u16(victim));
        CSE_Abstract* e_att = SVWorld::Find(SVNpc::ReferenceEntity());
        if (!SV || !e_vic || !e_att)
        {
            Msg("! [sv_hit_fake] need a live server, a victim and a shooter (victim %s, shooter %s)", e_vic ? "ok" : "missing", e_att ? "ok" : "missing");
            return;
        }
        // Who to send it AS. The shooter's own client if the entity carries the
        // back-link, otherwise whichever client is playing -- ⚠the two links are
        // not symmetric and only one of them can be relied on (a client points
        // at its entity long before, and sometimes without, the entity pointing
        // back). The sender only decides which connection the server thinks
        // this arrived on; the hit itself is attributed by the id in the packet.
        ClientID sender;
        bool sender_found = false;
        if (e_att->owner)
        {
            sender = e_att->owner->ID;
            sender_found = true;
        }
        else
        {
            for (u32 i = 0; i < SV->client_Count() && !sender_found; ++i)
            {
                xrClientData* C = (xrClientData*)SV->client_Get(i);
                if (!C)
                    continue;
                sender = C->ID;
                sender_found = true; // a connected client outranks nothing else on offer
                if (!C->flags.bLocal && C->flags.bConnected)
                    break;
            }
            if (sender_found)
                Msg("~ [sv_hit_fake] entity %u carries no back-link to a client; sending as client %u instead", e_att->ID, sender.value());
        }
        if (!sender_found)
        {
            Msg("! [sv_hit_fake] no client to send this as -- nobody is connected");
            return;
        }

        // What the packet names as the source. Either an entity the caller
        // picked -- an anomaly, a grenade, anything in the world -- or, by
        // default, something the shooter is really carrying that the server can
        // price for this KIND of hit. Naming one it cannot price would test the
        // "no ceiling, pass it" branch instead of the bound.
        CSE_Abstract* e_wpn = (source_id >= 0) ? SVWorld::Find(u16(source_id)) : nullptr;
        if (source_id >= 0 && !e_wpn)
        {
            Msg("! [sv_hit_fake] no entity %d to blame the hit on", source_id);
            return;
        }
        if (!e_wpn)
        {
            CSE_Abstract* fallback = nullptr;
            for (u16 child : e_att->children)
            {
                CSE_Abstract* c = SVWorld::Find(child);
                if (!c)
                    continue;
                if (!fallback && pSettings->line_exist(c->s_name, "hit_power"))
                    fallback = c;
                if (SVDamage::PowerCapFor(c, u16(hit_type)).known)
                {
                    e_wpn = c;
                    break;
                }
            }
            if (!e_wpn)
                e_wpn = fallback;
        }
        if (!e_wpn)
        {
            Msg("! [sv_hit_fake] the shooter carries nothing that deals damage -- give something first (sv_item_give actor wpn_ak74), or name a source id");
            return;
        }

        const SVDamage::ShotCap cap = SVDamage::PowerCapFor(e_wpn, u16(hit_type));
        Msg("~ [sv_hit_fake] claiming %.4f %s from '%s' (server's cap %.4f%s%s) against %u '%s' bone %u", power, type_name, e_wpn->s_name.c_str(), cap.known ? cap.power : -1.f,
            cap.known ? ", from " : ", nothing in content prices this -- it will pass", cap.known ? cap.how : "", u16(victim), e_vic->s_name.c_str(), u16(bone));

        NET_Packet P;
        P.w_begin(M_EVENT);
        P.w_u32(Level().timeServer());
        P.w_u16(GE_HIT);
        P.w_u16(u16(victim)); // the event's destination -- Process_event rewinds to it for GE_HIT
        // ...and from here byte for byte what SHit::Write_Packet_Cont writes.
        P.w_u16(e_att->ID);
        P.w_u16(e_wpn->ID);
        Fvector dir;
        dir.sub(e_vic->o_Position, e_att->o_Position);
        if (dir.square_magnitude() < EPS)
            dir.set(0.f, 0.f, 1.f);
        dir.normalize_safe();
        P.w_dir(dir);
        P.w_float(power);
        P.w_u16(u16(bone));
        Fvector in_bone{};
        P.w_vec3(in_bone);
        P.w_float(0.f); // impulse
        P.w_u16(0); // aim_bullet
        P.w_u16(u16(hit_type));
        P.w_float(0.f); // ap

        // OnMessage rewinds and reads the message id itself (NET_Packet::r_begin),
        // so the packet is handed over exactly as it comes off the wire.
        SV->OnMessage(P, sender);
    }

    virtual void Info(TInfo& I)
    {
        xr_strcpy(I, "sv_hit_fake <victim> <power> [bone] [type] [source id] -- send a GE_HIT claiming more damage than the content allows, to prove sv_hit_validate refuses it");
    }
};

struct CCC_NpcAttack : public IConsole_Command
{
    CCC_NpcAttack(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        // "all" is the form a script wants: it names nobody, so it cannot go
        // stale when ids shift. Same convention as every sv_npc_* order.
        if (args && strstr(args, "all"))
        {
            SVFight::AttackAll();
            return;
        }

        int a = -1, b = 0xffff, mutual = 1;
        if (!args || !args[0] || sscanf(args, "%d %d %d", &a, &b, &mutual) < 1)
        {
            Msg("! usage: sv_npc_attack all | <attacker id> [target id | omitted = nearest] [mutual 0/1]");
            return;
        }
        SVFight::Attack(u16(a), u16(b), mutual != 0);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "introduce two online creatures as enemies (target omitted = nearest) and let their own brains fight"); }
};

struct CCC_NpcWhy : public IConsole_Command
{
    CCC_NpcWhy(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        int id = 0xffff;
        if (args && args[0] && !strstr(args, "all"))
            sscanf(args, "%d", &id);
        SVFight::Explain(u16(id));
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "walk the whole see->judge->plan->aim->fire chain of a creature and print where it stops"); }
};

struct CCC_NpcWhyWatch : public IConsole_Command
{
    CCC_NpcWhyWatch(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        int secs = 10;
        if (args && args[0])
            sscanf(args, "%d", &secs);
        SVFight::Watch(secs);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "sv_npc_why once a second for N seconds, sampled inside the creature's own update"); }
};

struct CCC_NpcMind : public IConsole_Command
{
    CCC_NpcMind(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { SVFight::Census(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "what the whole roster is thinking: plan, stance and where each side is steering, grouped by faction"); }
};

struct CCC_NpcMindWatch : public IConsole_Command
{
    CCC_NpcMindWatch(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        int secs = 60, period = 1000;
        if (args && args[0])
            sscanf(args, "%d %d", &secs, &period);
        SVFight::CensusWatch(secs, period);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "sv_npc_mind on a timer: sv_npc_mind_watch <seconds> [period_ms] -- the timeline of what they decided"); }
};

struct CCC_Bones : public IConsole_Command
{
    CCC_Bones(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args) { SVBones::Audit(args); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "bone id -> name table the server read out of a visual's .ogf"); }
};

// ---------------------------------------------------------------------
// Server-driven NPCs -- see sv_npc.h.
//
// A destination is either three numbers or the word "actor", which every
// one of these resolves the same way: whoever is playing. Typing
// coordinates is what you do when placing a guard; "actor" is what you do
// when you want to see the thing work.
// ---------------------------------------------------------------------
namespace
{
// Console-side cap on a patrol typed by hand. The module has its own; this one
// only bounds the parse loop.
constexpr size_t MAX_PATROL_STOPS_CMD = 16;

// The leading <id> every order takes, where "all" means every puppet.
// Returns 0xffff for "all" as well as for a parse failure, which is fine:
// with nothing tracked both do nothing, and with puppets tracked the only
// way to type something unparseable is to mean "all of them".
u16 npc_read_id(LPCSTR args)
{
    if (!args || !args[0] || strstr(args, "all"))
        return 0xffff;
    int id = -1;
    return (sscanf(args, "%d", &id) == 1 && id >= 0) ? u16(id) : u16(0xffff);
}

// Returns false and complains if the argument is neither.
bool npc_read_target(LPCSTR args, Fvector& out)
{
    if (!args || !args[0])
        return false;

    if (strstr(args, "actor"))
    {
        CSE_Abstract* e = SVWorld::Find(SVNpc::ReferenceEntity());
        if (!e)
        {
            Msg("! [sv_npc] no player entity to aim at");
            return false;
        }
        out = e->position();
        return true;
    }

    return sscanf(args, "%f %f %f", &out.x, &out.y, &out.z) == 3;
}

// The same leading argument, resolved to a LIST: an id, "all", or a community
// name. Returns false and complains when a name was meant and matched nobody.
//
// ⭐sv_npc_guard grew a faction argument of its own first, and it was the only
// thing that made a two-sided test writable at all. Everything that equips a
// creature needs the same reach for the same reason -- "give the AKs to Duty"
// is the normal thing to want, and "give them to everybody" is not it.
bool npc_read_who(LPCSTR args, xr_vector<u16>& out)
{
    out.clear();

    string128 who = {};
    const bool named = args && args[0] && sscanf(args, "%127s", who) == 1 && who[0] && !strstr(who, "all") && !(who[0] >= '0' && who[0] <= '9');

    if (named)
    {
        SVNpc::IdsOfFaction(who, out);
        if (out.empty())
        {
            Msg("! [sv_npc] no tracked puppet is in community '%s'", who);
            return false;
        }
        return true;
    }

    // ⚠Parsed from the FIRST TOKEN only, unlike npc_read_id, which searches the
    // whole line for "all". These commands are followed by section names, and
    // one of those containing the letters "all" would otherwise silently turn
    // "equip this one creature" into "equip every one of them".
    int id = -1;
    if (who[0] && sscanf(who, "%d", &id) == 1 && id >= 0)
        out.push_back(u16(id));
    else
        SVNpc::Ids(out); // "all", and an empty roster is the caller's problem
    return true;
}
} // namespace

// dbg_bones <id>: where THIS process has the bones a hit is judged on -- head,
// spine, pelvis, hands, feet -- in the body's own space, posed fresh. The same
// command on the server and on a client prints two numbers for one bone that
// should describe one body: the difference is how far a bullet can miss what
// the player saw it hit.
struct CCC_DbgBones : public IConsole_Command
{
    CCC_DbgBones(LPCSTR N) : IConsole_Command(N) {}
    virtual void Execute(LPCSTR args)
    {
        u32 id = 0;
        sscanf(args, "%u", &id);
        CObject* O = g_pGameLevel ? Level().Objects.net_Find(u16(id)) : nullptr;
        IKinematics* K = O ? smart_cast<IKinematics*>(O->Visual()) : nullptr;
        if (!K || !K->LL_BoneCount())
        {
            Msg("! [bones] id=%u has no skeleton in this process", id);
            return;
        }
        K->CalculateBones_Invalidate();
        K->CalculateBones(TRUE);

        static const char* names[] = {"bip01_head", "bip01_spine2", "bip01_pelvis", "bip01_l_hand", "bip01_r_hand", "bip01_l_foot", "bip01_r_foot"};
        // ...and the same bones in the modeller's bind pose, which is where a
        // server without animations kept them: the reference both are judged by.
        xr_vector<Fmatrix> bind;
        K->LL_GetBindTransform(bind);
        string1024 line{}, line_bind{};
        for (const char* n : names)
        {
            const u16 b = K->LL_BoneID(n);
            if (BI_NONE == b)
                continue;
            const Fvector& p = K->LL_GetTransform(b).c;
            string128 item;
            xr_sprintf(item, "%s=(%.2f %.2f %.2f) ", n + 6, p.x, p.y, p.z); // drop "bip01_"
            xr_strcat(line, item);
            if (b < bind.size())
            {
                xr_sprintf(item, "%s=(%.2f %.2f %.2f) ", n + 6, bind[b].c.x, bind[b].c.y, bind[b].c.z);
                xr_strcat(line_bind, item);
            }
        }
        Msg("~ [bones] id=%u %s t=%u | %s", id, Level().Server ? "server" : "client", Device.dwTimeGlobal, line);
        Msg("~ [bones-bind] id=%u | %s", id, line_bind);
    }
};

struct CCC_NpcSpawn : public IConsole_Command
{
    CCC_NpcSpawn(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        string128 section = {}, community = {};
        int count = 1;
        Fvector at;
        // ⭐The trailing point is what makes two sides possible. Without it every
        // group rings the same anchor, so "50 versus 50" spawns as one hundred
        // bodies inside twelve metres -- a pile, not a battle, and any hit rate
        // measured in it is measuring the pile.
        const int n = args && args[0] ? sscanf(args, "%127s %d %127s %f %f %f", section, &count, community, &at.x, &at.y, &at.z) : 0;

        if (n == 6)
        {
            xr_vector<u16> ignored;
            SVNpc::SpawnAt(section[0] ? section : nullptr, count, community[0] ? community : nullptr, at, ignored);
            return;
        }
        SVNpc::Spawn(section[0] ? section : nullptr, count, community[0] ? community : nullptr);
    }

    virtual void Info(TInfo& I)
    {
        xr_strcpy(I, "sv_npc_spawn [section] [count] [faction] [x y z] -- server-driven NPCs, around the player or around a given point");
    }
};

struct CCC_NpcList : public IConsole_Command
{
    CCC_NpcList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { SVNpc::List(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "list server-driven NPCs: where they are, what they were told to do, what they carry"); }
};

struct CCC_NpcDel : public IConsole_Command
{
    CCC_NpcDel(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        if (args && strstr(args, "all"))
        {
            SVNpc::Remove(0xffff, true);
            return;
        }
        int id = -1;
        if (!args || !args[0] || sscanf(args, "%d", &id) != 1)
        {
            Msg("! [sv_npc] sv_npc_del <id> | all");
            return;
        }
        SVNpc::Remove(u16(id), false);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "remove a server-driven NPC by id, or 'all'"); }
};

struct CCC_NpcTeleport : public IConsole_Command
{
    CCC_NpcTeleport(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        Fvector at;
        if (!args || !npc_read_target(strchr(args, ' '), at))
        {
            Msg("! [sv_npc] sv_npc_tp <id|all> <x y z | actor>");
            return;
        }
        SVNpc::Teleport(npc_read_id(args), at);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "put server-driven NPCs somewhere immediately: sv_npc_tp <id|all> <x y z | actor>"); }
};

// Zero the foreign-write counters and report what they held.
//
// ⚠Exists because an A/B is only worth running if both halves start from the
// same place. Without this the "after" half inherits the "before" half's
// counts -- or, worse, inherits an exhausted cap and reports a clean zero.
struct CCC_ForeignReset : public IConsole_Command
{
    CCC_ForeignReset(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR)
    {
        extern u32 g_net_foreign_yaw;
        extern u32 g_net_foreign_pos;
        Msg("~ [netupd] foreign writes so far: yaw=%u pos=%u -- counters reset", g_net_foreign_yaw, g_net_foreign_pos);
        g_net_foreign_yaw = 0;
        g_net_foreign_pos = 0;
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "report and zero the foreign-write counters (for A/B runs)"); }
};

// Who is nearby and what they are doing -- so a measurement can pick its
// subject instead of naming one. See ClBoneAudit::List.
struct CCC_Stalkers : public IConsole_Command
{
    CCC_Stalkers(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR) { ClBoneAudit::List(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "list alive stalkers nearby with distance and speed"); }
};

// How smooth a creature is ON SCREEN, in the same units for an offline NPC and
// for a networked puppet. See cl_bone_audit.h.
struct CCC_BoneAudit : public IConsole_Command
{
    CCC_BoneAudit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        if (!args || !args[0])
        {
            Msg("! [bone] cl_bone_audit <id>  -- record; cl_bone_audit 0 -- stop and report");
            return;
        }
        const int id = atoi(args);
        if (id <= 0)
            ClBoneAudit::Stop();
        else
            ClBoneAudit::Start(u16(id));
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "measure on-screen smoothness of a creature's bones: cl_bone_audit <id|0>"); }
};

// ⭐What the CONTENT says about its own locomotion, before anyone reasons
// about playback rates. Answers one question -- do the walk/run cycles carry
// root translation, or do they animate in place while the AI carries the body?
// -- and the answer decides whether the stride rate can be derived from the
// data or has to keep coming out of an ltx by hand. See cl_anim_blendspace.h.
//
// Works in single player: the actor's own skeleton carries the same norm_*
// set, so `cl_anim_rootmotion` with no argument measures it without a server.
struct CCC_RootMotion : public IConsole_Command
{
    CCC_RootMotion(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args) { ClAnim::DumpRootMotion(u16((args && args[0]) ? atoi(args) : 0)); }

    virtual void Info(TInfo& I) { xr_strcpy(I, "print measured root motion of the locomotion cycles: cl_anim_rootmotion [id] (0 = your own body)"); }
};

// Is the motion the SERVER asserts actually smooth? See sv_motion_audit.h for
// why that question went unasked for two sessions of work on the receiver.
struct CCC_MotionAudit : public IConsole_Command
{
    CCC_MotionAudit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        if (!args || !args[0])
        {
            Msg("! [motion] sv_motion_audit <id>  -- record; sv_motion_audit 0 -- stop and report");
            return;
        }
        const int id = atoi(args);
        if (id <= 0)
            SVMotionAudit::Stop();
        else
            SVMotionAudit::Start(u16(id));
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "record a server-driven creature's asserted and exported motion: sv_motion_audit <id|0>"); }
};

// Shots against hits, per shooter. Asked from play: "а они когда стреляли по
// мне вообще промахивались?" -- and nothing could answer it, because the two
// halves were counted in different places and neither was kept.
struct CCC_Accuracy : public IConsole_Command
{
    CCC_Accuracy(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        if (args && args[0] && 0 == xr_strcmp(args, "reset"))
        {
            SVAccuracy::Reset();
            Msg("~ [accuracy] cleared");
            return;
        }
        SVAccuracy::Dump();
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "shots fired against hits landed, per shooter: sv_accuracy [reset]"); }
};

// The tape of damage that passed through the server. Its job is to make
// "nothing reached the victim" a measurement rather than an inference -- the
// one answer that is also what a broken instrument returns.
struct CCC_HitTrail : public IConsole_Command
{
    CCC_HitTrail(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        int id = 0xffff;
        if (args && args[0])
            sscanf(args, "%d", &id);
        SVHitTrail::Dump(u16(id));
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "the last hits the server carried: sv_hit_trail [victim id]"); }
};

// Which KINDS of damage arrive, and from whom. The power cap only ever looks
// at bullets; before capping anything else, the census says what else there is
// and whether a network client can produce it at all.
struct CCC_HitTypes : public IConsole_Command
{
    CCC_HitTypes(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        if (args && args[0] && 0 == xr_strcmp(args, "reset"))
        {
            SVHitTrail::ResetTypes();
            return;
        }
        SVHitTrail::Types();
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "hit types the server carried, split by server-simulated vs claimed by a client: sv_hit_types [reset]"); }
};

// The request ledger's summary. The live lines catch the sequence; this catches
// the rare one -- a name that went missing twice in ten minutes is invisible in
// a per-frame stream and obvious in a column.
struct CCC_AnimAskDump : public IConsole_Command
{
    CCC_AnimAskDump(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR) { ClAnimLedger::Dump(); }

    virtual void Info(TInfo& I) { xr_strcpy(I, "print what was asked of the animator and what became of it, then reset: cl_anim_ask_dump"); }
};

// ⭐⭐Does the fast nearest-vertex search return the SAME vertex as the full
// scan it replaced? The replacement claims to be exact rather than approximate,
// and a claim like that is worth nothing unless something checks it against the
// original -- which is the only reason nearest_vertex_id_linear still exists.
//
// Three kinds of query point, because they exercise different paths: ON the
// mesh (the answer is the containing vertex), NEAR it (a few metres off, the
// case the server actually hits when an order names a spot beside a walkable
// one), and FAR outside the level box (where the widening search has to give up
// and cover everything).
struct CCC_NavmeshCheck : public IConsole_Command
{
    CCC_NavmeshCheck(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        if (!ai().get_level_graph())
        {
            Msg("! [navmesh] no level graph loaded");
            return;
        }
        const CLevelGraph& G = ai().level_graph();
        // ⚠Small by default, and the reason is not politeness. The ORACLE is
        // the old full scan -- 800 ms a call on this level -- and it runs
        // inline on the tick, so this command freezes the server for roughly
        // 0.8 s per point. A first attempt at 60 points froze it for minutes
        // and the run ended in a pre-existing Lua abort. Thirty points is a
        // sample; a thousand is an outage.
        const u32 n = (args && args[0]) ? u32(atoi(args)) : 30;
        if (!n || !G.header().vertex_count())
            return;

        const Fbox& box = G.header().box();
        Msg("~ [navmesh] %u vertices, box (%.0f %.0f %.0f)-(%.0f %.0f %.0f), cell %.2f -- checking %u points", G.header().vertex_count(), VPUSH(box.min), VPUSH(box.max),
            G.header().cell_size(), n);

        u32 mismatch = 0;
        // ⚠Per kind, not pooled. A pooled average is dominated by the
        // far-outside points, which cost a full scan by construction, and would
        // report "3x faster" for a function that is thousands of times faster
        // in the only case the server actually hits.
        u64 us_fast[4] = {}, us_slow[4] = {};
        u32 n_kind[4] = {};
        float worst_extra = 0.f;

        for (u32 i = 0; i < n; ++i)
        {
            Fvector p;
            const u32 kind = i % 4;
            if (kind == 3)
            {
                // Just past the edge with mesh right there: an anomaly on the
                // level's border scattering a point over it (Cordon's electric
                // fences, z -480 against a box ending at -480.9). Pushed out
                // across whichever side of the box the vertex is nearest.
                // ⚠Only a vertex already near a side: a random one is usually
                // hundreds of metres in, and dragging it out to the border
                // would be the "far outside" row again.
                u32 side = 0;
                bool found = false;
                for (u32 tries = 0; tries < 4096 && !found; ++tries)
                {
                    p = G.vertex_position(Random.randI(G.header().vertex_count()));
                    const float d[4] = {p.x - box.min.x, box.max.x - p.x, p.z - box.min.z, box.max.z - p.z};
                    side = 0;
                    for (u32 j = 1; j < 4; ++j)
                        if (d[j] < d[side])
                            side = j;
                    found = d[side] < 10.f;
                }
                if (!found)
                    continue;
                const float out = Random.randF(1.f, 20.f);
                if (side == 0)
                    p.x = box.min.x - out;
                else if (side == 1)
                    p.x = box.max.x + out;
                else if (side == 2)
                    p.z = box.min.z - out;
                else
                    p.z = box.max.z + out;
            }
            else if (kind == 2)
            {
                // far outside: the widening search must fall back to everything
                p.set(box.max.x + Random.randF(50.f, 300.f), box.max.y, box.max.z + Random.randF(50.f, 300.f));
            }
            else if (kind == 0)
            {
                p = G.vertex_position(Random.randI(G.header().vertex_count()));
            }
            else
            {
                // ⚠⚠GENUINELY off the mesh, verified, not merely offset.
                //
                // The first version just moved a vertex by up to 25 m and
                // called that "beside the mesh". On a densely meshed level a
                // point 25 m from one vertex is almost always sitting on
                // another, so this row measured the ON-mesh path a second time
                // and reported it as the off-mesh case -- while the real thing
                // (an order naming a spot 21 m off the walkable area) was
                // stalling the server for half a second in the very next run.
                p.set(0.f, 0.f, 0.f);
                bool found = false;
                for (u32 tries = 0; tries < 64 && !found; ++tries)
                {
                    Fvector c = G.vertex_position(Random.randI(G.header().vertex_count()));
                    c.add(Fvector().set(Random.randF(-30.f, 30.f), Random.randF(-3.f, 3.f), Random.randF(-30.f, 30.f)));
                    if (!G.valid_vertex_id(G.vertex_id(c)))
                    {
                        p = c;
                        found = true;
                    }
                }
                if (!found)
                    continue; // this level has no reachable off-mesh spot nearby; do not fake one
            }

            const u64 t0 = CPU::QPC_us();
            const u32 fast = G.nearest_vertex_id(p);
            const u64 t1 = CPU::QPC_us();
            const u32 slow = G.nearest_vertex_id_linear(p);
            const u64 t2 = CPU::QPC_us();
            us_fast[kind] += t1 - t0;
            us_slow[kind] += t2 - t1;
            ++n_kind[kind];

            if (fast == slow)
                continue;

            // ⚠Two vertices can be exactly equidistant, and then "which one" is
            // an arbitrary tie the two loops may break differently. Only a
            // genuinely FARTHER answer is a defect, so compare the distances,
            // not the ids.
            const float d_fast = G.distance(p, fast);
            const float d_slow = G.distance(p, slow);
            if (d_fast <= d_slow + EPS_L)
                continue;

            ++mismatch;
            worst_extra = _max(worst_extra, _sqrt(d_fast) - _sqrt(d_slow));
            if (mismatch <= 5)
                Msg("! [navmesh] (%.1f %.1f %.1f): fast->%u at %.3f m, linear->%u at %.3f m", VPUSH(p), fast, _sqrt(d_fast), slow, _sqrt(d_slow));
        }

        Msg("~ [navmesh] *** %u/%u WORSE than the full scan (worst by %.3f m)", mismatch, n, worst_extra);
        static const char* k_names[4] = {"on the mesh", "OFF it (verified)", "outside the level", "past the edge"};
        for (u32 k = 0; k < 4; ++k)
        {
            if (!n_kind[k])
                continue;
            const float fast_ms = float(us_fast[k]) / float(n_kind[k]) / 1000.f;
            const float slow_ms = float(us_slow[k]) / float(n_kind[k]) / 1000.f;
            Msg("~ [navmesh] *** %-18s n=%2u | fast %8.3f ms | full scan %8.3f ms | %.0fx", k_names[k], n_kind[k], fast_ms, slow_ms, (fast_ms > 0.f) ? (slow_ms / fast_ms) : 0.f);
        }
        Msg("~ [navmesh] the middle row is the server's real case, and it is the one a careless generator turns back into the first");
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "check the fast nearest-vertex search against the full scan: sv_navmesh_check [points]"); }
};

struct CCC_NpcMove : public IConsole_Command
{
    CCC_NpcMove(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        Fvector at;
        if (!args || !npc_read_target(strchr(args, ' '), at))
        {
            Msg("! [sv_npc] sv_npc_move <id|all> <x y z | actor>");
            return;
        }
        SVNpc::OrderGoto(npc_read_id(args), at);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "walk server-driven NPCs to a point: sv_npc_move <id|all> <x y z | actor>"); }
};

struct CCC_NpcFollow : public IConsole_Command
{
    CCC_NpcFollow(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        int target = -1;
        if (args && args[0])
        {
            LPCSTR rest = strchr(args, ' ');
            if (rest)
                sscanf(rest, "%d", &target);
        }
        SVNpc::OrderFollow(npc_read_id(args), (target < 0) ? u16(0xffff) : u16(target));
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "have server-driven NPCs keep walking towards an entity: sv_npc_follow <id|all> [entity] (default: the player)"); }
};

struct CCC_NpcPatrol : public IConsole_Command
{
    CCC_NpcPatrol(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        LPCSTR rest = args ? strchr(args, ' ') : nullptr;
        if (!rest)
        {
            Msg("! [sv_npc] sv_npc_patrol <id|all> <x y z> <x y z> [...] [dwell=<seconds>]");
            return;
        }

        // Optional trailing "dwell=N", so the common case stays a bare list of
        // coordinates rather than a positional argument nobody remembers.
        float dwell = 4.f;
        if (LPCSTR d = strstr(rest, "dwell="))
            sscanf(d + 6, "%f", &dwell);

        xr_vector<Fvector> stops;
        LPCSTR p = rest;
        while (stops.size() < MAX_PATROL_STOPS_CMD)
        {
            Fvector v;
            int consumed = 0;
            if (sscanf(p, "%f %f %f%n", &v.x, &v.y, &v.z, &consumed) != 3)
                break;
            stops.push_back(v);
            p += consumed;
        }

        if (stops.empty())
        {
            Msg("! [sv_npc] sv_npc_patrol <id|all> <x y z> <x y z> [...] [dwell=<seconds>]");
            return;
        }

        SVNpc::OrderPatrol(npc_read_id(args), stops, dwell);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "walk server-driven NPCs round a route for ever: sv_npc_patrol <id|all> <x y z> <x y z> [...] [dwell=<seconds>]"); }
};

struct CCC_NpcGuard : public IConsole_Command
{
    CCC_NpcGuard(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        Fvector at;
        LPCSTR rest = args ? strchr(args, ' ') : nullptr;
        if (!rest || !npc_read_target(rest, at))
        {
            Msg("! [sv_npc] sv_npc_guard <id|all> <x y z | actor> [leash]");
            return;
        }

        // A fourth number, if there is one, is the leash. "actor" never has one.
        float leash = 0.f;
        Fvector ignored;
        int consumed = 0;
        if (sscanf(rest, "%f %f %f%n", &ignored.x, &ignored.y, &ignored.z, &consumed) == 3)
            sscanf(rest + consumed, "%f", &leash);

        // ⭐A community name where an id would go posts that SIDE. Without it two
        // factions cannot be given two different points, and a test with two
        // sides is exactly what this order exists to set up.
        string128 who = {};
        if (sscanf(args, "%127s", who) == 1 && who[0] && !strstr(who, "all") && !(who[0] >= '0' && who[0] <= '9'))
        {
            xr_vector<u16> side;
            SVNpc::IdsOfFaction(who, side);
            if (side.empty())
            {
                Msg("! [sv_npc] no tracked puppet is in community '%s'", who);
                return;
            }
            Msg("~ [sv_npc] posting %u of '%s' at (%.1f, %.1f, %.1f), leash %.0f m", u32(side.size()), who, VPUSH(at), leash);
            for (u16 id : side)
                SVNpc::OrderGuard(id, at, leash);
            return;
        }

        SVNpc::OrderGuard(npc_read_id(args), at, leash);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "post server-driven NPCs at a point, free to act within a leash: sv_npc_guard <id|all|faction> <x y z | actor> [leash]"); }
};

struct CCC_NpcStop : public IConsole_Command
{
    CCC_NpcStop(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args) { SVNpc::OrderHold(npc_read_id(args)); }

    virtual void Info(TInfo& I) { xr_strcpy(I, "cancel a server-driven NPC's order: sv_npc_stop <id|all>"); }
};

struct CCC_NpcFace : public IConsole_Command
{
    CCC_NpcFace(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        Fvector at;
        LPCSTR rest = args ? strchr(args, ' ') : nullptr;
        if (!rest || !npc_read_target(rest, at))
        {
            Msg("! [sv_npc] sv_npc_face <id|all|faction> <x y z | actor>");
            return;
        }

        xr_vector<u16> who;
        if (!npc_read_who(args, who))
            return;
        for (u16 id : who)
            SVNpc::OrderFace(id, at);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "point server-driven NPCs at something: sv_npc_face <id|all|faction> <x y z | actor>"); }
};

struct CCC_NpcGive : public IConsole_Command
{
    CCC_NpcGive(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        string128 section = {};
        int count = 1;
        LPCSTR rest = args ? strchr(args, ' ') : nullptr;
        if (!rest || sscanf(rest, "%127s %d", section, &count) < 1)
        {
            Msg("! [sv_npc] sv_npc_give <id|all|faction> <section> [count]");
            return;
        }
        clamp(count, 1, 64);

        xr_vector<u16> who;
        if (!npc_read_who(args, who))
            return;

        for (u16 id : who)
            for (int i = 0; i < count; ++i)
                SVNpc::Give(id, section, count == 1);

        if (count > 1)
            Msg("~ [sv_npc] gave %u creatures %d x '%s'", u32(who.size()), count, section);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "give server-driven NPCs an item: sv_npc_give <id|all|faction> <section> [count]"); }
};

struct CCC_NpcGear : public IConsole_Command
{
    CCC_NpcGear(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        xr_vector<u16> who;
        if (!npc_read_who(args, who))
            return;
        SVGear::Census(who);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "what server-driven NPCs carry, hold and wear: sv_npc_gear [id|all|faction]"); }
};

// ---------------------------------------------------------------------
// Items on the ground, and who ends up owning them -- see sv_loot.h.
// ---------------------------------------------------------------------
struct CCC_ItemDrop : public IConsole_Command
{
    CCC_ItemDrop(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        string128 section = {};
        int count = 1;
        if (!args || sscanf(args, "%127s %d", section, &count) < 1)
        {
            Msg("! [loot] sv_item_drop <section> [count] [x y z | actor | <id>] [spread]");
            return;
        }

        // Where: three numbers, "actor", a creature's id, or nothing at all --
        // which means "at the player", the same default every other order here
        // takes.
        Fvector at;
        LPCSTR rest = args ? strchr(args, ' ') : nullptr;
        bool placed = false;
        if (rest)
        {
            LPCSTR p = rest;
            // skip the count, if one was given
            int ignored = 0, consumed = 0;
            if (sscanf(p, " %d%n", &ignored, &consumed) == 1)
                p += consumed;
            placed = npc_read_target(p, at);
            if (!placed)
            {
                // a bare id: drop it at that creature's feet
                int id = -1;
                if (sscanf(p, " %d", &id) == 1 && id >= 0)
                    if (CSE_Abstract* e = SVWorld::Find(u16(id)))
                    {
                        at = e->position();
                        placed = true;
                    }
            }
        }
        if (!placed)
        {
            CSE_Abstract* e = SVWorld::Find(SVNpc::ReferenceEntity());
            if (!e)
            {
                Msg("! [loot] nowhere to drop it -- no player entity exists yet");
                return;
            }
            at = e->position();
        }

        float spread = (count > 1) ? 1.5f : 0.f;
        if (LPCSTR tail = args ? strstr(args, "spread=") : nullptr)
            sscanf(tail + 7, "%f", &spread);

        SVLoot::Drop(section, count, at, spread);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "put items on the ground: sv_item_drop <section> [count] [x y z | actor | <id>] [spread=<m>]"); }
};

struct CCC_ItemGive : public IConsole_Command
{
    CCC_ItemGive(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        string128 who = {}, section = {};
        int count = 1;
        if (!args || sscanf(args, "%127s %127s %d", who, section, &count) < 2)
        {
            Msg("! [loot] sv_item_give <id|actor> <section> [count]");
            return;
        }

        u16 id = 0xffff;
        if (strstr(who, "actor"))
            id = SVNpc::ReferenceEntity();
        else
        {
            int parsed = -1;
            if (sscanf(who, "%d", &parsed) == 1 && parsed >= 0)
                id = u16(parsed);
        }

        if (id == 0xffff)
        {
            Msg("! [loot] no such recipient '%s'", who);
            return;
        }
        SVLoot::GiveTo(id, section, count);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "put an item straight into somebody's inventory: sv_item_give <id|actor> <section> [count]"); }
};

struct CCC_ItemList : public IConsole_Command
{
    CCC_ItemList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR) { SVLoot::List(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "what we dropped and whether it is still lying there: sv_item_list"); }
};

struct CCC_ItemReport : public IConsole_Command
{
    CCC_ItemReport(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        if (args && strstr(args, "reset"))
        {
            SVLoot::Reset();
            return;
        }
        SVLoot::Report();
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "every item that changed hands, and from how far: sv_item_report [reset]"); }
};

struct CCC_NpcTemplates : public IConsole_Command
{
    CCC_NpcTemplates(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args) { SVGear::Templates(args); }

    virtual void Info(TInfo& I) { xr_strcpy(I, "how tough each creature template is: sv_npc_templates [substring]"); }
};

struct CCC_Revive : public IConsole_Command
{
    CCC_Revive(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        string128 who = {};
        float health = 1.f;
        if (!args || !args[0] || sscanf(args, "%127s %f", who, &health) < 1)
        {
            Msg("! [revive] sv_revive <id|actor|all|faction> [health 0..1]");
            return;
        }

        xr_vector<u16> ids;
        if (strstr(who, "actor"))
        {
            const u16 ref = SVNpc::ReferenceEntity();
            if (0xffff == ref)
            {
                Msg("! [revive] no player entity exists yet");
                return;
            }
            ids.push_back(ref);
        }
        else if (who[0] >= '0' && who[0] <= '9')
            ids.push_back(u16(atoi(who)));
        else if (0 == xr_strcmp(who, "all"))
            SVNpc::Ids(ids);
        else
            SVNpc::IdsOfFaction(who, ids);

        if (ids.empty())
        {
            Msg("! [revive] nothing matches '%s'", who);
            return;
        }
        SVRevive::Revive(ids, health);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "put a dead body back on its feet: sv_revive <id|actor|all|faction> [health 0..1]"); }
};

struct CCC_NpcModels : public IConsole_Command
{
    CCC_NpcModels(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args) { SVVisualArmour::Catalogue(args); }

    virtual void Info(TInfo& I) { xr_strcpy(I, "character models and the armour each one carries: sv_npc_models [filter]"); }
};

struct CCC_NpcArmour : public IConsole_Command
{
    CCC_NpcArmour(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args) { SVVisualArmour::Audit(args); }

    virtual void Info(TInfo& I) { xr_strcpy(I, "what protection the server reads from a model: sv_npc_armour [visual]"); }
};

struct CCC_NpcVisual : public IConsole_Command
{
    CCC_NpcVisual(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        // ⚠Who first, model second -- and the model is read from the REST of the
        // line, not with %s: model paths carry backslashes but no spaces, while
        // "all" and a faction name are single tokens, so splitting on the first
        // gap is the only unambiguous reading.
        string128 who = {};
        string_path model = {};
        if (!args || !args[0] || sscanf(args, "%127s %255s", who, model) != 2 || !model[0])
        {
            Msg("! [sv_npc] sv_npc_visual <id|all|faction> <model>");
            Msg("- [sv_npc] e.g. sv_npc_visual all actors\\dolg\\stalker_do_exoskeleton");
            return;
        }

        xr_vector<u16> ids;
        if (!npc_read_who(args, ids))
            return;

        SVNpc::SetVisual(ids, model);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "dress creatures in a model -- which IS their armour: sv_npc_visual <id|all|faction> <model>"); }
};

// sv_orphans [quiet] -- objects that outlived their own server entity.
//
// The census that would have named the 05-08-2026 fault storm in one line
// instead of three days. See SVWorld::AuditOrphans.
struct CCC_Orphans : public IConsole_Command
{
    CCC_Orphans(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args) { SVWorld::AuditOrphans(!(args && strstr(args, "quiet"))); }

    virtual void Info(TInfo& I) { xr_strcpy(I, "level objects whose server entity is gone -- they keep thinking and cannot die: sv_orphans [quiet]"); }
};

// sv_orphan_make <id|all|faction> -- produce the broken state on demand.
// ⚠Breaks whatever it is pointed at, on purpose. See SVWorld::OrphanEntity.
struct CCC_OrphanMake : public IConsole_Command
{
    CCC_OrphanMake(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        xr_vector<u16> who;
        if (!npc_read_who(args, who))
            return;
        u32 n = 0;
        for (u16 id : who)
            n += SVWorld::OrphanEntity(id);
        Msg("!! [orphan] stranded %u of %u -- the world is now deliberately broken", n, u32(who.size()));
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "TEST: strand a creature -- entity gone, object still running: sv_orphan_make <id|all|faction>"); }
};

struct CCC_NpcStrip : public IConsole_Command
{
    CCC_NpcStrip(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        xr_vector<u16> who;
        if (!npc_read_who(args, who))
            return;
        SVGear::Strip(who, args && strstr(args, "bare"));
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "take everything a server-driven NPC carries: sv_npc_strip <id|all|faction> [bare]"); }
};

// ⭐The order a test wants, as opposed to the two it would otherwise have to
// compose: strip, then give exactly this. A loadout that is ADDED to whatever
// the character profile decided is not a loadout, and every skirmish run so far
// has been measuring one of those.
struct CCC_NpcLoadout : public IConsole_Command
{
    CCC_NpcLoadout(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        LPCSTR rest = args ? strchr(args, ' ') : nullptr;
        if (!rest)
        {
            Msg("! [sv_npc] sv_npc_loadout <id|all|faction> <section>[*count] [section[*count] ...]");
            return;
        }

        xr_vector<u16> who;
        if (!npc_read_who(args, who))
            return;
        if (who.empty())
        {
            Msg("! [sv_npc] nobody to equip");
            return;
        }

        // Parse the whole list BEFORE anything is destroyed: a typo halfway
        // through would otherwise leave ten creatures naked in a firefight.
        struct Entry
        {
            string128 section;
            int count;
        };
        xr_vector<Entry> list;
        LPCSTR p = rest;
        while (p && *p)
        {
            string128 token = {};
            int consumed = 0;
            if (sscanf(p, "%127s%n", token, &consumed) != 1)
                break;
            p += consumed;

            Entry e = {};
            int n = 1;
            if (char* star = strchr(token, '*'))
            {
                *star = 0;
                n = atoi(star + 1);
            }
            clamp(n, 1, 64);
            xr_strcpy(e.section, token);
            e.count = n;

            if (!pSettings->section_exist(e.section))
            {
                Msg("! [sv_npc] no such section '%s' -- nothing was taken away", e.section);
                return;
            }
            list.push_back(e);
        }

        if (list.empty())
        {
            Msg("! [sv_npc] sv_npc_loadout <id|all|faction> <section>[*count] [...]");
            return;
        }

        SVGear::Strip(who, false);

        u32 given = 0;
        for (u16 id : who)
            for (const Entry& e : list)
                for (int i = 0; i < e.count; ++i)
                {
                    SVNpc::Give(id, e.section, false);
                    ++given;
                }

        string1024 what = {};
        for (const Entry& e : list)
        {
            string128 piece;
            if (e.count > 1)
                xr_sprintf(piece, "%s*%d", e.section, e.count);
            else
                xr_strcpy(piece, e.section);
            if (what[0])
                xr_strcat(what, sizeof(what), " ");
            xr_strcat(what, sizeof(what), piece);
        }
        Msg("~ [sv_npc] loadout on %u creatures: %s (%u items)", u32(who.size()), what, given);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "strip and equip exactly this: sv_npc_loadout <id|all|faction> <section>[*count] [...]"); }
};

struct CCC_NpcSpeed : public IConsole_Command
{
    CCC_NpcSpeed(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        float mps = 0.f;
        LPCSTR rest = args ? strchr(args, ' ') : nullptr;
        if (!rest || sscanf(rest, "%f", &mps) != 1)
        {
            Msg("! [sv_npc] sv_npc_speed <id|all> <m/s>");
            return;
        }
        SVNpc::SetSpeed(npc_read_id(args), mps);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "how fast server-driven NPCs walk, in m/s (over ~2 the client animates it as a run)"); }
};

// ---------------------------------------------------------------------
// The level as the server means it to be played -- see sv_scenario.h.
// ---------------------------------------------------------------------
struct CCC_Fields : public IConsole_Command
{
    CCC_Fields(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        int count = 0;
        if (args && args[0])
            sscanf(args, "%d", &count);
        SVScenario::Fields(u32(count < 0 ? 0 : count));
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "the anomalous fields this level actually contains, strongest first: sv_fields [count]"); }
};

struct CCC_Scenario : public IConsole_Command
{
    CCC_Scenario(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        int fields = 0, guards = 0;
        string128 section = {};
        if (args && args[0])
            sscanf(args, "%d %d %127s", &fields, &guards, section);
        SVScenario::Apply(u32(fields < 0 ? 0 : fields), u32(guards < 0 ? 0 : guards), section[0] ? section : nullptr);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "post faction guards on the level's strongest anomalous fields: sv_scenario [fields] [guards] [section]"); }
};

struct CCC_ScenarioStatus : public IConsole_Command
{
    CCC_ScenarioStatus(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { SVScenario::Status(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "which fields are held, by whom, and how many of each detail is still alive"); }
};

struct CCC_ScenarioClear : public IConsole_Command
{
    CCC_ScenarioClear(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { SVScenario::Clear(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "remove everything the scenario placed (hand-spawned puppets are left alone)"); }
};

// ---------------------------------------------------------------------
// Anomalies the server puts on the map -- see sv_anomaly.h.
//
// Every one of these is reachable over rcon by an administrator, so the
// spot an anomaly lands on is wherever the operator was standing when
// they typed it. That is the point: the alternative is reading three
// numbers off a debug overlay and typing them back in.
// ---------------------------------------------------------------------
struct CCC_AnomalyKinds : public IConsole_Command
{
    CCC_AnomalyKinds(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args) { SVAnomaly::Kinds(args); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "which sections of this content are anomalies: sv_anomaly_kinds [substring]"); }
};

struct CCC_AnomalyList : public IConsole_Command
{
    CCC_AnomalyList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        float radius = 0.f;
        if (args && args[0])
            sscanf(args, "%f", &radius);
        SVAnomaly::List(radius);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "anomalies near you, with their actual reach: sv_anomaly_list [radius]"); }
};

struct CCC_AnomalySpawn : public IConsole_Command
{
    CCC_AnomalySpawn(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        string128 section = {};
        float radius = 0.f;
        Fvector at;

        // Three shapes, and the parse tells them apart by how many numbers
        // followed the section: none, a radius, or a radius and a point. The
        // no-point form is the one an operator standing in the world uses.
        const int n = (args && args[0]) ? sscanf(args, "%127s %f %f %f %f", section, &radius, &at.x, &at.y, &at.z) : 0;
        if (n < 1)
        {
            Msg("! [sv_anomaly] sv_anomaly_spawn <section> [radius] [x y z] -- sv_anomaly_kinds lists the sections");
            return;
        }

        SVAnomaly::Spawn(section, radius, (n >= 5) ? &at : nullptr);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "place an anomaly where you stand: sv_anomaly_spawn <section> [radius] [x y z]"); }
};

struct CCC_AnomalySize : public IConsole_Command
{
    CCC_AnomalySize(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        LPCSTR rest = args ? strchr(args, ' ') : nullptr;
        float radius = 0.f;
        if (!rest || sscanf(rest, "%f", &radius) != 1)
        {
            Msg("! [sv_anomaly] sv_anomaly_size <id|mine> <radius>");
            return;
        }

        if (strstr(args, "mine"))
        {
            SVAnomaly::Resize(0xffff, true, radius);
            return;
        }

        // "last" is the one just placed -- see SVAnomaly::LastPlaced for why an
        // operator should not have to read an id out of the log to continue.
        if (strstr(args, "last"))
        {
            const u16 last = SVAnomaly::LastPlaced();
            if (last == 0xffff)
            {
                Msg("! [sv_anomaly] nothing has been placed yet -- 'last' means nothing");
                return;
            }
            SVAnomaly::Resize(last, false, radius);
            return;
        }

        int id = -1;
        if (sscanf(args, "%d", &id) != 1 || id < 0)
        {
            Msg("! [sv_anomaly] sv_anomaly_size <id|last|mine> <radius>");
            return;
        }
        SVAnomaly::Resize(u16(id), false, radius);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "change how far an anomaly reaches, live: sv_anomaly_size <id|last|mine> <radius>"); }
};

struct CCC_AnomalyArt : public IConsole_Command
{
    CCC_AnomalyArt(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        string128 who = {};
        string128 section = {};
        int count = 1;

        // The section is optional and the count is optional after it, so what
        // came back from the scan is what decides which form was typed.
        const int n = (args && args[0]) ? sscanf(args, "%127s %127s %d", who, section, &count) : 0;
        if (n < 1)
        {
            Msg("! [sv_anomaly] sv_anomaly_art <zone id|last> [section] [count] -- no section means the zone's own table");
            return;
        }

        u16 id = 0xffff;
        if (strstr(who, "last"))
        {
            id = SVAnomaly::LastPlaced();
            if (id == 0xffff)
            {
                Msg("! [sv_anomaly] nothing has been placed yet -- 'last' means nothing");
                return;
            }
        }
        else
        {
            int parsed = -1;
            if (sscanf(who, "%d", &parsed) != 1 || parsed < 0)
            {
                Msg("! [sv_anomaly] sv_anomaly_art <zone id|last> [section] [count]");
                return;
            }
            id = u16(parsed);
        }

        SVAnomaly::SpawnArtefacts(id, (n >= 2) ? section : nullptr, (n >= 3) ? count : 1);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "put artefacts inside an anomaly now: sv_anomaly_art <zone id|last> [section] [count]"); }
};

struct CCC_AnomalyDel : public IConsole_Command
{
    CCC_AnomalyDel(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        // "mine" rather than "all", and the word is doing real work: this
        // command can delete a zone the LEVEL shipped when one is named by id,
        // so a blanket "all" would be one keystroke away from stripping the map
        // of its own anomalies with no way back.
        if (args && strstr(args, "mine"))
        {
            SVAnomaly::Remove(0xffff, true);
            return;
        }

        int id = -1;
        if (!args || !args[0] || sscanf(args, "%d", &id) != 1 || id < 0)
        {
            Msg("! [sv_anomaly] sv_anomaly_del <id> | mine");
            return;
        }
        SVAnomaly::Remove(u16(id), false);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "remove an anomaly by id, or 'mine' for every one the server placed"); }
};

// Saving happens by itself after every change; these two exist for the cases
// where an operator edited the file by hand, or wants to see where it is.
struct CCC_AnomalySave : public IConsole_Command
{
    CCC_AnomalySave(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { SVAnomaly::Save(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "write the placed anomalies down now (they are saved after every change anyway)"); }
};

struct CCC_AnomalyLoad : public IConsole_Command
{
    CCC_AnomalyLoad(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args) { SVAnomaly::Load(args && strstr(args, "force")); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "place the anomalies saved for this level: sv_anomaly_load [force]"); }
};

// Publish the placed anomalies into gamedata. See SVAnomaly::Bake.
struct CCC_AnomalyBake : public IConsole_Command
{
    CCC_AnomalyBake(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { SVAnomaly::Bake(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "bake the placed anomalies into gamedata so every server places them"); }
};

struct CCC_AnomalyBirthStatus : public IConsole_Command
{
    CCC_AnomalyBirthStatus(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { SVAnomaly::BirthStatus(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "which fields are producing artefacts, what they hold and when they go again"); }
};

// ---------------------------------------------------------------------
// The furniture the server puts on the map -- see sv_prop.h.
//
// Same shape as the anomaly verbs above and reachable the same way, so
// the spot a crate lands on is wherever the operator was standing when
// they typed it.
//
// ⚠bLowerCaseArgs stays TRUE throughout: model paths and section names
// are lower case in the archives, and set_visual lower-cases anyway.
// ---------------------------------------------------------------------

// ⭐Shared by every verb that names one object: <id>, or 'last' for the
// one just placed, or 'mine' for everything the server placed.
//
// "last" is what makes the toolkit usable in a sentence -- "put one here,
// now turn it, now nudge it up" is three commands about the same thing,
// and making an operator read an id out of the log between each of them
// is the difference between a tool and a procedure.
static bool prop_target(LPCSTR args, u16& id, bool& all, const char* usage)
{
    id = 0xffff;
    all = false;

    if (!args || !args[0])
    {
        Msg("! [sv_prop] %s", usage);
        return false;
    }

    if (strstr(args, "mine"))
    {
        all = true;
        return true;
    }

    if (strstr(args, "last"))
    {
        id = SVProp::LastPlaced();
        if (id == 0xffff)
        {
            Msg("! [sv_prop] nothing has been placed yet -- 'last' means nothing");
            return false;
        }
        return true;
    }

    int n = -1;
    if (sscanf(args, "%d", &n) != 1 || n < 0)
    {
        Msg("! [sv_prop] %s", usage);
        return false;
    }
    id = u16(n);
    return true;
}

struct CCC_PropModels : public IConsole_Command
{
    CCC_PropModels(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args) { SVProp::Models(args); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "which models this install has: sv_prop_models [substring]"); }
};

struct CCC_PropKinds : public IConsole_Command
{
    CCC_PropKinds(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args) { SVProp::Kinds(args); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "prop sections, and what this level is actually built out of: sv_prop_kinds [substring]"); }
};

struct CCC_PropList : public IConsole_Command
{
    CCC_PropList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        float radius = 0.f;
        if (args && args[0])
            sscanf(args, "%f", &radius);
        SVProp::List(radius);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "props and lamps near you: sv_prop_list [radius]"); }
};

// Three shapes, and the parse tells them apart by how many numbers followed the
// model: none, a yaw, or a yaw and a point. The no-point form is the one an
// operator standing in the world uses.
struct CCC_PropSpawn : public IConsole_Command
{
    CCC_PropSpawn(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        string_path what = {};
        float yaw = 0.f;
        Fvector at;
        Fvector tilt{};

        // ⭐The tilt goes on the END, after the position, and both are optional.
        // Typed by a human this is still "sv_prop_spawn box 90"; sent by the
        // client editor it carries the slope it measured under the model. A
        // trailing pair cannot change the meaning of a line that omits it.
        const int n = (args && args[0]) ? sscanf(args, "%259s %f %f %f %f %f %f", what, &yaw, &at.x, &at.y, &at.z, &tilt.x, &tilt.z) : 0;
        if (n < 1)
        {
            Msg("! [sv_prop] sv_prop_spawn <model|section> [yaw] [x y z] [pitch bank] -- sv_prop_models lists the models");
            return;
        }

        SVProp::Spawn(what, yaw, (n >= 5) ? &at : nullptr, (n >= 7) ? &tilt : nullptr);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "place a prop where you stand: sv_prop_spawn <model|section> [yaw] [x y z] [pitch bank]"); }
};

struct CCC_PropLamp : public IConsole_Command
{
    CCC_PropLamp(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        string_path what = {};
        float yaw = 0.f;
        Fvector at;

        Fvector tilt{};
        SVProp::Light light;
        float cone = 0.f, range = 0.f;
        int shadow = 1, tone = 1;

        const int n = (args && args[0]) ? sscanf(args, "%259s %f %f %f %f %f %f %f %f %d %d", what, &yaw, &at.x, &at.y, &at.z, &tilt.x, &tilt.z, &range, &cone, &shadow, &tone) : 0;
        if (n < 1)
        {
            Msg("! [sv_prop] sv_prop_lamp <model|section> [yaw] [x y z] [pitch bank] [range] [cone] [shadow] [tone]");
            Msg("- [sv_prop]   cone 0 = a point light, >0 = a spot of that many degrees; tone 0 flame / 1 warm / 2 cold");
            return;
        }

        light.range = (n >= 8) ? range : 0.f;
        light.cone = (n >= 9) ? cone : 0.f;
        light.shadow = (n >= 10) ? (shadow != 0) : true;
        light.tone = (n >= 11) ? tone : 1;

        SVProp::Lamp(what, yaw, (n >= 5) ? &at : nullptr, (n >= 7) ? &tilt : nullptr, &light);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "place a light: sv_prop_lamp <model|section> [yaw] [x y z] [pitch bank] [range] [cone] [shadow] [tone]"); }
};

struct CCC_PropCampfire : public IConsole_Command
{
    CCC_PropCampfire(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        float yaw = 0.f;
        Fvector at;
        Fvector tilt{};
        const int n = (args && args[0]) ? sscanf(args, "%f %f %f %f %f %f", &yaw, &at.x, &at.y, &at.z, &tilt.x, &tilt.z) : 0;
        SVProp::Campfire(yaw, (n >= 4) ? &at : nullptr, (n >= 6) ? &tilt : nullptr);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "logs and a fire, as one act: sv_prop_campfire [yaw] [x y z] [pitch bank]"); }
};

struct CCC_PropClone : public IConsole_Command
{
    CCC_PropClone(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        int id = -1;
        Fvector at;
        const int n = (args && args[0]) ? sscanf(args, "%d %f %f %f", &id, &at.x, &at.y, &at.z) : 0;
        if (n < 1 || id < 0)
        {
            Msg("! [sv_prop] sv_prop_clone <id> [x y z] -- sv_prop_kinds gives you ids worth cloning");
            return;
        }
        SVProp::Clone(u16(id), (n >= 4) ? &at : nullptr);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "another one of that, here: sv_prop_clone <id> [x y z]"); }
};

struct CCC_PropMove : public IConsole_Command
{
    CCC_PropMove(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        u16 id;
        bool all;
        if (!prop_target(args, id, all, "sv_prop_move <id|last> <x y z>"))
            return;

        // The point comes after the target word, whatever the target word was.
        LPCSTR rest = args ? strchr(args, ' ') : nullptr;
        Fvector at;
        float yaw = 0.f;
        Fvector tilt{};
        const int n = rest ? sscanf(rest, "%f %f %f %f %f %f", &at.x, &at.y, &at.z, &yaw, &tilt.x, &tilt.z) : 0;
        if (n < 3)
        {
            Msg("! [sv_prop] sv_prop_move <id|last> <x y z> [yaw pitch bank]");
            return;
        }
        // ⭐The pose comes as a whole or not at all: the client that re-seats a
        // thing on new ground has measured the slope there, and sending the
        // point without the angles would leave the last hill's tilt on it.
        SVProp::Transform(id, all, &at, (n >= 6) ? &yaw : nullptr, 0.f, (n >= 6) ? &tilt : nullptr);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "move a placed object: sv_prop_move <id|last> <x y z> [yaw pitch bank]"); }
};

struct CCC_PropTurn : public IConsole_Command
{
    CCC_PropTurn(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        u16 id;
        bool all;
        if (!prop_target(args, id, all, "sv_prop_turn <id|last|mine> <yaw>"))
            return;

        LPCSTR rest = args ? strchr(args, ' ') : nullptr;
        float yaw = 0.f;
        if (!rest || sscanf(rest, "%f", &yaw) != 1)
        {
            Msg("! [sv_prop] sv_prop_turn <id|last|mine> <yaw>");
            return;
        }
        SVProp::Transform(id, all, nullptr, &yaw, 0.f);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "turn a placed object, degrees: sv_prop_turn <id|last|mine> <yaw>"); }
};

struct CCC_PropLift : public IConsole_Command
{
    CCC_PropLift(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        u16 id;
        bool all;
        if (!prop_target(args, id, all, "sv_prop_lift <id|last|mine> <metres>"))
            return;

        LPCSTR rest = args ? strchr(args, ' ') : nullptr;
        float dy = 0.f;
        if (!rest || sscanf(rest, "%f", &dy) != 1)
        {
            Msg("! [sv_prop] sv_prop_lift <id|last|mine> <metres>");
            return;
        }
        SVProp::Transform(id, all, nullptr, nullptr, dy);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "raise or lower a placed object: sv_prop_lift <id|last|mine> <metres>"); }
};

struct CCC_PropDel : public IConsole_Command
{
    CCC_PropDel(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        u16 id;
        bool all;
        if (!prop_target(args, id, all, "sv_prop_del <id|last> | mine"))
            return;
        SVProp::Remove(id, all);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "remove a prop by id, or 'mine' for every one the server placed"); }
};

// Saving happens by itself after every change; these exist for the cases where
// an operator edited the file by hand, or wants to see where it is.
struct CCC_PropSave : public IConsole_Command
{
    CCC_PropSave(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { SVProp::Save(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "write the placed props down now (they are saved after every change anyway)"); }
};

struct CCC_PropLoad : public IConsole_Command
{
    CCC_PropLoad(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args) { SVProp::Load(args && strstr(args, "force")); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "place the props saved for this level: sv_prop_load [force]"); }
};

struct CCC_PropBake : public IConsole_Command
{
    CCC_PropBake(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { SVProp::Bake(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "bake the placed props into gamedata so every server furnishes the map the same way"); }
};

// Exercise the belt/ruck placement feed -- see SVHealth::InjectPlace.
struct CCC_PlaceTest : public IConsole_Command
{
    CCC_PlaceTest(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args)
    {
        int to_belt = 0;
        if (args && args[0])
            sscanf(args, "%d", &to_belt);
        SVHealth::InjectPlace(to_belt != 0);
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "move every tracked player's artefacts to ruck (0, default) or belt (1) through the real inventory event"); }
};

// The content behind a victim's damage floor -- see SVHealth::AuditDamage.
struct CCC_DamageAudit : public IConsole_Command
{
    CCC_DamageAudit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args) { SVDamage::AuditDamage(args); }

    virtual void Info(TInfo& I) { xr_strcpy(I, "print the immunity / health-part / per-bone factors a section's damage floor is built from (default: actor)"); }
};

// What a shot is worth, per weapon and per round -- see SVHealth::AuditWeapons.
struct CCC_WeaponAudit : public IConsole_Command
{
    CCC_WeaponAudit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR args) { SVDamage::AuditWeapons(args); }

    virtual void Info(TInfo& I) { xr_strcpy(I, "print weapon hit_power against every round it accepts (optional substring filter)"); }
};

// What the armour matcher finds in this install's content -- see SVHealth::AuditArmour.
struct CCC_ArmourAudit : public IConsole_Command
{
    CCC_ArmourAudit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR /*args*/) { SVDamage::AuditArmour(); }

    virtual void Info(TInfo& I) { xr_strcpy(I, "list the content sections the damage floor would treat as armour"); }
};

// What belt artefacts take off a hit before armour ever sees it -- see SVDamage::AuditBelt.
struct CCC_BeltAudit : public IConsole_Command
{
    CCC_BeltAudit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR /*args*/) { SVDamage::AuditBelt(); }

    virtual void Info(TInfo& I) { xr_strcpy(I, "list the artefacts the damage floor treats as belt protection, and the worst case a full belt reaches"); }
};

// The ceiling the content declares for every kind of damage -- see
// SVDamage::AuditCaps. This is the report that made capping the non-bullet
// types possible: what a client MAY claim is an ini question, and waiting for
// one to claim it was waiting for the wrong evidence.
struct CCC_CapAudit : public IConsole_Command
{
    CCC_CapAudit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR /*args*/) { SVDamage::AuditCaps(); }

    virtual void Info(TInfo& I) { xr_strcpy(I, "per hit type, the strongest hit this content can produce and the section that produces it"); }
};

#ifdef DEBUG
class CCC_RadioGroupMask2;
class CCC_RadioMask : public CCC_Mask
{
    CCC_RadioGroupMask2* group;

public:
    CCC_RadioMask(LPCSTR N, Flags32* V, u32 M) : CCC_Mask(N, V, M) { group = NULL; }
    void SetGroup(CCC_RadioGroupMask2* G) { group = G; }
    virtual void Execute(LPCSTR args);

    IC void Set(BOOL V) { value->set(mask, V); }
};

class CCC_RadioGroupMask2
{
    CCC_RadioMask* mask0;
    CCC_RadioMask* mask1;

public:
    CCC_RadioGroupMask2(CCC_RadioMask* m0, CCC_RadioMask* m1)
    {
        mask0 = m0;
        mask1 = m1;
        mask0->SetGroup(this);
        mask1->SetGroup(this);
    }
    void Execute(CCC_RadioMask& m, LPCSTR args)
    {
        BOOL value = m.GetValue();
        if (value)
        {
            mask0->Set(!value);
            mask1->Set(!value);
        }
        m.Set(value);
    }
};

void CCC_RadioMask::Execute(LPCSTR args)
{
    CCC_Mask::Execute(args);
    VERIFY2(group, "CCC_RadioMask: group not set");
    group->Execute(*this, args);
}

#define CMD_RADIOGROUPMASK2(p1, p2, p3, p4, p5, p6) \
    { \
        static CCC_RadioMask x##CCC_RadioMask1(p1, p2, p3); \
        Console->AddCommand(&x##CCC_RadioMask1); \
        static CCC_RadioMask x##CCC_RadioMask2(p4, p5, p6); \
        Console->AddCommand(&x##CCC_RadioMask2); \
        static CCC_RadioGroupMask2 x##CCC_RadioGroupMask2(&x##CCC_RadioMask1, &x##CCC_RadioMask2); \
    }

struct CCC_DbgBullets : public CCC_Integer
{
    CCC_DbgBullets(LPCSTR N, int* V, int _min = 0, int _max = 999) : CCC_Integer(N, V, _min, _max){};

    virtual void Execute(LPCSTR args)
    {
        extern FvectorVec g_hit[];
        g_hit[0].clear();
        g_hit[1].clear();
        g_hit[2].clear();
        CCC_Integer::Execute(args);
    }
};
#endif // DEBUG

#include "attachable_item.h"
#include "attachment_owner.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "HUDManager.h"
#include "HUDTarget.h"
#include "WeaponMagazined.h"

// dbg_wpn_trace: print every weapon-state transition and every one that arrives
// over the network, on whichever process it is switched on. Both binaries link
// xrGame, so the same knob works in the client console and in the server's live
// command channel -- and the two logs side by side are what says who is actually
// driving a player's weapon.
int g_dbg_wpn_trace = 0;

// dbg_death: who actually decides a creature is dead.
//
// ⭐The server keeps a ledger of creature health (sv_npc_health.h) and believes
// it is the only authority. It is not: the host runs a full game object for
// every creature, that object receives the same relayed GE_HIT as everybody
// else, and it applies the hit through the engine's own damage path -- rank
// immunity, bone armour, the wounded-finisher, wounds that go on bleeding
// afterwards. None of that is in the ledger. When the two disagree the object
// wins, because the object is the one that calls KillEntity.
//
// This prints both sides of that disagreement: every multiplier the object
// applies, the health before and after, every drain that arrives without a hit,
// and the GE_DIE that comes out the far end with the ledger's number beside it.
int g_dbg_death = 0;
// see CActorCondition::UpdateCondition -- where the player's stamina goes, once a second
int g_dbg_stamina = 0;

// dbg_spawn_prefetch_list: files every ALife object of this level would load on
// spawn, with sizes (Level_network_spawn.cpp). Sizing the "cache it at load" idea.
extern void spawn_prefetch_list_dump();
extern int g_spawn_prefetch_mb, g_spawn_prefetch_margin;
extern int g_npc_prewarm; // Level_network_spawn.cpp: build the level's NPC models + animations at load
extern int g_spawn_items_budget_us; // Level_network_spawn.cpp: a stalker's items come online in the frames after him
#include "../xrCore/fs_load_prefetch.h"   // load_prefetch_mb
extern int g_oh_search_cap; // object_handler_planner.cpp
extern float g_ik_calc_dist, g_ik_always_calc_dist, g_ik_lod_dist; // CharacterPhysicsSupport.cpp: foot IK distances
class CCC_SpawnPrefetchList : public IConsole_Command
{
public:
    CCC_SpawnPrefetchList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
    void Execute(LPCSTR) override { spawn_prefetch_list_dump(); }
};
// see CStalkerCombatPlanner::dbg_tactics -- what each fighting stalker within N m of the player is doing
int g_dbg_npc_tactics = 0;
// see CCustomMonster::net_Export_intent -- the terms of the velocity quotient
int g_dbg_intent = 0;
// see CAI_Stalker::update_aim_servo -- how far the barrel misses, in degrees
int g_dbg_aim = 0;

// dbg_wpn: what the weapon in the player's hands actually believes.
//
// ⭐A CLIENT-side command, deliberately. "The reload animation plays and the
// magazine stays empty" is a client-side fact that no server census can see --
// the server's copy of a weapon carries a round count and nothing else, and it
// is not even told when the player fires. Three diagnoses were argued from
// server numbers before anybody looked at the six values a reload actually
// reads. This prints them.
struct CCC_DbgWeapon : public IConsole_Command
{
    CCC_DbgWeapon(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };

    virtual void Execute(LPCSTR /*args*/)
    {
        if (!g_pGameLevel || !Actor())
        {
            Msg("! [dbg_wpn] no level / no actor");
            return;
        }

        CInventoryItem* active = Actor()->inventory().ActiveItem();
        if (!active)
        {
            Msg("! [dbg_wpn] nothing in hands");
            return;
        }

        CWeaponMagazined* wpn = smart_cast<CWeaponMagazined*>(active);
        if (!wpn)
        {
            Msg("! [dbg_wpn] '%s' is not a magazine-fed weapon", active->object().cNameSect().c_str());
            return;
        }

        wpn->DbgDump();
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "print the state a reload depends on for the weapon in hands"); }
};
class CCC_TuneAttachableItem : public IConsole_Command
{
public:
    CCC_TuneAttachableItem(LPCSTR N) : IConsole_Command(N){};
    void Execute(LPCSTR args) override
    {
        if (!g_pGameLevel) // level not loaded
            return;

        if (CAttachableItem::m_dbgItem)
        {
            CAttachableItem::m_dbgItem = nullptr;
            Msg("~~[%s] switched to off", __FUNCTION__);
            return;
        }

        if (!g_hud)
            return;

        CObject* obj = ((CHUDManager*)g_hud)->GetTarget()->GetObj();
        auto owner = smart_cast<CAttachmentOwner*>(obj);
        if (!owner)
        {
            obj = Level().CurrentViewEntity();
            owner = smart_cast<CAttachmentOwner*>(obj);
        }

        if (!owner)
            return;

        if (auto itm = owner->attachedItem(args))
            CAttachableItem::m_dbgItem = itm;
        else
        {
            auto iowner = smart_cast<CInventoryOwner*>(obj);
            if (iowner)
            {
                auto active_item = iowner->m_inventory->ActiveItem();
                if (active_item && active_item->object().cNameSect() == args)
                    CAttachableItem::m_dbgItem = active_item->cast_attachable_item();
            }
        }

        if (CAttachableItem::m_dbgItem)
            Msg("--[%s] switched to ON for [%s]", __FUNCTION__, args);
        else
            Msg("!![%s] cannot find attached item [%s]", __FUNCTION__, args);
    }

    void Info(TInfo& I) override { sprintf_s(I, "allows to change bind rotation and position offsets for attached item, <section_name> given as arguments"); }
};

class CCC_TuneAttachableItemInSlot : public IConsole_Command
{
public:
    CCC_TuneAttachableItemInSlot(LPCSTR N) : IConsole_Command(N){};
    void Execute(LPCSTR args) override
    {
        if (!g_pGameLevel) // level not loaded
            return;

        if (CAttachableItem::m_dbgItem)
        {
            CAttachableItem::m_dbgItem = nullptr;
            Msg("~~[%s] switched to off", __FUNCTION__);
            return;
        }

        if (!g_hud)
            return;

        CObject* obj = ((CHUDManager*)g_hud)->GetTarget()->GetObj();
        auto owner = smart_cast<CAttachmentOwner*>(obj);
        if (!owner)
        {
            obj = Level().CurrentViewEntity();
            owner = smart_cast<CAttachmentOwner*>(obj);
        }

        if (!owner)
            return;

        auto iowner = smart_cast<CInventoryOwner*>(obj);
        if (iowner)
        {
            u32 slot = u32(std::stoi(args));
            if (slot < SLOTS_TOTAL)
            {
                auto active_item = iowner->m_inventory->ItemFromSlot(slot);
                if (active_item)
                    CAttachableItem::m_dbgItem = active_item->cast_attachable_item();
            }
        }

        if (CAttachableItem::m_dbgItem)
            Msg("--[%s] switched to ON for item in slot [%s]", __FUNCTION__, args);
        else
            Msg("!![%s] cannot find attached item in slot [%s]", __FUNCTION__, args);
    }

    void Info(TInfo& I) override { sprintf_s(I, "allows to change bind rotation and position offsets for attached item, <section_name> given as arguments"); }
};

#ifdef DEBUG
class CCC_Crash : public IConsole_Command
{
public:
    CCC_Crash(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /**args/**/)
    {
        VERIFY3(false, "This is a test crash", "Do not post it as a bug");
        int* pointer = 0;
        *pointer = 0;
    }
};

class CCC_DumpModelBones : public IConsole_Command
{
public:
    CCC_DumpModelBones(LPCSTR N) : IConsole_Command(N) {}

    virtual void Execute(LPCSTR arguments)
    {
        if (!arguments || !*arguments)
        {
            Msg("! no arguments passed");
            return;
        }

        string_path name;
        string_path fn;

        if (0 == strext(arguments))
            strconcat(sizeof(name), name, arguments, ".ogf");
        else
            strcpy_s(name, sizeof(name), arguments);

        if (!FS.exist(arguments) && !FS.exist(fn, "$level$", name) && !FS.exist(fn, "$game_meshes$", name))
        {
            Msg("! Cannot find visual \"%s\"", arguments);
            return;
        }

        IRenderVisual* visual = Render->model_Create(arguments);
        IKinematics* kinematics = smart_cast<IKinematics*>(visual);
        if (!kinematics)
        {
            Render->model_Delete(visual);
            Msg("! Invalid visual type \"%s\" (not a IKinematics)", arguments);
            return;
        }

        Msg("bones for model \"%s\"", arguments);
        for (u16 i = 0, n = kinematics->LL_BoneCount(); i < n; ++i)
            Msg("%s", *kinematics->LL_GetData(i).name);

        Render->model_Delete(visual);
    }
};

extern void show_animation_stats();

class CCC_ShowAnimationStats : public IConsole_Command
{
public:
    CCC_ShowAnimationStats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR) { show_animation_stats(); }
};

#endif // DEBUG

class CCC_DumpObjects : public IConsole_Command
{
public:
    CCC_DumpObjects(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR) { Level().Objects.dump_all_objects(); }
};


// Change weather immediately
class CCC_SetWeather : public IConsole_Command
{
public:
    CCC_SetWeather(LPCSTR N) : IConsole_Command(N) {}

    void Execute(LPCSTR args) override
    {
        if (!strlen(args))
            return;
        if (!g_pGameLevel)
            return;
        if (!g_pGamePersistent)
            return;

        // Weather decides how far and how well a player sees. On a machine that
        // only renders the host's world, picking it locally is a cheat, not a
        // preference -- the host sends one sky for everyone.
        if (is_network_client())
        {
            Msg("! set_weather: the weather comes from the server here");
            return;
        }

        g_pGamePersistent->Environment().SetWeather(args, true);
    }

    void fill_tips(vecTips& tips, u32 mode) override
    {
        if (!g_pGameLevel)
            return;
        if (!g_pGamePersistent)
            return;

        for (auto& [name, cycle] : g_pGamePersistent->Environment().WeatherCycles)
            tips.push_back(name);
    }
};

// ---- AUTHORITATIVE WEATHER CONTROL ------------------------------------------
// set_weather alone does not hold: the Lua forecast re-asserts itself on every
// weather change, so a manual request is undone within a second (and a broken
// forecast freezes the sky with no diagnostic at all). These take ownership away
// from the script — the same seam a server will drive for a shared world, where
// every client must see one sky.

// weather_force <cycle> — set it AND hold it against the forecast.
class CCC_WeatherForce : public IConsole_Command
{
public:
    CCC_WeatherForce(LPCSTR N) : IConsole_Command(N) {}

    void Execute(LPCSTR args) override
    {
        if (!strlen(args) || !g_pGameLevel || !g_pGamePersistent)
            return;
        // Same reason set_weather is refused here: on a machine that renders the
        // host's world, choosing the sky locally is a cheat (it decides sight range
        // and how loud the rain is), and the next update header would overrule it
        // anyway. sv_weather_set is the host-side equivalent.
        if (is_network_client())
        {
            Msg("! weather_force: the weather comes from the server here");
            return;
        }
        auto& env = g_pGamePersistent->Environment();
        if (env.WeatherCycles.find(args) == env.WeatherCycles.end())
        {
            Msg("! weather_force: no such cycle '%s' — see weather_list", args);
            return;
        }
        env.SetWeatherAuthoritative(args);
    }

    void fill_tips(vecTips& tips, u32 mode) override
    {
        if (!g_pGameLevel || !g_pGamePersistent)
            return;
        for (auto& [name, cycle] : g_pGamePersistent->Environment().WeatherCycles)
            tips.push_back(name);
    }
};

// weather_lock 0/1 — hold whatever is running now (no argument = report state).
class CCC_WeatherLock : public IConsole_Command
{
public:
    CCC_WeatherLock(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }

    void Execute(LPCSTR args) override
    {
        if (!g_pGamePersistent)
            return;
        auto& env = g_pGamePersistent->Environment();
        if (!strlen(args))
        {
            Msg("~ weather lock is %s (current '%s')", env.IsWeatherLocked() ? "ON" : "OFF", env.GetWeather().c_str());
            return;
        }
        // Reading the lock is harmless; releasing it on a network client is not --
        // the hold is what keeps this client on the server's sky instead of letting
        // the local forecast script pull it back.
        if (is_network_client())
        {
            Msg("! weather_lock: the weather comes from the server here");
            return;
        }
        env.LockWeather(atoi(args) != 0);
    }
};

// weather_status — what is actually live: cycle, the two hour sections being
// blended, the lock, and the weather values the renderer consumes. "The rain cycle
// is active" and "it is raining right now" are different claims; a cycle is a table
// over the day, and only the section pair tells them apart.
class CCC_WeatherStatus : public IConsole_Command
{
public:
    CCC_WeatherStatus(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }

    void Execute(LPCSTR /*args*/) override
    {
        if (!g_pGamePersistent)
            return;
        auto& env = g_pGamePersistent->Environment();
        Msg("~ weather '%s'%s%s [%s -> %s]", env.GetWeather().c_str(), env.IsWeatherLocked() ? " LOCKED" : "",
            env.IsWeatherFXPlaying() ? " WFX" : "", env.Current[0] ? env.Current[0]->m_identifier.c_str() : "?",
            env.Current[1] ? env.Current[1]->m_identifier.c_str() : "?");
        // Running vs requested vs held. A forced cycle that shows the right sky while
        // the RUNNING name is still the script's means the write landed only halfway,
        // and the sky reverts at the next hour section - print all three so that state
        // is one line away instead of an hour of log archaeology.
        const auto* running = env.GetWeather().c_str();
        const auto* wanted = env.GetWeatherRequested().c_str();
        if (env.IsWeatherLocked() || !running || !wanted || 0 != xr_strcmp(running, wanted))
            Msg("~   requested '%s'  held '%s'", wanted ? wanted : "-",
                env.IsWeatherLocked() && env.GetWeatherLockName().c_str() ? env.GetWeatherLockName().c_str() : "-");
        if (const auto* E = env.CurrentEnv)
            Msg("~   clouds %.2f  fog_density %.2f  rain %.2f  wetness %.2f  sun (%.2f %.2f %.2f)", E->clouds_color.w, E->fog_density, E->rain_density,
                env.wetness_factor, E->sun_color.x, E->sun_color.y, E->sun_color.z);
    }
};

// weather_list — the cycles this level actually has. Without it, a wrong name is
// indistinguishable from a broken system.
class CCC_WeatherList : public IConsole_Command
{
public:
    CCC_WeatherList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }

    void Execute(LPCSTR /*args*/) override
    {
        if (!g_pGamePersistent)
            return;
        auto& env = g_pGamePersistent->Environment();
        Msg("~ %u weather cycles (current '%s'):", (u32)env.WeatherCycles.size(), env.GetWeather().c_str());
        for (auto& [name, cycle] : env.WeatherCycles)
            Msg("~   %s", name.c_str());
    }
};

#ifdef USE_MEMORY_VALIDATOR

class CCC_DbgMemoryDump : public IConsole_Command
{
public:
    CCC_DbgMemoryDump(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }

    virtual void Execute(LPCSTR args)
    {
        float thresholdInKb = 1.f;

        if (strlen(args) > 0)
        {
            thresholdInKb = atof(args);
        }

        PointerRegistryDump(thresholdInKb);
    }
};

class CCC_DbgMemoryClear : public IConsole_Command
{
public:
    CCC_DbgMemoryClear(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }

    virtual void Execute(LPCSTR args) { PointerRegistryClear(); }
};

class CCC_DbgMemoryInfo : public IConsole_Command
{
public:
    CCC_DbgMemoryInfo(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }

    virtual void Execute(LPCSTR args) { PointerRegistryInfo(); }
};

#endif

class CCC_UI_Reload : public IConsole_Command
{
public:
    CCC_UI_Reload(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        if (g_pGamePersistent && g_pGameLevel && Level().game)
            HUD().OnScreenRatioChanged();
    }
};


void CCC_RegisterCommands()
{
    CMD1(CCC_MemStats, "stat_memory");
    CMD1(CCC_UI_Reload, "ui_reload");

    // game
    //CMD3(CCC_Mask, "g_always_run", &psActorFlags, AF_ALWAYSRUN);
    CMD1(CCC_GameDifficulty, "g_game_difficulty");
    CMD1(CCC_GameLanguage, "g_language");

    CMD3(CCC_Mask, "g_dof_zoom", &psActorFlags, AF_DOF_ZOOM);
    CMD3(CCC_Mask, "g_dof_reload", &psActorFlags, AF_DOF_RELOAD);

    CMD3(CCC_Mask, "wpn_aim_toggle", &psActorFlags, AF_WPN_AIM_TOGGLE);

    // alife
#ifdef DEBUG
    CMD1(CCC_ALifePath, "al_path"); // build path
#endif // DEBUG

    CMD1(CCC_ALifeSave, "save"); // save game
    CMD1(CCC_ALifeLoadFrom, "load"); // load game from ...
    CMD1(CCC_LoadLastSave, "load_last_save"); // load last saved game from ...

    // available in all builds: legitimate live-tuning knobs for the simulation
    CMD1(CCC_ALifeTimeFactor, "al_time_factor"); // set time factor
    CMD1(CCC_ALifeSwitchDistance, "al_switch_distance"); // set switch distance
    CMD1(CCC_ALifeProcessTime, "al_process_time"); // set process time
    CMD1(CCC_ALifeObjectsPerUpdate, "al_objects_per_update"); // set process time
    CMD1(CCC_ALifeSwitchFactor, "al_switch_factor"); // set switch factor

    CMD3(CCC_Mask, "hud_info", &psHUD_Flags, HUD_INFO);
    CMD3(CCC_Mask, "hud_draw", &psHUD_Flags, HUD_DRAW);
    CMD3(CCC_Mask, "hud_crosshair_build", &psHUD_Flags, HUD_CROSSHAIR_BUILD); // билдокурсор

    if (IS_OGSR_GA)
        psHUD_Flags.set(HUD_CROSSHAIR_HARD, TRUE);
    else
        CMD3(CCC_Mask, "hud_crosshair_hard", &psHUD_Flags, HUD_CROSSHAIR_HARD);

    CMD3(CCC_Mask, "hud_crosshair", &psHUD_Flags, HUD_CROSSHAIR);
    CMD3(CCC_Mask, "hud_crosshair_dist", &psHUD_Flags, HUD_CROSSHAIR_DIST);
    CMD3(CCC_Mask, "hud_info_overhead", &psHUD_Flags, HUD_INFO_OVERHEAD);

    if (IS_OGSR_GA)
        psHUD_FOV_def = 0.65f;
    CMD4(CCC_Float, "hud_fov", &psHUD_FOV_def, 0.1f, 1.0f);

    CMD4(CCC_Float, "fov", &g_fov, 5.0f, 140.0f);

    // Demo
    CMD1(CCC_DemoPlay, "demo_play");
    CMD1(CCC_DemoRecord, "demo_record");

#ifndef MASTER_GOLD
    // ai
    CMD3(CCC_Mask, "mt_ai_vision", &g_mt_config, mtAiVision);
    CMD3(CCC_Mask, "mt_level_path", &g_mt_config, mtLevelPath);
    CMD3(CCC_Mask, "mt_detail_path", &g_mt_config, mtDetailPath);
    CMD3(CCC_Mask, "mt_object_handler", &g_mt_config, mtObjectHandler);
    CMD3(CCC_Mask, "mt_sound_player", &g_mt_config, mtSoundPlayer);
    CMD3(CCC_Mask, "mt_bullets", &g_mt_config, mtBullets);
    CMD3(CCC_Mask, "mt_level_sounds", &g_mt_config, mtLevelSounds);
    CMD3(CCC_Mask, "mt_alife", &g_mt_config, mtALife);
#endif // MASTER_GOLD

#ifdef DEBUG
    CMD3(CCC_Mask, "ai_debug", &psAI_Flags, aiDebug);
    CMD3(CCC_Mask, "ai_dbg_brain", &psAI_Flags, aiBrain);
    CMD3(CCC_Mask, "ai_dbg_motion", &psAI_Flags, aiMotion);
    CMD3(CCC_Mask, "ai_dbg_frustum", &psAI_Flags, aiFrustum);
    CMD3(CCC_Mask, "ai_dbg_funcs", &psAI_Flags, aiFuncs);
    CMD3(CCC_Mask, "ai_dbg_alife", &psAI_Flags, aiALife);
    CMD3(CCC_Mask, "ai_dbg_lua", &psAI_Flags, aiLua);
    CMD3(CCC_Mask, "ai_dbg_goap", &psAI_Flags, aiGOAP);
    CMD3(CCC_Mask, "ai_dbg_goap_script", &psAI_Flags, aiGOAPScript);
    CMD3(CCC_Mask, "ai_dbg_goap_object", &psAI_Flags, aiGOAPObject);
    CMD3(CCC_Mask, "ai_dbg_cover", &psAI_Flags, aiCover);
    CMD3(CCC_Mask, "ai_dbg_anim", &psAI_Flags, aiAnimation);
    CMD3(CCC_Mask, "ai_dbg_vision", &psAI_Flags, aiVision);
    CMD3(CCC_Mask, "ai_dbg_monster", &psAI_Flags, aiMonsterDebug);
    CMD3(CCC_Mask, "ai_dbg_stalker", &psAI_Flags, aiStalker);
    CMD3(CCC_Mask, "ai_stats", &psAI_Flags, aiStats);
    CMD3(CCC_Mask, "ai_dbg_destroy", &psAI_Flags, aiDestroy);
    CMD3(CCC_Mask, "ai_dbg_serialize", &psAI_Flags, aiSerialize);
    CMD3(CCC_Mask, "ai_dbg_dialogs", &psAI_Flags, aiDialogs);
    CMD3(CCC_Mask, "ai_dbg_infoportion", &psAI_Flags, aiInfoPortion);

    CMD3(CCC_Mask, "ai_draw_game_graph", &psAI_Flags, aiDrawGameGraph);
    CMD3(CCC_Mask, "ai_draw_game_graph_stalkers", &psAI_Flags, aiDrawGameGraphStalkers);
    CMD3(CCC_Mask, "ai_draw_game_graph_objects", &psAI_Flags, aiDrawGameGraphObjects);

    CMD3(CCC_Mask, "ai_nil_object_access", &psAI_Flags, aiNilObjectAccess);

    CMD3(CCC_Mask, "ai_draw_visibility_rays", &psAI_Flags, aiDrawVisibilityRays);
    CMD3(CCC_Mask, "ai_animation_stats", &psAI_Flags, aiAnimationStats);

    CMD1(CCC_DumpModelBones, "debug_dump_model_bones");

    CMD1(CCC_DrawGameGraphAll, "ai_draw_game_graph_all");
    CMD1(CCC_DrawGameGraphCurrent, "ai_draw_game_graph_current_level");
    CMD1(CCC_DrawGameGraphLevel, "ai_draw_game_graph_level");

    CMD4(CCC_Integer, "ai_dbg_inactive_time", &g_AI_inactive_time, 0, 1000000);

    CMD1(CCC_DebugNode, "ai_dbg_node");

    CMD1(CCC_ShowMonsterInfo, "ai_monster_info");
    CMD1(CCC_DebugFonts, "debug_fonts");
#endif
    CMD1(CCC_TuneAttachableItem, "dbg_adjust_attachable_item");
    CMD1(CCC_TuneAttachableItemInSlot, "dbg_adjust_attachable_item_in_slot");
    // Optional out-of-tree game module registers its own console commands
    // here; no-op in the stock engine.
    ALifeExt::fire_register_console();
    // adjust mode support
    CMD4(CCC_Integer, "hud_adjust_mode", &g_bHudAdjustMode, 0, 11);
    CMD4(CCC_Float, "hud_adjust_delta_value", &g_bHudAdjustDeltaPos, 0.00005f, 1.f);
    CMD4(CCC_Float, "hud_adjust_delta_rot", &g_bHudAdjustDeltaRot, 0.00005f, 10.f);

    CMD4(CCC_Float, "adjust_delta_pos", &adj_delta_pos, -10.f, 10.f);
    CMD4(CCC_Float, "adjust_delta_rot", &adj_delta_rot, -10.f, 10.f);

#ifdef DEBUG
    CMD1(CCC_ShowAnimationStats, "ai_show_animation_stats");
#endif // DEBUG

#ifndef MASTER_GOLD
    CMD3(CCC_Mask, "ai_ignore_actor", &psAI_Flags, aiIgnoreActor);
#endif // MASTER_GOLD

    // Physics
    CMD1(CCC_PHFps, "ph_frequency");
    CMD1(CCC_PHIterations, "ph_iterations");
    extern int g_ph_avg_prof; // PHWorld.cpp: [ph avg] every 5 s, the average step by part and by kind of object
    CMD4(CCC_Integer, "ph_avg_prof", &g_ph_avg_prof, 0, 1);
    extern float g_ph_disable_k;
    CMD4(CCC_Float, "ph_disable_k", &g_ph_disable_k, 0.1f, 16.f); // DisablingParams.cpp: per-step sleep thresholds x this (Switch 50 Hz)
    extern float g_ph_far_freeze;
    CMD4(CCC_Float, "ph_far_freeze", &g_ph_far_freeze, 0.f, 1000.f); // PHWorld.cpp: freeze long-awake bodies farther than this (m), 0 = off
    extern float g_ph_settle;
    CMD4(CCC_Float, "ph_settle", &g_ph_settle, 0.f, 10.f); // PHWorld.cpp: freeze an awake shell that has not moved for this long (s), 0 = off
    extern int g_ph_exact_joints;
    CMD4(CCC_Integer, "ph_exact_joints", &g_ph_exact_joints, 0, 100); // PHIsland.cpp: islands with fixed bones under this many joints take the exact (LCP) stepper

    CMD1(CCC_PHGravity, "ph_gravity");
    CMD4(CCC_FloatBlock, "ph_timefactor", &phTimefactor, 0.0001f, 1000.f);

    // Crowd taming (measurement + hot-spot relief). Both default 0 = stock. RELEASE-visible.
    CMD4(CCC_Integer, "ph_npc_crowd_gate", &ph_npc_crowd_gate, 0, 512); // 0=off, 1=A/B ceiling, N=gate at N char-neighbours
    CMD4(CCC_Integer, "ph_max_substeps", &ph_max_substeps, 0, 100);     // 0=uncapped; caps sub-steps/frame (death-spiral fix)

    // ⭐How much collision damage one CHARACTER accepts from another. Default 0
    // -- walking into a person is not a crash. 1 restores the stock behaviour
    // that killed a player for stepping on a stalker (see
    // CPHSimpleCharacter::UpdateDynamicDamage).
    //
    // ⚠Not savable, on purpose. This is a balance authority, and a value that
    // drifts into user.ltx becomes a second one that nobody can see -- the
    // trap that has cost this build three sessions (sv_interest_radius,
    // sv_auth_required, sv_npc_damage_scale).
    {
        static CCC_FloatBlock chr_cd("ph_character_collision_damage", &ph_character_collision_damage, 0.f, 1.f);
        chr_cd.SetCanSave(false);
        Console->AddCommand(&chr_cd);

        // ⭐The speed limit on ODE's penetration recovery. ODE's own default is
        // dInfinity and nothing here ever set it, which let an overlap become a
        // 22 m/s launch. ⚠Read at world creation, so a change needs a level
        // reload -- it is a world property, not a per-frame one.
        static CCC_FloatBlock maxcv("ph_max_correcting_vel", &ph_max_correcting_vel, 0.f, 1000.f);
        maxcv.SetCanSave(false);
        Console->AddCommand(&maxcv);
    }

#ifdef DEBUG
    CMD4(CCC_FloatBlock, "ph_break_common_factor", &phBreakCommonFactor, 0.f, 1000000000.f);
    CMD4(CCC_FloatBlock, "ph_rigid_break_weapon_factor", &phRigidBreakWeaponFactor, 0.f, 1000000000.f);
    CMD4(CCC_Integer, "ph_tri_clear_disable_count", &ph_tri_clear_disable_count, 0, 255);
    CMD4(CCC_FloatBlock, "ph_tri_query_ex_aabb_rate", &ph_tri_query_ex_aabb_rate, 1.01f, 3.f);
#endif // DEBUG

    //#ifndef MASTER_GOLD
    CMD2(CCC_SetCmdEnabled, "cmd_enable", true);    // re-enable a mod-disabled console command
    CMD2(CCC_SetCmdEnabled, "cmd_disable", false);
    CMD1(CCC_CmdForce, "cmd_force");                // run a command bypassing bEnabled (mod re-disables race-free)
    {
        // Покрытие реестра ALife (см. alife_update_manager.cpp). Живёт рядом с
        // jump_to_level не случайно: вопрос "успевает ли ALife" задают ровно
        // после перехода на уровень.
        extern int g_al_stats;
        extern int g_al_switch_period;
        CMD4_NOSAVE(CCC_Integer, "al_stats", &g_al_stats, 0, 1);
        // Период полного обхода реестра ALife, реальные мс (0 = каждый тик
        // планировщика, историческое поведение). См. update_switch().
        CMD4_NOSAVE(CCC_Integer, "al_switch_period", &g_al_switch_period, 0, 2000);
        // Сколько объектов (владелец + его инвентарь) один обход уводит в оффлайн по
        // дистанции; остальные -- в следующий обход. 0 = без предела, как было.
        // См. alife_switch_manager.cpp.
        extern int g_al_offline_budget;
        CMD4_NOSAVE(CCC_Integer, "al_offline_budget", &g_al_offline_budget, 0, 1000);
        // Строка `[alife slow]`, когда апдейт ALife дольше N мс: самый долгий
        // объект, фаза, входы в Lua. 0 = выкл. См. alife_slow_probe.h.
        CMD4_NOSAVE(CCC_Integer, "al_slow_ms", &alife_slow::threshold_ms, 0, 1000);
        // Бюджет пачки планового апдейта ALife, мкс (0 = только objects_per_update,
        // как было). Хотя бы один объект за апдейт берётся всегда.
        CMD4_NOSAVE(CCC_Integer, "al_sched_budget_us", &alife_slow::sched_budget_us, 0, 100000);
        // Скриптовый прибор гулагов (ogsr_gulag_prof.script): 0 = не ставить,
        // N = печатать апдейты смартов дольше N мкс. Ставится при старте ALife.
        CMD4_NOSAVE(CCC_Integer, "al_gulag_prof_us", &alife_slow::gulag_prof_us, 0, 1000000);
        // Счётчики повторной оценки свойств мира в GOAP и сам кэш на один solve().
        // Оба NoSave: это рычаги A/B, а не настройки. См. goap_stats.h.
        CMD4_NOSAVE(CCC_Integer, "goap_stats", &GoapStats::enabled, 0, 2); // 2 = + time per evaluator/action (goap_stats.h)
        CMD4_NOSAVE(CCC_Integer, "goap_eval_cache", &GoapStats::use_cache, 0, 1);
        // Разбор одного апдейта NPC/актёра по статьям (npc_perf.h).
        // ⚠Колонка Lua наполняется ТОЛЬКО вместе с `lua_time 1`.
        CMD4_NOSAVE(CCC_Integer, "npc_stats", &NpcPerf::enabled, 0, 1);
        // Кэш ответа "предмет в разрешённой зоне" в CItemManager (item_manager.h).
        // ⚠Дефолт 0: это кэш решения ИИ, а не чистая арифметика.
        {
            extern int g_mem_item_access_cache;
            CMD4_NOSAVE(CCC_Integer, "mem_item_access_cache", &g_mem_item_access_cache, 0, 1);
        }
    }
    {
        // Период апдейта Lua-биндера у объектов очереди РЕАЛЬНОГО ВРЕМЕНИ
        // (актёр), реальные мс; 0 = каждый кадр, как было всегда.
        // См. CScriptBinder::binder_rt_gate() -- там замер и обоснование.
        // NoSave: значение для A/B в одном прогоне, дефолт живёт в коде.
        extern int g_script_binder_period;
        CMD4_NOSAVE(CCC_Integer, "script_binder_period", &g_script_binder_period, 0, 1000);
    }
    CMD1(CCC_ALifeCensus, "al_census");
    CMD1(CCC_JumpToLevel, "jump_to_level");
    CMD1(CCC_Spawn, "g_spawn");
    CMD1(CCC_SpawnToInventory, "g_spawn_to_inventory");
    CMD3(CCC_Mask, "g_god", &psActorFlags, AF_GODMODE);
    CMD3(CCC_Mask, "g_unlimitedammo", &psActorFlags, AF_UNLIMITEDAMMO);
    CMD3(CCC_Mask, "g_ammunition_on_belt", &psActorFlags, AF_AMMO_ON_BELT);

    CMD3(CCC_Mask, "g_3d_scopes", &psActorFlags, AF_3D_SCOPES);

    CMD3(CCC_Mask, "g_crosshair_dbg", &psActorFlags, AF_CROSSHAIR_DBG);
    CMD3(CCC_Mask, "g_camera_collision", &psActorFlags, AF_CAM_COLLISION);
    CMD3(CCC_Mask, "g_camera_collision_cop_mode", &psActorFlags, AF_CAM_COLLISION_COP);

    CMD3(CCC_Mask, "g_mouse_wheel_switch_slot", &psActorFlags, AF_MOUSE_WHEEL_SWITCH_SLOTS);

    CMD3(CCC_Mask, "g_3d_pda", &psActorFlags, AF_3D_PDA);

    CMD3(CCC_Mask, "g_first_person_death", &psActorFlags, AF_FIRST_PERSON_DEATH);

    CMD1(CCC_TimeFactor, "time_factor")
    CMD1(CCC_SetWeather, "set_weather");
    CMD1(CCC_WeatherForce, "weather_force");     // set + hold against the Lua forecast
    CMD1(CCC_WeatherLock, "weather_lock");       // hold/release the current cycle
    CMD1(CCC_WeatherStatus, "weather_status");   // cycle + hour sections + live values
    CMD1(CCC_WeatherList, "weather_list");       // cycles available on this level
    //#endif // MASTER_GOLD

    CMD3(CCC_Mask, "g_music_tracks", &psActorFlags, AF_MUSIC_TRACKS);

    CMD1(CCC_LuaHelp, "lua_help");
    CMD4_NOSAVE(CCC_Integer, "lua_gc_step_max_us", &psLUA_GC_STEP_MAX_US, 0, 100000); // предел шага сборщика Lua, 0 = без предела
    CMD4_NOSAVE(CCC_Integer, "lua_gc_stepmul", &psLUA_GC_STEPMUL, 1, 10000); // темп сборщика Lua (LUA_GCSETSTEPMUL)
    CMD4_NOSAVE(CCC_Integer, "lua_gc_pause", &psLUA_GC_PAUSE, 50, 1000); // пауза между циклами, % (LUA_GCSETPAUSE)
    CMD4_NOSAVE(CCC_Integer, "lua_gc_udmove", &psLUA_GC_UDMOVE, 0, 1); // живые userdata без __gc -- из списка, который обходит атомарная фаза
    CMD4_NOSAVE(CCC_Integer, "lua_gc_udcensus", &psLUA_GC_UDCENSUS, 0, 1); // перепись этого списка в строке [lua gc] cycle (прибор)
    CMD4_NOSAVE(CCC_Integer, "lua_gc_udsettle", &psLUA_GC_UDSETTLE, 0, 1); // живые userdata откладываются во время разметки, атомарная фаза их не обходит

    CMD3(CCC_Mask, "g_zones_dbg", &psActorFlags, AF_ZONES_DBG);
    CMD3(CCC_Mask, "g_vertex_dbg", &psActorFlags, AF_VERTEX_DBG);
    CMD3(CCC_Mask, "keypress_on_start", &psActorFlags, AF_KEYPRESS_ON_START);
    CMD3(CCC_Mask, "g_effects_on_demorecord", &psActorFlags, AF_EFFECTS_ON_DEMORECORD);
    CMD3(CCC_Mask, "g_lock_reload", &psActorFlags, AF_LOCK_RELOAD);

    CMD4(CCC_Integer, "g_cop_death_anim", &g_bCopDeathAnim, 0, 1);

#ifdef DEBUG
    CMD3(CCC_Mask, "dbg_draw_actor_alive", &dbg_net_Draw_Flags, (1 << 0));
    CMD3(CCC_Mask, "dbg_draw_actor_dead", &dbg_net_Draw_Flags, (1 << 1));
    CMD3(CCC_Mask, "dbg_draw_invitem", &dbg_net_Draw_Flags, (1 << 4));
    CMD3(CCC_Mask, "dbg_draw_actor_phys", &dbg_net_Draw_Flags, (1 << 5));
    CMD3(CCC_Mask, "dbg_draw_customdetector", &dbg_net_Draw_Flags, (1 << 6));
    CMD3(CCC_Mask, "dbg_destroy", &dbg_net_Draw_Flags, (1 << 7));
    CMD3(CCC_Mask, "dbg_draw_autopickupbox", &dbg_net_Draw_Flags, (1 << 8));
    CMD3(CCC_Mask, "dbg_draw_climbable", &dbg_net_Draw_Flags, (1 << 10));

    CMD3(CCC_Mask, "dbg_draw_ph_contacts", &ph_dbg_draw_mask, phDbgDrawContacts);
    CMD3(CCC_Mask, "dbg_draw_ph_enabled_aabbs", &ph_dbg_draw_mask, phDbgDrawEnabledAABBS);
    CMD3(CCC_Mask, "dbg_draw_ph_intersected_tries", &ph_dbg_draw_mask, phDBgDrawIntersectedTries);
    CMD3(CCC_Mask, "dbg_draw_ph_saved_tries", &ph_dbg_draw_mask, phDbgDrawSavedTries);
    CMD3(CCC_Mask, "dbg_draw_ph_tri_trace", &ph_dbg_draw_mask, phDbgDrawTriTrace);
    CMD3(CCC_Mask, "dbg_draw_ph_positive_tries", &ph_dbg_draw_mask, phDBgDrawPositiveTries);
    CMD3(CCC_Mask, "dbg_draw_ph_negative_tries", &ph_dbg_draw_mask, phDBgDrawNegativeTries);
    CMD3(CCC_Mask, "dbg_draw_ph_tri_test_aabb", &ph_dbg_draw_mask, phDbgDrawTriTestAABB);
    CMD3(CCC_Mask, "dbg_draw_ph_tries_changes_sign", &ph_dbg_draw_mask, phDBgDrawTriesChangesSign);
    CMD3(CCC_Mask, "dbg_draw_ph_tri_point", &ph_dbg_draw_mask, phDbgDrawTriPoint);
    CMD3(CCC_Mask, "dbg_draw_ph_explosion_position", &ph_dbg_draw_mask, phDbgDrawExplosionPos);
    CMD3(CCC_Mask, "dbg_draw_ph_statistics", &ph_dbg_draw_mask, phDbgDrawObjectStatistics);
    CMD3(CCC_Mask, "dbg_draw_ph_mass_centres", &ph_dbg_draw_mask, phDbgDrawMassCenters);
    CMD3(CCC_Mask, "dbg_draw_ph_death_boxes", &ph_dbg_draw_mask, phDbgDrawDeathActivationBox);
    CMD3(CCC_Mask, "dbg_draw_ph_hit_app_pos", &ph_dbg_draw_mask, phHitApplicationPoints);
    CMD3(CCC_Mask, "dbg_draw_ph_cashed_tries_stats", &ph_dbg_draw_mask, phDbgDrawCashedTriesStat);
    CMD3(CCC_Mask, "dbg_draw_ph_car_dynamics", &ph_dbg_draw_mask, phDbgDrawCarDynamics);
    CMD3(CCC_Mask, "dbg_draw_ph_car_plots", &ph_dbg_draw_mask, phDbgDrawCarPlots);
    CMD3(CCC_Mask, "dbg_ph_ladder", &ph_dbg_draw_mask, phDbgLadder);
    CMD3(CCC_Mask, "dbg_draw_ph_explosions", &ph_dbg_draw_mask, phDbgDrawExplosions);
    CMD3(CCC_Mask, "dbg_draw_car_plots_all_trans", &ph_dbg_draw_mask, phDbgDrawCarAllTrnsm);
    CMD3(CCC_Mask, "dbg_draw_ph_zbuffer_disable", &ph_dbg_draw_mask, phDbgDrawZDisable);
    CMD3(CCC_Mask, "dbg_ph_obj_collision_damage", &ph_dbg_draw_mask, phDbgDispObjCollisionDammage);
    CMD_RADIOGROUPMASK2("dbg_ph_ai_always_phmove", &ph_dbg_draw_mask, phDbgAlwaysUseAiPhMove, "dbg_ph_ai_never_phmove", &ph_dbg_draw_mask, phDbgNeverUseAiPhMove);
    CMD3(CCC_Mask, "dbg_ph_ik", &ph_dbg_draw_mask, phDbgIK);
    CMD3(CCC_Mask, "dbg_ph_ik_off", &ph_dbg_draw_mask1, phDbgIKOff);
    CMD3(CCC_Mask, "dbg_draw_ph_ik_goal", &ph_dbg_draw_mask, phDbgDrawIKGoal);
    CMD3(CCC_Mask, "dbg_ph_ik_limits", &ph_dbg_draw_mask, phDbgIKLimits);
    CMD3(CCC_Mask, "dbg_ph_character_control", &ph_dbg_draw_mask, phDbgCharacterControl);
    CMD3(CCC_Mask, "dbg_draw_ph_ray_motions", &ph_dbg_draw_mask, phDbgDrawRayMotions);
    CMD4(CCC_Float, "dbg_ph_vel_collid_damage_to_display", &dbg_vel_collid_damage_to_display, 0.f, 1000.f);
    CMD4(CCC_DbgBullets, "dbg_draw_bullet_hit", &g_bDrawBulletHit, 0, 1);
    CMD1(CCC_DbgPhTrackObj, "dbg_track_obj");
    CMD3(CCC_Mask, "dbg_ph_actor_restriction", &ph_dbg_draw_mask1, ph_m1_DbgActorRestriction);
    CMD3(CCC_Mask, "dbg_draw_ph_hit_anims", &ph_dbg_draw_mask1, phDbgHitAnims);
    CMD3(CCC_Mask, "dbg_draw_ph_ik_limits", &ph_dbg_draw_mask1, phDbgDrawIKLimits);
#endif

    CMD3(CCC_Mask, "dbg_draw_skeleton", &dbg_net_Draw_Flags, (1 << 11));

    CMD4(CCC_Integer, "dbg_string_table_error_msg", &CStringTable::WriteErrorsToLog, 0, 1);

    CMD1(CCC_FsExtract, "fs_extract");
    CMD1(CCC_UiSprites, "ui_sprites");
    CMD1(CCC_UiMenuAct, "ui_menu_act");
    CMD1(CCC_UiTest, "ui_test");
    extern int g_rml_live_rcss;
    CMD4(CCC_Integer, "ui_rml_live_rcss", &g_rml_live_rcss, 0, 1);   // 1 = every RmlUi screen re-reads its .rcss on open (editing); Switch 0
    CMD1(CCC_UiInputMute, "ui_input_mute");

    // (03-10) The `ui_new_inv` / `ui_new_hud` gates are gone with the legacy
    // windows: the inventory, search, trade, talk and HUD are RmlUi only.

#ifdef DEBUG
    CMD1(CCC_DumpInfos, "dump_infos");
    CMD1(CCC_DumpMap, "dump_map");
    CMD1(CCC_DumpCreatures, "dump_creatures");
#endif

    CMD3(CCC_Mask, "cl_dynamiccrosshair", &psHUD_Flags, HUD_CROSSHAIR_DYNAMIC);
    CMD1(CCC_MainMenu, "main_menu");

#ifndef MASTER_GOLD
    CMD1(CCC_StartTimeSingle, "start_time_single");
    CMD4(CCC_TimeFactorSingle, "time_factor_single", &g_fTimeFactor, 0.f, flt_max);
#endif // MASTER_GOLD

    // world clock: only the process that hosts the world answers these, and the
    // result travels to every connected client in the next update header
    // movement validation: a client reports its own player's position, so the
    // server checks that against how far it could have moved (see
    // xrServer::validate_client_movement)
    {
        extern int g_sv_move_validate;
        extern float g_sv_move_max_speed;
        extern float g_sv_move_fall_speed;
        // Range starts below any playable speed on purpose: CCC_Float REFUSES an
        // out-of-range value instead of clamping it, and a budget that cannot be
        // set absurdly low cannot be tested for "does this check run at all".
        CMD4(CCC_Integer, "sv_move_validate", &g_sv_move_validate, 0, 2);
        CMD4(CCC_Float, "sv_move_max_speed", &g_sv_move_max_speed, 0.1f, 1000.f);
        CMD4(CCC_Float, "sv_move_fall_speed", &g_sv_move_fall_speed, 0.1f, 1000.f);

        // sprint endurance, measured by the server rather than reported by the client
        extern float g_sv_sprint_speed;
        extern float g_sv_sprint_seconds;
        extern float g_sv_sprint_regen;
        CMD4(CCC_Float, "sv_sprint_speed", &g_sv_sprint_speed, 0.1f, 100.f);
        CMD4(CCC_Float, "sv_sprint_seconds", &g_sv_sprint_seconds, 1.f, 3600.f);
        CMD4(CCC_Float, "sv_sprint_regen", &g_sv_sprint_regen, 0.f, 100.f);

        // health authority: the client applies its own damage and reports the
        // result, so the server keeps its own ledger and refuses claims it
        // cannot account for (see sv_health.h)
        extern int g_sv_health_authority;
        extern float g_sv_health_regen;
        extern float g_sv_health_damage_scale;
        extern float g_sv_health_bone_floor;
        CMD4(CCC_Integer, "sv_health_authority", &g_sv_health_authority, 0, 2);
        CMD4(CCC_Float, "sv_health_regen", &g_sv_health_regen, 0.f, 10.f);
        CMD4(CCC_Float, "sv_health_damage_scale", &g_sv_health_damage_scale, 0.f, 100.f);
        CMD4(CCC_Float, "sv_health_bone_floor", &g_sv_health_bone_floor, 0.f, 1.f);
        CMD1(CCC_HealthDump, "sv_health_dump");
        CMD1(CCC_HealthDrain, "sv_health_drain");
        CMD1(CCC_HitTest, "sv_hit_test");
        CMD1(CCC_PlaceTest, "sv_place_test");
        CMD1(CCC_ArmourAudit, "sv_armour_audit");
        CMD1(CCC_BeltAudit, "sv_belt_audit");
        CMD1(CCC_DamageAudit, "sv_damage_audit");
        CMD1(CCC_WeaponAudit, "sv_weapon_audit");
        CMD1(CCC_CapAudit, "sv_cap_audit");
    }

    // server-driven NPCs: the server's handle on a creature (see sv_npc.h)
    {
        CMD1(CCC_NpcSpawn, "sv_npc_spawn");
        CMD1(CCC_DbgBones, "dbg_bones");
        CMD1(CCC_NpcList, "sv_npc_list");
        CMD1(CCC_NpcDel, "sv_npc_del");
        CMD1(CCC_NpcTeleport, "sv_npc_tp");
        CMD1(CCC_NpcMove, "sv_npc_move");
        CMD1(CCC_NpcFollow, "sv_npc_follow");
        CMD1(CCC_NpcPatrol, "sv_npc_patrol");
        CMD1(CCC_NpcGuard, "sv_npc_guard");
        CMD1(CCC_NpcStop, "sv_npc_stop");
        CMD1(CCC_NpcFace, "sv_npc_face");
        CMD1(CCC_NpcGive, "sv_npc_give");
        CMD1(CCC_NpcGear, "sv_npc_gear");
        CMD1(CCC_NpcStrip, "sv_npc_strip");
        CMD1(CCC_NpcTemplates, "sv_npc_templates");
        CMD1(CCC_NpcModels, "sv_npc_models");
        CMD1(CCC_NpcArmour, "sv_npc_armour");
        CMD1(CCC_NpcVisual, "sv_npc_visual");
        CMD1(CCC_Revive, "sv_revive");
        CMD1(CCC_Orphans, "sv_orphans");
        CMD1(CCC_OrphanMake, "sv_orphan_make");
        {
            // ⚠Turns the null dereference back on, so one binary can run both
            // arms of the experiment. An instrument, so it must never persist:
            // a gate saved into user.ltx comes back next session as a mystery.
            extern int g_dbg_orphan_unguarded;
            static CCC_Integer unguarded("dbg_orphan_unguarded", &g_dbg_orphan_unguarded, 0, 1);
            unguarded.SetCanSave(false);
            Console->AddCommand(&unguarded);
        }

        // Client-side: the weapon in the player's own hands.
        CMD1(CCC_DbgWeapon, "dbg_wpn");
        CMD4(CCC_Integer, "dbg_wpn_trace", &g_dbg_wpn_trace, 0, 1);
        {
            // An instrument, so it does not persist -- see the rule above the
            // sv_npc knobs below.
            static CCC_Integer death("dbg_death", &g_dbg_death, 0, 1);
            death.SetCanSave(false);
            Console->AddCommand(&death);
        }
        {
            static CCC_Integer stamina("dbg_stamina", &g_dbg_stamina, 0, 1);
            stamina.SetCanSave(false);
            Console->AddCommand(&stamina);
        }
        CMD1(CCC_SpawnPrefetchList, "dbg_spawn_prefetch_list");
        // read-ahead of objects about to come online (Level_network_spawn.cpp); MB budget, 0 = off
        CMD4(CCC_Integer, "spawn_prefetch_mb", &g_spawn_prefetch_mb, 0, 256);
        CMD4(CCC_Integer, "spawn_prefetch_margin", &g_spawn_prefetch_margin, 0, 300);
        // the level's NPC models, animations and stalker animation tables built at load (Level_network_spawn.cpp); 0 = off
        CMD4(CCC_Integer, "npc_prewarm", &g_npc_prewarm, 0, 1);
        // a stalker's inventory spawns in the frames after his own, at most this many us per frame (Level_network_spawn.cpp); 0 = stock
        CMD4(CCC_Integer, "spawn_items_budget_us", &g_spawn_items_budget_us, 0, 100000);
        // a level load reads its files ahead in the order of the last load (xrCore/fs_load_prefetch.h); MB ahead, 0 = off
        CMD4(CCC_Integer, "load_prefetch_mb", &LoadPrefetch::g_mb, 0, 512);
        CMD4(CCC_Integer, "load_prefetch_early", &LoadPrefetch::g_early, 0, 1);
        CMD4(CCC_Integer, "load_prefetch_core", &LoadPrefetch::g_core, 0, 2);
        // vertex cap of the weapon (object handler) planner's search; a no-plan search runs all of it
        CMD4(CCC_Integer, "ai_oh_search_cap", &g_oh_search_cap, 100, 8000);
        // foot IK: in view up to ph_ik_dist m, out of view up to ph_ik_near_dist (Switch preset 30 / 10; game 100 / 20)
        CMD4(CCC_Float, "ph_ik_dist", &g_ik_calc_dist, 0.f, 200.f);
        extern int g_ik_mt; // IKLimbsController.cpp: the legs of all creatures in one batch over cores 0-2; 0 = each in its own UpdateCL
        CMD4(CCC_Integer, "ph_ik_mt", &g_ik_mt, 0, 1);
        CMD4(CCC_Float, "ph_ik_near_dist", &g_ik_always_calc_dist, 0.f, 200.f);
        CMD4(CCC_Float, "ph_ik_lod_dist", &g_ik_lod_dist, 0.f, 200.f); // 12-10-2026: legs every other frame beyond (0 = off)
        extern int g_ph_activate_stall; // PHActivationShape.cpp: a dropped item's spawn correction gives up after N steps without progress; 0 = stock
        CMD4(CCC_Integer, "ph_activate_stall", &g_ph_activate_stall, 0, 150);
        extern int g_sched_prof_ms; // ai_stalker.cpp: [sched prof] threshold, ms
        CMD4_NOSAVE(CCC_Integer, "al_sched_prof_ms", &g_sched_prof_ms, 1, 1000);
        {
            // the value is the radius around the player in metres, 0 = off
            static CCC_Integer tactics("dbg_npc_tactics", &g_dbg_npc_tactics, 0, 500);
            tactics.SetCanSave(false);
            Console->AddCommand(&tactics);
        }
        {
            // the combat AI drawn into the world (ai_debug_draw.h): a mask, 0 = off;
            // 1 roles, 2 moves, 4 houses, 8 candidate points, 16 the threat map. Not saved: a debug view.
            static CCC_Integer draw("ai_dbg_draw", &AIDebugDraw::g_mask, 0, 31);
            draw.SetCanSave(false);
            Console->AddCommand(&draw);
            static CCC_Float range("ai_dbg_draw_range", &AIDebugDraw::g_range, 5.f, 300.f);
            range.SetCanSave(false);
            Console->AddCommand(&range);
            CMD1(CCC_AiTest, "ai_test");
            CMD1(CCC_PerfRoute, "perf_route");
            CMD1(CCC_ActorTorch, "actor_torch");
            CMD1(CCC_ActorSlot, "actor_slot");
            // the combat AI v2 (ai_position_query.h): 0 = the old pickers. Not saved:
            // the default stays the old path until the autotest says otherwise.
            static CCC_Integer v2("ai_tactics_v2", &PositionQuery::g_ai_tactics_v2, 0, 1);
            v2.SetCanSave(false);
            Console->AddCommand(&v2);
            static CCC_Float budget("ai_pq_budget_ms", &PositionQuery::g_budget_ms, .1f, 10.f);
            budget.SetCanSave(false);
            Console->AddCommand(&budget);
            CMD1(CCC_AiPqStats, "ai_pq_stats");
            // the threat map (ai_threat_map.h) under ai_tactics_v2: 0 = "where he is" is the
            // freshest thing anybody remembers, as before. Not saved, as ai_tactics_v2.
            static CCC_Integer threat("ai_threat_map", &ThreatMap::g_on, 0, 1);
            threat.SetCanSave(false);
            Console->AddCommand(&threat);
        }
        {
            // leaning out from behind cover, CAI_Stalker::update_lean: the roll of a full lean, 0 = off
            extern float g_ai_lean_deg;
            extern int g_ai_lean_force;
            // script evaluators answering from memory (script_property_evaluator_wrapper.cpp)
            {
                extern char g_eval_cache_list[512];
                extern int g_eval_cache_ms;
                extern u32 g_eval_cache_gen;
                class CCC_EvalCache : public CCC_String
                {
                public:
                    CCC_EvalCache(LPCSTR N, LPSTR V, int sz) : CCC_String(N, V, sz) {}
                    void Execute(LPCSTR args) override
                    {
                        CCC_String::Execute(args);
                        ++g_eval_cache_gen;
                    }
                };
                static CCC_EvalCache ec("ai_eval_cache", g_eval_cache_list, sizeof(g_eval_cache_list));
                Console->AddCommand(&ec);
                CMD4(CCC_Integer, "ai_eval_cache_ms", &g_eval_cache_ms, 0, 2000);
            }
            // AI LOD: a stalker off screen thinks less often (CAI_Stalker::shedule_Scale)
            extern int g_ai_think_lod;
            extern float g_ai_think_lod_scale, g_ai_think_lod_near;
            CMD4(CCC_Integer, "ai_think_lod", &g_ai_think_lod, 0, 1);
            // every online object's state into its server entity once in N frames, staggered (CLevel::ClientSend)
            extern int g_net_export_div;
            CMD4(CCC_Integer, "net_export_div", &g_net_export_div, 1, 8);
            CMD4(CCC_Float, "ai_think_lod_scale", &g_ai_think_lod_scale, 0.f, 1.f);
            CMD4(CCC_Float, "ai_think_lod_near", &g_ai_think_lod_near, 0.f, 100.f);
            CMD4(CCC_Float, "ai_lean_deg", &g_ai_lean_deg, 0.f, 45.f);
            // how long a stalker takes to make the player out, CVisualMemoryManager::actor_visible_value
            extern float g_ai_spot_time_per_m, g_ai_spot_time_min, g_ai_spot_calm_factor, g_ai_spot_night_factor;
            CMD4(CCC_Float, "ai_spot_time_per_m", &g_ai_spot_time_per_m, 0.f, 1.f); // 0 = stock (instant in a fight)
            CMD4(CCC_Float, "ai_spot_time_min", &g_ai_spot_time_min, 0.f, 5.f);
            CMD4(CCC_Float, "ai_spot_calm_factor", &g_ai_spot_calm_factor, 1.f, 10.f);
            CMD4(CCC_Float, "ai_spot_night_factor", &g_ai_spot_night_factor, 1.f, 5.f);
            // the player's body sounds (steps, brushing past) crouched: this much of their power
            extern float g_ai_hear_crouch_factor, g_ai_spot_crouch_factor;
            CMD4(CCC_Float, "ai_spot_crouch_factor", &g_ai_spot_crouch_factor, 1.f, 5.f);
            CMD4(CCC_Float, "ai_hear_crouch_factor", &g_ai_hear_crouch_factor, 0.f, 1.f);
            // how far stalkers hear the player, a share of the stock range (CSoundMemoryManager::hear_range):
            // his body sounds for all, again for one who has not noticed him, his shots; and the
            // distance at which one who has not noticed him can make him out at all (0 = no limit)
            extern float g_ai_hear_range, g_ai_hear_unaware_range, g_ai_hear_shot_range, g_ai_aware_time, g_ai_spot_unaware_range;
            CMD4_NOSAVE(CCC_Float, "ai_hear_range", &g_ai_hear_range, 0.f, 1.f);
            CMD4_NOSAVE(CCC_Float, "ai_hear_unaware_range", &g_ai_hear_unaware_range, 0.f, 1.f);
            CMD4_NOSAVE(CCC_Float, "ai_hear_shot_range", &g_ai_hear_shot_range, 0.f, 1.f);
            CMD4_NOSAVE(CCC_Float, "ai_aware_time", &g_ai_aware_time, 0.f, 600.f);
            CMD4_NOSAVE(CCC_Float, "ai_spot_unaware_range", &g_ai_spot_unaware_range, 0.f, 200.f);
            // an NPC bullet in the player's head counts at most this (actor.ltx has 2.5 for PvP); 0 = as in actor.ltx
            extern float g_ai_head_scale_vs_player;
            CMD4(CCC_Float, "ai_head_scale_vs_player", &g_ai_head_scale_vs_player, 0.f, 5.f);
            // the accuracy curve: first shots at a player just spotted miss by this many metres, closing over the ramp time
            extern float g_ai_aim_first_miss_m, g_ai_aim_ramp_time, g_ai_aim_reset_time;
            // ⚠not saved: a tuning value written into user.ltx would outlive the default it was tried against
            CMD4_NOSAVE(CCC_Float, "ai_aim_first_miss_m", &g_ai_aim_first_miss_m, 0.f, 5.f);
            CMD4_NOSAVE(CCC_Float, "ai_aim_ramp_time", &g_ai_aim_ramp_time, 0.f, 10.f);
            CMD4_NOSAVE(CCC_Float, "ai_aim_reset_time", &g_ai_aim_reset_time, 0.f, 10.f);
            // footsteps as the player hears them (the AI hears the same as before): NPC louder and further, his own quieter; 1 = stock
            extern float g_snd_npc_step_volume, g_snd_npc_step_range, g_snd_actor_step_volume;
            CMD4_NOSAVE(CCC_Float, "snd_npc_step_volume", &g_snd_npc_step_volume, 0.f, 10.f);
            CMD4_NOSAVE(CCC_Float, "snd_npc_step_range", &g_snd_npc_step_range, .1f, 5.f);
            CMD4_NOSAVE(CCC_Float, "snd_actor_step_volume", &g_snd_actor_step_volume, 0.f, 2.f);
            // rain hides the player's sounds from the AI: at full rain his steps carry 1 - this much
            extern float g_ai_hear_rain_mask, g_ai_hear_rain_mask_shot;
            CMD4_NOSAVE(CCC_Float, "ai_hear_rain_mask", &g_ai_hear_rain_mask, 0.f, 1.f);
            CMD4_NOSAVE(CCC_Float, "ai_hear_rain_mask_shot", &g_ai_hear_rain_mask_shot, 0.f, 1.f);
            // being shot at (ActorSuppression): the pressure one point-blank bullet adds, how long it
            // holds and drains, the camera flinch and tremor in degrees, the extra spread at full pressure
            extern float g_suppress_gain, g_suppress_hold, g_suppress_decay, g_suppress_flinch_deg, g_suppress_tremor_deg, g_suppress_disp;
            CMD4_NOSAVE(CCC_Float, "g_suppress_gain", &g_suppress_gain, 0.f, 2.f);
            CMD4_NOSAVE(CCC_Float, "g_suppress_hold", &g_suppress_hold, 0.f, 5.f);
            CMD4_NOSAVE(CCC_Float, "g_suppress_decay", &g_suppress_decay, .01f, 5.f);
            CMD4_NOSAVE(CCC_Float, "g_suppress_flinch", &g_suppress_flinch_deg, 0.f, 10.f);
            CMD4_NOSAVE(CCC_Float, "g_suppress_tremor", &g_suppress_tremor_deg, 0.f, 5.f);
            CMD4_NOSAVE(CCC_Float, "g_suppress_disp", &g_suppress_disp, 0.f, 5.f);
            // the pad's aim assist (actor_aim_assist.h): the switch and the strength are the player's and saved, the rest is tuning
            extern int g_aim_assist, g_aa_dbg;
            extern float g_aa_strength, g_aa_zone_m, g_aa_zone_ads_m, g_aa_zone_min_deg, g_aa_zone_max_deg, g_aa_range, g_aa_friction, g_aa_track, g_aa_pull,
                g_aa_snap_deg, g_aa_snap_time, g_aa_bullet_m, g_aa_bullet_max_deg;
            CMD4(CCC_Integer, "aim_assist", &g_aim_assist, 0, 2);
            CMD4(CCC_Float, "aim_assist_strength", &g_aa_strength, 0.f, 2.f);
            CMD4_NOSAVE(CCC_Float, "aa_zone", &g_aa_zone_m, 0.f, 5.f);
            CMD4_NOSAVE(CCC_Float, "aa_zone_ads", &g_aa_zone_ads_m, 0.f, 5.f);
            CMD4_NOSAVE(CCC_Float, "aa_zone_min", &g_aa_zone_min_deg, 0.f, 30.f);
            CMD4_NOSAVE(CCC_Float, "aa_zone_max", &g_aa_zone_max_deg, 0.f, 45.f);
            CMD4_NOSAVE(CCC_Float, "aa_range", &g_aa_range, 5.f, 300.f);
            CMD4_NOSAVE(CCC_Float, "aa_friction", &g_aa_friction, 0.f, 1.f);
            CMD4_NOSAVE(CCC_Float, "aa_track", &g_aa_track, 0.f, 1.f);
            CMD4_NOSAVE(CCC_Float, "aa_pull", &g_aa_pull, 0.f, 60.f);
            CMD4_NOSAVE(CCC_Float, "aa_snap", &g_aa_snap_deg, 0.f, 30.f);
            CMD4_NOSAVE(CCC_Float, "aa_snap_time", &g_aa_snap_time, .01f, 1.f);
            CMD4_NOSAVE(CCC_Float, "aa_bullet", &g_aa_bullet_m, 0.f, 3.f);
            CMD4_NOSAVE(CCC_Float, "aa_bullet_max", &g_aa_bullet_max_deg, 0.f, 10.f);
            CMD4_NOSAVE(CCC_Integer, "aa_dbg", &g_aa_dbg, 0, 1);
            CMD1(CCC_AaTest, "aa_test");
            // suppressing fire (CStalkerCombatPlanner::update_suppression): how long after the squad
            // lost sight of the player one of them keeps firing at where he went to ground; 0 = off
            extern float g_ai_suppress_time;
            CMD4_NOSAVE(CCC_Float, "ai_suppress_time", &g_ai_suppress_time, 0.f, 20.f);
            // every stalker leans this many degrees, + = right: to check the direction and the looks
            static CCC_Integer force("dbg_npc_lean", &g_ai_lean_force, -45, 45);
            force.SetCanSave(false);
            Console->AddCommand(&force);
        }
        {
            // The raw terms behind the intent block's velocity, on the server
            // that measures it: interval, distance, quotient. Off by default --
            // it is one line per creature per 61 samples and only means
            // anything when the wire and the picture disagree.
            extern int g_dbg_intent;
            static CCC_Integer intent("dbg_intent", &g_dbg_intent, 0, 1);
            intent.SetCanSave(false);
            Console->AddCommand(&intent);
        }
        {
            // How far the barrel misses where the creature is looking, in
            // degrees. The number that says whether aiming works, and the one
            // nobody could produce before the servo measured it.
            extern int g_dbg_aim;
            static CCC_Integer aim("dbg_aim", &g_dbg_aim, 0, 1);
            aim.SetCanSave(false);
            Console->AddCommand(&aim);
        }

        // Items in the world and the ownership ledger.
        CMD1(CCC_ItemDrop, "sv_item_drop");
        CMD1(CCC_ItemGive, "sv_item_give");
        CMD1(CCC_ItemList, "sv_item_list");
        CMD1(CCC_ItemReport, "sv_item_report");

        // ⚠Neither of the two knobs below may be saved. A measurement knob that
        // outlives the measurement silently becomes a default -- and one of
        // these decides whether the server refuses a pickup, which is the last
        // setting that should ever be inherited from a forgotten test session.
        extern int g_sv_loot_log;
        extern float g_sv_loot_max_dist;
        {
            static CCC_Integer c("sv_loot_log", &g_sv_loot_log, 0, 1);
            c.SetCanSave(false);
            Console->AddCommand(&c);
        }
        {
            static CCC_Float c("sv_loot_max_dist", &g_sv_loot_max_dist, 0.f, 1000.f);
            c.SetCanSave(false);
            Console->AddCommand(&c);
        }
        CMD1(CCC_NpcLoadout, "sv_npc_loadout");
        CMD1(CCC_NpcSpeed, "sv_npc_speed");

        // ⚠None of the three below persist, and that is deliberate.
        //
        // The console writes every settable command into user.ltx on exit, so
        // a value typed once during a test session silently becomes the
        // default for every later run -- which has already cost this project a
        // bug report ("ping problems, an invisible wall") that turned out to be
        // a test-only movement cap left behind in user.ltx, and cost an hour
        // here when a stale sv_field_join quietly overrode a freshly measured
        // default. A diagnostic knob that outlives the diagnosis is a trap, so
        // these are set for a session and forgotten.
        extern int g_sv_npc_trace;
        extern int g_sv_npc_own_motion;
        extern int g_sv_npc_watch;
        {
            // 1 = one line a second per moving puppet: where it is, which
            // waypoint it is on, how much of the journey it has closed.
            static CCC_Integer trace("sv_npc_trace", &g_sv_npc_trace, 0, 1);
            trace.SetCanSave(false);
            Console->AddCommand(&trace);
        }
        {
            // 1 = the server owns a driven puppet's position, 0 = the
            // creature's own simulation keeps it (and takes two thirds of the
            // ordered speed back). An A/B switch, not a preference -- and one
            // whose stale 0 would look exactly like the bug it proved.
            static CCC_Integer own("sv_npc_own_motion", &g_sv_npc_own_motion, 0, 1);
            own.SetCanSave(false);
            Console->AddCommand(&own);
        }
        {
            // Entity id whose position to sample at every point in the frame
            // that could be writing it; -1 for nobody. See SVNpc::WatchPos.
            static CCC_Integer watch("sv_npc_watch", &g_sv_npc_watch, -1, 65535);
            watch.SetCanSave(false);
            Console->AddCommand(&watch);
        }
        {
            // Entity id whose accepted snapshots to print, with a budget that
            // outlasts a recording window; 0 = the old capped all-entity diag.
            // A fight exhausts that cap in seconds, and an exhausted detector
            // reads as a clean result -- see the note over g_net_foreign_yaw.
            extern int g_cl_snap_watch;
            static CCC_Integer snapw("cl_snap_watch", &g_cl_snap_watch, 0, 65535);
            snapw.SetCanSave(false);
            Console->AddCommand(&snapw);
        }
        {
            // Which cycle was WANTED, whether it played, and whether it then
            // did what was asked. See cl_anim_ledger.h for the four ways a
            // request dies silently on this path.
            static CCC_Integer ask("cl_anim_ask", &ClAnimLedger::g_ask_id, 0, 65535);
            ask.SetCanSave(false);
            Console->AddCommand(&ask);
        }
        CMD1(CCC_AnimAskDump, "cl_anim_ask_dump");
        CMD1(CCC_MotionAudit, "sv_motion_audit");
        CMD1(CCC_NavmeshCheck, "sv_navmesh_check");
        CMD1(CCC_Accuracy, "sv_accuracy");
        CMD1(CCC_HitTrail, "sv_hit_trail");
        CMD1(CCC_HitTypes, "sv_hit_types");
        {
            // Names what the per-entity export skips for having no owner, and
            // whether the server is simulating it. See xrServer.cpp.
            extern int g_sv_export_census;
            static CCC_Integer census("sv_export_census", &g_sv_export_census, 0, 1);
            census.SetCanSave(false);
            Console->AddCommand(&census);
        }
        {
            // Whether a spawn a network client asks for is carried out at all.
            // The long version is in xrServer_process_spawn.cpp; the short one is
            // that the stock gate only ever saw an in-process client, so with a
            // real one connected every client spawn was discarded in silence --
            // and a thrown bolt is a client spawn.
            //
            // ⚠Not savable. It is an authority switch, and this build has lost
            // three sessions to authority switches that drifted into user.ltx
            // (sv_interest_radius, sv_auth_required, sv_npc_damage_scale).
            extern int g_sv_client_spawns;
            static CCC_Integer clsp("sv_client_spawns", &g_sv_client_spawns, 0, 1);
            clsp.SetCanSave(false);
            Console->AddCommand(&clsp);
        }
        CMD1(CCC_BoneAudit, "cl_bone_audit");
        CMD1(CCC_ForeignReset, "cl_foreign_reset");
        CMD1(CCC_Stalkers, "cl_stalkers");
    }

    // What the animator chose, and what it chose it from. The measurement that
    // makes "identical to single player" a claim instead of an opinion -- see
    // stalker_animation_trace.cpp.
    {
        extern int g_cl_anim_trace;
        extern int g_cl_anim_trace_id;
        {
            // 1 = every frame; 2 = only when a part changes clip (the sequence
            // of choices, short enough to diff a whole session)
            static CCC_Integer t("cl_anim_trace", &g_cl_anim_trace, 0, 2);
            t.SetCanSave(false);
            Console->AddCommand(&t);
        }
        {
            static CCC_Integer t("cl_anim_trace_id", &g_cl_anim_trace_id, -1, 65535);
            t.SetCanSave(false);
            Console->AddCommand(&t);
        }
        {
            // 1 = a networked puppet is animated by the engine's own
            // CStalkerAnimationManager, fed from replicated state; 0 = the
            // hand-rolled cycle selector it replaces. Both are in the build so
            // the difference can be seen without one.
            extern int g_cl_anim_native;
            static CCC_Integer t("cl_anim_native", &g_cl_anim_native, 0, 1);
            t.SetCanSave(false);
            Console->AddCommand(&t);
        }
        {
            // 1 = the interpolator picks the buffered pair that brackets its
            // render time; 0 = the shipped behaviour, which held only the
            // newest two and therefore clamped instead of interpolating.
            extern int g_cl_net_interp_ring;
            static CCC_Integer t("cl_net_interp_ring", &g_cl_net_interp_ring, 0, 1);
            t.SetCanSave(false);
            Console->AddCommand(&t);
        }
        {
            // 1 = the legs are a continuous blend space; 0 = one cycle at a
            // time out of a speed band. Wins over cl_anim_native when both are
            // on -- see cl_anim_blendspace.h.
            extern int g_cl_anim_blend;
            static CCC_Integer t("cl_anim_blend", &g_cl_anim_blend, 0, 1);
            t.SetCanSave(false);
            Console->AddCommand(&t);
        }
        {
            // 1 = animate from what the server SAYS the creature is doing; 0 =
            // from what the rendered path suggests it is doing. The A/B for the
            // whole intent channel (SNetCreatureIntent).
            extern int g_cl_net_intent;
            static CCC_Integer t("cl_net_intent", &g_cl_net_intent, 0, 1);
            t.SetCanSave(false);
            Console->AddCommand(&t);
        }
        {
            // 1 = play the clips the SERVER's animator chose (SNetAnimState),
            // when it sends them; 0 = the local selectors above even then. The
            // A/B between the real animator and our guess at it.
            extern int g_cl_net_anim;
            static CCC_Integer t("cl_net_anim", &g_cl_net_anim, 0, 1);
            t.SetCanSave(false);
            Console->AddCommand(&t);
        }
        {
            // 1 = the barrel is servoed onto the look direction; 0 = the
            // shipped fixed percentages alone, which is "roughly towards".
            extern int g_cl_aim_ik;
            static CCC_Integer t("cl_aim_ik", &g_cl_aim_ik, 0, 1);
            t.SetCanSave(false);
            Console->AddCommand(&t);
        }
        CMD1(CCC_RootMotion, "cl_anim_rootmotion");
    }

    // the level as the server means it to be played (see sv_scenario.h)
    {
        CMD1(CCC_Fields, "sv_fields");

        // How anomalies are gathered into fields. Level-dependent -- run
        // sv_fields, see whether the answer matches the map, adjust. Not
        // persisted, for the reason given above the sv_npc knobs: the default
        // here was measured, and a value left in user.ltx from a tuning
        // session overriding it silently is worse than retyping it.
        extern float g_sv_field_join;
        extern int g_sv_field_min_zones;
        {
            static CCC_Float join("sv_field_join", &g_sv_field_join, 5.f, 200.f);
            join.SetCanSave(false);
            Console->AddCommand(&join);
        }
        {
            static CCC_Integer minz("sv_field_min_zones", &g_sv_field_min_zones, 1, 50);
            minz.SetCanSave(false);
            Console->AddCommand(&minz);
        }

        CMD1(CCC_Scenario, "sv_scenario");
        CMD1(CCC_ScenarioStatus, "sv_scenario_status");
        CMD1(CCC_ScenarioClear, "sv_scenario_clear");
    }

    // Anomalies the server puts on the map (see sv_anomaly.h). The other half
    // of the loop sv_scenario started: it finds the fields a level shipped
    // with, these make new ones.
    {
        CMD1(CCC_AnomalyKinds, "sv_anomaly_kinds");
        CMD1(CCC_AnomalyList, "sv_anomaly_list");
        CMD1(CCC_AnomalySpawn, "sv_anomaly_spawn");
        CMD1(CCC_AnomalySize, "sv_anomaly_size");
        CMD1(CCC_AnomalyArt, "sv_anomaly_art");
        CMD1(CCC_AnomalyDel, "sv_anomaly_del");
        CMD1(CCC_AnomalySave, "sv_anomaly_save");
        CMD1(CCC_AnomalyLoad, "sv_anomaly_load");
        CMD1(CCC_AnomalyBake, "sv_anomaly_bake");
        CMD1(CCC_AnomalyBirthStatus, "sv_anomaly_birth_status");

        // How fast a field produces, and how much it may be sitting on.
        //
        // ⚠Not persisted, for the reason established across the sv_* knobs: a
        // value left in user.ltx from a tuning session silently overriding a
        // measured default is worse than retyping it -- and this pair decides
        // how much wealth the world manufactures per hour, which is not
        // something to inherit by accident. See gate-cvars-must-not-be-savable.
        extern int g_sv_anomaly_birth;
        extern int g_sv_anomaly_birth_cap;
        {
            static CCC_Integer birth("sv_anomaly_birth", &g_sv_anomaly_birth, 0, 86400);
            birth.SetCanSave(false);
            Console->AddCommand(&birth);
        }
        {
            static CCC_Integer cap("sv_anomaly_birth_cap", &g_sv_anomaly_birth_cap, 0, 64);
            cap.SetCanSave(false);
            Console->AddCommand(&cap);
        }
    }

    // ...and the furniture (see sv_prop.h). The anomalies are what the world
    // does to you; these are what makes a patch of it somewhere rather than
    // anywhere.
    {
        CMD1(CCC_PropModels, "sv_prop_models");
        CMD1(CCC_PropKinds, "sv_prop_kinds");
        CMD1(CCC_PropList, "sv_prop_list");
        CMD1(CCC_PropSpawn, "sv_prop_spawn");
        CMD1(CCC_PropLamp, "sv_prop_lamp");
        CMD1(CCC_PropCampfire, "sv_prop_campfire");
        CMD1(CCC_PropClone, "sv_prop_clone");
        CMD1(CCC_PropMove, "sv_prop_move");
        CMD1(CCC_PropTurn, "sv_prop_turn");
        CMD1(CCC_PropLift, "sv_prop_lift");
        CMD1(CCC_PropDel, "sv_prop_del");
        CMD1(CCC_PropSave, "sv_prop_save");
        CMD1(CCC_PropLoad, "sv_prop_load");
        CMD1(CCC_PropBake, "sv_prop_bake");
    }

    // How fast, and how faithfully, a shot becomes a record (shot_latency.h).
    {
        {
            // A trace switch left on in user.ltx would write a line per bullet
            // forever, and the first symptom would be a log nobody can read.
            // Same rule as the sv_npc knobs above: an instrument does not
            // persist. See anticheat-testcvars-leaked-into-userltx.
            static CCC_Integer trace("shot_trace", &ShotLatency::g_shot_trace, 0, 1);
            trace.SetCanSave(false);
            Console->AddCommand(&trace);
        }
        CMD1(CCC_ShotReport, "shot_report");
        CMD1(CCC_ShotReset, "shot_reset");
        // Creature health the server owns outright. Persisted, unlike the
        // instruments above: this is how the host is configured to behave, not
        // a switch flipped for one measurement.
        CMD4(CCC_Integer, "sv_npc_health", &SVNpcHealth::g_sv_npc_health, 0, 2);
        CMD1(CCC_NpcHealthDump, "sv_npc_health_dump");
        {
            // How much damage a shot is allowed to claim. Persisted, like
            // sv_npc_health and unlike the instruments: this is how the host is
            // configured to behave. Default 1 (measure, do not clamp) until the
            // margin has been watched on real play -- see sv_move_validate for
            // what enforcing blind cost last time.
            extern int g_sv_hit_validate;
            extern float g_sv_hit_slack;
            extern float g_sv_npc_damage_scale;
            CMD4(CCC_Integer, "sv_hit_validate", &g_sv_hit_validate, 0, 2);
            CMD4(CCC_Float, "sv_hit_slack", &g_sv_hit_slack, 0.f, 1.f);
            // How much of a shot a server-simulated creature takes. Persisted:
            // this is the world's balance, not a measurement.
            CMD4(CCC_Float, "sv_npc_damage_scale", &g_sv_npc_damage_scale, 0.05f, 5.f);
        }
        CMD1(CCC_NpcDamageTest, "sv_npc_damage_test");
        CMD1(CCC_NpcAttack, "sv_npc_attack");
        CMD1(CCC_HitFake, "sv_hit_fake");
        CMD1(CCC_NpcWhy, "sv_npc_why");
        CMD1(CCC_NpcWhyWatch, "sv_npc_why_watch");
        CMD1(CCC_NpcMind, "sv_npc_mind");
        CMD1(CCC_NpcMindWatch, "sv_npc_mind_watch");
        CMD1(CCC_Bones, "sv_bones");
        CMD1(CCC_FireTest, "cl_fire_test");
        CMD1(CCC_FireStop, "cl_fire_stop");
        CMD1(CCC_WaterScan, "dbg_water_scan");
        CMD1(CCC_FireStatus, "cl_fire_status");
        CMD1(CCC_BoltTest, "cl_bolt_test");
        CMD1(CCC_BoltStop, "cl_bolt_stop");
        CMD1(CCC_BoltStatus, "cl_bolt_status");
        {
            // The true volume of nearby zones. See cl_zone_debug.h: the drawn
            // anomaly and its collision shape are unrelated sizes, and a 6 m
            // ball that looks like 2 m is an invisible trap.
            //
            // ⚠Not savable. It is a diagnostic, and a diagnostic that drifts
            // into user.ltx is a second setting nobody can see -- the trap that
            // has cost this build several sessions.
            extern int g_cl_zone_debug;
            extern float g_cl_zone_debug_range;
            static CCC_Integer zdbg("cl_zone_debug", &g_cl_zone_debug, 0, 2);
            zdbg.SetCanSave(false);
            Console->AddCommand(&zdbg);

            static CCC_Float zrange("cl_zone_debug_range", &g_cl_zone_debug_range, 5.f, 1000.f);
            zrange.SetCanSave(false);
            Console->AddCommand(&zrange);

            // Metres between hatch lines at hit power 1.0 -- a fiercer anomaly
            // divides this down and so weaves tighter. 0 turns the fill off and
            // leaves the outline.
            extern float g_cl_zone_debug_hatch;
            static CCC_Float zhatch("cl_zone_debug_hatch", &g_cl_zone_debug_hatch, 0.f, 10.f);
            zhatch.SetCanSave(false);
            Console->AddCommand(&zhatch);

            // Which shader/texture pair the lines are drawn with. This is a
            // measurement, not a preference: the UI multiplies vertex colour by
            // the shader's texture, and a shader that resolves none gets the
            // renderer's "white" fallback -- which is actually a dark grey, so
            // the lines come out black. cl_zone_debug 3 shows one bar per
            // candidate; pick the one that is yellow.
            extern int g_cl_zone_debug_shader;
            static CCC_Integer zshader("cl_zone_debug_shader", &g_cl_zone_debug_shader, 0, 3);
            zshader.SetCanSave(false);
            Console->AddCommand(&zshader);

            // Line thickness in pixels. Real thickness, because the lines are
            // screen-space quads: the Vulkan UI pipeline's LINE_LIST is nailed
            // to lineWidth 1.0 with no dynamic state.
            extern float g_cl_zone_debug_width;
            static CCC_Float zwidth("cl_zone_debug_width", &g_cl_zone_debug_width, 0.5f, 12.f);
            zwidth.SetCanSave(false);
            Console->AddCommand(&zwidth);

            CMD1(CCC_ZoneDebugDump, "cl_zone_debug_dump");
            CMD1(CCC_ZoneEdit, "ed_zone");
            CMD1(CCC_ZoneEditSection, "ed_zone_section");
            CMD1(CCC_ZonePicker, "ed_zone_picker");

            // ...and the same three for furniture. See cl_prop_edit.h.
            CMD1(CCC_PropEdit, "ed_prop");
            CMD1(CCC_PropEditModel, "ed_prop_model");
            CMD1(CCC_PropPicker, "ed_prop_picker");

            // Whether the ghost is the model itself or only its outline. Not
            // saveable, like every other knob in here: an operator tool must not
            // write itself into the player's user.ltx.
            static CCC_Integer pghost("ed_prop_ghost", &ClPropEdit::ghost_solid, 0, 1);
            pghost.SetCanSave(false);
            Console->AddCommand(&pghost);

            // Whether placing follows the ground's slope or the world's axes.
            static CCC_Integer palign("ed_prop_align", &ClPropEdit::ghost_align, 0, 1);
            palign.SetCanSave(false);
            Console->AddCommand(&palign);

            // Whether one prop may be put inside another.
            static CCC_Integer pclear("ed_prop_clearance", &ClPropEdit::clearance, 0, 1);
            pclear.SetCanSave(false);
            Console->AddCommand(&pclear);

            // What a placed light is. Reach in metres (a room is about six), cone
            // in degrees (0 makes it a point light), shadows on or off (⚠off is
            // what makes a light cross walls), and tone 0 flame / 1 warm / 2 cold.
            static CCC_Integer prange("ed_prop_light_range", &ClPropEdit::light_range, 1, 15);
            prange.SetCanSave(false);
            Console->AddCommand(&prange);

            static CCC_Integer pcone("ed_prop_light_cone", &ClPropEdit::light_cone, 0, 170);
            pcone.SetCanSave(false);
            Console->AddCommand(&pcone);

            static CCC_Integer pshadow("ed_prop_light_shadow", &ClPropEdit::light_shadow, 0, 1);
            pshadow.SetCanSave(false);
            Console->AddCommand(&pshadow);

            static CCC_Integer ptone("ed_prop_light_tone", &ClPropEdit::light_tone, 0, 2);
            ptone.SetCanSave(false);
            Console->AddCommand(&ptone);
        }
    }

    // server frame budget: the tick rate IS the update rate (see sv_perf.h)
    {
        CMD1(CCC_SvPerf, "sv_perf");
        CMD1(CCC_SvPerfReset, "sv_perf_reset");
    }

    // interest management: how far around itself a client is told about the world
    {
        // ⚠⚠NOT SAVEABLE, and this is not tidiness -- it cost the whole world.
        //
        // Measured 30-07-2026: a `sv_interest_radius 300.` left behind in
        // user.ltx by some earlier measurement had been silently in force ever
        // since, and because the feature filters updates without spawning an
        // entity that ENTERS the radius, a client only ever knew the entities
        // that happened to be near it at connect time. Its whole replicated
        // world was SEVEN entities out of 1881, and the level's mutants hunted
        // and killed the player while never being drawn at all. Setting it back
        // to 0 brought ten species onto the screen at once.
        //
        // The code default is 0 precisely because the feature is unfinished
        // (see the note over g_sv_interest_radius: despawn-on-exit is missing
        // too). Until it is finished, a value that outlives the session that
        // set it is a way to blind somebody's game without leaving a trace --
        // exactly the lesson in anticheat-testcvars-leaked-into-userltx, which
        // this is the second instance of.
        extern float g_sv_interest_radius;
        static CCC_Float interest("sv_interest_radius", &g_sv_interest_radius, 0.f, 100000.f);
        interest.SetCanSave(false);
        Console->AddCommand(&interest);
    }

    // telemetry: what the world did, recorded by the side that decides it.
    // Written only by a process that owns a server; a client sees nothing.
    {
        extern int g_sv_telemetry;
        extern float g_sv_telemetry_census;
        extern int g_sv_telemetry_stock;
        extern int g_sv_telemetry_pos;
        CMD4(CCC_Integer, "sv_telemetry", &g_sv_telemetry, 0, 2);
        CMD4(CCC_Float, "sv_telemetry_census", &g_sv_telemetry_census, 0.f, 3600.f);
        CMD4(CCC_Integer, "sv_telemetry_stock", &g_sv_telemetry_stock, 0, 1);
        CMD4(CCC_Integer, "sv_telemetry_pos", &g_sv_telemetry_pos, 0, 1);
        CMD1(CCC_TelemetryDump, "sv_telemetry_dump");
    }

    // content fingerprint: 0 = don't ask, 1 = ask and log, 2 = refuse mismatches
    CMD4(CCC_Integer, "sv_content_timeout", &ContentIntegrity::g_content_check_timeout_ms, 1000, 120000);
    CMD1(CCC_ContentFingerprint, "sv_content_fingerprint");

    // ⚠⚠The two gates that decide who is let in are NOT SAVABLE, and this is not
    // tidiness. Both processes share _appdata_, so user.ltx has one writer at a
    // time and the last clean exit wins -- that is how the tuned
    // sv_npc_damage_scale was silently replaced by the client's default once
    // already (28-07). For a gate the failure is one-way and silent: the value
    // that survives is a 0, and a host comes up believing it authenticates.
    //
    // Measured 01-08-2026: user.ltx held `sv_auth_required 0` while servers had
    // been run at 2, and the headless server turns out to have no clean exit at
    // all (`quit` is accepted and the process stays up), so it never writes this
    // file. The client is the only writer, which means the persisted value can
    // only ever be the CLIENT's -- and the client's default is 0.
    //
    // A host that wants these on permanently should get them from a code default
    // or its own start-up script, not from a file the game rewrites.
    {
        static CCC_Integer content_check("sv_content_check", &ContentIntegrity::g_content_check_mode, 0, 2);
        content_check.SetCanSave(false);
        Console->AddCommand(&content_check);

        // join tickets: 0 = don't ask, 1 = ask and log, 2 = refuse without a valid
        // one. Same three steps as sv_content_check, for the same reason --
        // switching straight to refusals locks everybody out at once if anything
        // is misconfigured.
        static CCC_Integer auth_required("sv_auth_required", &NET_GNS::g_auth_required, 0, 2);
        auth_required.SetCanSave(false);
        Console->AddCommand(&auth_required);
    }

    CMD4(CCC_Integer, "sv_ban_refresh", &NET_GNS::g_ban_refresh_seconds, 5, 3600);
    CMD1(CCC_TicketKey, "sv_ticket_key");
    CMD1(CCC_AuthUrl, "sv_auth_url");
    CMD1(CCC_AuthToken, "sv_auth_token");

    // Administration. The same commands are reachable from the host console and,
    // for an authenticated admin, over the wire via `rcon` -- one implementation
    // either way (see xrServer::Process_admin_command for the role gate).
    CMD2(CCC_AdminVerb, "sv_who", "who");
    CMD2(CCC_AdminVerb, "sv_kick", "kick");
    CMD2(CCC_AdminVerb, "sv_ban", "ban");
    CMD2(CCC_AdminVerb, "sv_unban", "unban");
    CMD2(CCC_AdminVerb, "sv_setrole", "setrole");
    CMD1(CCC_Rcon, "rcon");

    CMD1(CCC_WorldTime, "sv_time");
    CMD1(CCC_WorldTimeSet, "sv_time_set");
    CMD1(CCC_WorldTimeAdvance, "sv_time_advance");
    CMD1(CCC_WorldTimeFactor, "sv_time_factor");
    CMD1(CCC_WorldTimeAuto, "sv_time_auto");

    CMD1(CCC_WorldWeather, "sv_weather");
    CMD1(CCC_WorldWeatherSet, "sv_weather_set");
    CMD1(CCC_WorldWeatherList, "sv_weather_list");
    CMD1(CCC_WorldWeatherAuto, "sv_weather_auto");
    CMD1(CCC_WorldWeatherStream, "sv_weather_stream");
    CMD1(CCC_WorldWeatherFX, "sv_weather_fx");
    CMD1(CCC_WorldWeatherFXStop, "sv_weather_fx_stop");
    CMD1(CCC_WorldWeatherFXList, "sv_weather_fx_list");

    // One pair per weather channel: <name> holds a value for every client,
    // <name>_auto hands it back to the running cycle. Upper bounds are generous on
    // purpose -- wind and lightning live in the cycle's own units, and refusing a
    // legitimate content value would be worse than allowing an extreme one.
    CMD4(CCC_WorldWeatherChannel, "sv_rain", eWeatherRain, 1.f, "0 = dry, 1 = hardest this level has");
    CMD4(CCC_WorldWeatherChannel, "sv_fog", eWeatherFog, 1.f, "0 = clear, 1 = thickest");
    CMD4(CCC_WorldWeatherChannel, "sv_wind", eWeatherWind, 2000.f, "cycle units; see sv_weather for the scale");
    CMD4(CCC_WorldWeatherChannel, "sv_thunder", eWeatherThunder, 600.f, "seconds between strikes; 0 = no lightning");
    CMD2(CCC_WorldWeatherChannelAuto, "sv_rain_auto", eWeatherRain);
    CMD2(CCC_WorldWeatherChannelAuto, "sv_fog_auto", eWeatherFog);
    CMD2(CCC_WorldWeatherChannelAuto, "sv_wind_auto", eWeatherWind);
    CMD2(CCC_WorldWeatherChannelAuto, "sv_thunder_auto", eWeatherThunder);

    CMD3(CCC_Mask, "ai_use_torch_dynamic_lights", &psActorFlags, AF_AI_VOLUMETRIC_LIGHTS);

#ifndef MASTER_GOLD
    CMD4(CCC_Vector3, "psp_cam_offset", &CCameraLook2::m_cam_offset, Fvector().set(-1000, -1000, -1000), Fvector().set(1000, 1000, 1000));
#endif // MASTER_GOLD

#ifdef DEBUG
    CMD1(CCC_DumpObjects, "dump_all_objects");
    CMD3(CCC_String, "stalker_death_anim", dbg_stalker_death_anim, 32);
    CMD4(CCC_Integer, "death_anim_velocity", &b_death_anim_velocity, FALSE, TRUE);
    CMD1(CCC_Crash, "crash");
    CMD4(CCC_Integer, "dbg_show_ani_info", &g_ShowAnimationInfo, 0, 1);
    CMD4(CCC_Integer, "dbg_dump_physics_step", &g_bDebugDumpPhysicsStep, 0, 1);
#endif

    CMD4(CCC_Integer, "show_wnd_rect", &g_show_wnd_rect, 0, 1);
    CMD4(CCC_Integer, "show_wnd_rect_all", &g_show_wnd_rect2, 0, 1);
    CMD4(CCC_Integer, "show_wnd_rect_names", &g_show_wnd_rect_text, 0, 1);
    CMD4(CCC_Integer, "g_console_show_always", &g_console_show_always, 0, 1);

    *g_last_saved_game = 0;

    CMD4(CCC_Float, "g_cam_height_speed", &cam_HeightInterpolationSpeed, 4.0f, 16.0f);
    CMD4(CCC_Float, "g_cam_lookout_speed", &cam_LookoutSpeed, 1.0f, 4.0f);

    CMD3(CCC_Mask, "g_actor_shadow", &psActorFlags, AF_ACTOR_SHADOW);
    CMD3(CCC_Mask, "g_first_person_body", &psActorFlags, AF_FIRST_PERSON_BODY);

    CMD3(CCC_Mask, "g_throw_dbg", &psActorFlags, AF_THROW_DEBUG);

#ifdef USE_MEMORY_VALIDATOR
    CMD4(CCC_Integer, "g_enable_memory_debug", &g_enable_memory_debug, 0, 1);
    CMD1(CCC_DbgMemoryDump, "dbg_memory_dump");
    CMD1(CCC_DbgMemoryInfo, "dbg_memory_info");
    CMD1(CCC_DbgMemoryClear, "dbg_memory_clear");
    if (!g_enable_memory_debug)
        PointerRegistryClear();
#endif

    extern BOOL bSenvironmentXrExport;
    CMD4(CCC_Integer, "senvironment_xr_export", &bSenvironmentXrExport, FALSE, TRUE);
}
