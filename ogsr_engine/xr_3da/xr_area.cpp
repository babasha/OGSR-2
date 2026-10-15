#include "stdafx.h"
#include "IGame_Level.h"

#include "xr_area.h"
#include "../xrCDB/xrCDB_tiled.h" // PreloadStatic
#include "xr_object.h"
#include "xrLevel.h"
#include "Feel_Sound.h"
#include "x_ray.h"
#include "GameFont.h"
#include "GameMtlLib.h"

using namespace collide;

extern BOOL g_bLoaded;

void IGame_Level::SoundEvent_Register(ref_sound_data_ptr S, float range)
{
    if (!g_bLoaded)
        return;
    if (!S)
        return;
    if (S->g_object && S->g_object->getDestroy())
    {
        S->g_object = nullptr;
        return;
    }
    if (nullptr == S->feedback)
        return;

    ZoneScoped;

    clamp(range, 0.1f, 500.f);

    const CSound_params* p = S->feedback->get_params();
    Fvector snd_position = p->position;
    if (S->feedback->is_2D())
    {
        snd_position.add(Sound->listener_position());
    }

    VERIFY(p && _valid(range));
    range = _min(range, p->max_ai_distance);
    VERIFY(_valid(snd_position));
    VERIFY(_valid(p->max_ai_distance));
    VERIFY(_valid(p->volume));

    // Query objects
    Fvector bb_size = {range, range, range};
    g_SpatialSpace->q_box(snd_ER, 0, STYPE_REACTTOSOUND, snd_position, bb_size);

    // Iterate
    xr_vector<ISpatial*>::iterator it = snd_ER.begin();
    xr_vector<ISpatial*>::iterator end = snd_ER.end();
    for (; it != end; ++it)
    {
        Feel::Sound* L = (*it)->dcast_FeelSound();
        if (nullptr == L)
            continue;
        CObject* CO = (*it)->dcast_CObject();
        VERIFY(CO);
        if (CO->getDestroy())
            continue;

        // Energy and signal
        VERIFY(_valid((*it)->spatial.sphere.P));
        float dist = snd_position.distance_to((*it)->spatial.sphere.P);
        if (dist > p->max_ai_distance)
            continue;
        VERIFY(_valid(dist));
        VERIFY(!fis_zero(p->max_ai_distance), S->handle->file_name());
        float Power = (1.f - dist / p->max_ai_distance) * p->volume;
        VERIFY(_valid(Power));
        if (Power > EPS_S)
        {
            const float occ = Sound->get_occlusion_to((*it)->spatial.sphere.P, snd_position);
            VERIFY(_valid(occ));
            Power *= occ;
            if (Power > EPS_S)
            {
                _esound_delegate D = {L, S, Power, dist, p->max_ai_distance, p->volume * occ};
                snd_Events.push_back(D);
            }
        }
    }
    snd_ER.clear();
}

void IGame_Level::SoundEvent_Dispatch()
{
    ZoneScoped;

    while (!snd_Events.empty())
    {
        _esound_delegate& D = snd_Events.back();
        VERIFY(D.dest && D.source);
        if (D.source->feedback && D.source->g_object)
        {
            // the listener's own range, asked here on the main thread (Register may run on
            // the sound thread, and the answer reads the listener's memory): the falloff
            // stays linear, it just ends sooner
            const float scale = D.dest->feel_sound_range(D.source->g_object, D.source->g_type);
            float power = D.power;
            if (scale < 1.f)
            {
                const float range = D.max_dist * scale;
                power = D.dist < range ? (1.f - D.dist / range) * D.loudness : 0.f;
            }
            if (power > EPS_S)
                D.dest->feel_sound_new(D.source->g_object, D.source->g_type, D.source->g_userdata,
                                       D.source->feedback->is_2D() ? Device.vCameraPosition : D.source->feedback->get_params()->position, power);
        }
        snd_Events.pop_back();
    }
}

// Lain: added
void IGame_Level::SoundEvent_OnDestDestroy(Feel::Sound* obj)
{
    snd_Events.erase(std::remove_if(snd_Events.begin(), snd_Events.end(), [obj](const _esound_delegate& d) { return d.dest == obj; }), snd_Events.end());
}

void __stdcall _sound_event(ref_sound_data_ptr S, float range)
{
    if (g_pGameLevel && S && S->feedback)
        g_pGameLevel->SoundEvent_Register(S, range);
}

//----------------------------------------------------------------------
// Class	: CObjectSpace
// Purpose	: stores space slots
//----------------------------------------------------------------------
CObjectSpace::CObjectSpace()
{
    m_BoundingVolume.invalidate();
}
//----------------------------------------------------------------------
CObjectSpace::~CObjectSpace()
{
    Sound->set_geometry_occ(nullptr);
    Sound->set_geometry_som(nullptr);
    Sound->set_geometry_env(nullptr);
    Sound->set_handler(nullptr);
}
//----------------------------------------------------------------------
int CObjectSpace::GetNearest(xr_vector<ISpatial*>& q_spatial, xr_vector<CObject*>& q_nearest, const Fvector& point, float range, CObject* ignore_object)
{
    ZoneScoped;

    q_spatial.clear();
    // Query objects
    q_nearest.clear();
    Fsphere Q;
    Q.set(point, range);
    Fvector B;
    B.set(range, range, range);
    g_SpatialSpace->q_box(q_spatial, 0, STYPE_COLLIDEABLE, point, B);

    // Iterate
    xr_vector<ISpatial*>::iterator it = q_spatial.begin();
    xr_vector<ISpatial*>::iterator end = q_spatial.end();
    for (; it != end; ++it)
    {
        CObject* O = (*it)->dcast_CObject();
        if (nullptr == O)
            continue;
        if (O == ignore_object)
            continue;
        Fsphere mS = {O->spatial.sphere.P, O->spatial.sphere.R};
        if (Q.intersect(mS))
            q_nearest.push_back(O);
    }

    return q_nearest.size();
}

//----------------------------------------------------------------------
int CObjectSpace::GetNearest(xr_vector<CObject*>& q_nearest, const Fvector& point, float range, CObject* ignore_object)
{
    // 12-10-2026 (Switch sampler, 12-bandit fight): every NPC step on its path (move_along_path) asks this, and a fresh
    // vector here was a malloc + free each time -- newlib's one lock. One per thread, reused (GetNearest does not nest).
    thread_local xr_vector<ISpatial*> r_spatial;
    return (GetNearest(r_spatial, q_nearest, point, range, ignore_object));
}

//----------------------------------------------------------------------
int CObjectSpace::GetNearest(xr_vector<CObject*>& q_nearest, ICollisionForm* obj, float range)
{
    CObject* O = obj->Owner();
    return GetNearest(q_nearest, O->spatial.sphere.P, range + O->spatial.sphere.R, O);
}

//----------------------------------------------------------------------
static void __stdcall build_callback(Fvector* V, const size_t Vcnt, CDB::TRI* T, const size_t Tcnt, void* params) { g_pGameLevel->Load_GameSpecific_CFORM(T, Tcnt); }

// Tiled CDB residency (streaming-world Stage C): stream the OPCODE tree
// (2/3 of level-cform RAM) in 256m tiles instead of holding it whole.
// 0=off (monolith), 1=on, 2=on+periodic stats. Takes effect on level load.
// Default ON since 16-07: verified on Pripyat (61M tris) — load 0.5s vs 2.4s,
// resident 1.3GB vs 3.7GB, zero rebuilds/mismatches; first visit bakes the cache.
int psCDB_Tiles = 1;
int psCDB_TileBudget = 512; // MB of resident tile trees before LRU eviction
int psCDB_TileRadius = 300; // m: prefetch/pin bubble around the camera

void CObjectSpace::UpdateStreaming(const Fvector& focus)
{
    Static.update_streaming(focus, (size_t)psCDB_TileBudget, (float)psCDB_TileRadius, psCDB_Tiles > 1);
}

// 📏08-10-2026 Switch v233: started at the cform cache hit, the preload took the
// SD from the render phase (its textures: 1066 -> 1631 ms) and gave back 841 ms
// of st_client_synchronising -- 0.3 s net. Behind the CPU-bound spawning instead.
void CObjectSpace::PreloadStatic(const Fvector* focus)
{
    if (CDB::TILE_GRID* G = Static.tiled())
        G->preload_all(focus);
}

void CObjectSpace::Load()
{
    CTimer t_read;
    t_read.Start();
    IReader* F = FS.r_open(fsgame::level, fsgame::level_files::level_cform);
    R_ASSERT(F);

    hdrCFORM H;
    F->r(&H, sizeof(hdrCFORM));

    Fvector* verts = (Fvector*)F->pointer();
    CDB::TRI* tris = (CDB::TRI*)(verts + H.vertcount);

    R_ASSERT(CFORM_CURRENT_VERSION == H.version);
    const u32 readMs = t_read.GetElapsed_ms();

    // ------------------------------------------------------------------------
    // CDB disk cache. The OPCODE build is single-threaded, ~50s on 60M-tri
    // levels, and its builder temporaries dominate the load-time RAM peak — so
    // the finished model (incl. the material remap the build callback applied
    // to the tris) is cached under $app_data_root$ and reloaded on later runs.
    // Key: cform payload sample + counts, XOR gamemtl.xr (the callback's input).
    // ------------------------------------------------------------------------
    const u8* payload = (const u8*)verts;
    const size_t payloadLen = (size_t)F->length() - sizeof(hdrCFORM);
    const u32 sample = (u32)std::min<size_t>(payloadLen, 4u << 20);
    u32 crcData = crc32(payload, sample);
    if (payloadLen > sample)
        crcData ^= crc32(payload + payloadLen - sample, sample);
    crcData ^= H.vertcount ^ (H.facecount * 2654435761u) ^ (u32)payloadLen;
    u32 crcMtl = 0;
    if (IReader* M = FS.r_open(fsgame::game_data, "gamemtl.xr")) // GAMEMTL_FILENAME — the build callback remaps tri materials by it
    {
        crcMtl = crc32(M->pointer(), (u32)M->length());
        FS.r_close(M);
    }
    const u64 cacheKey = ((u64)crcData << 32) | crcMtl;

    string_path levelPath{};
    FS.update_path(levelPath, fsgame::level, "");
    string64 cacheName;
    xr_sprintf(cacheName, "cdb_%08x.cdb", crc32(levelPath, (u32)xr_strlen(levelPath)));
    string_path cacheFile;
    FS.update_path(cacheFile, fsgame::app_data_root, cacheName);

    CTimer t_total;
    t_total.Start();

    // Tiled mode first (own cache file — both caches can coexist while A/B-ing)
    if (psCDB_Tiles)
    {
        string64 tiledName;
        xr_sprintf(tiledName, "cdbt_%08x.cdbt", crc32(levelPath, (u32)xr_strlen(levelPath)));
        string_path tiledFile;
        FS.update_path(tiledFile, fsgame::app_data_root, tiledName);

        if (Static.cache_load_tiled(tiledFile, cacheKey))
            Msg("[CDB tiles] HIT '%s' — load %u ms (tile trees lazy) | read %u ms | %u verts, %u tris | resident CDB RAM ~%.1f MB",
                tiledName, t_total.GetElapsed_ms(), readMs, H.vertcount, H.facecount, Static.memory() / (1024.f * 1024.f));
        else if (Static.build_tiled(verts, H.vertcount, tris, H.facecount, H.aabb, build_callback, nullptr, tiledFile, cacheKey))
            Msg("[CDB tiles] MISS — baked '%s' in %u ms | read %u ms | %u verts, %u tris", tiledName, t_total.GetElapsed_ms(), readMs, H.vertcount, H.facecount);
        else
            Msg("![CDB tiles] tiled path failed — falling back to monolith");
    }

    if (!Static.tiled())
    {
        if (Static.cache_load(cacheFile, cacheKey))
        {
            Msg("[CDB cache] HIT '%s' — load %u ms (OPCODE build skipped) | read %u ms | %u verts, %u tris | CDB RAM ~%.1f MB",
                cacheName, t_total.GetElapsed_ms(), readMs, H.vertcount, H.facecount, Static.memory() / (1024.f * 1024.f));
        }
        else
        {
            Static.build(verts, H.vertcount, tris, H.facecount, build_callback);
            const u32 buildMs = t_total.GetElapsed_ms();

            CTimer t_save;
            t_save.Start();
            if (Static.cache_save(cacheFile, cacheKey))
                Msg("[CDB cache] built %u ms, saved '%s' in %u ms | read %u ms | %u verts, %u tris | CDB RAM ~%.1f MB",
                    buildMs, cacheName, t_save.GetElapsed_ms(), readMs, H.vertcount, H.facecount, Static.memory() / (1024.f * 1024.f));
            else
                Msg("![CDB cache] built %u ms, save FAILED '%s'", buildMs, cacheFile);
        }
    }

    m_BoundingVolume.set(H.aabb);
    g_SpatialSpace->initialize(H.aabb);
    g_SpatialSpacePhysic->initialize(H.aabb);

    Sound->set_geometry_occ(&Static);
    {
        // snd_occ_mode 1: what one surface of each material lets through.
        // ⚠Not gamemtl's fSndOcclusionFactor as it stands: the engine never used it and its numbers are not
        // acoustic (08-10-2026 Cordon: concrete 0.60 lets more through than wood 0.50, a tiled floor 0.80). So the
        // kind of material decides, by its name; the gamemtl factors only say "open" (a chain-link fence 1.0, glass
        // objects, phantoms), and so does what you can see through and what you can walk through.
        auto by_name = [](const char* n) -> float {
            if (strstr(n, "tree"))
                return .5f; // a trunk takes part of a path
            if (strstr(n, "bush") || strstr(n, "grass") || strstr(n, "leaf") || strstr(n, "foliage"))
                return .85f;
            if (strstr(n, "cloth") || strstr(n, "tent") || strstr(n, "carpet") || strstr(n, "mattress"))
                return .6f;
            if (strstr(n, "glass"))
                return .35f;
            if (strstr(n, "wood") || strstr(n, "door") || strstr(n, "plank") || strstr(n, "fanera"))
                return .3f;
            if (strstr(n, "metal") || strstr(n, "iron") || strstr(n, "tin") || strstr(n, "jelez"))
                return .2f; // sheet metal: a car body, a shed
            return .08f; // concrete, brick, earth, rock, asphalt, default
        };
        xr_vector<float> t(GMLib.CountMaterial(), 0.f);
        string2048 list{};
        for (u32 i = 0; i < t.size(); ++i)
        {
            SGameMtl* M = GMLib.GetMaterialByIdx(u16(i));
            if (!M)
                continue;
            float v = by_name(M->m_Name.c_str());
            if (M->fSndOcclusionFactor >= .85f)
                v = _max(v, M->fSndOcclusionFactor);
            if (M->fVisTransparencyFactor >= .5f)
                v = _max(v, M->fVisTransparencyFactor);
            if (M->Flags.test(SGameMtl::flPassable))
                v = _max(v, .9f);
            t[i] = _min(v, 1.f);
            string128 one;
            xr_sprintf(one, " %s=%.2f", M->m_Name.c_str(), t[i]);
            if (xr_strlen(list) + xr_strlen(one) < sizeof(list) - 1)
                xr_strcat(list, one);
        }
        Sound->set_occlusion_materials(t.data(), u32(t.size()));
        Msg("[snd] occlusion materials (%u):%s", u32(t.size()), list);
    }
    Sound->set_handler(_sound_event);

    FS.r_close(F);
}

