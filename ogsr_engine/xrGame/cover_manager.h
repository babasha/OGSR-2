////////////////////////////////////////////////////////////////////////////
//	Module 		: cover_manager.h
//	Created 	: 24.03.2004
//  Modified 	: 24.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Cover manager class
////////////////////////////////////////////////////////////////////////////

#pragma once


#include "quadtree.h"
#include "cover_point.h"

namespace LevelGraph
{
class CVertex;
};

class CCoverManager
{
public:
    typedef CQuadTree<CCoverPoint> CPointQuadTree;

protected:
    CPointQuadTree* m_covers;
    // u8, not bool: xr_vector<bool> is bit-packed, so two threads writing
    // neighbouring indices write the same byte. compute_static_cover fills this
    // from sixteen threads (16.3M vertices on pripyat_full).
    xr_vector<u8>   m_temp;
    // Every static cover point of the level in ONE block, reserved to the exact
    // count so nothing reallocates and the tree's pointers into it stay valid.
    // A level has 1.5 million of them; they used to be 1.5 million xr_new.
    xr_vector<CCoverPoint> m_point_pool;
    mutable xr_vector<CCoverPoint*> m_nearest;

protected:
    IC bool edge_vertex(u32 index);
    IC bool cover(LevelGraph::CVertex* v, u32 index0, u32 index1);
    IC bool critical_point(LevelGraph::CVertex* v, u32 index, u32 index0, u32 index1);
    IC bool critical_cover(u32 index);

private:
    template <typename _evaluator_type, typename _restrictor_type>
    IC bool inertia(float radius, _evaluator_type& evaluator, const _restrictor_type& restrictor) const;

public:
    CCoverManager();
    virtual ~CCoverManager();
    void compute_static_cover();
    // The same work on a worker. It is reached during the alife phase, and the
    // first thing that can read a cover is an object update — which needs objects,
    // which are spawned after the whole render phase. So the build gets two
    // seconds of wall clock that were already being spent elsewhere, and the join
    // (wait_ready) sits in front of the spawn.
    void compute_static_cover_async();
    void wait_ready();
    IC CPointQuadTree& covers() const;
    IC CPointQuadTree* get_covers();
    void clear();
    template <typename _evaluator_type, typename _restrictor_type>
    IC const CCoverPoint* best_cover(const Fvector& position, float radius, _evaluator_type& evaluator, const _restrictor_type& restrictor) const;
    template <typename _evaluator_type>
    IC const CCoverPoint* best_cover(const Fvector& position, float radius, _evaluator_type& evaluator) const;
    IC bool operator()(const CCoverPoint*) const;
    IC float weight(const CCoverPoint*) const;
    IC void finalize(const CCoverPoint*) const;
};

#include "cover_manager_inline.h"