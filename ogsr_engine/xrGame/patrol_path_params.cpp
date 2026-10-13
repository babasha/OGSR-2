////////////////////////////////////////////////////////////////////////////
//	Module 		: patrol_path_params.cpp
//	Created 	: 30.09.2003
//  Modified 	: 29.06.2004
//	Author		: Dmitriy Iassenev
//	Description : Patrol path parameters class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "patrol_path_params.h"
#include "patrol_path_manager.h"
#include "ai_space.h"

CPatrolPathParams::CPatrolPathParams(LPCSTR caPatrolPathToGo, const PatrolPathManager::EPatrolStartType tPatrolPathStart, const PatrolPathManager::EPatrolRouteType tPatrolPathStop,
                                     bool bRandom, u32 index)
{
    m_path_name = caPatrolPathToGo;
    m_path = ai().patrol_paths().safe_path(m_path_name, true);

#ifdef CRASH_ON_INVALID_VERTEX_ID
    ASSERT_FMT(m_path, "[%s]: there is no patrol path %s", __FUNCTION__, caPatrolPathToGo);
#else
    THROW3(m_path, "There is no patrol path", caPatrolPathToGo);
#endif

    m_tPatrolPathStart = tPatrolPathStart;
    m_tPatrolPathStop = tPatrolPathStop;
    m_bRandom = bRandom;
    m_previous_index = index;
}

CPatrolPathParams::~CPatrolPathParams() {}

// Missing/empty patrol paths are REAL on cross-level mods: scripts written for the
// full campaign run on standalone maps where the path's level was never shipped
// (pripyat_full: OGSE fix_bandit_trader probes bandit_trader_walk from Dark Valley
// — the ex-assert here was a guaranteed crash on the "День 1" save). Same fail-soft
// policy as the broken-physics NaN-ray guard: log loudly, return inert data, let
// the script continue — it is querying a path that does not exist HERE by design.
bool CPatrolPathParams::dead_path() const
{
    if (m_path && !m_path->vertices().empty()) return false;
    Msg("!![CPatrolPathParams] path [%s] is missing or has no vertices on this level — returning inert data", m_path_name.c_str());
    return true;
}

u32 CPatrolPathParams::count() const { return dead_path() ? 0 : (m_path->vertices().size()); }

const Fvector& CPatrolPathParams::point(u32 index) const
{
    static const Fvector s_zero = { 0.f, 0.f, 0.f };
    if (dead_path()) return s_zero;
    if (!m_path->vertex(index))
    {
        Msg("!![%s] Can't get information about patrol point number [%u] in the patrol way [%s]", __FUNCTION__, index, m_path_name.c_str());
        index = (*m_path->vertices().begin()).second->vertex_id();
    }
    ASSERT_FMT(m_path->vertex(index), "!![%s] Can't get information about patrol point number [%u] in the patrol way [%s]", __FUNCTION__, index, m_path_name.c_str());
    return m_path->vertex(index)->data().position();
}

u32 CPatrolPathParams::level_vertex_id(u32 index) const
{
    if (dead_path()) return 0;
    if (!m_path->vertex(index))
    {
        Msg("!![%s] Can't get information about patrol point number [%u] in the patrol way [%s]", __FUNCTION__, index, m_path_name.c_str());
        index = (*m_path->vertices().begin()).second->vertex_id();
    }
    ASSERT_FMT(m_path->vertex(index), "!![%s] Can't get information about patrol point number [%u] in the patrol way [%s]", __FUNCTION__, index, m_path_name.c_str());
    return m_path->vertex(index)->data().level_vertex_id();
}

GameGraph::_GRAPH_ID CPatrolPathParams::game_vertex_id(u32 index) const
{
    if (dead_path()) return GameGraph::_GRAPH_ID(0);
    if (!m_path->vertex(index))
    {
        Msg("!![%s] Can't get information about patrol point number [%u] in the patrol way [%s]", __FUNCTION__, index, m_path_name.c_str());
        index = (*m_path->vertices().begin()).second->vertex_id();
    }
    ASSERT_FMT(m_path->vertex(index), "!![%s] Can't get information about patrol point number [%u] in the patrol way [%s]", __FUNCTION__, index, m_path_name.c_str());
    return m_path->vertex(index)->data().game_vertex_id();
}

u32 CPatrolPathParams::point(LPCSTR name) const
{
    if (!dead_path() && m_path->point(name))
        return (m_path->point(name)->vertex_id());
    return (u32(-1));
}

u32 CPatrolPathParams::point(const Fvector& point) const
{
    if (dead_path()) return (u32(-1));
    return (m_path->point(point)->vertex_id());
}

bool CPatrolPathParams::flag(u32 index, u8 flag_index) const
{
    if (dead_path() || !m_path->vertex(index)) return false;
    return (!!(m_path->vertex(index)->data().flags() & (u32(1) << flag_index)));
}

Flags32 CPatrolPathParams::flags(u32 index) const
{
    if (dead_path() || !m_path->vertex(index)) return Flags32().assign(0);
    return (Flags32().assign(m_path->vertex(index)->data().flags()));
}

LPCSTR CPatrolPathParams::name(u32 index) const
{
    if (dead_path() || !m_path->vertex(index)) return "";
    return (*m_path->vertex(index)->data().name());
}

bool CPatrolPathParams::terminal(u32 index) const
{
    if (dead_path() || !m_path->vertex(index)) return true;
    return (m_path->vertex(index)->edges().size() == 0);
}
