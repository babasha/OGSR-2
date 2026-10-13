////////////////////////////////////////////////////////////////////////////
//	Module 		: level_graph.cpp
//	Created 	: 02.10.2001
//  Modified 	: 11.11.2003
//	Author		: Oles Shihkovtsov, Dmitriy Iassenev
//	Description : Level graph
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "level_graph.h"


CLevelGraph::CLevelGraph(LPCSTR fName)
{
    sh_debug->create("editor\\wire"); // sh_debug->create("debug\\ai_nodes", "$null");

    m_reader = FS.r_open(fName);

    // m_header & data
    m_header = (CHeader*)m_reader->pointer();
    ASSERT_XRAI_VERSION_MATCH(header().version(), "Level graph");
    m_reader->advance(sizeof(CHeader));
    m_nodes = xr_new<CVertexStorage>(m_reader, header().vertex_count(), static_cast<xrAI_Versions>(header().version()));
    m_row_length = iFloor((header().box().max.z - header().box().min.z) / header().cell_size() + EPS_L + 1.5f);
    m_column_length = iFloor((header().box().max.x - header().box().min.x) / header().cell_size() + EPS_L + 1.5f);
    m_access_mask.assign(header().vertex_count(), true);
    unpack_xz(vertex_position(header().box().max), m_max_x, m_max_z);

#ifdef DEBUG
    m_current_level_id = -1;
    m_current_actual = false;
    m_current_center = Fvector().set(flt_max, flt_max, flt_max);
    m_current_radius = Fvector().set(flt_max, flt_max, flt_max);
#endif
}

CLevelGraph::~CLevelGraph() 
{ 
    xr_delete(m_nodes);
    FS.r_close(m_reader);
}

// The original implementation: every vertex on the level, one at a time. Kept
// because it is the ORACLE the fast one is checked against (sv_navmesh_check),
// and because it documents exactly what "nearest" is defined to mean here.
// Nothing in the game calls it.
u32 CLevelGraph::nearest_vertex_id_linear(const Fvector& position) const
{
    float min_dist = flt_max;
    u32 selected;
    set_invalid_vertex(selected);
    for (u32 i = 0; i < header().vertex_count(); ++i)
    {
        float dist = distance(i, position);
        if (dist < min_dist)
        {
            min_dist = dist;
            selected = i;
        }
    }

    return (selected);
}

// ⭐⭐THIS WAS A FULL-LEVEL SCAN, AND IT RAN ON THE SERVER TICK.
//
// Measured 30-07-2026 on Pripyat: 700-840 ms, synchronously, every time an
// order named a point off the navmesh -- six stalls in a 90 s run, six orders,
// six for six. For that most of a second the server does not tick at all: no
// physics, no AI, no damage, no snapshot to any client. The visible symptom was
// somewhere else entirely -- the drive integrates the whole frozen interval in
// one step, so the next packet carries a 4.2 m stride with an honest 47 ms
// stamp and every client draws a creature moving at 90 m/s. See
// server-tick-freeze-offmesh-navmesh-scan.
//
// ⭐The scan was never necessary. Vertices are sorted by their packed cell key
// (x * row_length + z, see vertex::predicate2), so one row of the grid is a
// CONTIGUOUS range that binary search finds in log time. Searching a square of
// rows around the query point and widening it until nothing outside could
// possibly be closer gives BIT-FOR-BIT the same vertex as the linear scan --
// this is not an approximation and there is no tolerance to tune.
//
// ⚠The termination test is the whole correctness argument. `distance()` here
// returns a SQUARED distance, and the query point sits somewhere inside its own
// cell rather than at its centre, so the width proven clear by a search of
// `radius` cells is one cell less than that. Getting either wrong turns an
// exact answer into a plausible one.
//
// The last widening covers the level, at which point this IS the old scan --
// so a point with no vertex anywhere near it costs what it always did, and
// nothing can regress into returning a different answer.
u32 CLevelGraph::nearest_vertex_id(const Fvector& position) const
{
    const float cell = header().cell_size();
    const Fbox& box = header().box();

    const int last_x = int(m_column_length) - 1;
    const int last_z = int(m_row_length) - 1;

    // The query point's own cell, in the same arithmetic the packed key uses
    // (see inside(u32, Fvector2)), CLAMPED onto the grid.
    //
    // ⭐⭐A POINT OFF THE LEVEL BOX USED TO BE THE FULL SCAN, AND IT IS A COMMON CASE.
    // 📏04-10-2026, Switch: 166-171 ms on the second thread, once every few
    // artefacts -- the electric fences on Cordon (zone_electroizgorod) stand on
    // the level's southern edge, z = -480 against a box ending at -480.9, so
    // about half the points scattered around them land past it. sv_navmesh_check
    // on the PC: "outside the level" 16.7 ms fast vs 17.3 full scan -- the same.
    //
    // Searching around the clamped cell stays EXACT. Every vertex is on the
    // grid, so on an axis where the point lies past the edge, every vertex is
    // at least that gap away along it -- and one left out of a square of
    // `radius` around the clamped cell is still `radius - 1` cells further on
    // top of that gap (or along the other axis, where the gap still holds).
    // So the width proven clear is ((radius - 1) * cell)^2 + gap_x^2 + gap_z^2,
    // and for a point inside the grid both gaps are 0 and this is the old proof.
    const int rx = iFloor((position.x - box.min.x) / cell + .5f);
    const int rz = iFloor((position.z - box.min.z) / cell + .5f);
    const int qx = _min(_max(rx, 0), last_x);
    const int qz = _min(_max(rz, 0), last_z);

    // How far past the outermost cells' own extent (half a cell beyond their
    // centres) the point is, in metres; 0 on the grid.
    auto gap = [cell](float p, float lo, float hi) {
        const float below = (lo - .5f * cell) - p;
        const float above = p - (hi + .5f * cell);
        return _max(0.f, _max(below, above));
    };
    const float gap_x = gap(position.x, box.min.x, box.min.x + float(last_x) * cell);
    const float gap_z = gap(position.z, box.min.z, box.min.z + float(last_z) * cell);
    const float gap_sq = gap_x * gap_x + gap_z * gap_z;

    CVertex* const first = m_nodes->begin();
    CVertex* const last = m_nodes->end();

    float best = flt_max; // squared
    u32 selected;
    set_invalid_vertex(selected);

    for (int radius = 4;; radius *= 2)
    {
        const int x0 = _max(0, qx - radius), x1 = _min(last_x, qx + radius);
        const int z0 = _max(0, qz - radius), z1 = _min(last_z, qz + radius);
        const bool whole_level = (x0 == 0) && (z0 == 0) && (x1 == last_x) && (z1 == last_z);

        for (int x = x0; x <= x1; ++x)
        {
            // One grid row, one contiguous key range: [x,z0] .. [x,z1].
            const u32 k0 = u32(x) * m_row_length + u32(z0);
            const u32 k1 = u32(x) * m_row_length + u32(z1);
            CVertex* I = std::lower_bound(first, last, k0, &vertex::predicate2);
            CVertex* E = std::upper_bound(first, last, k1, &vertex::predicate);
            for (; I != E; ++I)
            {
                const float dist = distance(position, I);
                if (dist < best)
                {
                    best = dist;
                    selected = vertex_id(I);
                }
            }
        }

        const float proven = float(radius - 1) * cell;
        if (valid_vertex_id(selected) && (best <= proven * proven + gap_sq))
            break;
        if (whole_level)
            break;

        // ⛔No "found nothing this pass, cover everything at once" shortcut. An
        // earlier version had one, and an ordinary order naming a spot 21 m off
        // the walkable area finds nothing in the FIRST square and everything in
        // the fourth -- that sent the common case straight back into a full
        // scan (measured, 0 stalls to 6 in one build). The later version kept
        // it for points off the level box only, and that was the Switch stall
        // above; doubling from the clamped cell costs at most twice its last
        // square, and the last square is never more than the level.
    }

    VERIFY(valid_vertex_id(selected));
    return (selected);
}

u32 CLevelGraph::vertex_id(u32 current_node_id, const Fvector& position) const
{
    START_PROFILE("Level_Graph::find vertex")
    Device.Statistic->AI_Node.Begin();

    if (valid_vertex_position(position))
    {
        // so, our position is inside the level graph bounding box
        if (valid_vertex_id(current_node_id) && inside(vertex(current_node_id), position))
        {
            // so, our node corresponds to the position
            Device.Statistic->AI_Node.End();
            return (current_node_id);
        }

        // so, our node doesn't correspond to the position
        // try to search it with O(logN) time algorithm
        u32 _vertex_id = vertex_id(position);
        if (valid_vertex_id(_vertex_id))
        {
            // so, there is a node which corresponds with x and z to the position
            bool ok = true;
            if (valid_vertex_id(current_node_id))
            {
                float y0 = vertex_plane_y(current_node_id, position.x, position.z);
                float y1 = vertex_plane_y(_vertex_id, position.x, position.z);
                bool over0 = position.y > y0;
                bool over1 = position.y > y1;
                float y_dist0 = position.y - y0;
                float y_dist1 = position.y - y1;
                if (over0)
                {
                    if (over1)
                    {
                        if (y_dist1 - y_dist0 > 1.f)
                            ok = false;
                        else
                            ok = true;
                    }
                    else
                    {
                        ok = false;
                    }
                }
                else
                {
                    if (over1)
                    {
                        ok = true;
                    }
                    else
                    {
                        ok = false;
                    }
                }
            }
            if (ok)
            {
                Device.Statistic->AI_Node.End();
                return (_vertex_id);
            }
        }
    }

    if (!valid_vertex_id(current_node_id))
    {
        // so, we do not have a correct current node
        // performing very slow full search
        const u32 id = nearest_vertex_id(position);
        VERIFY(valid_vertex_id(id));
        Device.Statistic->AI_Node.End();
        return (id);
    }

    // so, our position is outside the level graph bounding box
    // or
    // there is no node for the current position
    // try to search the nearest one iteratively
    SContour _contour;
    Fvector point;
    u32 best_vertex_id = current_node_id;
    contour(_contour, current_node_id);
    nearest(point, position, _contour);
    float best_distance_sqr = position.distance_to_sqr(point);
    const_iterator i, e;
    begin(current_node_id, i, e);
    for (; i != e; ++i)
    {
        u32 level_vertex_id = value(current_node_id, i);
        if (!valid_vertex_id(level_vertex_id))
            continue;

        contour(_contour, level_vertex_id);
        nearest(point, position, _contour);
        float distance_sqr = position.distance_to_sqr(point);
        if (best_distance_sqr > distance_sqr)
        {
            best_distance_sqr = distance_sqr;
            best_vertex_id = level_vertex_id;
        }
    }

    Device.Statistic->AI_Node.End();
    return (best_vertex_id);

    STOP_PROFILE
}

u32 CLevelGraph::vertex_id(const Fvector& position) const
{
    CPosition _vertex_position = vertex_position(position);
    CVertex* B = m_nodes->begin();
    CVertex* E = m_nodes->end();
    CVertex* I = std::lower_bound(B, E, _vertex_position.xz());
    if ((I == E) || ((*I).position().xz() != _vertex_position.xz()))
        return (u32(-1));

    u32 best_vertex_id = u32(I - B);
    float y = vertex_plane_y(best_vertex_id, position.x, position.z);
    for (++I; I != E; ++I)
    {
        if ((*I).position().xz() != _vertex_position.xz())
            break;

        u32 new_vertex_id = u32(I - B);
        float _y = vertex_plane_y(new_vertex_id, position.x, position.z);
        if (y <= position.y)
        {
            // so, current node is under the specified position
            if (_y <= position.y)
            {
                // so, new node is under the specified position
                if (position.y - _y < position.y - y)
                {
                    // so, new node is closer to the specified position
                    y = _y;
                    best_vertex_id = new_vertex_id;
                }
            }
        }
        else
            // so, current node is over the specified position
            if (_y <= position.y)
            {
                // so, new node is under the specified position
                y = _y;
                best_vertex_id = new_vertex_id;
            }
            else
                // so, new node is over the specified position
                if (_y - position.y < y - position.y)
                {
                    // so, new node is closer to the specified position
                    y = _y;
                    best_vertex_id = new_vertex_id;
                }
    }

    return (best_vertex_id);
}