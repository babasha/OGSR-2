////////////////////////////////////////////////////////////////////////////
//	Module 		: action_base_inline.h
//	Created 	: 28.01.2004
//  Modified 	: 10.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Base action inline function
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Level.h"

#define TEMPLATE_SPECIALIZATION template <typename _object_type>
#define CBaseAction CActionBase<_object_type>

TEMPLATE_SPECIALIZATION
IC CBaseAction::CActionBase(_object_type* object, LPCSTR action_name) { init(object, action_name); }

TEMPLATE_SPECIALIZATION
CBaseAction::~CActionBase() {}

TEMPLATE_SPECIALIZATION
void CBaseAction::init(_object_type* object, LPCSTR action_name)
{
    m_storage = 0;
    m_object = object;
    m_weight = _edge_value_type(1);
}

TEMPLATE_SPECIALIZATION
void CBaseAction::setup(_object_type* object, CPropertyStorage* storage)
{
    VERIFY(object);
    VERIFY(storage);
    m_object = object;
    m_storage = storage;
    m_inertia_time = 0;
}

TEMPLATE_SPECIALIZATION
void CBaseAction::initialize()
{
    m_start_level_time = Device.dwTimeGlobal;
    m_first_time = true;
}

TEMPLATE_SPECIALIZATION
void CBaseAction::execute()
{
    m_first_time = false;
}

TEMPLATE_SPECIALIZATION
void CBaseAction::finalize()
{
}

TEMPLATE_SPECIALIZATION
bool CBaseAction::completed() const { return (m_start_level_time + m_inertia_time <= Device.dwTimeGlobal); }

TEMPLATE_SPECIALIZATION
IC u32 CBaseAction::inertia_time() const { return (m_inertia_time); }

TEMPLATE_SPECIALIZATION
IC void CBaseAction::set_inertia_time(u32 inertia_time) { m_inertia_time = inertia_time; }

TEMPLATE_SPECIALIZATION
IC void CBaseAction::set_property(const _condition_type& condition_id, const _value_type& value)
{
    VERIFY(m_storage);
    m_storage->set_property(condition_id, value);
}

TEMPLATE_SPECIALIZATION
IC const typename CBaseAction::_value_type& CBaseAction::property(const _condition_type& condition_id) const
{
    VERIFY(m_storage);
    return (m_storage->property(condition_id));
}

TEMPLATE_SPECIALIZATION
IC void CBaseAction::set_weight(const _edge_value_type& weight) { m_weight = _max(min_weight(), weight); }

TEMPLATE_SPECIALIZATION
typename CBaseAction::_edge_value_type CBaseAction::weight(const CSConditionState& condition0, const CSConditionState& condition1) const
{
    _edge_value_type _min_weight = min_weight();
    if (m_weight < _min_weight)
        m_weight = _min_weight;
    return (m_weight);
}

TEMPLATE_SPECIALIZATION
IC bool CBaseAction::first_time() const { return (m_first_time); }

#undef TEMPLATE_SPECIALIZATION
#undef CBaseAction