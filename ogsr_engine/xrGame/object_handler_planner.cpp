////////////////////////////////////////////////////////////////////////////
//	Module 		: object_handler_planner.cpp
//	Created 	: 11.03.2004
//  Modified 	: 01.12.2004
//	Author		: Dmitriy Iassenev
//	Description : Object handler action planner
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "object_handler_planner.h"
#include "object_property_evaluators.h"
#include "object_actions.h"
#include "ai_monster_space.h"
#include "object_handler_space.h"
#include "ai/stalker/ai_stalker.h"
#include "Inventory.h"
#include "object_handler_planner_impl.h"
#include "WeaponMagazined.h"
#include "Missile.h"
#include <atomic>
#include "ai_monster_space.h"

using namespace ObjectHandlerSpace;

IC ObjectHandlerSpace::EWorldProperties CObjectHandlerPlanner::object_property(MonsterSpace::EObjectAction object_action) const
{
    switch (object_action)
    {
    case MonsterSpace::eObjectActionSwitch1: return (ObjectHandlerSpace::eWorldPropertySwitch1);
    case MonsterSpace::eObjectActionSwitch2: return (ObjectHandlerSpace::eWorldPropertySwitch2);
    case MonsterSpace::eObjectActionAim1: return (ObjectHandlerSpace::eWorldPropertyAimingReady1);
    case MonsterSpace::eObjectActionAim2: return (ObjectHandlerSpace::eWorldPropertyAiming2);
    case MonsterSpace::eObjectActionFire1: return (ObjectHandlerSpace::eWorldPropertyFiring1);
    case MonsterSpace::eObjectActionFire2: return (ObjectHandlerSpace::eWorldPropertyFiring2);
    case MonsterSpace::eObjectActionIdle: return (ObjectHandlerSpace::eWorldPropertyIdle);
    case MonsterSpace::eObjectActionStrapped: return (ObjectHandlerSpace::eWorldPropertyIdleStrap);
    case MonsterSpace::eObjectActionDrop: return (ObjectHandlerSpace::eWorldPropertyDropped);
    case MonsterSpace::eObjectActionActivate: return (ObjectHandlerSpace::eWorldPropertyIdle);
    case MonsterSpace::eObjectActionDeactivate: return (ObjectHandlerSpace::eWorldPropertyNoItemsIdle);
    case MonsterSpace::eObjectActionAimReady1: return (ObjectHandlerSpace::eWorldPropertyAimingReady1);
    case MonsterSpace::eObjectActionAimReady2: return (ObjectHandlerSpace::eWorldPropertyAimingReady2);
    case MonsterSpace::eObjectActionAimForceFull1: return (ObjectHandlerSpace::eWorldPropertyAimForceFull1);
    case MonsterSpace::eObjectActionAimForceFull2: return (ObjectHandlerSpace::eWorldPropertyAimForceFull2);
    case MonsterSpace::eObjectActionUse: return (ObjectHandlerSpace::eWorldPropertyUsed);
    default: NODEFAULT;
    }
}

void CObjectHandlerPlanner::set_goal(MonsterSpace::EObjectAction object_action, CGameObject* game_object, u32 min_queue_size, u32 max_queue_size, u32 min_queue_interval,
                                     u32 max_queue_interval)
{
    EWorldProperties goal = object_property(object_action);
    u32 condition_id = goal;

    if (game_object && (eWorldPropertyNoItemsIdle != goal))
    {
        CWeapon* weapon = smart_cast<CWeapon*>(game_object);
        if (weapon && (goal == eWorldPropertyIdleStrap) && !weapon->can_be_strapped())
            goal = eWorldPropertyIdle;
        condition_id = uid(game_object->ID(), goal);
    }
    else
        condition_id = u32(eWorldPropertyNoItemsIdle);

    CState condition;
    condition.add_condition(CWorldProperty(condition_id, true));
    set_target_state(condition);

    if (!game_object || (min_queue_size < 0))
        return;

    CWeaponMagazined* weapon = smart_cast<CWeaponMagazined*>(game_object);
    if (!weapon)
        return;

    if ((m_min_queue_size != min_queue_size) || (m_max_queue_size != max_queue_size) || (m_min_queue_interval != min_queue_interval) ||
        (m_max_queue_interval != max_queue_interval) || (m_next_time_change <= Device.dwTimeGlobal))
    {
        m_min_queue_size = min_queue_size;
        m_max_queue_size = max_queue_size;
        m_min_queue_interval = min_queue_interval;
        m_max_queue_interval = max_queue_interval;

        if (m_max_queue_size == m_min_queue_size)
            m_queue_size = _max(1, m_min_queue_size);
        else
            m_queue_size = _max(1, ::Random.randI(m_min_queue_size, m_max_queue_size));

        if (m_max_queue_interval == m_min_queue_interval)
            m_queue_interval = m_min_queue_interval;
        else
            m_queue_interval = ::Random.randI(m_min_queue_interval, m_max_queue_interval);

        m_next_time_change = Device.dwTimeGlobal + m_queue_interval;

        weapon->SetQueueSize(m_queue_size);
        this->action(uid(weapon->ID(), eWorldOperatorQueueWait1)).set_inertia_time(m_queue_interval ? m_queue_interval : 300);
        this->action(uid(weapon->ID(), eWorldOperatorQueueWait2)).set_inertia_time(m_queue_interval ? m_queue_interval : 300);
    }
}

void CObjectHandlerPlanner::remove_evaluators(CObject* object)
{
#pragma todo("Dima to Dima : safe, but not optimal!")
    for (;;)
    {
        EVALUATORS::iterator I = m_evaluators.lower_bound(uid(object->ID(), 0));
        if (I == m_evaluators.end() || !object_action((*I).first, object))
            break;
        remove_evaluator((*I).first);
    }
}

void CObjectHandlerPlanner::remove_operators(CObject* object)
{
#pragma todo("Dima to Dima : safe, but not optimal!")
    for (;;)
    {
        OPERATOR_VECTOR::iterator I = std::lower_bound(m_operators.begin(), m_operators.end(), uid(object->ID(), 0));
        if (I == m_operators.end() || !object_action((*I).m_operator_id, object))
            break;
        remove_operator((*I).m_operator_id);
    }
}

void CObjectHandlerPlanner::init_storage()
{
    m_storage.set_property(eWorldPropertyAimed1, false);
    m_storage.set_property(eWorldPropertyAimed2, false);
    m_storage.set_property(eWorldPropertyUseEnough, false);
    m_storage.set_property(eWorldPropertyStrapped, false);
    m_storage.set_property(eWorldPropertyStrapped2Idle, false);
}

void CObjectHandlerPlanner::setup(CAI_Stalker* object)
{
    inherited::setup(object);
    CActionBase<CAI_Stalker>* action;

    m_min_queue_size = 0;
    m_max_queue_size = 0;
    m_min_queue_interval = 0;
    m_max_queue_interval = 0;
    m_next_time_change = 0;

    clear();

    init_storage();

    add_evaluator(u32(eWorldPropertyNoItems), xr_new<CObjectPropertyEvaluatorNoItems>(m_object));
    add_evaluator(u32(eWorldPropertyNoItemsIdle), xr_new<CObjectPropertyEvaluatorConst>(false));
    action = xr_new<CSObjectActionBase>(m_object, m_object, &m_storage, "no items idle");
    add_condition(action, 0xffff, eWorldPropertyItemID, true);
    add_effect(action, 0xffff, eWorldPropertyIdle, true);
    add_operator(u32(eWorldOperatorNoItemsIdle), action);

    set_goal(MonsterSpace::eObjectActionIdle, 0, 0, 0, 0, 0);
}

void CObjectHandlerPlanner::add_item(CInventoryItem* inventory_item)
{
    CWeapon* weapon = smart_cast<CWeapon*>(inventory_item);
    if (weapon)
    {
        add_evaluators(weapon);
        add_operators(weapon);
        return;
    }

    CMissile* missile = smart_cast<CMissile*>(inventory_item);
    if (missile)
    {
        add_evaluators(missile);
        add_operators(missile);
        return;
    }
}

void CObjectHandlerPlanner::remove_item(CInventoryItem* inventory_item)
{
    VERIFY(target_state().conditions().size() == 1);
    if (action_object_id(target_state().conditions().back().condition()) == inventory_item->object().ID())
    {
        init_storage();
        set_goal(MonsterSpace::eObjectActionIdle, 0, 0, 0, 0, 0);
    }

    if (!Level().is_removing_objects())
        for (;;)
        {
            const auto it = std::find_if(m_current_state.conditions().begin(), m_current_state.conditions().end(),
                                         [&](const auto& it) { return action_object_id(it.condition()) == inventory_item->object().ID(); });
            if (it == m_current_state.conditions().end())
                break;
            m_current_state.remove_condition(it->condition());
        }

    remove_evaluators(&inventory_item->object());
    remove_operators(&inventory_item->object());
}

// ai_oh_search_cap: vertex cap of the weapon planner's search. v165-v168 on the
// Switch: a soldier at the Cordon bridge now and then asks for a goal there is no
// plan for, and the search ran the stock 8000 vertices -- ~100 ms on the second
// thread. A real weapon plan is a handful of steps; [oh search] below reports
// how many vertices the successful ones actually need, to keep the cap honest.
#ifdef __SWITCH__
int g_oh_search_cap = 1500;
#else
int g_oh_search_cap = 8000;
#endif

LPCSTR oh_prop_name(u32 p)
{
    static const char* names[] = {"ItemID", "Hidden", "Strapped", "Strapped2Idle", "Switch1", "Switch2", "Aimed1", "Aimed2", "Aiming1", "Aiming2", "Empty1",
                                  "Empty2", "Full1", "Full2", "Ready1", "Ready2", "Firing1", "Firing2", "Ammo1", "Ammo2", "Idle", "IdleStrap", "Dropped",
                                  "QueueWait1", "QueueWait2", "AimingReady1", "AimingReady2", "AimForceFull1", "AimForceFull2", "ThrowStarted", "ThrowIdle",
                                  "Throw", "Threaten", "Prepared", "Used", "UseEnough"};
    return p < std::size(names) ? names[p] : "?";
}

namespace
{
struct OhSearchStats
{
    std::atomic<u32> searches{0}, ok{0}, ok_max_visited{0}, ok_over_500{0}, ok_near_cap{0}, failed{0}, fail_max_us{0};
    std::atomic<u64> fail_us{0};
    std::atomic<u32> last_print{0};
};
OhSearchStats g_oh_stats;

void atomic_max(std::atomic<u32>& a, u32 v)
{
    u32 cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed))
        ;
}
} // namespace

// v169: goal wpn_ak74 Firing1=1 found "no plan" while the item read ready, loaded,
// unhidden, unstrapped -- aim1 -> aim_queue1 -> fire1 is three steps on paper. So
// the question is what the SEARCH saw. m_current_state holds exactly the values it
// evaluated (lazily, cleared per solve); print the goal item's part of it, mark
// the ones that read differently now (*), then search once more and say whether
// a plan turns up. First 40 failures of a session.
void CObjectHandlerPlanner::explain_failed_search(u32 first_us)
{
    static std::atomic<u32> s_prints{0};
    if (s_prints.fetch_add(1, std::memory_order_relaxed) >= 40)
        return;

    string2048 buf;
    size_t len = 0;
    buf[0] = 0;
    auto put = [&](const char* fmt, auto... a) {
        if (len + 1 < sizeof(buf))
        {
            const int w = snprintf(buf + len, sizeof(buf) - len, fmt, a...);
            if (w > 0)
                len = std::min(len + size_t(w), sizeof(buf) - 1);
        }
    };

    u32 goal_item = 0xffff;
    for (const auto& c : target_state().conditions())
    {
        goal_item = c.condition() >> 16;
        put("goal %u %s=%d | search saw:", goal_item, oh_prop_name(c.condition() & 0xffff), int(c.value()));
    }
    u32 seen = 0, flipped = 0, other = 0;
    for (const auto& c : m_current_state.conditions())
    {
        ++seen;
        const auto I = evaluators().find(c.condition());
        const bool now = I != evaluators().end() ? (*I).second->evaluate() : c.value();
        const bool flip = now != c.value();
        flipped += flip ? 1 : 0;
        if ((c.condition() >> 16) != goal_item)
        {
            ++other;
            if (flip)
                put(" [item %u %s=%d*]", c.condition() >> 16, oh_prop_name(c.condition() & 0xffff), int(c.value()));
            continue;
        }
        put(" %s=%d%s", oh_prop_name(c.condition() & 0xffff), int(c.value()), flip ? "*" : "");
    }
    put(" | %u props (%u of other items), %u read differently now (*)", seen, other, flipped);

    // second attempt: same goal, the world re-read from scratch
    const u64 t0 = CPU::QPC();
    m_actuality = false;
    solve();
    const double ms = 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq());
    Msg("~ [oh retry] '%s' %.1f ms no plan (cap %u): %s | retry: %s, visited %u, %.1f ms, plan %u steps", m_object ? m_object->cName().c_str() : "?",
        first_us / 1000.0, m_search_cap, buf, m_failed ? "NO PLAN AGAIN" : "FOUND", m_last_visited, ms, u32(solution().size()));
}

void CObjectHandlerPlanner::update()
{
    m_search_cap = u32(g_oh_search_cap);
    const u64 t0 = CPU::QPC();
    inherited::update();
    if (!m_last_searched)
        return;

    OhSearchStats& s = g_oh_stats;
    s.searches.fetch_add(1, std::memory_order_relaxed);
    if (m_failed)
    {
        const u32 us = u32((CPU::QPC() - t0) * 1000000ull / CPU::QPCFreq());
        s.failed.fetch_add(1, std::memory_order_relaxed);
        s.fail_us.fetch_add(us, std::memory_order_relaxed);
        atomic_max(s.fail_max_us, us);
        explain_failed_search(us);
    }
    else
    {
        s.ok.fetch_add(1, std::memory_order_relaxed);
        atomic_max(s.ok_max_visited, m_last_visited);
        if (m_last_visited > 500)
            s.ok_over_500.fetch_add(1, std::memory_order_relaxed);
        if (m_last_visited * 5 > m_search_cap * 4) // a real plan within 20% of the cap: raise it
            s.ok_near_cap.fetch_add(1, std::memory_order_relaxed);
    }

    const u32 now = Device.dwTimeGlobal;
    u32 last = s.last_print.load(std::memory_order_relaxed);
    if (now - last >= 60000 && s.last_print.compare_exchange_strong(last, now))
    {
        const u32 n = s.searches.exchange(0), ok = s.ok.exchange(0), okMax = s.ok_max_visited.exchange(0), ok500 = s.ok_over_500.exchange(0),
                  nearCap = s.ok_near_cap.exchange(0), fail = s.failed.exchange(0), failMax = s.fail_max_us.exchange(0);
        const u64 failUs = s.fail_us.exchange(0);
        Msg("~ [oh search] 60 s: %u searches | found %u, max visited %u, >500: %u, near cap: %u | no plan %u, worst %.1f ms, total %.1f ms | cap %u", n, ok, okMax,
            ok500, nearCap, fail, failMax / 1000.0, failUs / 1000.0, m_search_cap);
    }
}

//Вынесено из object_handler_planner_impl.h
/*IC*/ CObjectHandlerPlanner::_condition_type CObjectHandlerPlanner::uid(const u32 id0, const u32 id1) const
{
    VERIFY(!((id0 << 16) & id1));
    return ((id0 << 16) | id1);
}
