////////////////////////////////////////////////////////////////////////////
//	Module 		: CEdge.h
//	Created 	: 14.01.2004
//  Modified 	: 19.02.2005
//	Author		: Dmitriy Iassenev
//	Description : Graph edge class template
////////////////////////////////////////////////////////////////////////////

#pragma once

template <typename _edge_weight_type_, typename _vertex_type_>
class CEdge
{
public:
    typedef _edge_weight_type_ _edge_weight_type;
    typedef _vertex_type_ _vertex_type;
    typedef typename _vertex_type::_vertex_id_type _vertex_id_type;

private:
    _edge_weight_type m_weight;
    _vertex_type* m_vertex;

public:
    IC CEdge(const _edge_weight_type& weight, _vertex_type* vertex);
    IC const _edge_weight_type& weight() const;
    IC _vertex_type* vertex() const;
    IC const _vertex_id_type& vertex_id() const;
    IC bool operator==(const _vertex_id_type& vertex_id) const;
    IC bool operator==(const CEdge& obj) const;
};

#include "graph_edge_inline.h"