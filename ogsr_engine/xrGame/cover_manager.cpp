////////////////////////////////////////////////////////////////////////////
//	Module 		: cover_manager.cpp
//	Created 	: 24.03.2004
//  Modified 	: 24.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Cover manager class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "level_graph.h"
#include "cover_manager.h"
#include "ai_space.h"
#include "cover_point.h"
#include "object_broker.h"

#include <thread>

#define MIN_COVER_VALUE 16

// ai_cover_threads — workers for the two static-cover passes over the AI map.
// 0 = one per core (capped at 16), 1 = the single-threaded original. Registered in
// xr_ioc_cmd.cpp; kept so the change can be A/B'd in one binary.
int ai_cover_threads = 0;

// ai_cover_async — 1 = build the static cover on a worker and join before objects
// spawn, 0 = build it inline where it always was. Same A/B-in-one-binary rule.
int ai_cover_async = 1;

namespace
{
// One at a time, and never detached: the builder reads the level graph, which
// CAI_Space::unload() deletes.
std::thread s_coverBuilder;
}   // namespace

CCoverManager::CCoverManager() { m_covers = 0; }

CCoverManager::~CCoverManager()
{
    wait_ready();
    clear();
    xr_delete(m_covers);
}

void CCoverManager::wait_ready()
{
    if (s_coverBuilder.joinable())
        s_coverBuilder.join();
}

void CCoverManager::compute_static_cover_async()
{
    wait_ready();   // never two builders, and never over a level graph being freed
    if (!ai_cover_async)
    {
        compute_static_cover();
        return;
    }
    s_coverBuilder = std::thread([this] { compute_static_cover(); });
}

IC bool CCoverManager::edge_vertex(u32 index)
{
    CLevelGraph::CVertex* v = ai().level_graph().vertex(index);
    return ((!ai().level_graph().valid_vertex_id(v->link(0)) && (v->high_cover(0) < MIN_COVER_VALUE)) ||
            (!ai().level_graph().valid_vertex_id(v->link(1)) && (v->high_cover(1) < MIN_COVER_VALUE)) ||
            (!ai().level_graph().valid_vertex_id(v->link(2)) && (v->high_cover(2) < MIN_COVER_VALUE)) ||
            (!ai().level_graph().valid_vertex_id(v->link(3)) && (v->high_cover(3) < MIN_COVER_VALUE)) ||
            (!ai().level_graph().valid_vertex_id(v->link(0)) && (v->low_cover(0) < MIN_COVER_VALUE)) ||
            (!ai().level_graph().valid_vertex_id(v->link(1)) && (v->low_cover(1) < MIN_COVER_VALUE)) ||
            (!ai().level_graph().valid_vertex_id(v->link(2)) && (v->low_cover(2) < MIN_COVER_VALUE)) ||
            (!ai().level_graph().valid_vertex_id(v->link(3)) && (v->low_cover(3) < MIN_COVER_VALUE)));
}

IC bool CCoverManager::cover(CLevelGraph::CVertex* v, u32 index0, u32 index1)
{
    return (ai().level_graph().valid_vertex_id(v->link(index0)) && ai().level_graph().valid_vertex_id(ai().level_graph().vertex(v->link(index0))->link(index1)) &&
            m_temp[ai().level_graph().vertex(v->link(index0))->link(index1)] != 0);
}

IC bool CCoverManager::critical_point(CLevelGraph::CVertex* v, u32 index, u32 index0, u32 index1)
{
    return (!ai().level_graph().valid_vertex_id(v->link(index)) &&
            (!ai().level_graph().valid_vertex_id(v->link(index0)) || !ai().level_graph().valid_vertex_id(v->link(index1)) || cover(v, index0, index) || cover(v, index1, index)));
}

IC bool CCoverManager::critical_cover(u32 index)
{
    CLevelGraph::CVertex* v = ai().level_graph().vertex(index);
    return (critical_point(v, 0, 1, 3) || critical_point(v, 2, 1, 3) || critical_point(v, 1, 0, 2) || critical_point(v, 3, 0, 2));
}

void CCoverManager::compute_static_cover()
{
    clear();
    xr_delete(m_covers);
    m_covers = xr_new<CPointQuadTree>(ai().level_graph().header().box(), ai().level_graph().header().cell_size() * .5f, 4 * 65536, 2 * 65536);
    m_temp.resize(ai().level_graph().header().vertex_count());

    const CLevelGraph& graph = ai().level_graph();
    const u32 levelVertexCount = ai().level_graph().header().vertex_count();

    // 16.3 million vertices on pripyat_full, twice over, on one core: measured 423 ms
    // of every level load, which was the WHOLE 'st_loading_alife_simulator' phase
    // (it is reached once, from graph().update() on the actor). Both passes are
    // index-independent reads of the mapped AI map -- the first writes only m_temp[i],
    // the second only reads it -- so they split by range. The quadtree insert stays
    // on this thread and in index order, so the tree comes out identical.
    // (The 2004 code has the same idea in a comment, waiting for a task system.)
    auto parallel_range = [](u32 count, u32 threads, auto&& body) {
        if (threads <= 1 || count < 4096) { body(0u, count, 0u); return; }
        const u32 grain = (count + threads - 1) / threads;
        xr_vector<std::thread> pool;
        pool.reserve(threads);
        for (u32 t = 1; t < threads; ++t) {
            const u32 lo = _min(count, t * grain), hi = _min(count, lo + grain);
            if (lo >= hi) break;
            pool.emplace_back([&body, lo, hi, t] { body(lo, hi, t); });
        }
        body(0u, _min(count, grain), 0u);
        for (auto& th : pool) th.join();
    };

    u32 nThr = std::thread::hardware_concurrency();
    nThr = _min(_max(1u, nThr), 16u);
    if (ai_cover_threads > 0) nThr = _min((u32)ai_cover_threads, nThr);

    CTimer _t; _t.Start();
    parallel_range(levelVertexCount, nThr, [&](u32 lo, u32 hi, u32) {
        for (u32 i = lo; i < hi; ++i)
        {
            const CLevelGraph::CVertex& vertex = *graph.vertex(i);
            if (vertex.high_cover(0) + vertex.high_cover(1) + vertex.high_cover(2) + vertex.high_cover(3))
            {
                m_temp[i] = edge_vertex(i) ? 1 : 0;
                continue;
            }

            if (vertex.low_cover(0) + vertex.low_cover(1) + vertex.low_cover(2) + vertex.low_cover(3))
            {
                m_temp[i] = edge_vertex(i) ? 1 : 0;
                continue;
            }

            m_temp[i] = 0;
        }
    });
    const float ms_mark = _t.GetElapsed_ms_total();

    // Second pass: critical_cover() reads m_temp of NEIGHBOURING vertices, so it can
    // only start once the first pass is finished everywhere -- hence the join above --
    // and it must not write m_temp while others are reading it. Each range collects
    // its own hits; the merge below is in index order, so the quadtree is built in
    // exactly the order the single-threaded loop built it.
    _t.Start();
    xr_vector<xr_vector<u32>> hits(nThr);
    parallel_range(levelVertexCount, nThr, [&](u32 lo, u32 hi, u32 t) {
        xr_vector<u32>& out = hits[t];
        for (u32 i = lo; i < hi; ++i)
            if (m_temp[i] && critical_cover(i))
                out.push_back(i);
    });
    const float ms_crit = _t.GetElapsed_ms_total();

    _t.Start();
    size_t total = 0;
    for (const xr_vector<u32>& part : hits)
        total += part.size();
    m_point_pool.clear();
    m_point_pool.reserve(total);   // exact: emplace_back below must never reallocate

    u32 inserted = 0;
    for (const xr_vector<u32>& part : hits)
        for (u32 i : part) {
            m_point_pool.emplace_back(ai().level_graph().vertex_position(ai().level_graph().vertex(i)), i);
            m_covers->insert(&m_point_pool.back());
            ++inserted;
        }
    Msg("[load step]     static cover: %u vertices on %u threads = mark %.0f + critical %.0f + insert %.0f ms (%u covers)",
        levelVertexCount, nThr, ms_mark, ms_crit, _t.GetElapsed_ms_total(), inserted);
}

void CCoverManager::clear()
{
    // The points are not individually owned any more: they are slots in
    // m_point_pool, so the tree is emptied and the block goes back in one piece.
    // (The old code walked the tree to collect 1.5M pointers and deleted them one
    // by one.) m_nearest is a query scratch — nothing in it survives this.
    m_nearest.clear();
    m_point_pool.clear();

    if (!get_covers())
        return;

    m_covers->clear();
}
