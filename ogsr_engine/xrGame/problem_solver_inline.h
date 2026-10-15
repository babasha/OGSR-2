#include "goap_stats.h"
#include "../xrCore/load_prof.h"   // [oh prof]: plan.actual / plan.search / plan.eval
////////////////////////////////////////////////////////////////////////////
//	Module 		: problem_solver_inline.h
//	Created 	: 24.02.2004
//  Modified 	: 24.02.2004
//	Author		: Dmitriy Iassenev
//	Description : Problem solver inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "graph_engine_space.h"

#define TEMPLATE_SPECIALIZATION \
    template <typename _operator_condition, typename _operator, typename _condition_state, typename _condition_evaluator, typename _operator_id_type, bool _reverse_search, \
              typename _operator_ptr, typename _condition_evaluator_ptr>

#define CProblemSolverAbstract \
    CProblemSolver<_operator_condition, _operator, _condition_state, _condition_evaluator, _operator_id_type, _reverse_search, _operator_ptr, _condition_evaluator_ptr>

TEMPLATE_SPECIALIZATION
IC CProblemSolverAbstract::CProblemSolver() { init(); }

TEMPLATE_SPECIALIZATION
CProblemSolverAbstract::~CProblemSolver() { clear(); }

TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::clear()
{
    for (auto& it : m_operators)
        delete_data(it.m_operator);
    m_operators.clear();

    for (auto& pair : m_evaluators)
        delete_data(pair.second);
    m_evaluators.clear();
}

TEMPLATE_SPECIALIZATION
void CProblemSolverAbstract::init() {}

TEMPLATE_SPECIALIZATION
void CProblemSolverAbstract::setup()
{
    m_target_state.clear();
    m_current_state.clear();
    m_solve_cache.clear();
    m_temp.clear();
    m_solution.clear();
    m_applied = false;
    m_solution_changed = false;
    m_actuality = true;
    m_failed = false;
}

TEMPLATE_SPECIALIZATION
IC bool CProblemSolverAbstract::actual() const
{
    if (!m_actuality)
        return (false);

    auto I = current_state().conditions().cbegin();
    auto E = current_state().conditions().cend();
    auto i = evaluators().begin();
    auto e = evaluators().end();
    const bool track = GoapStats::enabled || GoapStats::use_cache;
    for (; I != E; ++I)
    {
        if ((*i).first < (*I).condition())
            i = std::lower_bound(i, e, (*I).condition(), evaluators().value_comp());
        VERIFY(i != e);
        VERIFY((*i).first == (*I).condition());
        _value_type val;
        {
            GoapStats::ProfScope _ps(GoapStats::PK_EVAL, u32((*I).condition()), GoapStats::enabled >= 2 ? typeid(*(*i).second).name() : nullptr);
            val = (*i).second->evaluate();
        }
        if (track)
        {
            ++GoapStats::g_eval_actual;
            m_solve_cache.add_condition(COperatorCondition((*I).condition(), val));
        }
        if (val != (*I).value())
            return (false);
    }
    return (true);
}

TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::add_operator(const _edge_type& operator_id, _operator_ptr op)
{
    auto I = std::lower_bound(m_operators.begin(), m_operators.end(), operator_id);
    THROW((I == m_operators.end()) || ((*I).m_operator_id != operator_id));
    m_actuality = false;
    m_operators.emplace(I, operator_id, op);
}

TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::remove_operator(const _edge_type& operator_id)
{
    auto I = std::lower_bound(m_operators.begin(), m_operators.end(), operator_id);
    if (m_operators.end() != I)
    {
        delete_data((*I).m_operator);
        m_operators.erase(I);
    }
    m_actuality = false;
}

TEMPLATE_SPECIALIZATION
IC const typename CProblemSolverAbstract::OPERATOR_VECTOR& CProblemSolverAbstract::operators() const { return (m_operators); }

// states
TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::set_target_state(const CState& state)
{
    m_actuality = m_actuality && (m_target_state == state);
    m_target_state = state;
}

TEMPLATE_SPECIALIZATION
IC const typename CProblemSolverAbstract::CState& CProblemSolverAbstract::current_state() const { return (m_current_state); }

TEMPLATE_SPECIALIZATION
IC const typename CProblemSolverAbstract::CState& CProblemSolverAbstract::target_state() const { return (m_target_state); }

TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::add_evaluator(const _condition_type& condition_id, _condition_evaluator_ptr evaluator)
{
    THROW(evaluators().end() == evaluators().find(condition_id));
    m_evaluators.insert(std::make_pair(condition_id, evaluator));
}

TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::remove_evaluator(const _condition_type& condition_id)
{
    auto I = m_evaluators.find(condition_id);
    if (I != m_evaluators.end())
    {
        delete_data((*I).second);
        m_evaluators.erase(I);
    }
    m_actuality = false;
}

TEMPLATE_SPECIALIZATION
IC typename CProblemSolverAbstract::_condition_evaluator_ptr CProblemSolverAbstract::evaluator(const _condition_type& condition_id) const
{
    auto I = evaluators().find(condition_id);
    THROW(evaluators().end() != I);
    if (I == evaluators().end())
    {
        // ⚠The high/low split only means something for the object handler,
        // whose condition ids are (item id << 16) | property. Every other
        // planner uses a bare property number, and printing it as an object id
        // sent one investigation looking for entity 0. So say both, and
        // fingerprint the planner by its size -- that is what actually
        // identifies which one is missing the evaluator (the stalker brain has
        // 7, its combat planner 22, its danger planner 5).
        u16 object_id = condition_id >> 16;
        u16 action_id = condition_id & 0xffff;
        auto obj = object_id ? Level().Objects.net_Find(object_id) : nullptr;
        Msg("! [CProblemSolverAbstract::evaluator]: property %u missing (as item uid: item %u '%s' prop %u) -- planner has %u evaluators / %u operators", condition_id,
            object_id, (obj ? obj->cName().c_str() : "-"), action_id, u32(m_evaluators.size()), u32(m_operators.size()));
        return nullptr;
    }
    return (*I).second;
}

TEMPLATE_SPECIALIZATION
IC const typename CProblemSolverAbstract::EVALUATORS& CProblemSolverAbstract::evaluators() const { return (m_evaluators); }

TEMPLATE_SPECIALIZATION
IC bool CProblemSolverAbstract::evaluate_condition(typename xr_vector<COperatorCondition>::const_iterator& I, typename xr_vector<COperatorCondition>::const_iterator& E,
                                                   const _condition_type& condition_id) const
{
    auto eva = evaluator(condition_id);
    if (eva)
    {
        _value_type val;
        const bool track = GoapStats::enabled || GoapStats::use_cache;
        // ⚠CState::property() делает lower_bound и НЕ сверяет id найденного
        // элемента: при отсутствии точного совпадения она вернёт СЛЕДУЮЩИЙ
        // (или указатель за конец). Сверяем границу и id сами.
        const COperatorCondition* cached = nullptr;
        if (track && !m_solve_cache.conditions().empty())
        {
            const COperatorCondition* c = m_solve_cache.property(condition_id);
            const COperatorCondition* endp = m_solve_cache.conditions().data() + m_solve_cache.conditions().size();
            if (c && c < endp && c->condition() == condition_id)
                cached = c;
        }
        if (cached)
        {
            ++GoapStats::g_hits;
            // При use_cache 0 попадания СЧИТАЮТСЯ, но значение берётся по-старому --
            // чтобы A/B делался в ОДНОМ прогоне одной сборкой.
            val = GoapStats::use_cache ? cached->value() : eva->evaluate();
        }
        else
        {
            {
                LoadProf::Scope _lp("plan.eval");
                GoapStats::ProfScope _ps(GoapStats::PK_EVAL, u32(condition_id), GoapStats::enabled >= 2 ? typeid(*eva).name() : nullptr);
                val = eva->evaluate();
            }
            if (track)
            {
                ++GoapStats::g_eval_search;
                m_solve_cache.add_condition(COperatorCondition(condition_id, val));
            }
        }
        size_t index = I - m_current_state.conditions().begin();
        m_current_state.add_condition(I, COperatorCondition(condition_id, val));
        I = m_current_state.conditions().begin() + index;
        E = m_current_state.conditions().end();
        return true;
    }
    return false;
}

TEMPLATE_SPECIALIZATION
IC typename CProblemSolverAbstract::_edge_value_type CProblemSolverAbstract::get_edge_weight(const _index_type& vertex_index0, const _index_type& vertex_index1,
                                                                                             const const_iterator& i) const
{
    _edge_value_type current, min;
    current = (*i).m_operator->weight(vertex_index1, vertex_index0);
    min = (*i).m_operator->min_weight();
    THROW(current >= min);
    return (current);
}

TEMPLATE_SPECIALIZATION
IC bool CProblemSolverAbstract::is_accessible(const _index_type& vertex_index) const { return (m_applied); }

TEMPLATE_SPECIALIZATION
IC const typename CProblemSolverAbstract::_index_type& CProblemSolverAbstract::value(const _index_type& vertex_index, const_iterator& i, bool reverse_search) const
{
    if (reverse_search)
    {
        if ((*i).m_operator->applicable_reverse((*i).m_operator->effects(), (*i).m_operator->conditions(), vertex_index))
            m_applied = (*i).m_operator->apply_reverse(vertex_index, (*i).m_operator->effects(), m_temp, (*i).m_operator->conditions());
        else
            m_applied = false;
    }
    else
    {
        if ((*i).m_operator->applicable(vertex_index, current_state(), (*i).m_operator->conditions(), *this))
        {
            (*i).m_operator->apply(vertex_index, (*i).m_operator->effects(), m_temp, m_current_state, *this);
            m_applied = true;
        }
        else
            m_applied = false;
    }
    return (m_temp);
}

TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::begin(const _index_type& vertex_index, const_iterator& b, const_iterator& e) const
{
    b = m_operators.begin();
    e = m_operators.end();
}

TEMPLATE_SPECIALIZATION
IC bool CProblemSolverAbstract::is_goal_reached(const _index_type& vertex_index) const { return (is_goal_reached_impl<reverse_search>(vertex_index)); }

TEMPLATE_SPECIALIZATION
IC bool CProblemSolverAbstract::is_goal_reached_impl(const _index_type& vertex_index) const
{
    STATIC_CHECK(!reverse_search, This_function_cannot_be_used_in_the_REVERSE_search);
    auto I = vertex_index.conditions().cbegin();
    auto E = vertex_index.conditions().cend();
    auto i = target_state().conditions().cbegin();
    auto e = target_state().conditions().cend();
    auto II = current_state().conditions().cbegin();
    auto EE = current_state().conditions().cend();
    for (; (i != e) && (I != E);)
    {
        if ((*I).condition() < (*i).condition())
        {
            ++I;
        }
        else if ((*I).condition() > (*i).condition())
        {
            for (; (II != EE) && ((*II).condition() < (*i).condition());)
                ++II;
            if ((II == EE) || ((*II).condition() > (*i).condition()))
                if (!evaluate_condition(II, EE, (*i).condition()))
                    return false;
            if ((*II).value() != (*i).value())
                return (false);
            ++II;
            ++i;
        }
        else
        {
            if ((*I).value() != (*i).value())
                return (false);
            ++I;
            ++i;
        }
    }

    if (I == E)
    {
        I = II;
        E = EE;
    }
    else
        return (true);

    for (; i != e;)
    {
        if ((I == E) || ((*I).condition() > (*i).condition()))
            if (!evaluate_condition(I, E, (*i).condition()))
                return false;

        if ((*I).condition() < (*i).condition())
            ++I;
        else
        {
            VERIFY((*I).condition() == (*i).condition());
            if ((*I).value() != (*i).value())
                return (false);
            ++I;
            ++i;
        }
    }
    return (true);
}

TEMPLATE_SPECIALIZATION
IC bool CProblemSolverAbstract::is_goal_reached_impl(const _index_type& vertex_index, bool) const
{
    STATIC_CHECK(reverse_search, This_function_cannot_be_used_in_the_STRAIGHT_search);
    auto I = m_current_state.conditions().cbegin();
    auto E = m_current_state.conditions().cend();
    auto i = vertex_index.conditions().cbegin();
    auto e = vertex_index.conditions().cend();
    for (; i != e;)
    {
        if ((I == E) || ((*I).condition() > (*i).condition()))
            if (!evaluate_condition(I, E, (*i).condition()))
                return false;

        if ((*I).condition() < (*i).condition())
            ++I;
        else
        {
            if ((*I).value() != (*i).value())
                return (false);
            ++I;
            ++i;
        }
    }
    return (true);
}

TEMPLATE_SPECIALIZATION
IC const xr_vector<typename CProblemSolverAbstract::_edge_type>& CProblemSolverAbstract::solution() const { return (m_solution); }

TEMPLATE_SPECIALIZATION
IC typename CProblemSolverAbstract::_operator_ptr CProblemSolverAbstract::get_operator(const _edge_type& operator_id)
{
    auto I = std::lower_bound(m_operators.begin(), m_operators.end(), operator_id);
    THROW(m_operators.end() != I);
    return ((*I).get_operator());
}

TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::solve()
{
    GoapStats::ProfScope _ps(GoapStats::PK_SOLVE, 0, GoapStats::enabled >= 2 ? typeid(*this).name() : nullptr);
    m_solution_changed = false;
    m_last_searched = false;

    const bool track = GoapStats::enabled || GoapStats::use_cache;
    if (track)
    {
        // Памятка живёт РОВНО один solve(): следующий апдейт -- другой мир.
        m_solve_cache.clear();
        ++GoapStats::g_solves;
        if (GoapStats::enabled)
            GoapStats::Tick();
    }

    {
        LoadProf::Scope _lp("plan.actual");
        if (actual())
            return;
    }

    if (track)
        ++GoapStats::g_replans;

    m_actuality = true;
    m_solution_changed = true;
    m_current_state.clear();

    // CGraphEngine is still incomplete here (graph_engine.h includes this header). Making the type dependent
    // defers the member lookup to instantiation — what MSVC did implicitly, and what two-phase lookup requires.
    std::conditional_t<sizeof(_operator_condition*) != 0, CGraphEngine, void>& graph_engine = ai().graph_engine();
    {
        LoadProf::Scope _lp("plan.search");
        m_failed = !graph_engine.search(*this, reverse_search ? target_state() : current_state(), reverse_search ? current_state() : target_state(), &m_solution,
                                               GraphEngineSpace::CSolverBaseParameters(GraphEngineSpace::_solver_dist_type(-1), GraphEngineSpace::_solver_condition_type(-1), m_search_cap));
    }
    m_last_searched = true;
    m_last_visited = graph_engine.solver_algorithm().data_storage().get_visited_node_count();
    if (LoadProf::Ctx& lp = LoadProf::tls(); lp.armed)
    {
        ++lp.searches;
        lp.visited += m_last_visited;
        lp.failed += m_failed ? 1 : 0;
    }
}

TEMPLATE_SPECIALIZATION
IC typename CProblemSolverAbstract::_edge_value_type CProblemSolverAbstract::estimate_edge_weight(const _index_type& condition) const
{
    return (estimate_edge_weight_impl<reverse_search>(condition));
}

TEMPLATE_SPECIALIZATION
IC typename CProblemSolverAbstract::_edge_value_type CProblemSolverAbstract::estimate_edge_weight_impl(const _index_type& condition) const
{
    STATIC_CHECK(!reverse_search, This_function_cannot_be_used_in_the_REVERSE_search);
    _edge_value_type result = 0;
    auto I = target_state().conditions().cbegin();
    auto E = target_state().conditions().cend();
    auto i = condition.conditions().cbegin();
    auto e = condition.conditions().cend();
    for (; (I != E) && (i != e);)
        if ((*I).condition() < (*i).condition())
        {
            ++result;
            ++I;
        }
        else if ((*I).condition() > (*i).condition())
            ++i;
        else
        {
            if ((*I).value() != (*i).value())
                ++result;
            ++I;
            ++i;
        }
    return (result + _edge_value_type(E - I));
}

TEMPLATE_SPECIALIZATION
IC typename CProblemSolverAbstract::_edge_value_type CProblemSolverAbstract::estimate_edge_weight_impl(const _index_type& condition, bool) const
{
    STATIC_CHECK(reverse_search, This_function_cannot_be_used_in_the_STRAIGHT_search);
    _edge_value_type result = 0;
    auto I = current_state().conditions().cbegin();
    auto E = current_state().conditions().cend();
    auto i = condition.conditions().cbegin();
    auto e = condition.conditions().cend();
    for (; (i != e);)
    {
        if ((I == E) || ((*I).condition() > (*i).condition()))
            if (!evaluate_condition(I, E, (*i).condition()))
                return result;

        if ((*I).condition() < (*i).condition())
            ++I;
        else
        {
            VERIFY((*I).condition() == (*i).condition());
            if ((*I).value() != (*i).value())
                ++result;
            ++I;
            ++i;
        }
    }
    return (result);
}

#undef TEMPLATE_SPECIALIZATION
#undef CProblemSolverAbstract
