////////////////////////////////////////////////////////////////////////////
//	Module 		: patrol_point.cpp
//	Created 	: 15.06.2004
//  Modified 	: 15.06.2004
//	Author		: Dmitriy Iassenev
//	Description : Patrol point
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "patrol_point.h"
#include "level_graph.h"
#include "level_graph.h"
#include "game_level_cross_table.h"
#include "game_graph.h"
#include "object_broker.h"

#include "ai_space.h"

CPatrolPoint::CPatrolPoint(const CPatrolPath* path)
{
}

IC void CPatrolPoint::correct_position(const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph)
{
    if (!level_graph || !level_graph->valid_vertex_position(position()) || !level_graph->valid_vertex_id(m_level_vertex_id))
        return;

    if (!level_graph->inside(level_vertex_id(level_graph, cross, game_graph), position()))
        m_position = level_graph->vertex_position(level_vertex_id(level_graph, cross, game_graph));

    m_game_vertex_id = cross->vertex(level_vertex_id(level_graph, cross, game_graph)).game_vertex_id();
}

CPatrolPoint::CPatrolPoint(const CPatrolPath* path, const Fvector& position, u32 level_vertex_id, u32 flags, shared_str name)
{
    m_position = position;
    m_level_vertex_id = level_vertex_id;
    m_flags = flags;
    m_name = name;
}

CPatrolPoint& CPatrolPoint::load_raw(const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph, IReader& stream)
{
    stream.r_fvector3(m_position);
    m_flags = stream.r_u32();
    stream.r_stringZ(m_name);
    if (level_graph && level_graph->valid_vertex_position(m_position))
    {
        Fvector position = m_position;
        position.y += .15f;
        m_level_vertex_id = level_graph->vertex_id(position);
    }
    else
        m_level_vertex_id = u32(-1);
    correct_position(level_graph, cross, game_graph);
    return (*this);
}

CPatrolPoint& CPatrolPoint::load_ini(CInifile::Sect& section, LPSTR prefix)
{
    string256 full_name;

    m_name = section.r_string(xr_strconcat(full_name, prefix, ":", "name"));
    m_position = section.r_fvector3(xr_strconcat(full_name, prefix, ":", "position"));
    m_level_vertex_id = strtol(section.r_string(xr_strconcat(full_name, prefix, ":", "level_vertex_id")), nullptr, 10);
    m_game_vertex_id = strtol(section.r_string(xr_strconcat(full_name, prefix, ":", "game_vertex_id")), nullptr, 10);

    xr_strconcat(full_name, prefix, ":", "flags");

    if (section.line_exist(full_name))
        m_flags = strtol(section.r_string(full_name), nullptr, 16);

    // correct_position(level_graph, cross, game_graph); ???
    return (*this);
}

void CPatrolPoint::load(IReader& stream)
{
    load_data(m_name, stream);
    load_data(m_position, stream);
    load_data(m_flags, stream);
    load_data(m_level_vertex_id, stream);
    load_data(m_game_vertex_id, stream);
}

void CPatrolPoint::save(IWriter& stream)
{
    save_data(m_name, stream);
    save_data(m_position, stream);
    save_data(m_flags, stream);
    save_data(m_level_vertex_id, stream);
    save_data(m_game_vertex_id, stream);
}

const u32& CPatrolPoint::level_vertex_id() const
{
    if (ai().game_graph().vertex(m_game_vertex_id)->level_id() == ai().level_graph().level_id())
        return (level_vertex_id(&ai().level_graph(), &ai().cross_table(), &ai().game_graph()));

    return (m_level_vertex_id);
}

const GameGraph::_GRAPH_ID& CPatrolPoint::game_vertex_id() const
{
    if (ai().game_graph().vertex(m_game_vertex_id)->level_id() == ai().level_graph().level_id())
        return (game_vertex_id(&ai().level_graph(), &ai().cross_table(), &ai().game_graph()));

    return (m_game_vertex_id);
}

CPatrolPoint& CPatrolPoint::position(Fvector position)
{
    m_position = position;

    auto level_graph = &ai().level_graph();
    if (level_graph && level_graph->valid_vertex_position(m_position))
    {
        Fvector pos = m_position;
        pos.y += .15f;
        m_level_vertex_id = level_graph->vertex_id(pos);
    }
    else
        m_level_vertex_id = u32(-1);
    correct_position(level_graph, &ai().cross_table(), &ai().game_graph());

    return *this;
}
