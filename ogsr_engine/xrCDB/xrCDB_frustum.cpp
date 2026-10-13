#include "StdAfx.h"

#include "xrCDB.h"
#include "xrCDB_tiled.h"
#include "Frustum.h"

using namespace CDB;
using namespace Opcode;

template <bool bClass3, bool bFirst>
class frustum_collider
{
public:
    COLLIDER* dest;
    TRI* tris;
    Fvector* verts;

    const CFrustum* F;

    IC void _init(COLLIDER* CL, Fvector* V, TRI* T, const CFrustum* _F)
    {
        dest = CL;
        tris = T;
        verts = V;
        F = _F;
    }
    IC EFC_Visible _box(Fvector& C, Fvector& E, u32& mask)
    {
        Fvector mM[2];
        mM[0].sub(C, E);
        mM[1].add(C, E);
        return F->testAABB(&mM[0].x, mask);
    }
    void _prim(DWORD prim)
    {
        if (bClass3)
        {
            sPoly src, dst;
            src.resize(3);
            src[0] = verts[tris[prim].verts[0]];
            src[1] = verts[tris[prim].verts[1]];
            src[2] = verts[tris[prim].verts[2]];
            if (F->ClipPoly(src, dst))
            {
                RESULT& R = dest->r_add();
                R.id = prim;
                R.verts[0] = verts[tris[prim].verts[0]];
                R.verts[1] = verts[tris[prim].verts[1]];
                R.verts[2] = verts[tris[prim].verts[2]];
                R.dummy = tris[prim].dummy;
            }
        }
        else
        {
            RESULT& R = dest->r_add();
            R.id = prim;
            R.verts[0] = verts[tris[prim].verts[0]];
            R.verts[1] = verts[tris[prim].verts[1]];
            R.verts[2] = verts[tris[prim].verts[2]];
            R.dummy = tris[prim].dummy;
        }
    }

    void _stab(const AABBNoLeafNode* node, u32 mask)
    {
        // Actual frustum/aabb test
        EFC_Visible result = _box((Fvector&)node->mAABB.mCenter, (Fvector&)node->mAABB.mExtents, mask);
        if (fcvNone == result)
            return;

        // 1st chield
        if (node->HasPosLeaf())
            _prim(node->GetPosPrimitive());
        else
            _stab(node->GetPos(), mask);

        // Early exit for "only first"
        if (bFirst && dest->r_count())
            return;

        // 2nd chield
        if (node->HasNegLeaf())
            _prim(node->GetNegPrimitive());
        else
            _stab(node->GetNeg(), mask);
    }
};

// Tiled routing: cull whole tiles against the frustum by their exact geometry
// bounds (the grid is at most a few thousand cells), stab the survivors.
template <bool bClass3, bool bFirst>
static void frustum_query_tiled(COLLIDER* CL, TILE_GRID& G, Fvector* verts, TRI* tris, const CFrustum& F)
{
    frustum_collider<bClass3, bFirst> BC;
    BC._init(CL, verts, tris, &F);

    u32 stabbed = 0;
    for (u32 iz = 0; iz < G.nz; ++iz)
        for (u32 ix = 0; ix < G.nx; ++ix)
        {
            TILE& t = G.cell(ix, iz);
            if (!t.file_nodes)
                continue;
            u32 mask = F.getMask();
            Fvector mM[2] = {t.bb_min, t.bb_max};
            if (fcvNone == F.testAABB(&mM[0].x, mask))
                continue;
            with_tile(G, t, [&](const AABBNoLeafNode* nodes) { BC._stab(nodes, mask); });
            ++stabbed;
            if (bFirst && CL->r_count())
                return;
        }
    if (!bFirst && stabbed > 1)
        CL->r_dedup_by_id(); // boundary tris live in 2+ tiles
}

void COLLIDER::frustum_query(u32 frustum_mode, const MODEL* m_def, const CFrustum& F)
{
    ZoneScoped;

    m_def->syncronize();

    if (TILE_GRID* G = m_def->tiled())
    {
        r_clear();
        if (frustum_mode & OPT_FULL_TEST)
        {
            if (frustum_mode & OPT_ONLYFIRST)
                frustum_query_tiled<true, true>(this, *G, m_def->verts, m_def->tris, F);
            else
                frustum_query_tiled<true, false>(this, *G, m_def->verts, m_def->tris, F);
        }
        else
        {
            if (frustum_mode & OPT_ONLYFIRST)
                frustum_query_tiled<false, true>(this, *G, m_def->verts, m_def->tris, F);
            else
                frustum_query_tiled<false, false>(this, *G, m_def->verts, m_def->tris, F);
        }
        return;
    }

    // Get nodes
    const AABBNoLeafTree* T = (const AABBNoLeafTree*)m_def->tree->GetTree();
    const AABBNoLeafNode* N = T->GetNodes();
    const DWORD mask = F.getMask();
    r_clear();

    // Binary dispatcher
    if (frustum_mode & OPT_FULL_TEST)
    {
        if (frustum_mode & OPT_ONLYFIRST)
        {
            frustum_collider<true, true> BC;
            BC._init(this, m_def->verts, m_def->tris, &F);
            BC._stab(N, mask);
        }
        else
        {
            frustum_collider<true, false> BC;
            BC._init(this, m_def->verts, m_def->tris, &F);
            BC._stab(N, mask);
        }
    }
    else
    {
        if (frustum_mode & OPT_ONLYFIRST)
        {
            frustum_collider<false, true> BC;
            BC._init(this, m_def->verts, m_def->tris, &F);
            BC._stab(N, mask);
        }
        else
        {
            frustum_collider<false, false> BC;
            BC._init(this, m_def->verts, m_def->tris, &F);
            BC._stab(N, mask);
        }
    }
}
