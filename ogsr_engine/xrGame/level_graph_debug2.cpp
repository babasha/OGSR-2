////////////////////////////////////////////////////////////////////////////
//	Module 		: level_graph_debug2.cpp
//	Created 	: 02.10.2001
//  Modified 	: 11.11.2003
//	Author		: Oles Shihkovtsov, Dmitriy Iassenev
//	Description : Level graph debug functions
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"

#include "level_graph.h"
#include "../xr_3da/CustomHUD.h"
#include "ai_space.h"
#include "HUDManager.h"
#include "game_graph.h"
#include "game_sv_single.h"
#include "CustomMonster.h"
#include "ai/stalker/ai_stalker.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "cover_point.h"
#include "cover_manager.h"
#include "cover_evaluators.h"
#include "alife_simulator.h"
#include "alife_graph_registry.h"
#include "alife_object_registry.h"
#include "game_cl_base.h"
#include "space_restriction_manager.h"
#include "space_restriction.h"
#include "space_restrictor.h"
#include "space_restriction_base.h"
#include "detail_path_manager.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "memory_space.h"
#include "Level.h"
#include "ai_object_location.h"
#include "movement_manager.h"
#include "graph_engine.h"
#include "debug_renderer.h"

void CLevelGraph::draw_nodes()
{
    auto O = smart_cast<CGameObject*>(Level().CurrentEntity());
    if (!O)
        return;

    Fvector POSITION = O->Position();
    POSITION.y += 0.5f;

    // display
    Fvector P = POSITION;

    //	CPosition			Local;
    //	vertex_position		(Local,P);

    u32 ID = O->ai_location().level_vertex_id();

    CGameFont* F = HUD().Font().pFontDI;
    F->SetHeightI(.02f);
    F->OutI(0.f, 0.5f, "%f,%f,%f", VPUSH(P));
    //	float				x,z;
    //	unpack_xz			(Local,x,z);
    //	F->Out				(0.f,0.55f,"%3d,%4d,%3d -> %d",	iFloor(x),iFloor(Local.y()),iFloor(z),u32(ID));

    svector<u32, 128> linked;
    {
        const_iterator i, e;
        begin(ID, i, e);
        for (; i != e; ++i)
            linked.push_back(value(ID, i));
    }

    // render
    float sc = header().cell_size() / 16;
    float st = 0.98f * header().cell_size() / 2;
    float tt = 0.01f;

    Fvector DUP;
    DUP.set(0, 1, 0);

    DRender->OnFrameEnd();
    DRender->SetShader(sh_debug);

    F->SetColor(color_rgba(255, 255, 255, 255));

    //////////////////////////////////////////////////////////////////////////
    Fvector min_position, max_position;
    max_position = min_position = Device.vCameraPosition;
    min_position.sub(30.f);
    max_position.add(30.f);

    CLevelGraph::const_vertex_iterator I, E;
    if (valid_vertex_position(min_position))
        I = std::lower_bound(begin(), end(), vertex_position(min_position).xz(), &vertex::predicate2);
    else
        I = begin();

    if (valid_vertex_position(max_position))
    {
        E = std::upper_bound(begin(), end(), vertex_position(max_position).xz(), &vertex::predicate);
        if (E != end())
            ++E;
    }
    else
        E = end();

    //////////////////////////////////////////////////////////////////////////

    for (; I != E; ++I)
    {
        const CLevelGraph::CVertex& N = *I;
        Fvector PC;
        PC = vertex_position(N);

        u32 Nid = vertex_id(I);

        if (Device.vCameraPosition.distance_to(PC) > 30)
            continue;

        float sr = header().cell_size();
        if (::Render->ViewBase.testSphere_dirty(PC, sr))
        {
            u32 CC = color_xrgb(0, 0, 255);
            u32 CT = color_xrgb(255, 255, 255);
            u32 CH = color_xrgb(0, 128, 0);

            BOOL bHL = FALSE;
            if (Nid == u32(ID))
            {
                bHL = TRUE;
                CT = color_xrgb(0, 255, 0);
            }
            else
            {
                for (u32 t = 0; t < linked.size(); ++t)
                {
                    if (linked[t] == Nid)
                    {
                        bHL = TRUE;
                        CT = CH;
                        break;
                    }
                }
            }
            if (!m_access_mask[Nid])
            {
                CT = color_xrgb(255, 0, 0);
            }

            // unpack plane
            Fplane PL;
            Fvector vNorm;
            pvDecompress(vNorm, N.plane());
            PL.build(PC, vNorm);

            // create vertices
            Fvector v, v1, v2, v3, v4;
            v.set(PC.x - st, PC.y, PC.z - st);
            PL.intersectRayPoint(v, DUP, v1);
            v1.mad(v1, PL.n, tt); // minX,minZ
            v.set(PC.x + st, PC.y, PC.z - st);
            PL.intersectRayPoint(v, DUP, v2);
            v2.mad(v2, PL.n, tt); // maxX,minZ
            v.set(PC.x + st, PC.y, PC.z + st);
            PL.intersectRayPoint(v, DUP, v3);
            v3.mad(v3, PL.n, tt); // maxX,maxZ
            v.set(PC.x - st, PC.y, PC.z + st);
            PL.intersectRayPoint(v, DUP, v4);
            v4.mad(v4, PL.n, tt); // minX,maxZ

            // render quad
            DRender->dbg_DrawTRI(Fidentity, v3, v2, v1, CT);
            DRender->dbg_DrawTRI(Fidentity, v1, v4, v3, CT);

            // render center
            Level().debug_renderer().draw_aabb(PC, sc, sc, sc, CC);

            // render id
            if (bHL)
            {
                Fvector T;
                Fvector4 S;
                T.set(PC);
                T.y += 0.3f;
                Device.mFullTransform.transform(S, T);
                if (S.z < 0)
                    continue;
                if (S.x < -1.f || S.x > 1.f || S.y < -1.f)
                    continue;
                F->SetHeightI(0.05f / _sqrt(_abs(S.w)));
                F->SetColor(0xffffffff);
                F->OutI(S.x, -S.y, "~%d", Nid);
            }
        }
    }
}

void CLevelGraph::draw_restrictions()
{
    CSpaceRestrictionManager::SPACE_RESTRICTIONS::const_iterator I = Level().space_restriction_manager().restrictions().begin();
    CSpaceRestrictionManager::SPACE_RESTRICTIONS::const_iterator E = Level().space_restriction_manager().restrictions().end();

    for (; I != E; ++I)
    {
        if ((*I).second->released())
            continue;
        if (!(*I).second->initialized())
            continue;

        const u8 b = u8(::Random.randI(255));
        const u8 g = u8(::Random.randI(255));
        const u8 r = u8(::Random.randI(255));

        xr_vector<u32>::const_iterator i = (*I).second->border().begin();
        xr_vector<u32>::const_iterator e = (*I).second->border().end();
        for (; i != e; ++i)
        {
            Fvector temp = ai().level_graph().vertex_position(*i);
            temp.y += .1f;
            Level().debug_renderer().draw_aabb(temp, .05f, .05f, .05f, color_xrgb(r, g, b));
        }
    }
}

