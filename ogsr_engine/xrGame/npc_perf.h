#pragma once

#include <chrono>
#include "../xrCore/load_prof.h"

// ---------------------------------------------------------------------------
// ⭐РАЗБОР ОДНОГО АПДЕЙТА NPC/АКТЁРА ПО СТАТЬЯМ (консольный `npc_stats 1`).
//
// Зачем. 19-09 фаза планировщика разобрана до объектов: 1.21 core-ms/кадр, из
// них 64% ВНУТРИ Lua-VM. Оставшиеся 36% (~0.4 мс/кадр) -- движковый C++, и это
// ЕДИНСТВЕННОЕ место фазы, которое можно удешевить, не трогая ни частоту
// апдейтов, ни логику ИИ. Но пока не известно, из чего эти 0.4 состоят,
// оптимизировать там нечего -- будет гадание.
//
// Меряется своё время статьи и доля Lua внутри неё (через
// lua_vm_watch::lua_time_ns), как в `sched_stats`: разность даёт C++-часть.
//
// ⚠Цена прибора: два обращения к часам на статью. При ~1.3 апдейтах NPC в кадре
// и десятке статей это сотые доли миллисекунды, но по умолчанию ВЫКЛЮЧЕН.
// ⚠Только главный поток: Exec_Visibility уходит на воркер и здесь НЕ мерится
// (в апдейте видна только цена постановки задачи).
// ⚠Отчёт по РЕАЛЬНЫМ часам (dwTimeContinual): игровые встают на паузе.
// ---------------------------------------------------------------------------
namespace NpcPerf
{
extern int enabled;

enum Slot
{
    // сталкер, CAI_Stalker::shedule_Update
    S_OBJHANDLER = 0,
    S_ANIMCB,
    S_AGENT,
    S_VISION,
    S_ENEMIES,
    S_MEMORY,
    // внутри S_MEMORY (CMemoryManager::update) -- самая крупная C++-статья фазы
    M_VISUAL,
    M_SOUND,
    M_HIT,
    M_SELFSCAN,
    // внутри M_SELFSCAN: тело цикла по запомненным объектам
    M_SCANITER,   // счётчик итераций (время не мерится, важна колонка calls/f)
    M_DANGERADD,
    M_CASTS,
    M_ITEMADD,
    // внутри M_ITEMADD: из чего складываются его 22 мкс
    U_BASE,
    U_AILOC,
    U_ACCESS_POS,
    U_ACCESS_VTX,
    U_INVITEM,
    U_GRAPH,
    M_ENEMY,
    M_ITEM,
    M_DANGER,
    S_ANOMALY,
    S_INHERITED,
    S_THINK,
    S_FEELTOUCH,
    S_NETUPD,
    S_INVOWNER,
    S_PHYSICS,
    // актёр, CActor::shedule_Update
    A_HUD,
    A_INVENTORY,
    A_CAM,
    A_CONTROLS,
    A_INHERITED,
    // общее для ВСЕХ объектов, CGameObject::shedule_Update
    G_SKELETON,
    G_FEELADDONS,
    G_BINDER,
    S_COUNT
};

void add(int slot, double ms, double lua_ms);
// any/script_binder by the object's C++ class (11-10: 25 binder updates a frame in a fight, ~1.3 ms -- whose?)
void add_binder(const char* cls, double ms);
const char* slot_name(int slot);

struct Scope
{
    int s;
    bool on;
    std::chrono::steady_clock::time_point t0;
    unsigned long long lua0;
    // The same zone as one part of a LoadProf breakdown ([sched prof]: one slow
    // stalker update split up) -- free unless an Arm is alive on this thread.
    LoadProf::Scope lp;

    explicit Scope(int slot);
    // Закрыть зону РАНЬШЕ конца области видимости (там, где статья
    // кончается посередине функции и вводить вложенный блок нельзя --
    // после него идут локальные, нужные дальше). Повторный вызов безвреден.
    void stop();
    ~Scope();
};
}

// ⚠__LINE__ через два уровня -- иначе в имени окажется "__LINE__", а не число.
#define NPC_ZONE_CAT2(a, b) a##b
#define NPC_ZONE_CAT(a, b) NPC_ZONE_CAT2(a, b)
#define NPC_ZONE(slot) NpcPerf::Scope NPC_ZONE_CAT(_npcz_, __LINE__)(NpcPerf::slot)
