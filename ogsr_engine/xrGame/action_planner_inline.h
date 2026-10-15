////////////////////////////////////////////////////////////////////////////
//	Module 		: action_planner_inline.h
//	Created 	: 28.01.2004
//  Modified 	: 10.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Action planner inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "xr_platform_seh.h"
#include "../xrCore/load_prof.h"   // [oh prof]: planner phases of a slow update_object_handler
#include <typeinfo>
#include "goap_stats.h"

// The body of CPlanner::update sits in __try, where MSVC allows no object with a
// destructor (C2712): the timed scope lives in this helper's frame instead.
template <typename F>
IC void planner_timed(const char* name, F&& f)
{
    LoadProf::Scope _lp(name);
    f();
}

// goap_stats 2: the same, plus the per-action table (goap_stats.h). The id tells actions apart;
// a script action's Lua class is set by its wrapper.
template <typename F>
IC void planner_prof(const char* name, GoapStats::ProfKind kind, u32 id, F&& f)
{
    LoadProf::Scope _lp(name);
    GoapStats::ProfScope _ps(kind, id, GoapStats::enabled >= 2 ? name : nullptr);
    f();
}

#define TEMPLATE_SPECIALIZATION \
    template <typename _object_type, bool _reverse_search, typename _world_operator, typename _condition_evaluator, typename _world_operator_ptr, typename _condition_evaluator_ptr>

#define CPlanner CActionPlanner<_object_type, _reverse_search, _world_operator, _condition_evaluator, _world_operator_ptr, _condition_evaluator_ptr>

// ⭐⭐WHO faulted, and which of the four planners it was.
//
// Live 05-08-2026 the host faulted here 3021 times in one session and the log
// could not say a single thing about any of them: the handler below prints
// nothing at all unless LOG_ACTION is defined (it is not, and turning it on
// costs a solution dump per creature per frame), so the whole record of the
// storm was one repeated line naming the template. That is not enough to act
// on -- four different things instantiate this planner (a stalker's brain, the
// same stalker's object handler, a group's agent manager, and script planners
// the mod creates), they fail for different reasons, and "the AI crashed" is
// where the last two sessions started and stopped.
//
// ⚠Deliberately not `m_object->cName()` written straight into the handler: the
// planner is a template and its owners need not have that method (one was the
// legacy PDA map, CUIMapWnd, until 13-10-2026). The detector keeps the diagnostic type-agnostic instead
// of the alternative, which is specialising the handler per owner and getting
// it wrong for the fifth instantiation somebody adds.
namespace planner_diag
{
template <typename T, typename = void>
struct has_cname : std::false_type
{
};
template <typename T>
struct has_cname<T, decltype(void(std::declval<const T&>().cName()))> : std::true_type
{
};

template <typename T, typename = void>
struct has_id : std::false_type
{
};
template <typename T>
struct has_id<T, decltype(void(std::declval<const T&>().ID()))> : std::true_type
{
};

template <typename T>
IC const char* owner_name(const T* object)
{
    if constexpr (has_cname<T>::value)
        return object ? object->cName().c_str() : "<null>";
    else
        return "<unnamed>";
}

template <typename T>
IC u32 owner_id(const T* object)
{
    if constexpr (has_id<T>::value)
        return object ? u32(object->ID()) : u32(-1);
    else
        return u32(-1);
}
} // namespace planner_diag

TEMPLATE_SPECIALIZATION
IC CPlanner::CActionPlanner()
{
    m_initialized = false;
}

TEMPLATE_SPECIALIZATION
IC CPlanner::~CActionPlanner() { m_object = 0; }

TEMPLATE_SPECIALIZATION
void CPlanner::setup(_object_type* object)
{
    inherited::setup();
    m_object = object;
    m_current_action_id = _action_id_type(-1);
    m_storage.clear();
    m_initialized = false;
    m_loaded = false;
}

TEMPLATE_SPECIALIZATION
IC _object_type& CPlanner::object() const
{
    VERIFY(m_object);
    return (*m_object);
}

TEMPLATE_SPECIALIZATION
void CPlanner::update()
{
    XR_SEH_TRY
    {
        planner_timed("plan.solve", [&] { solve(); });

        THROW(!solution().empty());

        if (solution().empty())
        {
            if (initialized())
            {
                Msg("! [CPlanner::update]: %s has solution().empty()", m_object->cName().c_str());
                if (current_action_id() != _action_id_type(-1))
                {
                    current_action().finalize();
                    m_current_action_id = _action_id_type(-1);
                }
                m_initialized = false;
            }
        }
        else
        {
            if (initialized())
            {
                if (current_action_id() != solution().front())
                {
                    planner_prof("act.fin", GoapStats::PK_INIT, u32(current_action_id()), [&] { current_action().finalize(); });
                    m_current_action_id = solution().front();
                    planner_prof("act.init", GoapStats::PK_INIT, u32(current_action_id()), [&] { current_action().initialize(); });
                }
            }
            else
            {
                m_initialized = true;
                m_current_action_id = solution().front();
                planner_prof("act.init", GoapStats::PK_INIT, u32(current_action_id()), [&] { current_action().initialize(); });
            }
            // [frame prof]: the action's own class, not "act.exec" -- three nested planners
            // share that name and the slow one (15-26 ms, v216/v217) could not be told apart.
            planner_prof(typeid(current_action()).name(), GoapStats::PK_EXEC, u32(current_action_id()), [&] { current_action().execute(); });
        }
    }
    XR_SEH_EXCEPT(ExceptStackTrace("[CPlanner::update] stack_trace:\n"))
    {
        // Static inside a template member, so the count is PER PLANNER TYPE --
        // which is itself the first thing worth knowing: one storm on the
        // stalker brain and none anywhere else is a different bug from every
        // planner in the game faulting at once.
        //
        // Rate-limited the same way the stack dumps are: this fires at frame
        // rate when it fires at all, and a diagnostic that turns the log into
        // the outage has been the actual failure mode here before.
        static u32 s_faults = 0;
        ++s_faults;
        if (s_faults <= 8 || 0 == (s_faults % 1024))
            Msg("!![CPlanner::update] fault #%u in %s -- owner %p id=%u '%s', initialized=%d, action=%d", s_faults, __FUNCTION__, (void*)m_object, planner_diag::owner_id(m_object),
                planner_diag::owner_name(m_object), m_initialized ? 1 : 0, int(m_current_action_id));
    }
}

TEMPLATE_SPECIALIZATION
IC typename CPlanner::COperator& CPlanner::action(const _action_id_type& action_id) { return (*get_operator(action_id)); }

TEMPLATE_SPECIALIZATION
IC typename CPlanner::CConditionEvaluator& CPlanner::evaluator(const _condition_type& evaluator_id) { return (*inherited::evaluator(evaluator_id)); }

TEMPLATE_SPECIALIZATION
IC typename CPlanner::_action_id_type CPlanner::current_action_id() const
{
    VERIFY(initialized());
    return (m_current_action_id);
}

TEMPLATE_SPECIALIZATION
IC typename CPlanner::COperator& CPlanner::current_action() { return (action(current_action_id())); }

TEMPLATE_SPECIALIZATION
IC bool CPlanner::initialized() const { return (m_initialized); }

TEMPLATE_SPECIALIZATION
IC void CPlanner::add_condition(_world_operator* action, _condition_type condition_id, _value_type condition_value)
{
    action->add_condition(CWorldProperty(condition_id, condition_value));
}

TEMPLATE_SPECIALIZATION
IC void CPlanner::add_effect(_world_operator* action, _condition_type condition_id, _value_type condition_value)
{
    action->add_effect(CWorldProperty(condition_id, condition_value));
}

TEMPLATE_SPECIALIZATION
IC void CPlanner::add_operator(const _edge_type& operator_id, _operator_ptr _operator)
{
    inherited::add_operator(operator_id, _operator);
    _operator->setup(m_object, &m_storage);
}

TEMPLATE_SPECIALIZATION
IC void CPlanner::add_evaluator(const _condition_type& condition_id, _condition_evaluator_ptr evaluator)
{
    inherited::add_evaluator(condition_id, evaluator);
    evaluator->setup(m_object, &m_storage);
}

TEMPLATE_SPECIALIZATION
IC void CPlanner::save(NET_Packet& packet)
{
    {
        auto I = m_evaluators.begin();
        auto E = m_evaluators.end();
        for (; I != E; ++I)
            (*I).second->save(packet);
    }

    {
        auto I = m_operators.begin();
        auto E = m_operators.end();
        for (; I != E; ++I)
            (*I).m_operator->save(packet);
    }

    {
        packet.w_u32(m_storage.m_storage.size());
        typedef CPropertyStorage::CConditionStorage CConditionStorage;
        CConditionStorage::const_iterator I = m_storage.m_storage.begin();
        CConditionStorage::const_iterator E = m_storage.m_storage.end();
        for (; I != E; ++I)
        {
            packet.w(&(*I).m_condition, sizeof((*I).m_condition));
            packet.w(&(*I).m_value, sizeof((*I).m_value));
        }
    }
}

TEMPLATE_SPECIALIZATION
IC void CPlanner::load(IReader& packet)
{
    {
        auto I = m_evaluators.begin();
        auto E = m_evaluators.end();
        for (; I != E; ++I)
            (*I).second->load(packet);
    }

    {
        auto I = m_operators.begin();
        auto E = m_operators.end();
        for (; I != E; ++I)
            (*I).m_operator->load(packet);
    }

    {
        u32 count = packet.r_u32();
        GraphEngineSpace::_solver_condition_type condition;
        GraphEngineSpace::_solver_value_type value;
        for (u32 i = 0; i < count; ++i)
        {
            packet.r(&condition, sizeof(condition));
            packet.r(&value, sizeof(value));
            m_storage.set_property(condition, value);
        }
    }
    m_loaded = true;
}

#undef TEMPLATE_SPECIALIZATION
#undef CPlanner
