#pragma once

#define XRCDB_API

class CFrustum;
namespace Opcode
{
class Model;
};

namespace CDB
{
class TILE_GRID;

// Triangle
class alignas(16) XRCDB_API TRI //*** 16 bytes total (was 32 :)
{
public:
    u32 verts[3]; // 3*4 = 12b
    union
    {
        u32 dummy; // 4b
        struct
        {
            u32 material : 14; //
            u32 suppress_shadows : 1; //
            u32 suppress_wm : 1; //
            u32 sector : 16; //
        };
    };

public:
    TRI() = default;
};
static_assert(sizeof(TRI) == 16);

// Build callback
using build_callback = void(Fvector* V, const size_t Vcnt, TRI* T, const size_t Tcnt, void* params);

// Model definition
// Releases a view handed out by cache_load_tiled (defined in xrCDB_tiled.cpp):
// joins the warm-up walking it, then unmaps.
void UnmapStorageView(void* view);

class XRCDB_API MODEL : Noncopyable
{
    friend class COLLIDER;
    enum : u32
    {
        S_INIT = 0,
        S_BUILD = 1,
        S_READY = 2
    };

private:
    Opcode::Model* tree{};
    TILE_GRID* tiles_{}; // tiled residency mode (streaming-world Stage C); tree==null then
    std::atomic<u32> status{S_INIT}; // 0=init, 1=build, 2=ready

    // tris
    TRI* tris{};
    size_t tris_count{};
    Fvector* verts{};
    size_t verts_count{};

    // When the tiled cache is loaded, verts/tris are not copies: they point into a
    // mapped view of the cache file (see cache_load_tiled). Non-null means the
    // teardown unmaps instead of freeing, and that nothing may write to them.
    void* storage_view{};

public:
    MODEL() = default;
    ~MODEL();

    IC Fvector* get_verts() { return verts; }
    IC const Fvector* get_verts() const { return verts; }
    IC size_t get_verts_count() const { return verts_count; }
    IC TRI* get_tris() { return tris; }
    IC const TRI* get_tris() const { return tris; }
    IC size_t get_tris_count() const { return tris_count; }
    IC void syncronize() const
    {
        if (S_READY != status)
            Log("! WARNING: syncronized CDB::query");

        while (S_READY != status)
            YieldProcessor();
    }

    void build(const Fvector* V, const size_t Vcnt, const TRI* T, const size_t Tcnt, build_callback* bc = nullptr, void* bcp = nullptr);
    u32 memory();

    // Disk cache of the BUILT model (verts + tris + OPCODE no-leaf tree). The OPCODE
    // builder is single-threaded and O(minutes) on 60M-tri levels with a multi-GB
    // temporary tree — caching the finished product skips all of it. `key` must fold
    // in everything the built data depends on (cform bytes AND whatever the build
    // callback consumed — it already ran on the cached tris, it is NOT re-run on load).
    // cache_load: on success the model is READY and build() must not be called.
    bool cache_load(const char* path, u64 key);
    bool cache_save(const char* path, u64 key); // call after build(); best-effort

    // Tiled residency mode (see xrCDB_tiled.h): verts/tris resident, OPCODE
    // tree split into XZ tiles streamed from `cache_path`. Same key contract
    // as cache_load/cache_save. build_tiled bakes + saves + adopts (false =
    // caller should fall back to the monolithic build); cache_load_tiled
    // loads header/verts/tris/directory, tile trees stay lazy.
    IC TILE_GRID* tiled() const { return tiles_; }
    bool build_tiled(const Fvector* V, size_t Vcnt, const TRI* T, size_t Tcnt, const Fbox& aabb, build_callback* bc, void* bcp, const char* cache_path, u64 key);
    bool cache_load_tiled(const char* cache_path, u64 key);
    void update_streaming(const Fvector& focus, size_t budget_mb, float bubble_radius, BOOL verbose); // no-op unless tiled

private:

    void build_internal(const Fvector* V, const size_t Vcnt, const TRI* T, const size_t Tcnt, build_callback* bc = nullptr, void* bcp = nullptr);
};

// Collider result
struct XRCDB_API alignas(16) RESULT
{
    Fvector verts[3];
    union
    {
        u32 dummy; // 4b
        struct
        {
            u32 material : 14; //
            u32 suppress_shadows : 1; //
            u32 suppress_wm : 1; //
            u32 sector : 16; //
        };
    };
    int id;
    float range;
    float u, v;
    u64 pad;
};
static_assert(sizeof(RESULT) == 64);

// Collider Options
enum
{
    OPT_CULL = (1 << 0),
    OPT_ONLYFIRST = (1 << 1),
    OPT_ONLYNEAREST = (1 << 2),
    OPT_FULL_TEST = (1 << 3) // for box & frustum queries - enable class III test(s)
};

// Collider itself
class XRCDB_API COLLIDER
{
    // Result management
    xr_vector<RESULT> rd;

public:
    COLLIDER() = default;
    ~COLLIDER() = default;

    void ray_query(u32 ray_mode, const MODEL* m_def, const Fvector& r_start, const Fvector& r_dir, float r_range = 10000.f);
    void box_query(u32 box_mode, const MODEL* m_def, const Fvector& b_center, const Fvector& b_dim);
    void frustum_query(u32 frustum_mode, const MODEL* m_def, const CFrustum& F);

    ICF RESULT* r_begin() { return std::data(rd); }
    ICF RESULT* r_end() { return std::data(rd) + std::size(rd); }
    RESULT& r_add();
    void r_dedup_by_id(); // tiled queries: a boundary tri lives in 2+ tiles and would double contacts
    ICF size_t r_count() { return rd.size(); };
    ICF void r_clear() { rd.clear(); };
};

class XRCDB_API Collector
{
    xr_vector<Fvector> verts;
    xr_vector<TRI> faces;

    u32 VPack(const Fvector& V, float eps);

public:
    void add_face_packed_D(const Fvector& v0, const Fvector& v1, const Fvector& v2, u32 dummy, float eps = EPS);

    void calc_adjacency(xr_vector<u32>& dest) const;

    const Fvector* getV() const { return verts.data(); }
    size_t getVS() const { return verts.size(); }
    TRI* getT() { return faces.data(); }
    size_t getTS() const { return faces.size(); }
    void clear()
    {
        verts.clear();
        faces.clear();
    }
};

}; // namespace CDB
