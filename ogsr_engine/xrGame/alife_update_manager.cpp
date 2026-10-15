////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_update_manager.h
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator update manager
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_update_manager.h"
#include "alife_simulator_header.h"
#include "alife_time_manager.h"
#include "alife_graph_registry.h"
#include "alife_schedule_registry.h"
#include "alife_spawn_registry.h"
#include "alife_object_registry.h"
#include "ef_storage.h"
#include "xrServer.h"
#include "Level.h"
#include "graph_engine.h"
#include "alife_ext_hooks.h"
#include "../xr_3da/x_ray.h"
#include "restriction_space.h"

#include "mt_config.h"
#include "GamePersistent.h"
#include "../xrCore/lua_vm_watch.h"
#include "../xrCore/fs_load_prefetch.h"
#include "script_engine.h"
#include "ai_space.h"

using namespace ALife;

extern string_path g_last_saved_game;

// ---------------------------------------------------------------------------
// ⭐Прибор `al_slow_ms` (см. alife_slow_probe.h). Апдейт ALife в один момент
// идёт на одном потоке (второй поток кадра или главный на первом тике), поэтому
// состояние ниже статическое и без замков.
// ---------------------------------------------------------------------------
namespace alife_slow
{
// Switch: 15 -- each printed line is a write to the SD from the second thread
// (measured up to ~10 ms), so a low threshold made the probe a source of hitches.
#ifdef __SWITCH__
int threshold_ms = 15;
#else
int threshold_ms = 8;
#endif
int sched_budget_us = 2000;

namespace
{
struct Worst
{
    float ms = 0.f;
    char phase = 0;
    u16 id = 0xffff;
    string64 name{};
    string64 sect{};
    int online_before = -1, online_after = -1;
    string256 lua{};
};

struct PhaseSum
{
    double ms = 0.0;
    u32 n = 0;
};

const CALifeUpdateManager* s_mgr = nullptr;
bool s_on = false;
u64 s_item_t0 = 0;
u16 s_item_id = 0xffff;
int s_item_online = -1;
Worst s_worst[2]; // [0] самый долгий объект апдейта, [1] второй
PhaseSum s_sum_switch, s_sum_sched;
u32 s_printed = 0, s_print_window = 0;
// Сколько стоила ПЕЧАТЬ прошлой строки: она идёт со второго потока в лог на SD,
// и на Switch v155 задача ALife разошлась с замером апдейта на ~20 мс (30.9
// против 10.2) -- подозрение на сброс лога. Время печати в замер апдейта не
// входит, поэтому показывается отдельно, в следующей строке.
float s_last_print_ms = 0.f, s_max_print_ms = 0.f;
// Срезы пачки бюджетом (al_sched_budget_us) за окно сводки.
u32 s_cuts = 0, s_cut_done = 0, s_cut_window = 0;

double qpc_ms(u64 dt) { return 1000.0 * double(dt) / double(CPU::QPCFreq()); }

CSE_ALifeDynamicObject* find_obj(u16 id) { return s_mgr ? s_mgr->objects().object(id, true) : nullptr; }

void put_worst(const Worst& w)
{
    if (w.ms > s_worst[0].ms)
    {
        s_worst[1] = s_worst[0];
        s_worst[0] = w;
    }
    else if (w.ms > s_worst[1].ms)
        s_worst[1] = w;
}

void print_worst(char* buf, unsigned size, const Worst& w)
{
    if (w.id == 0xffff)
    {
        buf[0] = 0;
        return;
    }
    const char* ph = w.phase == phase_switch ? "switch" : "sched";
    const char* move = "";
    if (w.online_before == 0 && w.online_after == 1)
        move = " WENT ONLINE";
    else if (w.online_before == 1 && w.online_after == 0)
        move = " went offline";
    xr_sprintf(buf, size, "%s '%s' [%s] id %u %.1f ms%s%s%s%s", ph, w.name, w.sect, w.id, w.ms, move, w.lua[0] ? " | lua: " : "", w.lua,
               w.name[0] ? "" : " (released in its own update)");
}
} // namespace

void update_begin(const CALifeUpdateManager* mgr)
{
    s_on = threshold_ms > 0;
    if (!s_on)
        return;
    s_mgr = mgr;
    s_worst[0] = s_worst[1] = Worst{};
    s_sum_switch = s_sum_sched = PhaseSum{};
}

void item_begin(const CSE_Abstract* object)
{
    if (!s_on)
        return;
    s_item_id = object ? object->ID : 0xffff;
    const CSE_ALifeDynamicObject* d = find_obj(s_item_id);
    s_item_online = d ? int(d->m_bOnline) : -1;
    lua_vm_watch::capture_begin();
    s_item_t0 = CPU::QPC();
}

void item_end(EPhase phase)
{
    if (!s_on)
        return;
    const float ms = float(qpc_ms(CPU::QPC() - s_item_t0));
    PhaseSum& sum = phase == phase_switch ? s_sum_switch : s_sum_sched;
    sum.ms += ms;
    ++sum.n;

    // Имя и Lua-входы снимаются только у кандидата в двое худших: это
    // единственное, что попадёт в строку.
    if (ms <= s_worst[1].ms)
    {
        lua_vm_watch::capture_end(nullptr, 0);
        return;
    }
    Worst w;
    w.ms = ms;
    w.phase = char(phase);
    w.id = s_item_id;
    w.online_before = s_item_online;
    lua_vm_watch::capture_end(w.lua, sizeof(w.lua));
    if (const CSE_ALifeDynamicObject* d = find_obj(s_item_id))
    {
        xr_strcpy(w.name, d->name_replace() ? d->name_replace() : "?");
        xr_strcpy(w.sect, d->s_name.size() ? d->s_name.c_str() : "?");
        w.online_after = int(d->m_bOnline);
    }
    put_worst(w);
}

void note_budget_cut(u32 done_in_batch)
{
    ++s_cuts;
    s_cut_done += done_in_batch;
}

namespace
{
// Печать с замером её собственной цены.
template <typename... Args>
void timed_msg(const char* fmt, Args... args)
{
    const u64 tp = CPU::QPC();
    Msg(fmt, args...);
    s_last_print_ms = float(qpc_ms(CPU::QPC() - tp));
    if (s_last_print_ms > s_max_print_ms)
        s_max_print_ms = s_last_print_ms;
}
} // namespace

// t_sched_end..t1 = spawn_prefetch_tick: 📏11-10 (au7, a fight) the second thread's "ALife update" took 243 ms with
// no [alife slow] line -- the time was after the scheduled pass, where the probe did not look.
void update_end(u64 t0, u64 t_switch_end, u64 t_sched_end, u64 t1)
{
    if (!s_on)
        return;
    s_on = false;
    const u32 now = Device.dwTimeContinual;

    // Сводка бюджета пачки раз в 10 с (только если срезы были).
    if (!s_cut_window)
        s_cut_window = now;
    if (now - s_cut_window >= 10000)
    {
        if (s_cuts)
            timed_msg("~ [alife sched] budget %d us cut %u batches in %.0f s (%.1f objects per cut batch) | log line print max %.1f ms", sched_budget_us, s_cuts,
                      float(now - s_cut_window) / 1000.f, float(s_cut_done) / float(s_cuts), s_max_print_ms);
        s_cut_window = now;
        s_cuts = s_cut_done = 0;
        s_max_print_ms = 0.f;
    }

    const double total = qpc_ms(t1 - t0);
    if (total < double(threshold_ms))
        return;

    // Рывки идут сериями по кадрам подряд, и каждая строка нужна, но поток
    // строк ограничен: не больше 20 за 5 с.
    if (now - s_print_window > 5000)
    {
        s_print_window = now;
        s_printed = 0;
    }
    if (++s_printed > 20)
        return;

    string512 w0, w1;
    print_worst(w0, sizeof(w0), s_worst[0]);
    print_worst(w1, sizeof(w1), s_worst[1]);
    timed_msg("~ [alife slow] update %.1f ms: switch %.1f ms (%u obj, %.1f in them) | scheduled %.1f ms (%u obj) | spawn prefetch %.1f ms | worst %s%s%s | "
              "prev log line print %.1f ms",
              total, qpc_ms(t_switch_end - t0), s_sum_switch.n, s_sum_switch.ms, qpc_ms(t_sched_end - t_switch_end), s_sum_sched.n, qpc_ms(t1 - t_sched_end), w0,
              w1[0] ? " | next " : "", w1, s_last_print_ms);
}
} // namespace alife_slow

class CSwitchPredicate
{
private:
    CALifeSwitchManager* m_switch_manager;

public:
    IC CSwitchPredicate(CALifeSwitchManager* switch_manager) { m_switch_manager = switch_manager; }

    IC bool operator()(CALifeLevelRegistry::_iterator& i, u64 cycle_count, bool) const
    {
        if ((*i).second->m_switch_counter == cycle_count)
            return (false);

        (*i).second->m_switch_counter = cycle_count;
        return (true);
    }

    IC void operator()(CALifeLevelRegistry::_iterator& i, u64 cycle_count) const
    {
        alife_slow::item_begin((*i).second);
        m_switch_manager->switch_object((*i).second);
        alife_slow::item_end(alife_slow::phase_switch);
    }
};

CALifeUpdateManager::CALifeUpdateManager(xrServer* server, LPCSTR section)
    : CALifeSwitchManager(server, section), CALifeSurgeManager(server, section), CALifeStorageManager(server, section), CALifeSimulatorBase(server, section)
{
    shedule.t_min = pSettings->r_s32(section, "schedule_min");
    shedule.t_max = pSettings->r_s32(section, "schedule_max");
    Engine.Sheduler.Register(this, TRUE); // shedule_register();

    m_max_process_time = pSettings->r_s32(section, "process_time");
    m_update_monster_factor = pSettings->r_float(section, "update_monster_factor");
    m_objects_per_update = pSettings->r_u32(section, "objects_per_update");
    m_changing_level = false;
    m_first_time = true;
}

CALifeUpdateManager::~CALifeUpdateManager()
{
    Engine.Sheduler.Unregister(this); // shedule_unregister(true);
    Device.remove_from_seq_parallel(fastdelegate::MakeDelegate(this, &CALifeUpdateManager::update));
}

float CALifeUpdateManager::shedule_Scale()
{
    return (.5f); // (schedule_min + schedule_max)*0.5f
}

// ---------------------------------------------------------------------------
// ⭐УСПЕВАЕТ ЛИ ALIFE ПРИ РАБОТАЮЩЕМ БЮДЖЕТЕ (консольный `al_stats 1`)
//
// До починки единиц (см. set_process_time ниже) каждый апдейт обходил реестр
// ЦЕЛИКОМ, и вопрос "успевает ли" не стоял: полный круг делался всегда. Теперь
// обход режется бюджетом process_time, и правильный вопрос другой -- ЗА КАКОЕ
// ВРЕМЯ реестр обходится целиком. Именно это число определяет, через сколько
// секунд после перехода на уровень NPC вокруг актёра переключатся в онлайн.
//
// ⚠Печатается по dwTimeContinual: игровые часы паузятся и масштабируются
// time_factor, а прибор должен говорить о секундах реального времени.
// ⚠online считается обходом реестра -- раз в 5 с это ничего не стоит, но на
// горячем пути такого делать нельзя.
// ---------------------------------------------------------------------------
int g_al_stats = 0;

// PERIOD OF THE SWITCH SWEEP, in real milliseconds (console `al_switch_period`).
// 0 = every scheduler tick, i.e. the historical behaviour; see the long note at
// update_switch() for why that is wrong and what 100 ms was measured to save.
int g_al_switch_period = 100;

namespace { u32 s_al_switch_last = 0; }

namespace
{
u32 s_al_visited = 0;   // объектов обойдено предикатом переключения
u32 s_al_passes = 0;    // сколько раз звался update_switch
u32 s_al_last_ms = 0;
}

void ALifeStatsOnLevelChange(const char* level_name)
{
    if (!g_al_stats)
        return;
    Msg("[ALife] --- level change -> %s (coverage counters restart) ---", level_name ? level_name : "?");
    s_al_visited = 0;
    s_al_passes = 0;
    s_al_last_ms = Device.dwTimeContinual;
}

void CALifeUpdateManager::update_switch()
{
    init_ef_storage();

    // ----------------------------------------------------------------------
    // HOW OFTEN THE WHOLE REGISTRY IS RE-ASKED "should you be online?"
    //
    // system.ltx [alife_base] has schedule_min = schedule_max = 1 ms, so the
    // scheduler calls this EVERY FRAME. That was harmless while process_time
    // truncated the sweep and while the predicates were too slow to finish it.
    // Both of those stopped being true (see the unit fix below and the luabind
    // wrapper fastpath), and the result is a FULL sweep of ~1070 objects 230
    // times a second.
    //
    // Nothing needs that. The decision the sweep makes is governed by
    // switch_distance = 150 m against an actor moving ~5 m/s: at 10 Hz the
    // worst-case staleness is half a metre out of a hundred and fifty.
    // What it costs is the largest single block of Lua in the build -- the
    // se_stalker / se_monster switch predicates are 52% of all VM samples --
    // and Lua cannot be parallelised away, there is one lua_State.
    //
    // Real milliseconds, not game ones: the sweep is about where the ACTOR is,
    // and dwTimeGlobal is both pausable and scaled by time_factor.
    // ----------------------------------------------------------------------
    if (g_al_switch_period > 0)
    {
        const u32 now = Device.dwTimeContinual;
        if (s_al_switch_last && now - s_al_switch_last < (u32)g_al_switch_period)
            return;
        s_al_switch_last = now;
    }

    offline_budget_reset(); // `al_offline_budget` is per sweep
    u32 visited = 0;
    START_PROFILE("ALife/switch");
    visited = graph().level().update(CSwitchPredicate(this));
    STOP_PROFILE

    if (!g_al_stats)
        return;

    s_al_visited += visited;
    ++s_al_passes;
    const u32 now = Device.dwTimeContinual;
    if (!s_al_last_ms)
        s_al_last_ms = now;
    if (now - s_al_last_ms < 5000)
        return;

    const float secs = float(now - s_al_last_ms) / 1000.f;
    s_al_last_ms = now;

    u32 total = 0, online = 0;
    for (const auto& kv : graph().level().objects())
    {
        ++total;
        if (kv.second->m_bOnline)
            ++online;
    }

    const float per_sec = float(s_al_visited) / secs;
    Msg("[ALife] registry %u (online %u) | %.0f obj/s over %.1f passes/s | full sweep every %.0f ms | %.1f obj/pass",
        total, online, per_sec, float(s_al_passes) / secs,
        per_sec > 0.f ? 1000.f * float(total) / per_sec : -1.f,
        s_al_passes ? float(s_al_visited) / float(s_al_passes) : 0.f);
    s_al_visited = 0;
    s_al_passes = 0;
}

void CALifeUpdateManager::update_scheduled(bool init_ef)
{
    if (init_ef)
        init_ef_storage();

    START_PROFILE("ALife/scheduled");
    scheduled().update();
    STOP_PROFILE
}

extern void spawn_prefetch_tick();

void CALifeUpdateManager::update()
{
    const u64 t0 = CPU::QPC();
    alife_slow::update_begin(this);
    update_switch();
    const u64 t_switch_end = CPU::QPC();
    update_scheduled(false);
    const u64 t_sched_end = CPU::QPC();
    spawn_prefetch_tick();   // Level_network_spawn.cpp: read ahead what is about to come online
    alife_slow::update_end(t0, t_switch_end, t_sched_end, CPU::QPC());
}

void CALifeUpdateManager::shedule_Update(u32 dt)
{
    ISheduled::shedule_Update(dt);

    if (!initialized())
        return;

    if (!m_first_time && g_mt_config.test(mtALife))
    {
        Device.add_to_seq_parallel(fastdelegate::MakeDelegate(this, &CALifeUpdateManager::update), "ALife update");
        return;
    }

    m_first_time = false;

    START_PROFILE("ALife/update")
    update();
    STOP_PROFILE
}

// ⚠⚠ЕДИНИЦЫ. Сюда приходят МИКРОСЕКУНДЫ (system.ltx: "process_time = 900 ; microseconds"),
// а потребитель — CSafeMapIterator::time_over() — сравнивает с CTimer::GetElapsed_sec(),
// то есть с СЕКУНДАМИ. Без деления условие "прошло 900 секунд" не наступало НИКОГДА,
// и обход реестра шёл до полного круга на КАЖДОМ апдейте ALife вместо отведённых 0.9 мс.
//
// 📏Цена бага (замер 18-09-2026, Кордон, ~950 объектов): предикаты переключения
// (se_stalker/se_monster can_switch_online/offline) съедали 626 тысяч входов в Lua-VM
// за 5 секунд на ШЕСТИ потоках — ~1.05 мс CPU на кадр, притом что один полный обход
// стоит ~3.2 мс. Это была самая крупная одиночная статья процессорного времени.
//
// ⭐Починка восстанавливает ЗАМЫСЕЛ, а не меняет его: бюджет затем и заведён в конфиге.
// ⚠Побочный эффект ожидаем и правилен: объекты переключаются online/offline не разом,
// а порциями по 0.9 мс за апдейт — как и было задумано автором настройки.
void CALifeUpdateManager::set_process_time(int microseconds)
{
    const float seconds = (float(microseconds) - float(microseconds) * update_monster_factor() / 1000000.f) / 1000000.f;
    graph().set_process_time(seconds);
}

void CALifeUpdateManager::objects_per_update(const u32& objects_per_update) { scheduled().objects_per_update(objects_per_update); }

void CALifeUpdateManager::init_ef_storage() const { ai().ef_storage().alife_evaluation(true); }

bool CALifeUpdateManager::change_level(NET_Packet& net_packet)
{
    if (m_changing_level)
        return (false);

    //	prepare_objects_for_save		();
    // we couldn't use prepare_objects_for_save since we need
    // get updates from client
    // then change actor server entity
    // then call client net_Save
    // then restore actor server entity
    Level().ClientSend();

    m_changing_level = true;

    GameGraph::_GRAPH_ID safe_graph_vertex_id = graph().actor()->m_tGraphID;
    u32 safe_level_vertex_id = graph().actor()->m_tNodeID;
    Fvector safe_position = graph().actor()->o_Position;
    Fvector safe_angles = graph().actor()->o_Angle;
    SRotation safe_torso = graph().actor()->o_torso;

    GameGraph::_GRAPH_ID holder_safe_graph_vertex_id = GameGraph::_GRAPH_ID(-1);
    u32 holder_safe_level_vertex_id = u32(-1);
    Fvector holder_safe_position = Fvector().set(flt_max, flt_max, flt_max);
    Fvector holder_safe_angles = Fvector().set(flt_max, flt_max, flt_max);
    CSE_ALifeObject* holder = 0;

    net_packet.r(&graph().actor()->m_tGraphID, sizeof(graph().actor()->m_tGraphID));
    net_packet.r(&graph().actor()->m_tNodeID, sizeof(graph().actor()->m_tNodeID));
    net_packet.r_vec3(graph().actor()->o_Position);
    net_packet.r_vec3(graph().actor()->o_Angle);

    Level().ClientSave();

    graph().actor()->o_torso.yaw = graph().actor()->o_Angle.y;
    graph().actor()->o_torso.pitch = graph().actor()->o_Angle.x;
    graph().actor()->o_torso.roll = 0.f;

    if (graph().actor()->m_holderID != 0xffff)
    {
        holder = objects().object(graph().actor()->m_holderID);

        holder_safe_graph_vertex_id = holder->m_tGraphID;
        holder_safe_level_vertex_id = holder->m_tNodeID;
        holder_safe_position = holder->o_Position;
        holder_safe_angles = holder->o_Angle;

        holder->m_tGraphID = graph().actor()->m_tGraphID;
        holder->m_tNodeID = graph().actor()->m_tNodeID;
        holder->o_Position = graph().actor()->o_Position;
        holder->o_Angle = graph().actor()->o_Angle;
    }

    string256 autoave_name;
    strconcat(sizeof(autoave_name), autoave_name, Core.UserName, "_", "autosave");
    LPCSTR temp0 = strchr(**m_server_command_line, '/');
    VERIFY(temp0);
    string256 temp;
    *m_server_command_line = strconcat(sizeof(temp), temp, autoave_name, temp0);

    save(autoave_name);

    graph().actor()->m_tGraphID = safe_graph_vertex_id;
    graph().actor()->m_tNodeID = safe_level_vertex_id;
    graph().actor()->o_Position = safe_position;
    graph().actor()->o_Angle = safe_angles;
    graph().actor()->o_torso = safe_torso;

    if (graph().actor()->m_holderID != 0xffff)
    {
        VERIFY(holder);
        holder->m_tGraphID = holder_safe_graph_vertex_id;
        holder->m_tNodeID = holder_safe_level_vertex_id;
        holder->o_Position = holder_safe_position;
        holder->o_Angle = holder_safe_angles;
    }

    GamePersistent().models_savePrefetch();

    return (true);
}

#include "../xr_3da/IGame_Persistent.h"
void CALifeUpdateManager::new_game(LPCSTR save_name)
{
    g_pGamePersistent->LoadTitle("st_creating_new_game");
    Msg("* Creating new game...");

    // Half a second of every level load lands in here and has never been split.
    // Same convention as the render phases ([load step]) so one log reads end to end.
    CTimer _t; _t.Start();
    float ms_unload = 0.f, ms_spawns = 0.f, ms_graph = 0.f, ms_spawnNew = 0.f, ms_reg = 0.f, ms_save = 0.f;
    auto lap = [&_t] { const float v = _t.GetElapsed_ms_total(); _t.Start(); return v; };

    unload();
    reload(m_section);
    ms_unload = lap();

    spawns().load(save_name);
    ms_spawns = lap();

    graph().on_load();
    ms_graph = lap();

    server().PerformIDgen(0x0000);
    time_manager().init(m_section);
    VERIFY(can_register_objects());

    can_register_objects(false);
    spawn_new_objects();
    can_register_objects(true);
    ms_spawnNew = lap();

    CALifeObjectRegistry::OBJECT_REGISTRY::iterator I = objects().objects().begin();
    CALifeObjectRegistry::OBJECT_REGISTRY::iterator E = objects().objects().end();
    for (; I != E; ++I)
        (*I).second->on_register();
    ms_reg = lap();

    save(save_name);
    ms_save = lap();

    Msg("[load step]   alife new_game: unload+reload %.0f | spawns().load %.0f | graph %.0f | spawn_new_objects %.0f | on_register %.0f | save %.0f ms",
        ms_unload, ms_spawns, ms_graph, ms_spawnNew, ms_reg, ms_save);
    Msg("* New game is successfully created!");
}

void CALifeUpdateManager::load(LPCSTR game_name, bool no_assert, bool new_only)
{
    g_pGamePersistent->LoadTitle("st_loading_alife_simulator");
    // the SD is idle while the save is parsed: start reading what the last loaded level read after it
    LoadPrefetch::Begin();

    strcpy_s(g_last_saved_game, game_name);

    CTimer _tPhase; _tPhase.Start();
    const bool _fromSave = !new_only;
    if (new_only || !CALifeStorageManager::load(game_name))
    {
        R_ASSERT3(new_only || no_assert && xr_strlen(game_name), "Cannot find the specified saved game ", game_name);
        new_game(game_name);
        ALifeExt::fire_on_new_game(); // optional out-of-tree game module; no-op in stock engine
    }
    Msg("[load step] alife simulator: %.0f ms (%s)", _tPhase.GetElapsed_ms_total(),
        _fromSave ? "tried a saved game first" : "new game");

    g_pGamePersistent->LoadTitle("st_server_connecting");
    g_pGamePersistent->SetTip();
}

void CALifeUpdateManager::reload(LPCSTR section)
{
    CALifeSimulatorBase::reload(section);
    set_process_time((int)m_max_process_time);
    objects_per_update(m_objects_per_update);
}

bool CALifeUpdateManager::load_game(LPCSTR game_name, bool no_assert)
{
    {
        string_path temp, file_name;
        strconcat(sizeof(temp), temp, game_name, SAVE_EXTENSION);
        FS.update_path(file_name, "$game_saves$", temp);
        if (!FS.exist(file_name))
        {
            R_ASSERT3(no_assert, "There is no saved game ", file_name);
            return (false);
        }
    }
    string512 S, S1;
    strcpy_s(S, **m_server_command_line);
    LPSTR temp = strchr(S, '/');
    R_ASSERT2(temp, "Invalid server options!");
    strconcat(sizeof(S1), S1, game_name, temp);
    *m_server_command_line = S1;
    return (true);
}

void CALifeUpdateManager::set_switch_online(ALife::_OBJECT_ID id, bool value)
{
    CSE_ALifeDynamicObject* object = objects().object(id);
    VERIFY(object);
    object->can_switch_online(value);
}

void CALifeUpdateManager::set_switch_offline(ALife::_OBJECT_ID id, bool value)
{
    CSE_ALifeDynamicObject* object = objects().object(id);
    VERIFY(object);
    object->can_switch_offline(value);
}

void CALifeUpdateManager::set_interactive(ALife::_OBJECT_ID id, bool value)
{
    CSE_ALifeDynamicObject* object = objects().object(id);
    VERIFY(object);
    object->interactive(value);
}

void CALifeUpdateManager::jump_to_level(LPCSTR level_name) const
{
    ALifeStatsOnLevelChange(level_name);   // al_stats: restart the coverage counters
    const CGameGraph::SLevel& level = ai().game_graph().header().level(level_name);
    GameGraph::_GRAPH_ID dest = GameGraph::_GRAPH_ID(-1);

    GraphEngineSpace::CGameLevelParams evaluator(level.id());
    bool failed = !ai().graph_engine().search(ai().game_graph(), graph().actor()->m_tGraphID, GameGraph::_GRAPH_ID(-1), 0, evaluator);
    if (failed)
    {
        Msg("! Cannot build path via game graph from the current level to the level %s!", level_name);
        float min_dist = flt_max;
        Fvector current = ai().game_graph().vertex(graph().actor()->m_tGraphID)->game_point();
        GameGraph::_GRAPH_ID n = ai().game_graph().header().vertex_count();
        for (GameGraph::_GRAPH_ID i = 0; i < n; ++i)
            if (ai().game_graph().vertex(i)->level_id() == level.id())
            {
                float distance = ai().game_graph().vertex(i)->game_point().distance_to_sqr(current);
                if (distance < min_dist)
                {
                    min_dist = distance;
                    dest = i;
                }
            }
        if (!ai().game_graph().vertex(dest))
        {
            Msg("! There is no game vertices on the level %s, cannot jump to the specified level", level_name);
            return;
        }
    }
    else
        dest = (GameGraph::_GRAPH_ID)evaluator.selected_vertex_id();

    NET_Packet net_packet;
    net_packet.w_begin(M_CHANGE_LEVEL);
    net_packet.w(&dest, sizeof(dest));

    u32 vertex_id = ai().game_graph().vertex(dest)->level_vertex_id();
    net_packet.w(&vertex_id, sizeof(vertex_id));

    Fvector level_point = ai().game_graph().vertex(dest)->level_point();
    net_packet.w(&level_point, sizeof(level_point));
    net_packet.w_vec3(Fvector().set(0.f, 0.f, 0.f));
    Level().Send(net_packet, net_flags(TRUE));
}

void CALifeUpdateManager::teleport_object(ALife::_OBJECT_ID id, GameGraph::_GRAPH_ID game_vertex_id, u32 level_vertex_id, const Fvector& position)
{
    CSE_ALifeDynamicObject* object = objects().object(id, true);
    if (!object)
    {
        Msg("! cannot teleport entity with id %d", id);
        return;
    }

    if (object->m_bOnline)
        switch_offline(object);
    graph().change(object, object->m_tGraphID, game_vertex_id);
    object->m_tNodeID = level_vertex_id;
    object->o_Position = position;
    CSE_ALifeMonsterAbstract* monster_abstract = smart_cast<CSE_ALifeMonsterAbstract*>(object);
    if (monster_abstract)
        monster_abstract->m_tNextGraphID = object->m_tGraphID;
}

void CALifeUpdateManager::add_restriction(ALife::_OBJECT_ID id, ALife::_OBJECT_ID restriction_id, const RestrictionSpace::ERestrictorTypes& restriction_type)
{
    CSE_ALifeDynamicObject* object = objects().object(id, true);
    if (!object)
    {
        Msg("! cannot add restriction with id %d to the entity with id %d, because there is no creature with the specified id", restriction_id, id);
        return;
    }

    CSE_ALifeDynamicObject* object_restrictor = objects().object(restriction_id, true);
    if (!object_restrictor)
    {
        Msg("! cannot add restriction with id %d to the entity with id %d, because there is no space restrictor with the specified id", restriction_id, id);
        return;
    }

    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(object);
    if (!creature)
    {
        Msg("! cannot add restriction with id %d to the entity with id %d, because there is an object with the specified id, but it is not a creature", restriction_id, id);
        return;
    }

    CSE_ALifeSpaceRestrictor* restrictor = smart_cast<CSE_ALifeSpaceRestrictor*>(object_restrictor);
    if (!restrictor)
    {
        Msg("! cannot add restriction with id %d to the entity with id %d, because there is an object with the specified id, but it is not a space restrictor", restriction_id, id);
        return;
    }

    switch (restriction_type)
    {
    case RestrictionSpace::eRestrictorTypeOut: {
        creature->m_dynamic_out_restrictions.push_back(restriction_id);

        break;
    }
    case RestrictionSpace::eRestrictorTypeIn: {
        creature->m_dynamic_in_restrictions.push_back(restriction_id);

        break;
    }
    default: {
        Msg("! Invalid restriction type!");
        return;
    }
    }
}

void CALifeUpdateManager::remove_restriction(ALife::_OBJECT_ID id, ALife::_OBJECT_ID restriction_id, const RestrictionSpace::ERestrictorTypes& restriction_type)
{
    CSE_ALifeDynamicObject* object = objects().object(id, true);
    if (!object)
    {
        Msg("! cannot remove restriction with id %d to the entity with id %d, because there is no creature with the specified id", restriction_id, id);
        return;
    }

    CSE_ALifeDynamicObject* object_restrictor = objects().object(restriction_id, true);
    if (!object_restrictor)
    {
        Msg("! cannot remove restriction with id %d to the entity with id %d, because there is no space restrictor with the specified id", restriction_id, id);
        return;
    }

    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(object);
    if (!creature)
    {
        Msg("! cannot remove restriction with id %d to the entity with id %d, because there is an object with the specified id, but it is not a creature", restriction_id, id);
        return;
    }

    CSE_ALifeSpaceRestrictor* restrictor = smart_cast<CSE_ALifeSpaceRestrictor*>(object_restrictor);
    if (!restrictor)
    {
        Msg("! cannot remove restriction with id %d to the entity with id %d, because there is an object with the specified id, but it is not a space restrictor", restriction_id,
            id);
        return;
    }

    switch (restriction_type)
    {
    case RestrictionSpace::eRestrictorTypeOut: {
        xr_vector<ALife::_OBJECT_ID>::iterator I = std::find(creature->m_dynamic_out_restrictions.begin(), creature->m_dynamic_out_restrictions.end(), restriction_id);
        if (I == creature->m_dynamic_out_restrictions.end())
        {
            Msg("~ cannot remove restriction with id [%d][%s] to the entity with id [%d][%s], because it is not added", restriction_id, object_restrictor->name_replace(), id,
                object->name_replace());
            return;
        }

        creature->m_dynamic_out_restrictions.erase(I);

        break;
    }
    case RestrictionSpace::eRestrictorTypeIn: {
        xr_vector<ALife::_OBJECT_ID>::iterator I = std::find(creature->m_dynamic_in_restrictions.begin(), creature->m_dynamic_in_restrictions.end(), restriction_id);
        if (I == creature->m_dynamic_in_restrictions.end())
        {
            Msg("~ cannot remove restriction with id [%d][%s] to the entity with id [%d][%s], because it is not added", restriction_id, object_restrictor->name_replace(), id,
                object->name_replace());
            return;
        }

        creature->m_dynamic_in_restrictions.erase(I);

        break;
    }
    default: {
        Msg("! Invalid restriction type!");
        return;
    }
    }
}

void CALifeUpdateManager::remove_all_restrictions(ALife::_OBJECT_ID id, const RestrictionSpace::ERestrictorTypes& restriction_type)
{
    CSE_ALifeDynamicObject* object = objects().object(id, true);
    if (!object)
    {
        Msg("! cannot remove restrictions to the entity with id %d, because there is no creature with the specified id", id);
        return;
    }

    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(object);
    if (!creature)
    {
        Msg("! cannot remove restriction to the entity with id [%d][%s], because there is an object with the specified id, but it is not a creature", id, object->name_replace());
        return;
    }

    switch (restriction_type)
    {
    case RestrictionSpace::eRestrictorTypeOut: {
        creature->m_dynamic_out_restrictions.clear();
        break;
    }
    case RestrictionSpace::eRestrictorTypeIn: {
        creature->m_dynamic_in_restrictions.clear();
        break;
    }
    default: NODEFAULT;
    }
}
