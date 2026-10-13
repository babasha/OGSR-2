#pragma once

// ============================================================================
// Tiled static-collision residency (streaming-world Stage C).
//
// On big levels the OPCODE no-leaf tree is ~40B/tri — 2/3 of the whole
// level-cform RAM (2.4GB of 3.7GB on 61M tris) — while verts+tris are the
// other third. Only the TREE is worth streaming, and only the tree is SAFE to
// stream: ODE physics caches raw CDB::TRI* across frames and ~30 consumers
// index the global tris/verts arrays by RESULT::id. So verts/tris stay
// resident forever and the tree is split into XZ tiles loaded from a disk
// cache on demand. Tile-tree leaves store GLOBAL tri indices, therefore every
// existing consumer (including RESULT::id semantics and ODE contact caches)
// works untouched; boundary tris are duplicated into every tile they overlap
// and collect-ALL queries dedup by id.
// ============================================================================

#include "xrCDB.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <cstdio>

namespace Opcode
{
class AABBNoLeafNode;
};

namespace CDB
{
struct TILE
{
    enum : u32
    {
        S_EMPTY = 0, // not resident (never loaded, or evicted)
        S_READY = 1, // nodes valid
    };

    Opcode::AABBNoLeafNode* nodes{};
    u32 nb_nodes{};

    // Exact 3D bounds of the tris binned into this tile (resident from the
    // directory). Pre-testing the query against them lets a ray that merely
    // crosses the tile's XZ column (e.g. a sun ray high above the roofs) skip
    // the tile WITHOUT forcing it resident.
    Fvector bb_min{}, bb_max{};

    u64 file_offset{};
    u32 file_nodes{}; // node count of the cache blob; 0 = tile has no geometry

    std::atomic<u32> state{S_EMPTY};
    std::atomic<u32> last_use{0}; // TILE_GRID::tick stamp
    std::shared_mutex lock; // shared: traversal; exclusive: load/evict
};

class XRCDB_API TILE_GRID : Noncopyable
{
public:
    // grid params — immutable after init
    float origin_x{}, origin_z{};
    float tile_size{}, inv_tile{};
    u32 nx{}, nz{};

    std::unique_ptr<TILE[]> tiles;

    // resident arrays of the owning MODEL (leaf prim ids are GLOBAL indices here)
    const Fvector* verts{};
    const TRI* tris{};
    size_t verts_count{}, tris_count{};

    // streaming source; null = no cache file (bake-without-save) → everything
    // stays resident and eviction is disabled
    FILE* file{};
    std::mutex io_lock;

    std::atomic<size_t> resident_bytes{0};
    std::atomic<u32> tick{1}; // advanced by update_streaming; last_use stamps
    std::atomic<bool> prefetch_inflight{false};

    // telemetry
    std::atomic<u32> stat_loads{0}, stat_sync_loads{0}, stat_evictions{0}, stat_rebuilds{0};

    ~TILE_GRID();

    IC TILE& cell(u32 ix, u32 iz) { return tiles[(size_t)iz * nx + ix]; }
    IC int cell_x(float x) const { return (int)std::floor((x - origin_x) * inv_tile); }
    IC int cell_z(float z) const { return (int)std::floor((z - origin_z) * inv_tile); }
    IC void touch(TILE& t) { t.last_use.store(tick.load(std::memory_order_relaxed), std::memory_order_relaxed); }

    // Synchronous load from the cache file (rebuild-from-resident-tris on a
    // corrupt blob). false = tile unusable (also for file_nodes==0).
    // sync=false marks the load as prefetch in the telemetry.
    bool make_ready(TILE& t, bool sync = true);

    // Per-frame: prefetch tiles inside the bubble around `focus` (async, one
    // task in flight) + evict LRU tiles outside it while over budget.
    void update_streaming(const Fvector& focus, size_t budget_bytes, float bubble_radius, BOOL verbose);

    // The whole tree on a background thread, if it fits in g_cdb_tile_preload_mb
    // (a small level read at once, during the load, instead of tile by tile on
    // the first query in each). Holds prefetch_inflight while it runs.
    // focus: the player's position, if known -- the tiles nearest to it first (the stalkers coming online are there).
    void preload_all(const Fvector* focus);

private:
    bool load_tile(TILE& t); // caller holds exclusive lock
    bool rebuild_tile(TILE& t); // caller holds exclusive lock
    friend class MODEL;
};

// Traversal acquire: shared-lock the tile, make it resident if needed, run fn
// on the root node. The shared lock pins the node array against eviction for
// the duration of the stab.
template <class F>
IC void with_tile(TILE_GRID& G, TILE& t, F&& fn)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        {
            std::shared_lock<std::shared_mutex> sl(t.lock);
            if (t.state.load(std::memory_order_acquire) == TILE::S_READY)
            {
                G.touch(t);
                fn(t.nodes);
                return;
            }
        }
        if (!G.make_ready(t))
            return; // no geometry / unrecoverable — skip tile
    }
}

}; // namespace CDB
