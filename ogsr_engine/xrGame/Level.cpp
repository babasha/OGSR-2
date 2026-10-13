#include "StdAfx.h"
#include "../xrCore/cpu_budget.h"
#include "xr_platform_seh.h"
#include "../xr_3da/FDemoRecord.h"
#include "../xr_3da/FDemoPlay.h"
#include "../xr_3da/Environment.h"
#include "../xr_3da/IGame_Persistent.h"
#include "ParticlesObject.h"
#include "Level.h"
#include "xrServer.h"
#include "NET_Queue.h"
#include "game_cl_base.h"
#include "entity_alive.h"
#include "HUDManager.h"
#include "ai_space.h"
#include "ai_debug.h"
#include "PHDynamicData.h"
#include "Physics.h"
#include "ShootingObject.h"
#include "Level_Bullet_Manager.h"
#include "script_engine.h"
#include "lua_vm_watch.h"
#include "InfoPortion.h"
#include "patrol_path_storage.h"
#include "date_time.h"
#include "space_restriction_manager.h"
#include "seniority_hierarchy_holder.h"
#include "space_restrictor.h"
#include "client_spawn_manager.h"
#include "cl_console_watch.h"
#include "cl_bone_audit.h"
#include "ClimableObject.h"
#include "IKLimbsController.h"   // ph_ik_mt: CIKLimbsController::FlushBatch
#include "level_graph.h"
#include "mt_config.h"
#include "PHCommander.h"
#include "map_manager.h"
#include "../xr_3da/CameraManager.h"
#include "level_sounds.h"
#include "Car.h"
#include "trade_parameters.h"
#include "clsid_game.h"
#include "MainMenu.h"
#include "../xr_3da/XR_IOConsole.h"
#include "debug_renderer.h"
#include "Actor_Flags.h"
#include "cl_fire_test.h"
#include "cl_bolt_test.h"
#include "cl_zone_debug.h"
#include "ai_debug_draw.h"
#include "ai_test.h"
#include "perf_route.h"
#include "ai_position_query.h"
#include "ai_commander.h"
#include "ai_threat_map.h"
#include "ai_house.h"
#include "sound_propagation.h"
#include "cl_zone_edit.h"
#include "cl_prop_edit.h"
#include "ui_rml.h"
#include "level_changer.h"
#include "player_hud.h"
#include "Actor.h"
#include "Inventory.h"
#include "HudItem.h"
#include "PDA.h"

#ifdef DEBUG
#include "level_debug.h"
#include "ai/stalker/ai_stalker.h"
#include "PhysicObject.h"
#endif

#include "LevelDebugScript.h"
#include "PhysicObject.h"
#include "UIGameSP.h"
#include "ui/UIPdaWnd.h"
#include "ui/UIBtnHint.h"

#include "embedded_editor/embedded_editor_main.h"
#include "../xr_3da/xr_ioc_cmd.h"
#include "../xrCore/load_prof.h" // [frame prof] parts

CPHWorld* ph_world = 0;

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CLevel::CLevel()
    : IPureClient(Device.GetTimerGlobal())
#ifdef PROFILE_CRITICAL_SECTIONS
      ,
      DemoCS(MUTEX_PROFILE_ID(DemoCS))
#endif // PROFILE_CRITICAL_SECTIONS
{
    lua_gc(ai().script_engine().lua(), LUA_GCSTOP, 0);
    extern int psLUA_GC_STEPMUL; // script_gc: ручка lua_gc_stepmul, там же замеры
    lua_gc(ai().script_engine().lua(), LUA_GCSETSTEPMUL, psLUA_GC_STEPMUL);

    g_bDebugEvents = strstr(Core.Params, "-debug_ge") ? TRUE : FALSE;

    Server = NULL;

    game = NULL;
    //	game						= xr_new<game_cl_GameState>();
    game_events = xr_new<NET_Queue_Event>();

    game_configured = FALSE;
    m_bGameConfigStarted = FALSE;

    eChangeRP = Engine.Event.Handler_Attach("LEVEL:ChangeRP", this);
    eDemoPlay = Engine.Event.Handler_Attach("LEVEL:PlayDEMO", this);
    eChangeTrack = Engine.Event.Handler_Attach("LEVEL:PlayMusic", this);
    eEnvironment = Engine.Event.Handler_Attach("LEVEL:Environment", this);

    eEntitySpawn = Engine.Event.Handler_Attach("LEVEL:spawn", this);

    m_pBulletManager = xr_new<CBulletManager>();

    m_map_manager = xr_new<CMapManager>();

    physics_step_time_callback = (PhysicsStepTimeCallback*)&PhisStepsCallback;
    m_seniority_hierarchy_holder = xr_new<CSeniorityHierarchyHolder>();

    m_level_sound_manager = xr_new<CLevelSoundManager>();
    m_space_restriction_manager = xr_new<CSpaceRestrictionManager>();
    m_client_spawn_manager = xr_new<CClientSpawnManager>();

    m_debug_renderer = xr_new<CDebugRenderer>();
#ifdef DEBUG
    m_level_debug = xr_new<CLevelDebug>();
#endif

    m_ph_commander = xr_new<CPHCommander>();
    m_ph_commander_scripts = xr_new<CPHCommander>();

#ifdef DEBUG
    m_bSynchronization = false;
#endif

    pCurrentControlEntity = NULL;

    //---------------------------------------------------------
    m_dwCL_PingLastSendTime = 0;
    m_dwCL_PingDeltaSend = 1000;
    m_dwRealPing = 0;

    m_is_removing_objects = false;

    g_player_hud = xr_new<player_hud>();
    g_player_hud->load_default();
}

extern void spawn_prefetch_shutdown();   // Level_network_spawn.cpp: stop the reader before anything it reads through goes

CLevel::~CLevel()
{
    spawn_prefetch_shutdown();
    // the position queries name this level's covers
    // the houses' worker reads this level's AI map and collision
    AIHouse::BakeStop();
    // the sound thread walks this level's AI map (snd_prop)
    SoundPropagation::Reset();
    PositionQuery::Reset();
    ThreatMap::Reset();
    Commander::Reset();
    // Before anything else lets go: the editor holds a model out of the
    // renderer's pool, and the pool does not outlive the level.
    ClPropEdit::Unload();

    xr_delete(g_player_hud);
    //	g_pGameLevel		= NULL;
    Msg("- Destroying level");

    Engine.Event.Handler_Detach(eEntitySpawn, this);

    Engine.Event.Handler_Detach(eEnvironment, this);
    Engine.Event.Handler_Detach(eChangeTrack, this);
    Engine.Event.Handler_Detach(eDemoPlay, this);
    Engine.Event.Handler_Detach(eChangeRP, this);

    if (ph_world)
    {
        ph_world->Destroy();
        xr_delete(ph_world);
    }

    // destroy PSs
    for (POIt p_it = m_StaticParticles.begin(); m_StaticParticles.end() != p_it; ++p_it)
        CParticlesObject::Destroy(*p_it);
    m_StaticParticles.clear();

	// Unload sounds
	// unload prefetched sounds
    sound_registry.clear();
	sound_registry_defer.clear();

    // unload static sounds
    for (u32 i = 0; i < static_Sounds.size(); ++i)
    {
        static_Sounds[i]->destroy();
        xr_delete(static_Sounds[i]);
    }
    static_Sounds.clear();

    xr_delete(m_level_sound_manager);

    xr_delete(m_space_restriction_manager);

    xr_delete(m_seniority_hierarchy_holder);

    xr_delete(m_client_spawn_manager);

    xr_delete(m_debug_renderer);

    xr_delete(game);
    xr_delete(game_events);

    // by Dandy
    // destroy fog of war
    //	xr_delete					(m_pFogOfWar);
    // destroy bullet manager
    xr_delete(m_pBulletManager);

    //-----------------------------------------------------------
    xr_delete(m_ph_commander);
    xr_delete(m_ph_commander_scripts);
    //-----------------------------------------------------------

    ai().unload();
    //-----------------------------------------------------------
#ifdef DEBUG
    xr_delete(m_level_debug);
#endif
    //-----------------------------------------------------------
    xr_delete(m_map_manager);
    //	xr_delete					(m_pFogOfWarMngr);
    //-----------------------------------------------------------

    // here we clean default trade params
    // because they should be new for each saved/loaded game
    // and I didn't find better place to put this code in
    CTradeParameters::clean();

    for (auto& i : m_debug_render_queue)
    {
        xr_delete(i.second);
    }

    m_debug_render_queue.clear();
}

shared_str CLevel::name() const { return (m_name); }

bool CLevel::PrefetchSound(LPCSTR name)
{
    // preprocess sound name
    string_path tmp;
    strcpy_s(tmp, name);
    xr_strlwr(tmp);
    if (strext(tmp))
        *strext(tmp) = 0;
    shared_str snd_name = tmp;
    // find in registry
    SoundRegistryMapIt it = sound_registry.find(snd_name);
    // if find failed - preload sound
    if (it == sound_registry.end())
    {
        sound_registry[snd_name].create(snd_name.c_str(), st_Effect, sg_SourceType);
        return true;
    }
    return false;
}

bool CLevel::PrefetchManySounds(LPCSTR prefix)
{
    bool created = false;
    string_path fn;
    if (FS.exist(fn, "$game_sounds$", prefix, ".ogg"))
        created = PrefetchSound(prefix) || created;
    u32 i = 0;
    while (true)
    {
        string256 name;
        sprintf_s(name, "%s%d", prefix, i);
        if (FS.exist(fn, "$game_sounds$", name, ".ogg"))
            created = PrefetchSound(name) || created;
        else if (i > 0)
            break;
        i++;
    }
    return created;
}

bool CLevel::PrefetchManySoundsLater(LPCSTR prefix)
{
    std::string s(prefix);
    for (const auto& it : sound_registry_defer)
    {
        if (it == s)
            return false;
    }
    sound_registry_defer.push_back(s);
    return true;
}

void CLevel::PrefetchDeferredSounds()
{
    while (!sound_registry_defer.empty())
    {
        std::string s = sound_registry_defer.front();
        sound_registry_defer.pop_front();
        if (PrefetchManySounds(s.c_str()))
            break;
    }
}

void CLevel::CancelPrefetchManySounds(LPCSTR prefix)
{
    std::string s(prefix);
    sound_registry_defer.erase(std::remove(sound_registry_defer.begin(), sound_registry_defer.end(), s), sound_registry_defer.end());
}

// Game interface ////////////////////////////////////////////////////
int CLevel::get_RPID(LPCSTR /**name/**/)
{
    /*
    // Gain access to string
    LPCSTR	params = pLevel->r_string("respawn_point",name);
    if (0==params)	return -1;

    // Read data
    Fvector4	pos;
    int			team;
    sscanf		(params,"%f,%f,%f,%d,%f",&pos.x,&pos.y,&pos.z,&team,&pos.w); pos.y += 0.1f;

    // Search respawn point
    svector<Fvector4,maxRP>	&rp = Level().get_team(team).RespawnPoints;
    for (int i=0; i<(int)(rp.size()); ++i)
        if (pos.similar(rp[i],EPS_L))	return i;
    */
    return -1;
}

BOOL g_bDebugEvents = FALSE;

void CLevel::cl_Process_Event(u16 dest, u16 type, NET_Packet& P)
{
    //			Msg				("--- event[%d] for [%d]",type,dest);
    CObject* O = Objects.net_Find(dest);
    if (0 == O)
    {
#ifdef DEBUG
        Msg("* WARNING: c_EVENT[%d] to [%d]: unknown dest", type, dest);
#endif // DEBUG

        // ⭐⭐⭐A destroy delivered to nobody is how an object outlives its own
        // entity -- and until now it did so in silence, because the warning
        // above is DEBUG-only and Release just dropped the event on the floor.
        //
        // The two ways of getting here are not equally innocent, and the
        // difference is the whole diagnosis:
        //
        //   * nothing in the level carries this id -- the object is genuinely
        //     already gone, or it is still sitting in the delayed spawn queue,
        //     which is exactly what ProcessGameSpawnsDestroy below handles;
        //
        //   * a LIVE object still carries it while map_NETID does not -- the id
        //     slot was taken out from under it (see CObjectList::net_Register).
        //     The destroy has just been delivered to an empty slot and the
        //     object it was meant for will go on running, and thinking, with
        //     its server entity already released.
        //
        // ⭐The second line names the orphan at the moment it is made, instead
        // of ten minutes and three thousand anonymous faults later.
        if (GE_DESTROY == type)
        {
            CObject* stranded = nullptr;
            for (u32 i = 0, n = Objects.o_count(); i < n && !stranded; ++i)
            {
                CObject* C = Objects.o_get_by_iterator(i);
                if (C && C->ID() == dest && !C->getDestroy())
                    stranded = C;
            }

            if (stranded)
                Msg("!! [netmap] GE_DESTROY for %u hit an EMPTY id slot while '%s' [%s] is still live at (%.1f, %.1f, %.1f) -- that object is orphaned as of now", u32(dest),
                    stranded->cName().c_str(), stranded->cNameSect().c_str(), VPUSH(stranded->Position()));
            else
                Msg("~ [netmap] GE_DESTROY for %u: nothing here to destroy (already gone, or still queued to spawn)", u32(dest));
        }

        ProcessGameSpawnsDestroy(dest, type, P);
        return;
    }
    CGameObject* GO = smart_cast<CGameObject*>(O);
    if (!GO)
    {
        Msg("! ERROR: c_EVENT[%d] : non-game-object", dest);
        return;
    }

    if (type == GE_DESTROY)
        Game().OnDestroy(GO);

    {
        // [frame prof]: which event a slow level.events was (the hit/death/drop of v215-v218)
        static const char* const k_ev[] = {"ev.own_take", "ev.own_reject", "ev.transfer_ammo", "ev.hit", "ev.die", "ev.assign_killer", "ev.destroy"};
        LoadProf::Scope _lp(type < sizeof(k_ev) / sizeof(k_ev[0]) ? k_ev[type] : "ev.other");
        GO->OnEvent(P, type);
    }
}

namespace SpawnProf { void Dump(); }

void CLevel::ProcessGameEvents()
{
    LoadProf::Scope _lp("level.events"); // [frame prof]: hits, deaths, ownership...
    // Game events
    {
        NET_Packet P;
        u32 svT = timeServer() - NET_Latency;

        /*
        if (!game_events->queue.empty())
            Msg("- d[%d],ts[%d] -- E[svT=%d],[evT=%d]",Device.dwTimeGlobal,timeServer(),svT,game_events->queue.begin()->timestamp);
        */

        m_just_destroyed.clear();

        while (game_events->available(svT))
        {
            u16 ID, dest, type;
            game_events->get(ID, dest, type, P);

            // [event slow]: 📏07-10-2026 Switch v211 the fight-start stall was
            // 84 ms of level.events SELF time (nothing named under it) while
            // bandits took weapons into their hands. One line per slow event.
            const u64 ev_t0 = CPU::QPC();
            struct EvTimer
            {
                u64 t0; u16 id, dest, type;
                ~EvTimer()
                {
                    const double ms = 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq());
                    static u32 s_n = 0;
                    if (ms >= 10.0 && !Device.dwPrecacheFrame && s_n < 64)
                    {
                        ++s_n;
                        CObject* O = Level().Objects.net_Find(dest);
                        Msg("~ [event slow] %s type %u -> %u '%s' %.1f ms (frame %u)", id == M_SPAWN ? "M_SPAWN" : "M_EVENT", type, dest,
                            O ? O->cName().c_str() : "?", ms, Device.dwFrame);
                    }
                }
            } ev_timer{ev_t0, ID, dest, type};

            switch (ID)
            {
            case M_SPAWN: {
                u16 dummy16;
                P.r_begin(dummy16);
                cl_Process_Spawn(P);
            }
            break;
            case M_EVENT: {
                cl_Process_Event(dest, type, P);
            }
            break;
            default: {
                VERIFY(0);
            }
            break;
            }
        }
    }

    // Load-time only: this drain IS the 'st_client_spawning' cost the phase timer
    // never saw (the queue empties on the first precache frame, behind the load
    // screen). Silent in gameplay -- one line per burst frame while it is up.
    if (Device.dwPrecacheFrame)
        SpawnProf::Dump();

    if (!is_removing_objects())
        Device.add_to_seq_parallel(fastdelegate::MakeDelegate(this, &CLevel::ProcessGameSpawns), "level spawns");
}

// RAII section meter feeding the seqFrame profiler hook (pure.h): names which
// part of CLevel::OnFrame the growing seq:CLevel time goes to. Armed only while
// the VK profiler runs (g_seq_profile_cb non-null inside seqFrame.Process).
struct SeqSectionProbe
{
    const char* n; CTimer t; bool on;
    LoadProf::Scope lp; // [frame prof] names the section too (no-op unless armed)
    explicit SeqSectionProbe(const char* name) : n(name), on(g_seq_profile_cb != nullptr), lp(name) { if (on) t.Start(); }
    ~SeqSectionProbe() { if (on && g_seq_profile_cb) g_seq_profile_cb(n, t.GetElapsed_sec() * 1000.f); }
};

// ⭐⭐⭐The frame, with a stack trace attached to the fault that has been eating it.
//
// Twice now a live session has died sideways with nothing but this in the log:
//
//   !![CRegistrator<pureFrame>] member #3 (class CLevel) threw [non-std] — skipped
//
// That line is the frame registrar's per-member guard in pure.h doing its job:
// it keeps the chain alive so the rest of the world does not freeze. But xrGame
// is built WITHOUT /EH, so its catch(...) also swallows ACCESS VIOLATIONS, and
// destructors are not run while unwinding -- the frame is cut in half and
// whatever it was holding stays held. The symptom the player sees has nothing to
// do with frames: last time it was "I can see the ammo and no weapon will
// reload", because a reload is finished by an animation-end callback
// (CWeaponMagazined::OnAnimationEnd -> ReloadMagazine) that the severed frame
// never delivers. The animation plays in full. Nothing is loaded. Ever again.
//
// ⭐catch(...) cannot say where it happened -- by then the stack is gone. An SEH
// filter runs BEFORE unwinding, so this wrapper turns the next occurrence into a
// named line and a full stack instead of one anonymous "member #3".
//
// ⚠It does NOT fix the fault, and it must not be mistaken for a fix: the frame
// is still cut short, exactly as the registrar's catch would have cut it. What
// changes is that the next occurrence says where.
void CLevel::OnFrame()
{
    XR_SEH_TRY
    {
        OnFrame_impl();
    }
    XR_SEH_EXCEPT(ExceptStackTrace("!![CLevel::OnFrame] stack trace:\n"))
    {
        static u32 s_faults = 0;
        Msg("!![CLevel::OnFrame] faulted (total %u) -- stack above. The frame was cut short; state may be inconsistent from here.", ++s_faults);
    }
}

void CLevel::OnFrame_impl()
{
    // (the command channel ticks from CGamePersistent::OnFrame -- it has to
    // run in the main menu too, where there is no level)

    // ⭐First-person models of the actor's slot weapons, loaded as the
    // precache ends -- under the loading screen -- instead of the first time
    // each is drawn. 📏04-10-2026 Switch v153: d-pad down (wpn_3) cost 163 ms
    // of frame_move: player_hud::create_hud_item does model_Create and the
    // hand motions synchronously, and keeps the result in its pool for the
    // rest of the level, so doing it early is free afterwards. Only slots:
    // anything picked up later still pays on its first draw.
    // ⚠On the frame the precache ENDS, not on "the last two frames": the
    // countdown stops adaptively (device.cpp End_Main_Pre) and jumps from ~48
    // straight to 0. Re-armed by every load, not keyed on `this` -- the next
    // level can be allocated at the same address.
    static bool s_was_precaching = false;
    const bool precache_just_ended = s_was_precaching && Device.dwPrecacheFrame == 0;
    s_was_precaching = Device.dwPrecacheFrame != 0;
    if (precache_just_ended && g_player_hud)
    {
        if (CActor* actor = smart_cast<CActor*>(Level().CurrentEntity()))
        {
            const u64 t0 = CPU::QPC();
            u32 made = 0;
            for (const auto& slot : actor->inventory().m_slots)
                if (CHudItem* hud = smart_cast<CHudItem*>(slot.m_pIItem))
                    if (hud->HudSection().size())
                    {
                        g_player_hud->create_hud_item(hud->HudSection());
                        ++made;
                    }
            Msg("~ [hud] warmed %u slot item(s) in %.0f ms", made, 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq()));
        }
    }

    { SeqSectionProbe _p("Lvl/feel");      m_feel_deny.update(); }

    // commit events from bullet manager from prev-frame
    Device.Statistic->BulletManager.Begin();
    { SeqSectionProbe _p("Lvl/bullets");   BulletManager().CommitEvents(); }
    Device.Statistic->BulletManager.End();

    { SeqSectionProbe _p("Lvl/netRecv");   ClientReceive(); }

    // thin-client puppet interpolation (no-op in fused SP -- see the early
    // Server check inside)
    {
        extern void net_import_interpolate_tick();
        net_import_interpolate_tick();
    }

    { SeqSectionProbe _p("Lvl/gameEv");    ProcessGameEvents(); }

    { SeqSectionProbe _p("Lvl/map");       MapManager().Update(); }
    // Inherited update (objects/scheduler/space)
    { SeqSectionProbe _p("Lvl/inherited"); inherited::OnFrame(); }
    // ph_ik_mt: every creature updated -- now all their legs at once, on cores 0-2
    { SeqSectionProbe _p("Lvl/ik"); LoadProf::Scope _lp("ik.batch"); CIKLimbsController::FlushBatch(); }

    // Automated trigger-pull for the shot-latency measurement. After the object
    // update deliberately: it aims at where creatures ARE this frame, and the
    // weapon it drives reads the camera in the same frame it fires.
    CLFire::Frame();

    // ...and the same for the bolt: throwing one is a keypress, and the two
    // defects it exposes (no object thrown, blend list overflow) both used to
    // need a person at the keyboard to reproduce.
    CLBolt::Frame();

    // Builds the zone overlay's UI shader. Here rather than at draw time: a
    // shader created inside the render pass came back with no texture.
    ClZoneDebug::Frame();
    ClZoneEdit::Frame();
    ClPropEdit::Frame();
    // the combat AI overlay (ai_dbg_draw) and its autotest (ai_test): both
    // return at once when off
    AIDebugDraw::Frame();
    AITest::Frame();
    PerfRoute::Frame();
    // after the AI asked this frame's questions: hand them to the second thread
    PositionQuery::Frame();
    ThreatMap::Frame();
    SoundPropagation::Frame(); // snd_prop: the AI map's paths to the sound, once per level
    UIRml::Update();

    // Smoothness sampling, AFTER the object update: by here the puppet
    // interpolator has placed the body and the animator has chosen its blends,
    // so this reads what the frame will actually draw.
    ClBoneAudit::Frame_();

    extern bool s_ScriptTime;

    // s_ScriptTime is the weather editor's "drive the sky myself" switch. It must
    // not reach a machine that only renders somebody else's world: detaching the
    // sky from the host clock there is exactly the "give myself daylight" cheat,
    // and it is one keypress away in every build (Shift+F10). Host processes keep
    // the switch.
    if (!s_ScriptTime || !Server)
    {
        g_pGamePersistent->Environment().SetGameTime(GetEnvironmentGameDayTimeSec(), game->GetEnvironmentGameTimeFactor());
    }

    { SeqSectionProbe _p("Lvl/phCmd");     m_ph_commander->update(); }
    { SeqSectionProbe _p("Lvl/phCmdScr");  m_ph_commander_scripts->update(); }

    //просчитать полет пуль
    Device.Statistic->BulletManager.Begin();
    { SeqSectionProbe _p("Lvl/bullets2");  BulletManager().CommitRenderSet(); }
    Device.Statistic->BulletManager.End();

    // update static sounds
    if (g_mt_config.test(mtLevelSounds))
        Device.add_to_seq_parallel(fastdelegate::MakeDelegate(m_level_sound_manager, &CLevelSoundManager::Update), "level sounds");
    else
        m_level_sound_manager->Update();

    if (!sound_registry_defer.empty())
        Device.add_to_seq_parallel(fastdelegate::MakeDelegate(this, &CLevel::PrefetchDeferredSounds), "prefetch sounds");

    if (ps_lua_gc_method != gc_timeout)
        Device.add_to_seq_parallel(fastdelegate::MakeDelegate(this, &CLevel::script_gc), "level script_gc");

    ShowEditor();
}

extern Flags32 dbg_net_Draw_Flags;

extern void draw_wnds_rects();

extern bool use_reshade;
extern void render_reshade_effects();

void CLevel::OnRender()
{
    inherited::OnRender();

    Device_StallMark("extract");   // RENDER_THREAD_PLAN: Calculate = the extract half
    Render->Calculate();
    // ⭐The furniture editor's ghost, between the collect and the draw. Calculate
    // clears and refills the frame's dynamic-visual list by walking the visible
    // objects; the ghost belongs to no object, so this is the only point where
    // handing it over both survives the clear and makes the draw.
    ClPropEdit::SubmitGhost();
    Device_StallMark("submit");    // Render->Render = what the render thread will own
    Render->Render();
    Device_StallMark("ui_post");   // UI / bullets / HUD assembly after the world draw

    Game().OnRender();

    //отрисовать трассы пуль
    // Device.Statistic->TEST1.Begin();
    BulletManager().Render();
    // Device.Statistic->TEST1.End();

    if (use_reshade)
        render_reshade_effects();

    auto pGameSP = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame());
    auto pActor = smart_cast<CActor*>(Level().CurrentEntity());
    CPda* Pda = pActor ? pActor->GetPDA() : nullptr;
    const bool need_pda_render = Pda && Pda->Is3DPDA() && psActorFlags.test(AF_3D_PDA) && pGameSP->PdaMenu->IsShown();

    if (need_pda_render)
    {
        Render->AfterWorldRender();

        pGameSP->PdaMenu->Draw();

        const CUIDialogWnd* receiver = pGameSP->MainInputReceiver();
        const bool is_top = receiver == pGameSP->PdaMenu;

        if (is_top || !receiver)
        {
            if (g_btnHint)
                g_btnHint->OnRender();
            GetUICursor()->OnRender();
        }

        draw_wnds_rects();

        Fvector2 cursor_pos = GetUICursor()->GetCursorPosition();
        Fvector2 cursor_pos_dif{cursor_pos};
        cursor_pos_dif.sub(pGameSP->PdaMenu->last_cursor_pos);
        pGameSP->PdaMenu->last_cursor_pos.set(cursor_pos);
        pGameSP->PdaMenu->MouseMovement(cursor_pos_dif.x, cursor_pos_dif.y);

        Render->AfterUIRender();
    }

    HUD().RenderUI();

    if (g_btnHint)
        g_btnHint->OnRender();

    draw_wnds_rects();

#ifndef DEBUG

    if (psActorFlags.test(AF_ZONES_DBG))
    {
        for (u32 I = 0; I < Level().Objects.o_count(); I++)
        {
            auto _O = Level().Objects.o_get_by_iterator(I);
            auto space_restrictor = smart_cast<CSpaceRestrictor*>(_O);
            if (space_restrictor)
                space_restrictor->OnRender();

            auto level_changer = smart_cast<CLevelChanger*>(_O);
            if (level_changer)
                level_changer->OnRender();

            auto climable = smart_cast<CClimableObject*>(_O);
            if (climable)
                climable->OnRender();
        }
    }

    if (psActorFlags.test(AF_VERTEX_DBG))
    {
        if (ai().get_level_graph())
            ai().level_graph().render();
    }

    if (dbg_net_Draw_Flags.test(1 << 11)) // draw skeleton
    {
        for (u32 I = 0; I < Level().Objects.o_count(); I++)
        {
            CObject* _O = Level().Objects.o_get_by_iterator(I);

            CGameObject* pGO = smart_cast<CGameObject*>(_O);
            if (pGO /*&& pGO != Level().CurrentViewEntity()*/ && !pGO->H_Parent())
            {
                if (pGO->Position().distance_to_sqr(Device.vCameraPosition) < 400.0f)
                {
                    //CPhysicObject* physic_object = smart_cast<CPhysicObject*>(_O);
                    //if (physic_object)
                    //    physic_object->OnRender();

                    pGO->dbg_DrawSkeleton();
                }
            }
        }
    }

    //-- static shown debug items
    Level().debug_renderer().OnRenderStaticItems();

#else

    ph_world->OnRender();

    if (ai().get_level_graph())
        ai().level_graph().render();

#ifdef DEBUG_PRECISE_PATH
    test_precise_path();
#endif

    CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(Level().CurrentEntity());
    if (stalker)
        stalker->OnRender();

    if (bDebug)
    {
        ObjectSpace.dbgRender();

        //---------------------------------------------------------------------
        HUD().Font().pFontStat->OutSet(170, 630);
        HUD().Font().pFontStat->SetHeight(16.0f);
        HUD().Font().pFontStat->SetColor(0xffff0000);

        if (Server)
            HUD().Font().pFontStat->OutNext("Client Objects:      [%d]", Server->GetEntitiesNum());
        HUD().Font().pFontStat->OutNext("Server Objects:      [%d]", Objects.o_count());
        HUD().Font().pFontStat->OutNext("Interpolation Steps: [%d]", Level().GetInterpolationSteps());
        HUD().Font().pFontStat->SetHeight(8.0f);
        //---------------------------------------------------------------------
    }

    ScriptDebugRender();

    if (psActorFlags.test(AF_ZONES_DBG))
    {
        DBG().draw_object_info();
        DBG().draw_text();
        DBG().draw_level_info();
    }

    //-- static shown debug items (CDebugRenderer::add_static_box/sphere/line) — this call only existed
    //-- in the #ifndef DEBUG branch above, so a DEBUG build never actually rendered static debug markers.
    Level().debug_renderer().OnRenderStaticItems();
#endif

    debug_renderer().render();

#ifdef DEBUG

    if (psAI_Flags.is(aiVision))
    {
        for (u32 I = 0; I < Level().Objects.o_count(); I++)
        {
            CObject* object = Objects.o_get_by_iterator(I);
            CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(object);
            if (!stalker)
                continue;
            stalker->dbg_draw_vision();
        }
    }

    if (psAI_Flags.test(aiDrawVisibilityRays))
    {
        for (u32 I = 0; I < Level().Objects.o_count(); I++)
        {
            CObject* object = Objects.o_get_by_iterator(I);
            CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(object);
            if (!stalker)
                continue;

            stalker->dbg_draw_visibility_rays();
        }
    }

#endif
}

void CLevel::OnEvent(EVENT E, u64 P1, u64 /**P2/**/)
{
    if (E == eEntitySpawn)
    {
        char Name[128];
        Name[0] = 0;
        sscanf(LPCSTR(P1), "%s", Name);
        Level().g_cl_Spawn(Name, 0xff, M_SPAWN_OBJECT_LOCAL, Fvector().set(0, 0, 0));
    }
    else if (E == eChangeRP && P1)
    {}
    else if (E == eDemoPlay && P1)
    {
        char* name = (char*)P1;
        string_path RealName;
        strcpy_s(RealName, name);
        strcat_s(RealName, ".xrdemo");
        Cameras().AddCamEffector(xr_new<CDemoPlay>(RealName, 1.3f, 0));
    }
    else if (E == eChangeTrack && P1)
    {
        // int id = atoi((char*)P1);
        // Environment->Music_Play(id);
    }
    else if (E == eEnvironment)
    {
        // int id=0; float s=1;
        // sscanf((char*)P1,"%d,%f",&id,&s);
        // Environment->set_EnvMode(id,s);
    }
    else
        return;
}

void CLevel::PhisStepsCallback(u32, u32) {}

ALife::_TIME_ID CLevel::GetGameTime()
{
    return (game->GetGameTime());
    //	return			(Server->game->GetGameTime());
}

ALife::_TIME_ID CLevel::GetEnvironmentGameTime()
{
    return (game->GetEnvironmentGameTime());
    //	return			(Server->game->GetGameTime());
}

u8 CLevel::GetDayTime()
{
    u32 dummy32;
    u32 hours;
    GetGameDateTime(dummy32, dummy32, dummy32, hours, dummy32, dummy32, dummy32);
    VERIFY(hours < 256);
    return u8(hours);
}

float CLevel::GetGameDayTimeSec() { return (float(s64(GetGameTime() % (24 * 60 * 60 * 1000))) / 1000.f); }

u32 CLevel::GetGameDayTimeMS() { return (u32(s64(GetGameTime() % (24 * 60 * 60 * 1000)))); }

float CLevel::GetEnvironmentGameDayTimeSec() { return (float(s64(GetEnvironmentGameTime() % (24 * 60 * 60 * 1000))) / 1000.f); }

void CLevel::ScriptDebugRender()
{
    if (m_debug_render_queue.empty())
        return;

    bool hasVisibleObj = false;

    xr_map<u16, DBG_ScriptObject*>::iterator it = m_debug_render_queue.begin();
    xr_map<u16, DBG_ScriptObject*>::iterator it_e = m_debug_render_queue.end();
    for (; it != it_e; ++it)
    {
        DBG_ScriptObject* obj = (*it).second;
        if (obj->m_visible)
        {
            obj->Render();
            hasVisibleObj = true;
        }
    }

	// demonized: fix of showing console window when there are no visible gizmos
    if (hasVisibleObj)
        DRender->OnFrameEnd();
}

void CLevel::GetGameDateTime(u32& year, u32& month, u32& day, u32& hours, u32& mins, u32& secs, u32& milisecs)
{
    split_time(GetGameTime(), year, month, day, hours, mins, secs, milisecs);
}

float CLevel::GetGameTimeFactor()
{
    return (game->GetGameTimeFactor());
}

void CLevel::SetGameTimeFactor(const float fTimeFactor)
{
    game->SetGameTimeFactor(fTimeFactor);
}

void CLevel::SetGameTimeFactor(ALife::_TIME_ID GameTime, const float fTimeFactor)
{
    game->SetGameTimeFactor(GameTime, fTimeFactor);
}

void CLevel::GetGameTimeForShaders(u32& hours, u32& minutes, u32& seconds, u32& milliseconds)
{
    u32 unused;
    split_time(GetGameTime(), unused, unused, unused, hours, minutes, seconds, milliseconds);
}

//bool CLevel::IsServer() // always false
//{
//    //if (!Server)
//    //    return false;
//
//    bool r = !Server ? false : (Server->client_Count() != 0);
//    Msg("IsServer = %d", r ? 1 : 0);
//    return r;
//}
//
//bool CLevel::IsClient() // always false
//{
//    //if (!Server)
//    //    return true;
//
//    bool r = !Server ? true : (Server->client_Count() == 0);
//    Msg("IsClient = %d", r ? 1 : 0);
//    return r;
//}

void CLevel::OnSessionTerminate(LPCSTR reason) { MainMenu()->OnSessionTerminate(reason); }

void CLevel::OnDestroyObject(u16 id) { m_just_destroyed.push_back(id); }

void CLevel::OnChangeCurrentWeather(const char* sect)
{
    // The weather forecast is game logic, and on a network client the authoritative
    // copy of it runs on the server: this process is only told which cycle to draw.
    // Two reasons not to notify the script here. It could only try to overrule the
    // server (the authoritative-weather hold refuses that anyway), and mod forecast
    // code freely touches the local ALife simulator, which a thin client does not
    // have -- that class of nil dereference is what used to kill the client outright
    // during connect. Same lens as the object script binders.
    if (is_network_client())
        return;

    if (on_change_weather_callback.empty())
        return;
    luabind::functor<void> funct;
    if (ai().script_engine().functor(on_change_weather_callback.c_str(), funct))
        funct(sect);
}

u32 GameID() { return Game().Type(); }

#include "../xr_3da/IGame_Persistent.h"

GlobalFeelTouch::GlobalFeelTouch() {}

GlobalFeelTouch::~GlobalFeelTouch() {}

struct delete_predicate_by_time
{
    bool operator()(Feel::Touch::DenyTouch const& left, DWORD const expire_time) const
    {
        if (left.Expire <= expire_time)
            return true;
        return false;
    };
};
struct objects_ptrs_equal
{
    bool operator()(Feel::Touch::DenyTouch const& left, CObject const* const right) const
    {
        if (left.O == right)
            return true;
        return false;
    }
};

void GlobalFeelTouch::update()
{
    // we ignore P and R arguments, we need just delete evaled denied objects...
    xr_vector<Feel::Touch::DenyTouch>::iterator new_end =
        std::remove_if(feel_touch_disable.begin(), feel_touch_disable.end(), std::bind(delete_predicate_by_time(), std::placeholders::_1, Device.dwTimeGlobal));
    feel_touch_disable.erase(new_end, feel_touch_disable.end());
}

bool GlobalFeelTouch::is_object_denied(CObject const* O)
{
    /*Fvector temp_vector;
    feel_touch_update(temp_vector, 0.f);*/
    if (std::find_if(feel_touch_disable.begin(), feel_touch_disable.end(), std::bind(objects_ptrs_equal(), std::placeholders::_1, O)) == feel_touch_disable.end())
    {
        return false;
    }
    return true;
}

// Сборщик Lua в режиме gc_step -- ручки (консоль, NoSave), см. script_gc:
//   lua_gc_step_max_us -- предел одного шага в мкс, 0 = без предела (LUA_GCSTEP);
//   lua_gc_stepmul / lua_gc_pause -- темп и пауза LuaJIT.
// ⭐stepmul 100, было 5. 📏05-10-2026, ПК, Кордон, 60 fps, после того как
// обёртки luabind перестали быть финализируемыми (luaJIT_newuserdata_nofinalizer):
//   stepmul 5   -- куча 46-74 МБ, сборщик 0.10 мс/кадр, атомарная фаза 3-4.3 мс;
//   stepmul 100 -- куча 30-46 МБ, 0.15 мс/кадр, атомарная 2.4-3 мс;
//   stepmul 200 -- куча 29-41 МБ, 0.19 мс/кадр, атомарная 2.3-3 мс.
// Для Switch с его бюджетом памяти -25 МБ кучи стоят +0.05 мс второго потока.
// (До той правки stepmul 5 при 60 fps не успевал вовсе: куча 37 -> 213 МБ за
// 2 минуты и продолжала расти.)
int psLUA_GC_STEP_MAX_US = 0;
int psLUA_GC_STEPMUL = 100;
int psLUA_GC_PAUSE = 100;
// lua_gc_udmove: живые userdata без __gc уходят из списка, который атомарная фаза
// обходит целиком (gc_udmove в lj_gc.c). lua_gc_udcensus: кто на этом списке.
int psLUA_GC_UDMOVE = 1;
int psLUA_GC_UDCENSUS = 0;
// lua_gc_udsettle: уже размеченные живые userdata откладываются по ходу разметки,
// и атомарная фаза их не обходит (gc_udsettle в lj_gc.c).
int psLUA_GC_UDSETTLE = 1;

void CLevel::script_gc() const
{
    ZoneScoped;

    // The collector is a full user of the VM, not a side activity: lj_gc_step
    // runs finalizers, which is where 20-08's DEP EXECUTE fault landed. Named
    // separately from the binders so the inventory can tell them apart.
    LUA_VM_WATCH_SCOPE("engine::script_gc");
    // ⚠⚠Зонд стоит ЗДЕСЬ, а не на вызывающей стороне, и это результат ошибки
    // 19-09: сначала он был поставлен в device.cpp, но тот путь работает ТОЛЬКО
    // при gc_timeout, а при gc_step/gc_default сборку зовёт seqParallel
    // (Level.cpp, OnFrame). Прибор показал "gc_step почти бесплатен", тогда как
    // его цена просто уехала в чужую фазу. Правило: мерить ФУНКЦИЮ, а не одну
    // из её вызывающих сторон.
    CpuBudget::PhaseScope _ph(CpuBudget::PH_SCRIPTGC);

    //int memory_before_kb = lua_gc(ai().script_engine().lua(), LUA_GCCOUNT, 0);

    switch (ps_lua_gc_method)
    {
    case gc_step: {
        // ⭐ШАГ СБОРЩИКА АДАПТИВЕН ПО РАЗМЕРУ КУЧИ (19-09-2026).
        //
        // 📏Развёртка `lua_gcstep` при fpslock60 (A/B в одном прогоне, по 25 с):
        //     шаг 32 КБ  -> сборка 0.35-0.43 core-ms/кадр, куча встаёт на 33 МБ
        //     шаг 128 КБ -> 1.06-1.37,                     куча 27 МБ
        //     шаг 512 КБ -> 4.83-4.91,                     куча 26 МБ
        // Цена шага почти линейна, а выигрыш по памяти -- нет: за переход с 32
        // на 512 КБ платится в ДВЕНАДЦАТЬ раз больше процессора ради 7 МБ.
        //
        // ⚠Но и «поставить 32 и забыть» нельзя: постоянный маленький шаг ничем
        // не ограничивает кучу сверху. Пока игра аллоцирует меньше, чем шаг
        // успевает собирать (32 КБ x 60 = 1.9 МБ/с), куча стоит; в бою или на
        // тяжёлой локации она поедет вверх, и ничто её не вернёт.
        // ⇒ шаг растёт, когда куча выше цели, и падает, когда ниже. Дёшево в
        // обычном режиме, жёстко при перерасходе.
        const int heap_kb = lua_gc(ai().script_engine().lua(), LUA_GCCOUNT, 0);
        int step = psLUA_GCSTEP;
        if (heap_kb > psLUA_GC_TARGET_KB + psLUA_GC_TARGET_KB / 4)
            step *= 8; // выше цели на четверть -- догонять всерьёз
        else if (heap_kb > psLUA_GC_TARGET_KB)
            step *= 2;
        else if (heap_kb < psLUA_GC_TARGET_KB - psLUA_GC_TARGET_KB / 4)
            step /= 4; // сильно ниже цели -- идти накатом

        // ⭐РЫВКИ СБОРЩИКА РАЗ В ~3 С -- ЭТО БЫЛА АТОМАРНАЯ ФАЗА, А В НЕЙ --
        // ФИНАЛИЗИРУЕМЫЕ USERDATA LUABIND.
        // 📏04-10-2026: шаг «в 8 КБ» раз в несколько секунд стоил 14-29 мс на
        // Switch, 9-16 на ПК. Прибор (luaJIT_gcprof*, строка цикла ниже): 70 из
        // 73 долгих шагов -- атомарная фаза (неделимая), и в ней 90% --
        // «udata»: ~55 тыс. объектов luabind за цикл (game_object 33k, Fvector
        // 10k, alife() 4k...), каждый с __gc, хотя финализатор у обёртки
        // ничего не делает. ✅05-10: такие обёртки создаются без финализатора
        // (luaJIT_newuserdata_nofinalizer + зависимости в env, policy.hpp /
        // object_rep.cpp): атомарная фаза 13-15 -> 2.5-3 мс, финализаций за
        // цикл 55 тыс. -> ~500. Строки цикла оставлены как прибор.
        lua_State* L = ai().script_engine().lua();
        {
            static int s_stepmul = -1, s_pause = -1;
            if (s_stepmul != psLUA_GC_STEPMUL)
                lua_gc(L, LUA_GCSETSTEPMUL, s_stepmul = psLUA_GC_STEPMUL);
            if (s_pause != psLUA_GC_PAUSE)
                lua_gc(L, LUA_GCSETPAUSE, s_pause = psLUA_GC_PAUSE);
            luaJIT_gc_udmove(psLUA_GC_UDMOVE);
            luaJIT_gc_udsettle(psLUA_GC_UDSETTLE);
        }
        luaJIT_gcprof(L, 1, nullptr, nullptr, nullptr, nullptr); // сброс + вкл
        const size_t debt_before = luaJIT_gcdebt(L);
        const u64 t0 = CPU::QPC();
        const int res = luaJIT_gcstep_capped(L, std::max(step, 8), psLUA_GC_STEP_MAX_US);
        const double step_ms = 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq());

        // Цикл закончился -- его портрет: сколько шла разметка (окно, в
        // котором скрипты пачкают уже размеченные таблицы), из чего сложилась
        // атомарная фаза, сжатие таблицы строк.
        static double s_cycle_gc_ms = 0.0; // вся работа сборщика за цикл
        s_cycle_gc_ms += step_ms;
        if (res > 0)
        {
            double a_us[5], mark_us = 0.0, strresize_us = 0.0;
            size_t a_sz[5], heap = 0;
            unsigned grayagain = 0, mark_steps = 0;
            luaJIT_gcprof_cycle(a_us, a_sz, &grayagain, &mark_us, &mark_steps, &heap, &strresize_us);
            Msg("~ [lua gc] cycle: %.1f ms of GC over it | mark %.0f ms wall, %u objects | heap %u KB at atomic | ATOMIC %.2f ms = remark %.2f + roots %.2f "
                "+ grayagain %.2f (%u objs, %u KB) + udata %.2f (%u KB) + weak %.2f | str shrink %.2f ms | stepmul %d pause %d",
                s_cycle_gc_ms, mark_us / 1000.0, mark_steps, u32(heap >> 10), (a_us[0] + a_us[1] + a_us[2] + a_us[3] + a_us[4]) / 1000.0, a_us[0] / 1000.0,
                a_us[1] / 1000.0, a_us[2] / 1000.0, grayagain, u32(a_sz[2] >> 10), a_us[3] / 1000.0, u32(a_sz[3] >> 10), a_us[4] / 1000.0, strresize_us / 1000.0,
                psLUA_GC_STEPMUL, psLUA_GC_PAUSE);
            // Кто был этим мусором: финализированные за цикл объекты luabind по
            // классам (их и разбирает «udata» атомарной фазы).
            string1024 census;
            luabind::detail::gc_census_take(census, sizeof(census), 10);
            Msg("~ [lua gc] finalized this cycle: %s", census);
            // 📏Switch v227: «udata» 4-7 мс при 100-400 КБ финализированного --
            // время уходит на обход списка ЖИВЫХ userdata (см. gc_udmove).
            unsigned ud_visited = 0, ud_moved = 0, ud_settled = 0, ud_pre = 0;
            double ud_walk_us = 0.0;
            luaJIT_gc_udlist_stats(&ud_visited, &ud_moved, &ud_walk_us, &ud_settled, &ud_pre);
            Msg("~ [lua gc] udata list: atomic walked %u, moved off %u, walk %.2f ms | settled while marking %u (%u looks) | udmove %d udsettle %d", ud_visited,
                ud_moved, ud_walk_us / 1000.0, ud_settled, ud_pre, psLUA_GC_UDMOVE, psLUA_GC_UDSETTLE);
            if (psLUA_GC_UDCENSUS)
            {
                luabind::detail::gc_udlist_census(L, census, sizeof(census), 12);
                Msg("~ [lua gc] udata list now: %s", census);
            }
            s_cycle_gc_ms = 0.0;
        }

        // Неделимый кусок (атомарная фаза, огромная таблица, финализатор)
        // предел не остановит -- его и ловит эта строка.
        if (step_ms > 8.0)
        {
            double sum[6], mx[6];
            unsigned n[6];
            size_t maxcost = 0;
            const int state = luaJIT_gcprof(L, 1, sum, mx, n, &maxcost);
            Msg("~ [lua gc] slow step %.1f ms (%d KB asked, debt %u KB before, heap %d KB, %s, now in state %d): propagate %.1f (%u, max %.2f, biggest cost %u) | "
                "atomic %.1f | sweepstring %.1f (%u) | sweep %.1f (%u, max %.2f) | finalize %.1f (%u, max %.2f) | pause %.1f ms",
                step_ms, std::max(step, 8), u32(debt_before >> 10), heap_kb, res > 0 ? "cycle finished" : (res < 0 ? "capped" : "paid"), state, sum[1] / 1000.0, n[1],
                mx[1] / 1000.0, u32(maxcost), sum[2] / 1000.0, sum[3] / 1000.0, n[3], sum[4] / 1000.0, n[4], mx[4] / 1000.0, sum[5] / 1000.0, n[5], mx[5] / 1000.0,
                sum[0] / 1000.0);
        }

        // Сводка раз в ~5 с: цена сборщика на кадр (среднее и худший шаг),
        // сколько шагов упёрлось в предел, куда идёт долг. Растущий долг =
        // сборщик не успевает за скриптами (куча поедет вверх -- см. heap).
        {
            static u32 s_last_ms = 0, s_steps = 0, s_capped = 0;
            static size_t s_debt_max = 0;
            static double s_sum_ms = 0.0, s_max_ms = 0.0;
            ++s_steps;
            if (res < 0)
                ++s_capped;
            s_sum_ms += step_ms;
            s_max_ms = std::max(s_max_ms, step_ms);
            s_debt_max = std::max(s_debt_max, debt_before);
            const u32 now = Device.dwTimeContinual;
            if (!s_last_ms)
                s_last_ms = now;
            else if (now - s_last_ms >= 5000)
            {
                Msg("~ [lua gc] %u steps: avg %.3f ms, max %.2f ms | capped %u (at %d us) | debt now %u KB, max %u KB | heap %d KB | stepmul %d pause %d", s_steps,
                    s_sum_ms / double(s_steps), s_max_ms, s_capped, psLUA_GC_STEP_MAX_US, u32(luaJIT_gcdebt(L) >> 10), u32(s_debt_max >> 10), heap_kb,
                    psLUA_GC_STEPMUL, psLUA_GC_PAUSE);
                s_last_ms = now;
                s_steps = s_capped = 0;
                s_debt_max = 0;
                s_sum_ms = s_max_ms = 0.0;
            }
        }

        lua_gc(ai().script_engine().lua(), LUA_GCSTOP, 0);
        break;
    }
    case gc_timeout: {
        // чистит столько раз пока не вышел timeout. это типо лучше, НО иногда бывает что какой то шаг очистки может занять сильно больше времени
        lua_gc(ai().script_engine().lua(), LUA_GCTIMEOUT, psLUA_GCTIMEOUT);

        lua_gc(ai().script_engine().lua(), LUA_GCSTOP, 0);
        break;
    }
    case gc_default: {
        // луаджит чистит себе память сам когда захочет.
        lua_gc(ai().script_engine().lua(), LUA_GCRESTART, 0);
        break;
    }
    }

    // ⭐СКОЛЬКО ПАМЯТИ ДЕРЖИТ VM И КАКОВ БЮДЖЕТ СБОРЩИКА (при cpu_budget >= 1).
    //
    // 📏19-09-2026 прибор cpu_budget 2 показал, что сборка мусора -- САМАЯ
    // ДОРОГАЯ статья кадра: 2.09 core-ms/кадр, 26% всей работы процесса, больше
    // записи команд (1.4) и планировщика (1.1). И это ровно бюджет: метод
    // gc_timeout крутит lj_gc_step, пока не истечёт psLUA_GCTIMEOUT, а он не
    // меньше psLUA_GCTIMEOUT_MIN = 2000 МКРОСЕКУНД на кадр.
    // ⇒ сборщику ВЫДЕЛЕНО 2 мс каждый кадр, и он их тратит, независимо от того,
    // сколько мусора реально произвела игра.
    //
    // Куча печатается рядом с бюджетом не для красоты: резать время сборки,
    // не глядя на кучу, -- это обменять процессор на утечку и получить хитч
    // позже. Любой A/B по методу сборки читать ПАРОЙ: core-ms и KB.
    if (CpuBudget::enabled)
    {
        static u32 s_last_ms = 0;
        static u32 s_calls = 0;
        ++s_calls;
        const u32 now = Device.dwTimeContinual;
        if (!s_last_ms)
            s_last_ms = now;
        else if (now - s_last_ms >= 5000)
        {
            const int kb = lua_gc(ai().script_engine().lua(), LUA_GCCOUNT, 0);
            Msg("[lua gc] heap %d KB | method %u | budget %d us/call | %u calls in %.1f s",
                kb, ps_lua_gc_method, psLUA_GCTIMEOUT, s_calls, double(now - s_last_ms) / 1000.0);
            s_last_ms = now;
            s_calls = 0;
        }
    }
}
