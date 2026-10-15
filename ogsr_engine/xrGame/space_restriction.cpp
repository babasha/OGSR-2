////////////////////////////////////////////////////////////////////////////
//	Module 		: space_restriction.cpp
//	Created 	: 17.08.2004
//  Modified 	: 27.08.2004
//	Author		: Dmitriy Iassenev
//	Description : Space restriction
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "space_restriction.h"
#include "space_restriction_manager.h"
#include "ai_space.h"
#include "level_graph.h"
#include "space_restriction_base.h"


template <bool a>
struct CMergeInOutPredicate
{
    SpaceRestrictionHolder::CBaseRestrictionPtr m_out;
    SpaceRestrictionHolder::CBaseRestrictionPtr m_in;

    IC CMergeInOutPredicate(SpaceRestrictionHolder::CBaseRestrictionPtr out, SpaceRestrictionHolder::CBaseRestrictionPtr in)
    {
        m_out = out;
        m_in = in;
    }

    IC bool operator()(u32 level_vertex_id) const
    {
        if (!m_out || !m_in)
            return (false);
        return (a ? m_in->inside(level_vertex_id, false) : !m_out->inside(level_vertex_id, true));
    }
};

bool CSpaceRestriction::accessible(const Fsphere& sphere)
{
    if (!initialized())
    {
        initialize();
        if (!initialized())
            return (true);
    }

    return (ai().level_graph().valid_vertex_position(sphere.P) &&
            (m_out_space_restriction ?
                 (m_out_space_restriction->inside(sphere) && !m_out_space_restriction->on_border(sphere.P) && !m_out_space_restriction->out_of_border(sphere.P)) :
                 true) &&
            (m_in_space_restriction ? (!m_in_space_restriction->inside(sphere) && !m_in_space_restriction->on_border(sphere.P)) : true));
}

bool CSpaceRestriction::accessible(u32 level_vertex_id, float radius)
{
    if (!initialized())
    {
        initialize();
        if (!initialized())
            return (true);
    }

    return ((m_out_space_restriction ? m_out_space_restriction->inside(level_vertex_id, false, radius) : true) &&
            (m_in_space_restriction ? !m_in_space_restriction->inside(level_vertex_id, true, radius) : true));
}

IC bool CSpaceRestriction::intersects(SpaceRestrictionHolder::CBaseRestrictionPtr bridge0, SpaceRestrictionHolder::CBaseRestrictionPtr bridge1)
{
    xr_vector<u32>::const_iterator I = bridge1->border().begin();
    xr_vector<u32>::const_iterator E = bridge1->border().end();
    for (; I != E; ++I)
        if (bridge0->inside(*I, true))
            return (true);

    if (!bridge0->border().empty() && bridge1->inside(bridge0->border().front(), true))
        return (true);

    m_temp.resize(bridge0->border().size() + bridge1->border().size());
    xr_vector<u32>::iterator J = std::set_intersection(bridge0->border().begin(), bridge0->border().end(), bridge1->border().begin(), bridge1->border().end(), m_temp.begin());
    return (J != m_temp.begin());
}

IC bool CSpaceRestriction::intersects(SpaceRestrictionHolder::CBaseRestrictionPtr bridge)
{
    if (!m_out_space_restriction)
        return (false);

    return (intersects(m_out_space_restriction, bridge));
}

void CSpaceRestriction::merge_in_out_restrictions()
{
    START_PROFILE("Restricted Object/Merge In-Out");
    xr_vector<u32> temp_border;

    m_border = m_out_space_restriction->border();
    m_border.erase(std::remove_if(m_border.begin(), m_border.end(), CMergeInOutPredicate<true>(m_out_space_restriction, m_in_space_restriction)), m_border.end());

    if (m_in_space_restriction)
    {
        temp_border = m_in_space_restriction->border();
        temp_border.erase(std::remove_if(temp_border.begin(), temp_border.end(), CMergeInOutPredicate<false>(m_out_space_restriction, m_in_space_restriction)), temp_border.end());
        m_border.insert(m_border.end(), temp_border.begin(), temp_border.end());
    }

    std::sort(m_border.begin(), m_border.end());
    m_border.erase(std::unique(m_border.begin(), m_border.end()), m_border.end());
    STOP_PROFILE;
}

CSpaceRestriction::CBaseRestrictionPtr CSpaceRestriction::merge(CBaseRestrictionPtr bridge, const RESTRICTIONS& temp_restrictions) const
{
    string4096 S;
    shared_str temp = bridge->name();

    for (const auto& I : temp_restrictions)
        temp = strconcat(sizeof(S), S, temp.c_str(), ",", I->name().c_str());

    return m_space_restriction_manager->restriction(temp);
}

void CSpaceRestriction::initialize()
{
    VERIFY(!m_initialized);
    m_out_space_restriction = m_space_restriction_manager->restriction(m_out_restrictions);
    m_in_space_restriction = m_space_restriction_manager->restriction(m_in_restrictions);

    if (!m_out_space_restriction && !m_in_space_restriction)
    {
        m_initialized = true;
        return;
    }

    if (m_out_space_restriction && !m_out_space_restriction->initialized())
        m_out_space_restriction->initialize();

    if (m_in_space_restriction && !m_in_space_restriction->initialized())
        m_in_space_restriction->initialize();

    if ((m_out_space_restriction && !m_out_space_restriction->initialized()) || (m_in_space_restriction && !m_in_space_restriction->initialized()))
        return;

    if (m_out_space_restriction)
        merge_in_out_restrictions();

    m_initialized = true;
}

void CSpaceRestriction::remove_border()
{
    if (!initialized())
        return;

    VERIFY(m_applied);

    m_applied = false;

    if (m_out_space_restriction)
    {
        ai().level_graph().clear_mask(border());
        return;
    }

    ai().level_graph().clear_mask(m_in_space_restriction->border());
}

u32 CSpaceRestriction::accessible_nearest(const Fvector& position, Fvector& result)
{
    if (m_out_space_restriction)
        return (m_out_space_restriction->accessible_nearest(this, position, result, true));

    VERIFY(m_in_space_restriction);
    return (m_in_space_restriction->accessible_nearest(m_in_space_restriction, position, result, false));
}

bool CSpaceRestriction::affect(SpaceRestrictionHolder::CBaseRestrictionPtr bridge, const Fsphere& sphere) const
{
    if (bridge->inside(sphere))
        return (false);

    return (true);
}

bool CSpaceRestriction::affect(SpaceRestrictionHolder::CBaseRestrictionPtr bridge, u32 start_vertex_id, float radius) const
{
    Fsphere sphere;
    sphere.P = ai().level_graph().vertex_position(start_vertex_id);
    sphere.R = radius;
    return (affect(bridge, sphere));
}

bool CSpaceRestriction::affect(SpaceRestrictionHolder::CBaseRestrictionPtr bridge, const Fvector& start_position, const Fvector& dest_position) const
{
    Fsphere sphere0, sphere1;
    sphere0.P = start_position;
    sphere0.R = 0.f;
    sphere1.P = dest_position;
    sphere1.R = 0.f;
    return (affect(bridge, sphere0) || affect(bridge, sphere1));
}

bool CSpaceRestriction::affect(SpaceRestrictionHolder::CBaseRestrictionPtr bridge, u32 start_vertex_id, u32 dest_vertex_id) const
{
    return (affect(bridge, start_vertex_id, 0.f) || affect(bridge, dest_vertex_id, 0.f));
}

shared_str CSpaceRestriction::name() const { return (m_out_restrictions); }
