#pragma once

#include "xr_collide_form.h"
#include "../xrCDB/xr_collide_defs.h"

// refs
class ENGINE_API ISpatial;
class ENGINE_API ICollisionForm;
class ENGINE_API CObject;

//-----------------------------------------------------------------------------------------------------------
// Space Area
//-----------------------------------------------------------------------------------------------------------
class ENGINE_API CObjectSpace
{
private:
    // Debug
    CDB::MODEL Static;
    Fbox m_BoundingVolume;
public:

private:
    BOOL _RayTest(const Fvector& start, const Fvector& dir, float range, collide::rq_target tgt, collide::ray_cache* cache, const CObject* ignore_object);
    BOOL _RayPick(const Fvector& start, const Fvector& dir, float range, collide::rq_target tgt, collide::rq_result& R, const CObject* ignore_object) const;
    BOOL _RayQuery(collide::rq_results& dest, const collide::ray_defs& rq, collide::rq_callback* cb, LPVOID user_data, collide::test_callback* tb, const CObject* ignore_object) const;

public:
    CObjectSpace();
    ~CObjectSpace();

    void Load();

    // Tiled-CDB residency tick (no-op when cdb_tiles is off): prefetch the
    // bubble around `focus`, evict LRU tiles beyond the budget.
    void UpdateStreaming(const Fvector& focus);
    // Tiled CDB: read a small level's whole tree on a background thread (cdb_tile_preload_mb).
    // Called when the client starts spawning: before that the render phase reads its textures from the same SD.
    void PreloadStatic(const Fvector* focus); // focus: the player's position if known, its tiles first

    // Occluded/No
    BOOL RayTest(const Fvector& start, const Fvector& dir, float range, collide::rq_target tgt, collide::ray_cache* cache, const CObject* ignore_object);

    // Game raypick (nearest) - returns object and addititional params
    BOOL RayPick(const Fvector& start, const Fvector& dir, float range, collide::rq_target tgt, collide::rq_result& R, const CObject* ignore_object) const;

    // General collision query
    BOOL RayQuery(collide::rq_results& dest, const collide::ray_defs& rq, collide::rq_callback* cb, LPVOID user_data, collide::test_callback* tb, const CObject* ignore_object) const;

    bool BoxQuery(Fvector const& box_center, Fvector const& box_z_axis, Fvector const& box_y_axis, Fvector const& box_sizes, xr_vector<Fvector>* out_tris) const;

    int GetNearest(xr_vector<CObject*>& q_nearest, ICollisionForm* obj, float range);
    int GetNearest(xr_vector<CObject*>& q_nearest, const Fvector& point, float range, CObject* ignore_object);
    int GetNearest(xr_vector<ISpatial*>& q_spatial, xr_vector<CObject*>& q_nearest, const Fvector& point, float range, CObject* ignore_object);

    CDB::TRI* GetStaticTris() { return Static.get_tris(); }
    Fvector* GetStaticVerts() { return Static.get_verts(); }
    CDB::MODEL* GetStaticModel() { return &Static; }

    const Fbox& GetBoundingVolume() const { return m_BoundingVolume; }
};
