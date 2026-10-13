#include "StdAfx.h"
#include "../xrCore/load_prof.h"   // per-spawn breakdown ([spawn prof])
#include <atomic>
#include "xrServer_Objects_ALife_All.h"
#include "Level.h"
#include "game_cl_base.h"
#include "ui_rml.h"          // UIRml::PrewarmInventory
#include "NET_Queue.h"
#include "ai_space.h"
#include "game_level_cross_table.h"
#include "level_graph.h"
#include "client_spawn_manager.h"
#include "../xr_3da/xr_object.h"
#include "../xr_3da/IGame_Persistent.h"

// ============================================================================
// Spawn-burst profiling
// ============================================================================
// The load screen reports 'st_client_spawning' as ~115 ms, and that number is a
// lie of omission: the queue is drained by ProcessGameEvents on the FIRST
// precache frame, which the frame trace measures at ~400 ms (seq:Lvl/gameEv=290).
// So half a second of the load lives here and nobody has ever seen its parts.
// QPC, not CTimer: ~1800 objects x a handful of samples is far too few to care
// about the read cost, and QPC does not truncate the way GetElapsed_ms_total does.
namespace SpawnProf
{
u64 s_entCreate = 0, s_entRead = 0, s_objCreate = 0, s_netSpawn = 0, s_visual = 0, s_after = 0, s_entDtor = 0, s_total = 0;
// Objects.Create split (IGame_ObjectPool::create) and net_Spawn split (CGameObject).
u64 s_clsid = 0, s_alloc = 0, s_objLoad = 0;
u64 s_upgrades = 0, s_reload = 0, s_reinit = 0, s_inherited = 0, s_binder = 0, s_binderReload = 0;
u32 s_count = 0, s_postponed = 0, s_visuals = 0;

// Per-section tally. A flat total says "the spawn costs 300 ms"; it does not say
// WHICH of the 1161 objects is worth looking at, and the sections repeat heavily
// (one level ships dozens of identical restrictors / lamps / items).
struct SectTally { u32 n = 0; u64 clk = 0; };
xr_map<xr_string, SectTally> s_bySect;

struct scope
{
    u64& sink;
    u64  t0;
    scope(u64& s) : sink(s), t0(CPU::QPC()) {}
    ~scope() { sink += CPU::QPC() - t0; }
};

void Dump()
{
    if (!s_count)
        return;
    const double k = 1000.0 / double(CPU::QPCFreq());
    Msg("[load step] client spawn burst: %u objects this frame | total %.0f ms = entity create %.0f + read %.0f + "
        "Objects.Create %.0f + net_Spawn %.0f + callbacks %.0f + entity free %.0f ms | %u postponed",
        s_count, k * double(s_total), k * double(s_entCreate), k * double(s_entRead), k * double(s_objCreate),
        k * double(s_netSpawn), k * double(s_after), k * double(s_entDtor), s_postponed);
    Msg("[load step]   Objects.Create split: r_clsid %.0f | new %.0f | Load(section) %.0f ms",
        k * double(s_clsid), k * double(s_alloc), k * double(s_objLoad));
    Msg("[load step]   net_Spawn split: visual %.0f (%u visuals) | upgrades %.0f | reload %.0f | reinit %.0f | "
        "inherited %.0f | script binder %.0f (reload %.0f) ms", k * double(s_visual), s_visuals, k * double(s_upgrades),
        k * double(s_reload), k * double(s_reinit), k * double(s_inherited), k * double(s_binder),
        k * double(s_binderReload));

    // Top sections by total time (create + net_Spawn), so the next question is
    // "why is THIS class 60 us" and not "why is a spawn 250 us".
    {
        xr_vector<std::pair<xr_string, SectTally>> top(s_bySect.begin(), s_bySect.end());
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.clk > b.second.clk; });
        xr_string line;
        for (u32 i = 0; i < top.size() && i < 16; ++i) {
            string256 one;
            xr_sprintf(one, "%s%s x%u %.0fms", i ? " | " : "", top[i].first.c_str(), top[i].second.n,
                       k * double(top[i].second.clk));
            line += one;
        }
        Msg("[load step]   spawn top sections (%u distinct): %s", (u32)s_bySect.size(), line.c_str());
    }
    s_bySect.clear();
    s_entCreate = s_entRead = s_objCreate = s_netSpawn = s_visual = s_after = s_entDtor = s_total = 0;
    s_clsid = s_alloc = s_objLoad = 0;
    s_upgrades = s_reload = s_reinit = s_inherited = s_binder = s_binderReload = 0;
    s_count = s_postponed = s_visuals = 0;
}
}   // namespace SpawnProf

// RAII for game_spawn_queue_cs (xrCriticalSection has no guard type)
struct SpawnQueueLock
{
    xrCriticalSection& cs;
    SpawnQueueLock(xrCriticalSection& c) : cs(c) { cs.Enter(); }
    ~SpawnQueueLock() { cs.Leave(); }
};

void CLevel::cl_Process_Spawn(NET_Packet& P)
{
    SpawnProf::scope _profAll(SpawnProf::s_total);
    ++SpawnProf::s_count;

    // Begin analysis
    shared_str s_name;
    P.r_stringZ(s_name);

    // Create DC (xrSE)
    CSE_Abstract* E = nullptr;
    { SpawnProf::scope _p(SpawnProf::s_entCreate); E = F_entity_Create(*s_name); }
    R_ASSERT2(E, *s_name);

    {
        SpawnProf::scope _p(SpawnProf::s_entRead);
        E->Spawn_Read(P);
        if (E->s_flags.is(M_SPAWN_UPDATE))
            E->UPDATE_Read(P);
    }
    //-------------------------------------------------
    //	Msg ("M_SPAWN - %s[%d][%x] - %d", *s_name,  E->ID, E,E->ID_Parent);
    //-------------------------------------------------
    // diag: actor spawns are rare and load-bearing for multiplayer -- log every
    // one with the flags that decide local-vs-puppet binding
    if (smart_cast<CSE_ALifeCreatureActor*>(E))
        Msg("* [net] cl_Process_Spawn: ACTOR id=%u '%s' local=%d asplayer=%d pos=(%.1f, %.1f, %.1f)", E->ID, E->name_replace(), E->s_flags.is(M_SPAWN_OBJECT_LOCAL) ? 1 : 0,
            E->s_flags.is(M_SPAWN_OBJECT_ASPLAYER) ? 1 : 0, VPUSH(E->o_Position));
    // force object to be local for server client
    {
        E->s_flags.set(M_SPAWN_OBJECT_LOCAL, TRUE);
    };

    if (std::find(m_just_destroyed.begin(), m_just_destroyed.end(), E->ID) != m_just_destroyed.end())
    {
        Msg("* [%s]: skip just destroyed [%s] ID: [%u] ID_Parent: [%u]", __FUNCTION__, E->name_replace(), E->ID, E->ID_Parent);
        m_just_destroyed.erase(std::remove(m_just_destroyed.begin(), m_just_destroyed.end(), E->ID), m_just_destroyed.end());
        F_entity_Destroy(E);
        return;
    }

    if (Device.dwPrecacheFrame == 0)
    {
        const CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(E);
        const CSE_ALifeTraderAbstract* trader = smart_cast<CSE_ALifeTraderAbstract*>(E);
        bool postpone = (trader || monster) && !is_removing_objects();
        game_spawn_queue_cs.Enter();
        if (!postpone)
        {
            for (const auto& it : game_spawn_queue)
            {
                if (E->ID_Parent == it->ID)
                {
                    postpone = true;
                    break;
                }
            }
        }
        if (postpone)
        {
            //Msg( "* [%s]: delay spawn ID[%d] ID_Parent[%d] name_replace[%s]", __FUNCTION__, E->ID, E->ID_Parent, E->name_replace() );
            game_spawn_queue.push_back(E);
            game_spawn_queue_cs.Leave();
            ++SpawnProf::s_postponed;
            return;
        }
        game_spawn_queue_cs.Leave();
    }

    /*
    game_spawn_queue.push_back(E);
    if (g_bDebugEvents)		ProcessGameSpawns();
    /*/
    g_sv_Spawn(E);

    { SpawnProf::scope _p(SpawnProf::s_entDtor); F_entity_Destroy(E); }
    //*/
};

void CLevel::g_cl_Spawn(LPCSTR name, u8 rp, u16 flags, Fvector pos)
{
    // Create
    CSE_Abstract* E = F_entity_Create(name);
    VERIFY(E);

    // Fill
    E->s_name = name;
    E->set_name_replace("");
    E->s_gameid = u8(GameID());
    E->s_RP = rp;
    E->ID = 0xffff;
    E->ID_Parent = 0xffff;
    E->ID_Phantom = 0xffff;
    E->s_flags.assign(flags);
    E->RespawnTime = 0;
    E->o_Position = pos;

    // Send
    NET_Packet P;
    E->Spawn_Write(P, TRUE);
    Send(P, net_flags(TRUE));

    // Destroy
    F_entity_Destroy(E);
}

#ifdef DEBUG
extern Flags32 psAI_Flags;
#include "ai_debug.h"
#endif // DEBUG

void CLevel::g_sv_Spawn(CSE_Abstract* E)
{
    // Where one gameplay spawn spends its time, in disjoint parts that add up to
    // the whole (xrCore/load_prof.h). On the Switch a soldier with his kit took
    // ~170 ms of the second thread and the [spawn] line below cannot say where:
    // most of it is inside the class chain's net_Spawn and inside model_Create.
    LoadProf::Arm _lpArm;
    const u64 _lpT0 = CPU::QPC();
    {
    LoadProf::Scope _lpRoot("spawn");
    [&]() {
    //-----------------------------------------------------------------
    //	CTimer		T(false);

#ifdef DEBUG
    Msg("* CLIENT: Spawn: %s, ID=%d", *E->s_name, E->ID);
#endif

    auto obj = Objects.net_Find(E->ID);
    if (obj && obj->getDestroy())
    {
        Msg("[%s]: %s[%u] already net_Spawn'ed, call ProcessDestroyQueue()", __FUNCTION__, obj->cName().c_str(), obj->ID());
        Objects.ProcessDestroyQueue();
    }

    // Client spawn
    //	T.Start		();
    // Чужой контент: для неизвестной секции сервер уже подставил inert-рестриктор
    // (см. F_entity_Create), но s_name остаётся оригинальным — клиентский Create
    // упал бы фаталом на pSettings->r_string(секция, "class"). Зеркалим placeholder.
    const char* client_section = *E->s_name;
    if (!pSettings->section_exist(client_section))
    {
        Msg("! [%s] unknown section '%s' -> client space_restrictor placeholder", __FUNCTION__, client_section);
        client_section = "space_restrictor";
    }
    const u64 _profSectT0 = CPU::QPC();
    // Gameplay spawn timing (the load-time burst has SpawnProf::Dump): one line per client
    // spawn over 15 ms, split the same way -- on the Switch a nearby anomaly's artifact
    // took ~160 ms of the second thread here.
    const u64 _p0Create = SpawnProf::s_objCreate, _p0Net = SpawnProf::s_netSpawn, _p0After = SpawnProf::s_after,
              _p0Vis = SpawnProf::s_visual, _p0Bind = SpawnProf::s_binder, _p0Reinit = SpawnProf::s_reinit,
              _p0Inh = SpawnProf::s_inherited, _p0Rel = SpawnProf::s_reload;
    CObject* O = nullptr;
    { SpawnProf::scope _p(SpawnProf::s_objCreate); LoadProf::Scope _lp("obj.create"); O = Objects.Create(client_section); }
    // Msg				("--spawn--CREATE: %f ms",1000.f*T.GetAsync());

    //	T.Start		();
    if (0 == O)
    {
        Msg("! Failed to create entity '%s'", *E->s_name);
        return;
    }
    bool _spawn_ok;
    { SpawnProf::scope _p(SpawnProf::s_netSpawn); _spawn_ok = !!O->net_Spawn(E); }
    if (!_spawn_ok)
    {
        O->setDestroy(TRUE);
        O->net_Destroy();
        client_spawn_manager().clear(O->ID());
        Objects.Destroy(O);
        Msg("! Failed to spawn entity '%s'", *E->s_name);
    }
    else
    {
        SpawnProf::scope _p(SpawnProf::s_after);
        client_spawn_manager().callback(O);
        // Msg			("--spawn--SPAWN: %f ms",1000.f*T.GetAsync());
        if ((E->s_flags.is(M_SPAWN_OBJECT_LOCAL)) && (E->s_flags.is(M_SPAWN_OBJECT_ASPLAYER)))
        {
            if (CurrentEntity() != NULL)
            {
                CGameObject* pGO = smart_cast<CGameObject*>(CurrentEntity());
                if (pGO)
                    pGO->On_B_NotCurrentEntity();
            }
            SetEntity(O);
            SetControlEntity(O);
        }

        if (0xffff != E->ID_Parent)
        {
            NET_Packet GEN;
            GEN.write_start();
            GEN.read_start();
            GEN.w_u16(u16(O->ID()));
            cl_Process_Event(E->ID_Parent, GE_OWNERSHIP_TAKE, GEN);
        }
    }
    //---------------------------------------------------------
    { SpawnProf::scope _p(SpawnProf::s_after); Game().OnSpawn(O); }
    //---------------------------------------------------------
    {
        SpawnProf::SectTally& t = SpawnProf::s_bySect[client_section];
        ++t.n;
        t.clk += CPU::QPC() - _profSectT0;
    }
    {
        const double k = 1000.0 / double(CPU::QPCFreq());
        const double ms = k * double(CPU::QPC() - _profSectT0);
        if (ms > 15.0 && !Device.dwPrecacheFrame)
            Msg("~ [spawn] '%s' id %u took %.0f ms: Objects.Create %.0f | net_Spawn %.0f (visual %.0f, reload %.0f, reinit %.0f, "
                "inherited %.0f, script binder %.0f) | callbacks %.0f",
                client_section, E->ID, ms, k * double(SpawnProf::s_objCreate - _p0Create), k * double(SpawnProf::s_netSpawn - _p0Net),
                k * double(SpawnProf::s_visual - _p0Vis), k * double(SpawnProf::s_reload - _p0Rel), k * double(SpawnProf::s_reinit - _p0Reinit),
                k * double(SpawnProf::s_inherited - _p0Inh), k * double(SpawnProf::s_binder - _p0Bind), k * double(SpawnProf::s_after - _p0After));
    }
    }();
    }
    if (_lpArm.outer)
    {
        // in the load's spawn burst (precache frames: 1050 objects in one frame) only the heavy ones -- the actor alone
        // was 3.8 s of a 16 s synchronising phase on the Switch (07-10)
        const double ms = 1000.0 * double(CPU::QPC() - _lpT0) / double(CPU::QPCFreq());
        if (ms > (Device.dwPrecacheFrame ? 100.0 : 15.0))
        {
            string1024 parts;
            _lpArm.format(parts, sizeof(parts), 0.5);
            Msg("~ [spawn prof] '%s' id %u %.0f ms: %s", E->s_name.c_str(), E->ID, ms, parts);
        }
    }
}

CSE_Abstract* CLevel::spawn_item(LPCSTR section, const Fvector& position, u32 level_vertex_id, u16 parent_id, bool return_item)
{
    CSE_Abstract* abstract = F_entity_Create(section);
    R_ASSERT3(abstract, "Cannot find item with section", section);
    CSE_ALifeDynamicObject* dynamic_object = smart_cast<CSE_ALifeDynamicObject*>(abstract);
    if (dynamic_object && ai().get_level_graph())
    {
        dynamic_object->m_tNodeID = level_vertex_id;
        if (ai().level_graph().valid_vertex_id(level_vertex_id) && ai().get_game_graph() && ai().get_cross_table())
            dynamic_object->m_tGraphID = ai().cross_table().vertex(level_vertex_id).game_vertex_id();
    }

    //оружие спавним с полным магазинои
    CSE_ALifeItemWeapon* weapon = smart_cast<CSE_ALifeItemWeapon*>(abstract);
    if (weapon)
        weapon->a_elapsed = weapon->get_ammo_magsize();

    // Fill
    abstract->s_name = section;
    abstract->set_name_replace(section);
    abstract->s_gameid = u8(GameID());
    abstract->o_Position = position;
    abstract->s_RP = 0xff;
    abstract->ID = 0xffff;
    abstract->ID_Parent = parent_id;
    abstract->ID_Phantom = 0xffff;
    abstract->s_flags.assign(M_SPAWN_OBJECT_LOCAL);
    abstract->RespawnTime = 0;

    if (!return_item)
    {
        NET_Packet P;
        abstract->Spawn_Write(P, TRUE);
        Send(P, net_flags(TRUE));
        F_entity_Destroy(abstract);
        return (0);
    }
    else
        return (abstract);
}


// When ProcessGameSpawns last spawned something (Device.dwTimeContinual): the HUD-model prefetcher waits for a calm
// (player_hud.cpp prefetch_loop) instead of holding the model pool lock under a spawn burst.
std::atomic<u32> g_spawn_last_ms{0};

// 📏08-10-2026 Switch, on the move (walk_v269): a soldier's clone came online in ONE frame of the second thread, 22-26 ms --
// the body + his logic ~14 ms and his ~15 items (Objects.Create + Load, model pool, ownership) ~9 ms. The stock queue
// already spawns one creature per frame; with this the creature has its frame alone and its inventory follows in the
// next ones, at most this long per frame (the first item always goes). Until the last item is in, the stalker's
// scheduled update waits (CAI_Stalker::m_inventory_spawn_pending). Only stalkers: the rest keep the stock order.
// spawn_items_budget_us 0 = stock (the creature and all its items in one frame).
int g_spawn_items_budget_us = 5000;

#include "ai/stalker/ai_stalker.h"

namespace
{
struct InventoryPending { u16 id; u32 frame; };
xr_vector<InventoryPending> s_inventoryPending;   // stalkers whose items still wait in game_spawn_queue (under game_spawn_queue_cs)

void set_inventory_pending(u16 id, bool on)
{
    if (CAI_Stalker* S = smart_cast<CAI_Stalker*>(Level().Objects.net_Find(id)))
        S->m_inventory_spawn_pending = on;
}

bool has_queued_children(const xr_deque<CSE_Abstract*>& queue, u16 id)
{
    return std::any_of(queue.begin(), queue.end(), [id](const CSE_Abstract* E) { return E->ID_Parent == id; });
}
} // namespace

void CLevel::ProcessGameSpawns()
{
    SpawnQueueLock guard(game_spawn_queue_cs);
    CSE_Abstract* trader = nullptr;
    if (!game_spawn_queue.empty())
        g_spawn_last_ms.store(Device.dwTimeContinual, std::memory_order_relaxed);
    // never behind the load screen: there the whole queue goes at once
    const bool spread = g_spawn_items_budget_us > 0 && !Device.dwPrecacheFrame;
    const u64 t0 = CPU::QPC(), budget = u64(g_spawn_items_budget_us) * CPU::QPCFreq() / 1000000;
    bool spent = false;
    while (!game_spawn_queue.empty())
    {
        CSE_Abstract* E = game_spawn_queue.front();
        const bool creature = smart_cast<CSE_ALifeMonsterAbstract*>(E) || smart_cast<CSE_ALifeTraderAbstract*>(E);
        // a creature only starts a frame; items stop at the budget
        if (spread && spent && (creature || CPU::QPC() - t0 >= budget))
            break;
        game_spawn_queue.pop_front();
        //Msg( "* [%s]: delayed spawn dwFrame[%u] ID[%d] ID_Parent[%d] name_replace[%s]", __FUNCTION__, Device.dwFrame, E->ID, E->ID_Parent, E->name_replace() );
        g_sv_Spawn(E);
        spent = true;
        if (creature)
        {
            if (spread && smart_cast<CSE_ALifeHumanStalker*>(E) && has_queued_children(game_spawn_queue, E->ID))
            {
                set_inventory_pending(E->ID, true);
                s_inventoryPending.push_back({E->ID, Device.dwFrame});
                F_entity_Destroy(E);
                break;
            }
            trader = E;
            break;
        }
        F_entity_Destroy(E);
    }

    // the last item is in (or the queue dropped them with a GE_DESTROY): the stalker updates from the next frame
    std::erase_if(s_inventoryPending, [&](const InventoryPending& p) {
        if (has_queued_children(game_spawn_queue, p.id))
            return false;
        set_inventory_pending(p.id, false);
        if (Device.dwFrame - p.frame > 4)   // normally 1-2 frames: the queue is being held up by something
            Msg("~ [spawn items] stalker %u waited %u frames for its inventory", p.id, Device.dwFrame - p.frame);
        return true;
    });

    if (trader)
    {
        for (auto& E : game_spawn_queue)
        {
            if (E->ID_Parent == trader->ID)
            {
                // Msg( "* [%s]: delayed spawn dwFrame[%u] trader[%d] ID[%d] ID_Parent[%d] name_replace[%s]", __FUNCTION__, Device.dwFrame, trader->ID, E->ID, E->ID_Parent, E->name_replace() );
                g_sv_Spawn(E);
            }
        }
        
        std::erase_if(game_spawn_queue, [&](auto& E) {
            if (E->ID_Parent == trader->ID)
            {
                F_entity_Destroy(E);
                return true;
            }
            return false;
        });

        F_entity_Destroy(trader);
    }

    // the load's spawn burst is over: build the rest of the level's NPC models + animations now, behind the load screen
    extern void npc_prewarm_level();
    if (Device.dwPrecacheFrame && game_spawn_queue.empty())
    {
        npc_prewarm_level();
        UIRml::PrewarmInventory();   // 10-10: the first inventory of a fight was a 129 ms frame (once per run)
    }
}

void CLevel::ProcessGameSpawnsDestroy(u16 dest, u16 type, NET_Packet& P)
{
    if (type != GE_DESTROY)
        return;

    SpawnQueueLock guard(game_spawn_queue_cs);
    game_spawn_queue.erase(std::remove_if(game_spawn_queue.begin(), game_spawn_queue.end(),
                                          [&](auto& E) {
                                                  if (E->ID == dest || E->ID_Parent == dest)
                                                  {
                                                      // Msg( "* [CLevel::ProcessGameSpawnsDestroy]: delayed spawn GE_DESTROY dest[%d] ID[%d] ID_Parent[%d] name_replace[%s]", dest,
                                                      // E->ID, E->ID_Parent, E->name_replace() );
                                                      F_entity_Destroy(E);
                                                      return true;
                                                  }
                                              return false;
                                          }),
                           game_spawn_queue.end());
}

// ============================================================================
// dbg_spawn_prefetch_list: which files would gameplay spawns on THIS level read?
// ============================================================================
// On the Switch ~40% of a spawn is reading its files from the SD (model, its
// textures, its .omf). Before caching them at level load, measure what "all of
// them" is: every ALife object registered on the actor's level, its visual, and
// the OGF followed the way the loader follows it (OGF_TEXTURE of every child,
// motion refs, LOD model). Sizes are this install's; the list is written to
// $logs$/spawn_prefetch_list.txt so the Switch pack's sizes can be summed too.
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "game_graph.h"
#include "Actor.h"
#include "ai_object_location.h"
#include "../xr_3da/fmesh.h"

namespace
{
struct PrefetchSet
{
    xr_set<xr_string> models, textures, motions;
};

void prefetch_add_list(xr_set<xr_string>& dst, LPCSTR list, LPCSTR ext)
{
    const u32 n = _GetItemCount(list);
    for (u32 i = 0; i < n; ++i)
    {
        string_path item;
        _GetItem(list, i, item);
        _strlwr(item);
        if (!item[0] || strchr(item, '*'))
            continue;
        xr_string s = item;
        if (ext && !strext(item))
            s += ext;
        dst.insert(s);
    }
}

void prefetch_walk_ogf(IReader* R, PrefetchSet& set)
{
    if (R->find_chunk(OGF_TEXTURE))
    {
        string_path tex, shader;
        R->r_stringZ(tex, sizeof(tex));
        R->r_stringZ(shader, sizeof(shader));
        prefetch_add_list(set.textures, tex, ".dds");
    }
    if (R->find_chunk(OGF_S_MOTION_REFS))
    {
        string4096 refs;
        R->r_stringZ(refs, sizeof(refs));
        prefetch_add_list(set.motions, refs, ".omf");
    }
    else if (R->find_chunk(OGF_S_MOTION_REFS2))
    {
        const u32 n = R->r_u32();
        for (u32 i = 0; i < n; ++i)
        {
            string_path ref;
            R->r_stringZ(ref, sizeof(ref));
            prefetch_add_list(set.motions, ref, ".omf");
        }
    }
    if (IReader* L = R->open_chunk(OGF_S_LODS))
    {
        string_path lod;
        L->r_string(lod, sizeof(lod));
        prefetch_add_list(set.models, lod, ".ogf");
        L->close();
    }
    if (IReader* C = R->open_chunk(OGF_CHILDREN))
    {
        IReader* child;
        for (u32 i = 0; nullptr != (child = C->open_chunk(i)); ++i)
        {
            prefetch_walk_ogf(child, set);
            child->close();
        }
        C->close();
    }
}

u64 prefetch_size(LPCSTR root, const xr_string& name, u64& packed, u32& missing)
{
    const CLocatorAPI::file* f = FS.exist(root, name.c_str());
    if (!f)
    {
        ++missing;
        return 0;
    }
    packed += f->size_compressed;
    return f->size_real;
}
} // namespace

void spawn_prefetch_list_dump()
{
    if (!ai().get_alife() || !Actor())
    {
        Msg("! [prefetch list] needs a running single-player level");
        return;
    }
    const u32 my_level = ai().game_graph().vertex(Actor()->ai_location().game_vertex_id())->level_id();

    PrefetchSet set;
    u32 objects = 0, offline = 0, with_visual = 0;
    for (const auto& kv : ai().alife().objects().objects())
    {
        CSE_ALifeDynamicObject* so = kv.second;
        if (!ai().game_graph().valid_vertex_id(so->m_tGraphID) || ai().game_graph().vertex(so->m_tGraphID)->level_id() != my_level)
            continue;
        ++objects;
        if (!so->m_bOnline)
            ++offline;
        LPCSTR vis = nullptr;
        if (const CSE_Visual* v = smart_cast<CSE_Visual*>(so); v && v->get_visual() && v->get_visual()[0])
            vis = v->get_visual();
        else if (pSettings->line_exist(so->s_name, "visual"))
            vis = pSettings->r_string(so->s_name, "visual");
        if (!vis || !vis[0])
            continue;
        ++with_visual;
        prefetch_add_list(set.models, vis, ".ogf");
    }

    // Follow every model (the set grows while walking: LOD models are models too).
    xr_set<xr_string> walked;
    u32 unreadable = 0;
    for (bool grew = true; grew;)
    {
        grew = false;
        const xr_set<xr_string> pending = set.models;
        for (const xr_string& m : pending)
        {
            if (!walked.insert(m).second)
                continue;
            grew = true;
            IReader* R = FS.r_open("$game_meshes$", m.c_str());
            if (!R)
            {
                ++unreadable;
                continue;
            }
            prefetch_walk_ogf(R, set);
            FS.r_close(R);
        }
    }

    string_path out;
    FS.update_path(out, "$logs$", "spawn_prefetch_list.txt");
    IWriter* W = FS.w_open(out);
    u64 real[3]{}, packed[3]{};
    u32 missing[3]{};
    const struct { const xr_set<xr_string>* s; LPCSTR root; LPCSTR dir; LPCSTR kind; } groups[3] = {
        {&set.models, "$game_meshes$", "meshes\\", "model"},
        {&set.textures, "$game_textures$", "textures\\", "texture"},
        {&set.motions, "$game_meshes$", "meshes\\", "motion"},
    };
    for (u32 g = 0; g < 3; ++g)
        for (const xr_string& f : *groups[g].s)
        {
            u64 pk = 0;
            const u64 sz = prefetch_size(groups[g].root, f, pk, missing[g]);
            real[g] += sz;
            packed[g] += pk;
            if (W)
            {
                string1024 line;
                xr_sprintf(line, "%s %llu %llu %s%s", groups[g].kind, (unsigned long long)sz, (unsigned long long)pk, groups[g].dir, f.c_str());
                W->w_string(line);
            }
        }
    if (W)
        FS.w_close(W);

    const double MB = 1.0 / (1024.0 * 1024.0);
    Msg("~ [prefetch list] level %u: %u objects (%u offline), %u with a visual -> %u models, %u textures, %u motion files (%u models unreadable)", my_level, objects,
        offline, with_visual, u32(set.models.size()), u32(set.textures.size()), u32(set.motions.size()), unreadable);
    Msg("~ [prefetch list] bytes: models %.1f MB, textures %.1f MB, motions %.1f MB = %.1f MB (in archives %.1f MB) | missing %u/%u/%u | list: %s", MB * real[0],
        MB * real[1], MB * real[2], MB * (real[0] + real[1] + real[2]), MB * (packed[0] + packed[1] + packed[2]), missing[0], missing[1], missing[2], out);
}

// ============================================================================
// SpawnPrefetch: read the files of objects about to come online, ahead of time
// ============================================================================
// ALife puts an object online at a fixed distance (Switch: 72 m), and its first
// spawn then reads model + textures + .omf from the SD on the second thread while
// the main thread waits (v160: ~35-40 ms of a 115 ms soldier). Everything on the
// level is ~360 MB on the Switch -- more than is free -- so only the ring just
// beyond the online distance is read, nearest first, by one worker into
// FSReadyCache (xrCore/fs_ready_cache.h), which r_open hands over on the spawn.
// Budget-bounded; what never came online is dropped oldest-first.
// spawn_prefetch_mb (0 = off), spawn_prefetch_margin (metres past the online distance).
#include "../xrCore/fs_ready_cache.h"
#include "../xrCore/fs_load_prefetch.h"
#include "../xr_3da/SkeletonMotions.h" // motions_docked
#include "alife_graph_registry.h"
#include "alife_level_registry.h"
#include <thread>
#include <condition_variable>
#include <string_view>
#include <unordered_set>

int g_spawn_prefetch_mb = 48;
int g_spawn_prefetch_margin = 60;

namespace
{
struct PrefetchWorker
{
    std::thread th;
    std::mutex mx;
    std::condition_variable cv;
    std::deque<xr_string> queue;   // visual names
    bool stop = false;
    // ALife thread only:
    // visuals queued (or seen online) on this level, by the hash of the lower-case name: the 1 Hz tick asks it for
    // every object of the level, and an xr_string key cost a malloc+free per ask -- on the Switch every allocation
    // takes newlib's one malloc lock (suspect of 11-10's 243 ms "ALife update", see [malloc slow])
    std::unordered_set<u64> requested;
    u32 level = u32(-1);
    u32 lastTick = 0, lastReport = 0, queuedSinceReport = 0;
    // worker -> report
    std::atomic<u32> visualsDone{0}, filesRead{0};
    std::atomic<u64> bytesRead{0}, readUs{0};
};

PrefetchWorker& PW()
{
    static PrefetchWorker w;
    return w;
}

void prefetch_put_file(LPCSTR root, LPCSTR name, u64 maxBytes)
{
    string_path full;
    FS.update_path(full, root, name);
    const CLocatorAPI::file* f = FS.exist(full);
    if (!f || f->size_real > maxBytes)
        return;
    const std::string key = FSReadyCache::Key(full);
    if (!FSReadyCache::BeginRead(key))
        return;
    IReader* R = FS.r_open(full);
    if (!R)
    {
        FSReadyCache::EndRead(key);
        return;
    }
    PW().filesRead.fetch_add(1, std::memory_order_relaxed);
    PW().bytesRead.fetch_add(u64(R->length()), std::memory_order_relaxed);
    if (R->length() > (8 << 20))
        Msg("~ [spawn prefetch] big file ahead of its spawn: %s (%u KB)", name, u32(R->length() >> 10));
    FSReadyCache::Put(key, R);
    FSReadyCache::EndRead(key);
}

void prefetch_visual(const xr_string& vis, int depth)
{
    string_path full;
    FS.update_path(full, "$game_meshes$", vis.c_str());
    if (!FS.exist(full))
        return;
    const std::string key = FSReadyCache::Key(full);
    if (!FSReadyCache::BeginRead(key))
        return;
    IReader* R = FS.r_open(full);
    if (!R)
    {
        FSReadyCache::EndRead(key);
        return;
    }
    PW().filesRead.fetch_add(1, std::memory_order_relaxed);
    PW().bytesRead.fetch_add(u64(R->length()), std::memory_order_relaxed);
    PrefetchSet set;
    prefetch_walk_ogf(R, set);
    R->seek(0);
    FSReadyCache::Put(key, R);
    FSReadyCache::EndRead(key);

    for (const xr_string& t : set.textures)
        prefetch_put_file("$game_textures$", t.c_str(), u64(-1));
    // .omf: a set already docked is never read again, whatever its size -- the big shared ones (stalker_animation*,
    // 28-38 MB) are parsed at level load. 📏12-10-2026 Switch: one that is not was cut by the old 8 MB cap --
    // actors\barman_animation.omf (18 MB, worn by stalker_bandit_6) was read on the spawn in a fight, 350-390 ms
    // (+100 parsing) with the main thread waiting.
    for (const xr_string& m : set.motions)
        if (!motions_docked(m.c_str()))
            prefetch_put_file("$game_meshes$", m.c_str(), 32ull << 20);
    if (depth < 2)
        for (const xr_string& lod : set.models)
            prefetch_visual(lod, depth + 1);
}

void prefetch_worker_loop()
{
    LoadPrefetch::MarkReaderThread();
    FSReadyCache::t_prefetcher = true;
    PrefetchWorker& w = PW();
    for (;;)
    {
        xr_string vis;
        {
            std::unique_lock<std::mutex> l(w.mx);
            w.cv.wait(l, [&] { return w.stop || !w.queue.empty(); });
            if (w.stop)
                return;
            vis = w.queue.front();
            w.queue.pop_front();
        }
        const u64 t0 = CPU::QPC();
        prefetch_visual(vis, 0);
        w.readUs.fetch_add((CPU::QPC() - t0) * 1000000ull / CPU::QPCFreq(), std::memory_order_relaxed);
        w.visualsDone.fetch_add(1, std::memory_order_relaxed);
    }
}

LPCSTR prefetch_visual_of(CSE_ALifeDynamicObject* so)
{
    if (const CSE_Visual* v = smart_cast<CSE_Visual*>(so); v && v->get_visual() && v->get_visual()[0])
        return v->get_visual();
    if (pSettings->line_exist(so->s_name, "visual"))
        return pSettings->r_string(so->s_name, "visual");
    return nullptr;
}

void prefetch_model_name(string_path& name, LPCSTR vis)
{
    xr_strcpy(name, vis);
    _strlwr(name);
    if (!strext(name))
        xr_strcat(name, ".ogf");
}

u64 name_key(LPCSTR name) { return std::hash<std::string_view>{}(std::string_view(name)); }
} // namespace

// Called from CALifeUpdateManager::update (the ALife thread: the registry is not
// touched by anyone else meanwhile). Once a second.
void spawn_prefetch_tick()
{
    PrefetchWorker& w = PW();
    if (!g_spawn_prefetch_mb || !ai().get_alife())
        return;
    const u32 now = Device.dwTimeGlobal;
    if (now - w.lastTick < 1000)
        return;
    w.lastTick = now;

    CALifeSimulator& sim = const_cast<CALifeSimulator&>(ai().alife());
    const CSE_ALifeCreatureActor* actor = sim.graph().actor();
    if (!actor)
        return;
    const u32 level = sim.graph().level().level_id();
    if (level != w.level)
    {
        // The first tick of a level comes during its load: the cache holds what the
        // load prefetcher (fs_load_prefetch.h) read for it, and the last level's files
        // went with spawn_prefetch_shutdown (w.level = -1 since).
        const bool hadLevel = w.level != u32(-1);
        w.level = level;
        w.requested.clear();
        {
            std::lock_guard<std::mutex> g(w.mx);
            w.queue.clear();
        }
        if (hadLevel)
            FSReadyCache::Clear();
    }
    FSReadyCache::SetBudget(size_t(g_spawn_prefetch_mb) << 20);
    if (!w.th.joinable())
        w.th = std::thread(prefetch_worker_loop);

    const float r = sim.online_distance() + float(g_spawn_prefetch_margin);
    const float r2 = r * r;
    xr_vector<std::pair<float, LPCSTR>> want;
    for (const auto& kv : sim.graph().level().objects())
    {
        CSE_ALifeDynamicObject* so = kv.second;
        LPCSTR vis = prefetch_visual_of(so);
        if (!vis || !vis[0])
            continue;
        string_path name;
        prefetch_model_name(name, vis);
        if (so->m_bOnline)
        {
            w.requested.insert(name_key(name));   // online => its model is in the pool already
            continue;
        }
        const float d2 = so->o_Position.distance_to_sqr(actor->o_Position);
        if (d2 >= r2)
            continue;
        if (!w.requested.count(name_key(name)))
            want.emplace_back(d2, vis);
        // Inventory items are not in the level registry at all (graph().attach
        // removes them), so the soldier's rifle is only reachable as his child.
        for (const ALife::_OBJECT_ID child_id : so->children)
        {
            CSE_ALifeDynamicObject* child = ai().alife().objects().object(child_id, true);
            LPCSTR cvis = child ? prefetch_visual_of(child) : nullptr;
            if (!cvis || !cvis[0])
                continue;
            string_path cname;
            prefetch_model_name(cname, cvis);
            if (!w.requested.count(name_key(cname)))
                want.emplace_back(d2, cvis);
        }
    }
    if (!want.empty())
    {
        std::sort(want.begin(), want.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::lock_guard<std::mutex> g(w.mx);
        for (const auto& [d2, vis] : want)
        {
            string_path name;
            prefetch_model_name(name, vis);
            if (!w.requested.insert(name_key(name)).second)
                continue;
            w.queue.push_back(name);
            ++w.queuedSinceReport;
        }
        w.cv.notify_one();
    }

    if (now - w.lastReport >= 10000)
    {
        w.lastReport = now;
        const FSReadyCache::Snapshot c = FSReadyCache::Stats(true);
        const u32 vis = w.visualsDone.exchange(0), files = w.filesRead.exchange(0);
        const u64 bytes = w.bytesRead.exchange(0), us = w.readUs.exchange(0);
        if (w.queuedSinceReport || vis || c.hits)
            Msg("~ [spawn prefetch] 10 s: queued %u visuals (ring %.0f m), read %u visuals / %u files %.1f MB in %.0f ms | cache: %u hits %.1f MB, %u evicted %.1f MB | holds %u files %.1f MB",
                w.queuedSinceReport, r, vis, files, double(bytes) / 1048576.0, double(us) / 1000.0, c.hits, double(c.hitBytes) / 1048576.0, c.evicted,
                double(c.evictedBytes) / 1048576.0, c.files, double(c.bytes) / 1048576.0);
        w.queuedSinceReport = 0;
    }
}

// CLevel teardown: the worker reads through FS, so it must be gone before FS is.
void spawn_prefetch_shutdown()
{
    PrefetchWorker& w = PW();
    if (w.th.joinable())
    {
        {
            std::lock_guard<std::mutex> g(w.mx);
            w.stop = true;
            w.queue.clear();
        }
        w.cv.notify_one();
        w.th.join();
        w.stop = false;
    }
    w.requested.clear();
    w.level = u32(-1);
    FSReadyCache::Clear();
}

// ============================================================================
// NpcPrewarm: the models + animations of everything that lives on the level, built behind the load screen
// ============================================================================
// 📏08-10-2026 Switch, on the move (walk_v268): the first soldier at the bridge came online in 67 ms, his clones in 24-33 --
// the difference is what the first one of a kind builds and the rest find ready: the model (pool), its .omf animations
// (anim.parse, g_pMotionsContainer) and the stalker animation table (stalker.anim_reload 25 ms,
// stalker_animation_data_storage). SpawnPrefetch above only reads the FILES ahead; this builds them once per level, in
// the load's spawn burst (second thread, where spawns create models anyway; ALife runs on the same thread, in turn).
// The instances are held until the level goes: the animation table keeps a pointer to the first skeleton of its kind.
// npc_prewarm 0 = off.
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../Include/xrRender/RenderVisual.h"
#include "stalker_animation_data_storage.h"
#include "sv_anomaly.h"

int g_npc_prewarm = 1;

namespace
{
xr_vector<IRenderVisual*> s_prewarm;   // held until npc_prewarm_release (level teardown)
u32 s_prewarmLevel = u32(-1);

void prewarm_add(xr_map<xr_string, bool>& want, LPCSTR vis, bool stalker)
{
    if (!vis || !vis[0])
        return;
    string_path name;
    xr_strcpy(name, vis);
    _strlwr(name);
    bool& s = want[name];
    s = s || stalker;
}
} // namespace

// From ProcessGameSpawns in the load (Device.dwPrecacheFrame): once per level.
void npc_prewarm_level()
{
    if (!g_npc_prewarm || !ai().get_alife())
        return;
    CALifeSimulator& sim = const_cast<CALifeSimulator&>(ai().alife());
    const u32 level = sim.graph().level().level_id();
    if (level == s_prewarmLevel)
        return;
    s_prewarmLevel = level;

    const u64 t0 = CPU::QPC();
    xr_map<xr_string, bool> want;   // visual -> a stalker wears it (needs the animation table)
    auto add_section = [&want](const shared_str& sect) {
        if (pSettings->section_exist(sect) && pSettings->line_exist(sect, "visual"))
            prewarm_add(want, pSettings->r_string(sect, "visual"), false);
    };
    // 09-10 on the move (walk_v275): what spawns ownerless at runtime -- not queued for the second thread like a
    // creature -- read its model on the main thread: an anomaly's artefact 25 ms, an NPC's thrown grenade 11 ms.
    u32 creatures = 0, loose = 0, extra = 0;
    for (const auto& kv : sim.graph().level().objects())
    {
        CSE_ALifeDynamicObject* so = kv.second;
        CSE_ALifeCreatureAbstract* cr = smart_cast<CSE_ALifeCreatureAbstract*>(so);
        if (!cr)
        {
            // an item lying on the level comes online straight on the main thread
            if (so->ID_Parent == 0xffff && smart_cast<CSE_ALifeInventoryItem*>(so))
            {
                prewarm_add(want, prefetch_visual_of(so), false);
                ++loose;
            }
            continue;
        }
        if (!cr->g_Alive() || smart_cast<CSE_ALifeCreatureActor*>(so))
            continue;
        ++creatures;
        prewarm_add(want, prefetch_visual_of(so), smart_cast<CSE_ALifeHumanStalker*>(so) != nullptr);
        // what it carries: the soldier's rifle comes online with him (inventory items are only reachable as children)
        for (const ALife::_OBJECT_ID child_id : so->children)
            if (CSE_ALifeDynamicObject* child = ai().alife().objects().object(child_id, true))
            {
                prewarm_add(want, prefetch_visual_of(child), false);
                // the mod's grenade throw (xrs_grenade.script) spawns "<grenade>_fake" in flight and "<grenade>_test"
                // for its aim check -- sections of their own, with a model of their own
                for (const char* suffix : {"_fake", "_test"})
                {
                    string256 derived;
                    xr_strconcat(derived, child->s_name.c_str(), suffix);
                    if (pSettings->section_exist(derived))
                    {
                        add_section(derived);
                        ++extra;
                    }
                }
            }
    }
    // what the level's anomaly fields can make (sv_anomaly.cpp Birth)
    xr_vector<shared_str> artefacts;
    SVAnomaly::LevelArtefactSections(artefacts);
    for (const shared_str& a : artefacts)
        add_section(a);

    u32 models = 0, tables = 0, failed = 0;
    for (const auto& [name, stalker] : want)
    {
        IRenderVisual* V = Render->model_Create(name.c_str());
        if (!V)
        {
            ++failed;
            continue;
        }
        s_prewarm.push_back(V);
        ++models;
        if (stalker)
            if (IKinematicsAnimated* K = V->dcast_PKinematicsAnimated(); K && K->LL_MotionsSlotCount())
            {
                stalker_animation_data_storage().object(K);
                ++tables;
            }
    }
    Msg("~ [npc prewarm] level %u: %u models (%u stalker animation tables, %u failed) in %.0f ms -- from %u creatures, %u loose items, %u "
        "grenade _fake/_test hits, %u anomaly artefact sections",
        level, models, tables, failed, 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq()), creatures, loose, extra, (u32)artefacts.size());
}

// Level teardown, right after stalker_animation_data_storage().clear() and before the model pool goes.
void npc_prewarm_release()
{
    for (IRenderVisual*& V : s_prewarm)
        Render->model_Delete(V);
    s_prewarm.clear();
    s_prewarmLevel = u32(-1);
    s_inventoryPending.clear();   // the ids belong to the level that is going (its queue went with SLS_Clear's destroys)
}
