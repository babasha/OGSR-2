#include "StdAfx.h"

#include "xrCDB_tiled.h"
#include "../xrCore/pinned_worker.h" // the tile preload thread (core 2 on the Switch)

#include <cstdio>
#include <algorithm>
#include <future>
#include <thread>
#include <chrono>

using namespace CDB;
using namespace Opcode;

// cdb_cache_read_threads (registered in xr_ioc_cmd.cpp). The cache hit reads ~1.3
// GB of verts and tris; one fread does it at ~5 GB/s with fifteen cores idle.
// 0 = map the file instead of copying it (the default); >=1 = copy it on that
// many readers, which is what the measurement compares against.
int g_cdb_cache_read_threads = 0;

// cdb_tile_preload_mb (xr_ioc_cmd.cpp): a level whose whole tile tree fits in
// this many MB reads it on a background thread right after the cache hit.
// 📏08-10-2026 Switch v232: Cordon (16 tiles, 53 MB) was read on DEMAND during
// the save load -- the first collision query in each tile (a stalker's
// activation box coming online) read 2-8 MB from the SD synchronously, 99-145 ms
// each, on the loading path. 0 = off (demand loads only, as before).
int g_cdb_tile_preload_mb = 256;

// ============================================================================
// Cache file: header | verts | tris | directory | per-tile node blobs.
// Node links use the same disk encoding as the monolithic CDB cache
// (index<<1 relative to the TILE-local node array; leaf payloads are
// (globalPrim<<1)|1 and stay verbatim — they are position-independent).
// ============================================================================
namespace
{
constexpr u32 kTiledCacheMagic = 0x54444358u; // 'XCDT'
constexpr u32 kTiledCacheVersion = 1;
constexpr float kTileSize = 256.f;
constexpr u32 kMaxTiles = 65536;
constexpr u32 kEvictProtectTicks = 300; // ~5s at 60fps: recently-touched tiles are hot even outside the bubble

#pragma pack(push, 4)
struct TiledCacheHeader
{
    u32 magic, version;
    u64 key; // caller-provided validity stamp
    u64 verts_count, tris_count;
    float origin_x, origin_z, tile_size;
    u32 nx, nz;
};
struct TiledDirEntry
{
    u64 offset;
    u32 nb_nodes;
    u32 _pad;
    Fvector bb_min, bb_max;
};
#pragma pack(pop)

inline size_t encode_link(size_t v, const AABBNoLeafNode* base)
{
    if (v & 1)
        return v;
    return (size_t)((const AABBNoLeafNode*)v - base) << 1;
}
inline size_t decode_link(size_t v, const AABBNoLeafNode* base)
{
    if (v & 1)
        return v;
    return (size_t)(base + (v >> 1));
}

bool write_all(FILE* f, const void* data, size_t bytes)
{
    const u8* p = (const u8*)data;
    while (bytes)
    {
        const size_t chunk = std::min<size_t>(bytes, 64u << 20);
        if (fwrite(p, 1, chunk, f) != chunk)
            return false;
        p += chunk;
        bytes -= chunk;
    }
    return true;
}

// Vertices and triangles are the two big blocks in the cache: 34M verts and 61M
// tris on pripyat_full, ~1.3 GB, and one fread pulls them in at ~5 GB/s while
// fifteen cores wait. They are contiguous on disk and the destination is a fresh
// allocation, so a split is just N handles seeking to N offsets -- and it also
// spreads the first-touch faults on the destination, which are half the cost.
bool read_range(const char* path, u64 offset, void* dst, size_t bytes)
{
    FILE* f = fopen(XR_OSPATH(path), "rb");
    if (!f)
        return false;
    bool ok = (0 == _fseeki64(f, (s64)offset, SEEK_SET));
    u8* p = (u8*)dst;
    while (ok && bytes)
    {
        const size_t chunk = std::min<size_t>(bytes, 64u << 20);
        ok = (fread(p, 1, chunk, f) == chunk);
        p += chunk;
        bytes -= chunk;
    }
    fclose(f);
    return ok;
}

bool read_parallel(const char* path, u64 offset, void* dst, size_t bytes, u32 threads)
{
    if (threads <= 1 || bytes < (32u << 20))
        return read_range(path, offset, dst, bytes);

    const size_t grain = (bytes + threads - 1) / threads;
    xr_vector<std::thread> pool;
    std::atomic<bool> ok{true};
    pool.reserve(threads);
    for (u32 i = 1; i < threads; ++i)
    {
        const size_t lo = std::min<size_t>(bytes, (size_t)i * grain);
        const size_t hi = std::min<size_t>(bytes, lo + grain);
        if (lo >= hi)
            break;
        pool.emplace_back([&, lo, hi] {
            if (!read_range(path, offset + lo, (u8*)dst + lo, hi - lo))
                ok.store(false, std::memory_order_relaxed);
        });
    }
    if (!read_range(path, offset, dst, std::min<size_t>(bytes, grain)))
        ok.store(false, std::memory_order_relaxed);
    for (auto& t : pool)
        t.join();
    return ok.load(std::memory_order_relaxed);
}

// The cache's verts and tris do not have to be COPIED into the process at all:
// they are 1.3 GB of read-only data sitting contiguously in a file, which is what
// a mapped view is for. The load then costs three syscalls instead of 1.3 GB of
// reading, and the pages arrive on a warm-up thread that nobody waits for.
// (Nothing writes to them after the bake — the material remap the build callback
// applies is baked INTO the cache file.)
std::thread s_warmer;
std::atomic<u32> s_warmSink{0};

void* MapCacheStorage(const char* path, u64 warmFrom, u64 warmBytes)
{
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return nullptr;
    HANDLE map = CreateFileMapping(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!map)
    {
        CloseHandle(file);
        return nullptr;
    }
    // Only the header + verts + tris: the tile trees after them are read per tile
    // through the FILE*. 📏08-10-2026 Switch v234: mmap there is a read of the
    // whole range, and a whole-file view read Cordon's 55 MB of tile trees at
    // st_loading_cform (85 MB in 1279 ms) only for the tiles to read them again.
    void* view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, size_t(warmFrom + warmBytes));
    CloseHandle(map);    // the view keeps its own reference
    CloseHandle(file);
    if (!view)
        return nullptr;

    // Fault the collision data in behind the load instead of during it. A query
    // that arrives first still works — it just faults its own page.
    const u8* base = (const u8*)view;
    s_warmer = std::thread([base, warmFrom, warmBytes] {
        u32 sink = 0;
        for (u64 o = warmFrom; o < warmFrom + warmBytes; o += 4096)
            sink += base[o];
        s_warmSink.fetch_add(sink, std::memory_order_relaxed);
    });
    return view;
}

bool read_all(FILE* f, void* data, size_t bytes)
{
    u8* p = (u8*)data;
    while (bytes)
    {
        const size_t chunk = std::min<size_t>(bytes, 64u << 20);
        if (fread(p, 1, chunk, f) != chunk)
            return false;
        p += chunk;
        bytes -= chunk;
    }
    return true;
}

// Build one tile's OPCODE tree over the given GLOBAL tri ids and leave the
// stolen node array (leaf prims rewritten to global ids) in the tile.
bool build_tile_from_ids(TILE_GRID& G, TILE& t, const xr_vector<u32>& ids)
{
    const size_t n0 = ids.size();
    if (!n0)
    {
        t.file_nodes = 0;
        return true;
    }

    // OPCODE needs >=2 tris for a real tree; a 1-tri tile duplicates it (two
    // leaves, same global id — identical to a boundary-dup, deduped the same)
    const size_t n = std::max<size_t>(n0, 2);
    xr_vector<TRI> local(n);
    Fvector bb_min{type_max<float>, type_max<float>, type_max<float>};
    Fvector bb_max{type_min<float>, type_min<float>, type_min<float>};
    for (size_t i = 0; i < n; ++i)
    {
        const TRI& src = G.tris[ids[std::min(i, n0 - 1)]];
        local[i] = src;
        for (u32 k = 0; k < 3; ++k)
        {
            const Fvector& v = G.verts[src.verts[k]];
            bb_min.min(v);
            bb_max.max(v);
        }
    }

    MeshInterface mif;
    mif.SetNbTriangles((udword)n);
    mif.SetNbVertices((udword)G.verts_count);
    mif.SetPointers(reinterpret_cast<const IceMaths::IndexedTriangle*>(local.data()), reinterpret_cast<const IceMaths::Point*>(G.verts));
    mif.SetStrides(sizeof(TRI));

    OPCODECREATE OPCC;
    OPCC.mIMesh = &mif;
    OPCC.mNoLeaf = true;
    OPCC.mQuantized = false;

    Opcode::Model tree;
    if (!tree.Build(OPCC))
        return false;
    if (tree.IsQuantized() || tree.HasLeafNodes() || tree.HasSingleNode() || !tree.GetTree())
        return false;

    AABBNoLeafTree* T = (AABBNoLeafTree*)tree.GetTree();
    const u32 nb = T->GetNbNodes();
    AABBNoLeafNode* nodes = (AABBNoLeafNode*)T->GetData();
    if (!nodes || !nb)
        return false;

    // leaf prims: local -> global
    auto remap = [&](size_t& v) {
        if (v & 1)
            v = ((size_t)ids[std::min<size_t>(v >> 1, n0 - 1)] << 1) | 1;
    };
    for (u32 i = 0; i < nb; ++i)
    {
        remap(nodes[i].mPosData);
        remap(nodes[i].mNegData);
    }

    T->SetData(nullptr, 0); // steal ownership; ~Model deletes null

    t.nodes = nodes;
    t.nb_nodes = nb;
    t.file_nodes = nb;
    t.bb_min = bb_min;
    t.bb_max = bb_max;
    t.state.store(TILE::S_READY, std::memory_order_release);
    G.resident_bytes.fetch_add((size_t)nb * sizeof(AABBNoLeafNode), std::memory_order_relaxed);
    return true;
}

bool save_tiled(TILE_GRID& G, const char* path, u64 key)
{
    static_assert(sizeof(AABBNoLeafNode) == 40, "node layout changed — bump kTiledCacheVersion and re-check encode_link");

    string_path tmp;
    xr_sprintf(tmp, "%s.tmp", path);
    FILE* f = fopen(XR_OSPATH(tmp), "wb");
    if (!f)
        return false;

    const u32 nTiles = G.nx * G.nz;
    TiledCacheHeader h{kTiledCacheMagic, kTiledCacheVersion, key, (u64)G.verts_count, (u64)G.tris_count, G.origin_x, G.origin_z, G.tile_size, G.nx, G.nz};
    xr_vector<TiledDirEntry> dir(nTiles);

    bool ok = write_all(f, &h, sizeof(h)) && write_all(f, G.verts, G.verts_count * sizeof(Fvector)) && write_all(f, G.tris, G.tris_count * sizeof(TRI));
    const s64 dirPos = ok ? _ftelli64(f) : 0;
    ok = ok && write_all(f, dir.data(), nTiles * sizeof(TiledDirEntry)); // placeholder

    constexpr u32 kBatch = 65536;
    xr_vector<AABBNoLeafNode> scratch(kBatch);
    for (u32 i = 0; ok && i < nTiles; ++i)
    {
        TILE& t = G.tiles[i];
        dir[i].nb_nodes = t.file_nodes;
        dir[i].bb_min = t.bb_min;
        dir[i].bb_max = t.bb_max;
        if (!t.file_nodes)
            continue;
        dir[i].offset = (u64)_ftelli64(f);
        for (u32 j = 0; ok && j < t.nb_nodes; j += kBatch)
        {
            const u32 c = std::min(kBatch, t.nb_nodes - j);
            std::memcpy(scratch.data(), t.nodes + j, c * sizeof(AABBNoLeafNode));
            for (u32 k = 0; k < c; ++k)
            {
                scratch[k].mPosData = encode_link(scratch[k].mPosData, t.nodes);
                scratch[k].mNegData = encode_link(scratch[k].mNegData, t.nodes);
            }
            ok = write_all(f, scratch.data(), c * sizeof(AABBNoLeafNode));
        }
    }

    if (ok)
    {
        ok = 0 == _fseeki64(f, dirPos, SEEK_SET) && write_all(f, dir.data(), nTiles * sizeof(TiledDirEntry));
        // runtime tile offsets come straight from what we just wrote
        for (u32 i = 0; ok && i < nTiles; ++i)
            G.tiles[i].file_offset = dir[i].offset;
    }

    fclose(f);
    if (!ok)
    {
        remove(XR_OSPATH(tmp));
        return false;
    }
    remove(XR_OSPATH(path)); // rename() refuses to overwrite
    if (0 != rename(XR_OSPATH(tmp), XR_OSPATH(path)))
    {
        remove(XR_OSPATH(tmp));
        return false;
    }
    return true;
}

// nodes blob -> live pointers, with bounds sanity (a corrupt cache must fail
// the load, not stab random memory during the first query)
bool fixup_nodes(AABBNoLeafNode* nodes, u32 nb, size_t tris_count)
{
    for (u32 i = 0; i < nb; ++i)
    {
        auto fix = [&](size_t& v) -> bool {
            if (v & 1)
                return (v >> 1) < tris_count;
            if ((v >> 1) >= nb)
                return false;
            v = decode_link(v, nodes);
            return true;
        };
        if (!fix(nodes[i].mPosData) || !fix(nodes[i].mNegData))
            return false;
    }
    return true;
}
} // namespace

// ============================================================================
// TILE_GRID
// ============================================================================

TILE_GRID::~TILE_GRID()
{
    while (prefetch_inflight.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (file)
        fclose(file);
    if (tiles)
        for (u32 i = 0, n = nx * nz; i < n; ++i)
            delete[] tiles[i].nodes;
}

bool TILE_GRID::load_tile(TILE& t)
{
    if (!file)
        return false;

    CTimer timer;
    timer.Start();

    AABBNoLeafNode* nodes = new AABBNoLeafNode[t.file_nodes];
    bool ok;
    {
        std::lock_guard<std::mutex> io(io_lock);
        ok = 0 == _fseeki64(file, (s64)t.file_offset, SEEK_SET) && read_all(file, nodes, (size_t)t.file_nodes * sizeof(AABBNoLeafNode));
    }
    ok = ok && fixup_nodes(nodes, t.file_nodes, tris_count);
    if (!ok)
    {
        delete[] nodes;
        return false;
    }

    t.nodes = nodes;
    t.nb_nodes = t.file_nodes;
    t.state.store(TILE::S_READY, std::memory_order_release);
    resident_bytes.fetch_add((size_t)t.nb_nodes * sizeof(AABBNoLeafNode), std::memory_order_relaxed);
    touch(t);

    const u32 n = stat_loads.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) == 0)
        Msg("[CDB tiles] load #%u: %u nodes (%.1f MB) in %u ms | resident %.1f MB", n, t.nb_nodes, t.nb_nodes * sizeof(AABBNoLeafNode) / (1024.f * 1024.f),
            timer.GetElapsed_ms(), resident_bytes.load(std::memory_order_relaxed) / (1024.f * 1024.f));
    return true;
}

bool TILE_GRID::rebuild_tile(TILE& t)
{
    // Corrupt/unreadable blob: the source of truth (verts+tris) is resident,
    // so the tile tree can always be rebuilt in memory. Slow (full tri scan +
    // OPCODE build) but correctness-preserving.
    const size_t idx = (size_t)(&t - tiles.get());
    const u32 ix = (u32)(idx % nx), iz = (u32)(idx / nx);
    const float x0 = origin_x + ix * tile_size, x1 = x0 + tile_size;
    const float z0 = origin_z + iz * tile_size, z1 = z0 + tile_size;

    xr_vector<u32> ids;
    ids.reserve(4096);
    for (u32 i = 0; i < (u32)tris_count; ++i)
    {
        const TRI& T = tris[i];
        float tx0 = type_max<float>, tx1 = type_min<float>, tz0 = type_max<float>, tz1 = type_min<float>;
        for (u32 k = 0; k < 3; ++k)
        {
            const Fvector& v = verts[T.verts[k]];
            tx0 = std::min(tx0, v.x);
            tx1 = std::max(tx1, v.x);
            tz0 = std::min(tz0, v.z);
            tz1 = std::max(tz1, v.z);
        }
        if (tx1 >= x0 && tx0 < x1 && tz1 >= z0 && tz0 < z1)
            ids.push_back(i);
    }

    if (ids.empty())
        return false; // file_nodes>0 promised geometry — directory is corrupt too

    const u32 keep_file_nodes = t.file_nodes; // rebuilt node count may differ from the blob's
    if (!build_tile_from_ids(*this, t, ids))
        return false;
    t.file_nodes = keep_file_nodes;
    stat_rebuilds.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool TILE_GRID::make_ready(TILE& t, bool sync)
{
    if (!t.file_nodes)
        return false;

    std::unique_lock<std::shared_mutex> ul(t.lock);
    if (t.state.load(std::memory_order_relaxed) == TILE::S_READY)
        return true;

    if (sync)
        stat_sync_loads.fetch_add(1, std::memory_order_relaxed);

    CTimer timer;
    timer.Start();
    if (load_tile(t))
    {
        // [CDB tiles] sync load: a query stood waiting for this read (the loading
        // path during a save load, a frame in the game)
        static std::atomic<u32> s_logged{0};
        const u32 ms = timer.GetElapsed_ms();
        if (sync && ms >= 5 && s_logged.fetch_add(1, std::memory_order_relaxed) < 64)
            Msg("~ [CDB tiles] sync load: tile %u, %.1f MB in %u ms (the query waited)", u32(&t - tiles.get()),
                t.nb_nodes * sizeof(AABBNoLeafNode) / (1024.f * 1024.f), ms);
        return true;
    }

    Msg("![CDB tiles] tile load FAILED (offset %llu, %u nodes) — rebuilding from resident tris", t.file_offset, t.file_nodes);
    if (rebuild_tile(t))
        return true;

    Msg("![CDB tiles] tile REBUILD failed — collision hole possible!");
    return false;
}

void TILE_GRID::preload_all(const Fvector* focus)
{
    if (g_cdb_tile_preload_mb <= 0 || !file)
        return;
    size_t total = 0;
    xr_vector<u32> order;
    for (u32 i = 0, n = nx * nz; i < n; ++i)
        if (tiles[i].file_nodes)
        {
            total += (size_t)tiles[i].file_nodes * sizeof(AABBNoLeafNode);
            order.push_back(i);
        }
    const u32 count = u32(order.size());
    if (!count)
        return;
    // 📏08-10-2026 Switch v235: in index order the first stalker query still beat
    // the preload to its tile (149 ms sync load) -- nearest to the player first.
    if (focus)
    {
        const float fx = focus->x, fz = focus->z;
        auto dist2 = [this, fx, fz](u32 i) {
            const float cx = origin_x + (float(i % nx) + 0.5f) * tile_size, cz = origin_z + (float(i / nx) + 0.5f) * tile_size;
            return (cx - fx) * (cx - fx) + (cz - fz) * (cz - fz);
        };
        std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return dist2(a) < dist2(b); });
    }
    if (total > (size_t(g_cdb_tile_preload_mb) << 20))
    {
        Msg("[CDB tiles] preload skipped: %u tiles, %.1f MB > cdb_tile_preload_mb %d", count, total / (1024.f * 1024.f), g_cdb_tile_preload_mb);
        return;
    }
    if (prefetch_inflight.exchange(true))
        return;

    // Core 2 at background priority: this thread mostly waits for the SD, and
    // core 0 is the loading main thread's (libnx puts new threads there).
    static PinnedWorker* s_worker = new PinnedWorker(2, 0x3B);
    s_worker->Run([this, total, count, order = std::move(order)] {
        CTimer timer;
        timer.Start();
        u32 loaded = 0;
        for (const u32 i : order)
        {
            TILE& t = tiles[i];
            if (t.file_nodes && t.state.load(std::memory_order_acquire) != TILE::S_READY && make_ready(t, false /*prefetch*/))
                ++loaded;
        }
        Msg("[CDB tiles] preload: %u of %u tiles (%.1f MB) in %u ms on the side | demand loads meanwhile %u", loaded, count, total / (1024.f * 1024.f),
            timer.GetElapsed_ms(), stat_sync_loads.load(std::memory_order_relaxed));
        prefetch_inflight.store(false, std::memory_order_release);
    });
}

void TILE_GRID::update_streaming(const Fvector& focus, size_t budget_bytes, float bubble_radius, BOOL verbose)
{
    const u32 now = tick.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!file)
        return; // no streaming source: fully resident, nothing to evict or prefetch

    const int bx0 = std::max(cell_x(focus.x - bubble_radius), 0), bx1 = std::min(cell_x(focus.x + bubble_radius), (int)nx - 1);
    const int bz0 = std::max(cell_z(focus.z - bubble_radius), 0), bz1 = std::min(cell_z(focus.z + bubble_radius), (int)nz - 1);

    // prefetch the bubble (one async task in flight; the sync-load path in the
    // queries stays as the correctness fallback)
    if (!prefetch_inflight.load(std::memory_order_relaxed))
    {
        xr_vector<u32> want;
        for (int iz = bz0; iz <= bz1; ++iz)
            for (int ix = bx0; ix <= bx1; ++ix)
            {
                TILE& t = cell(ix, iz);
                if (t.file_nodes && t.state.load(std::memory_order_relaxed) != TILE::S_READY)
                    want.push_back((u32)(iz * nx + ix));
            }
        if (!want.empty() && !prefetch_inflight.exchange(true))
        {
            TTAPI->submit_detach([this, want] {
                for (const u32 idx : want)
                    make_ready(tiles[idx], false /*prefetch*/);
                prefetch_inflight.store(false, std::memory_order_release);
            });
        }
    }

    // evict LRU outside the bubble while over budget
    if (resident_bytes.load(std::memory_order_relaxed) > budget_bytes)
    {
        struct cand
        {
            u32 idx, use;
        };
        xr_vector<cand> cands;
        for (u32 iz = 0; iz < nz; ++iz)
            for (u32 ix = 0; ix < nx; ++ix)
            {
                if ((int)ix >= bx0 && (int)ix <= bx1 && (int)iz >= bz0 && (int)iz <= bz1)
                    continue; // bubble is pinned
                TILE& t = cell(ix, iz);
                if (t.state.load(std::memory_order_relaxed) != TILE::S_READY)
                    continue;
                const u32 use = t.last_use.load(std::memory_order_relaxed);
                if (now - use < kEvictProtectTicks)
                    continue; // recently touched (distant NPC/sound) — hot
                cands.push_back({iz * nx + ix, use});
            }
        std::sort(cands.begin(), cands.end(), [](const cand& a, const cand& b) { return a.use < b.use; });
        for (const cand& c : cands)
        {
            if (resident_bytes.load(std::memory_order_relaxed) <= budget_bytes)
                break;
            TILE& t = tiles[c.idx];
            if (!t.lock.try_lock()) // someone is traversing/loading — skip
                continue;
            if (t.state.load(std::memory_order_relaxed) == TILE::S_READY)
            {
                resident_bytes.fetch_sub((size_t)t.nb_nodes * sizeof(AABBNoLeafNode), std::memory_order_relaxed);
                t.state.store(TILE::S_EMPTY, std::memory_order_release);
                delete[] t.nodes;
                t.nodes = nullptr;
                t.nb_nodes = 0;
                stat_evictions.fetch_add(1, std::memory_order_relaxed);
            }
            t.lock.unlock();
        }
    }

    if (verbose && 0 == (now % 300))
        Msg("[CDB tiles] resident %.1f MB / budget %.1f MB | loads %u (sync %u) evictions %u rebuilds %u", resident_bytes.load() / (1024.f * 1024.f),
            budget_bytes / (1024.f * 1024.f), stat_loads.load(), stat_sync_loads.load(), stat_evictions.load(), stat_rebuilds.load());
}

// ============================================================================
// MODEL: tiled build / cache load
// ============================================================================

bool MODEL::build_tiled(const Fvector* V, size_t Vcnt, const TRI* T, size_t Tcnt, const Fbox& aabb, build_callback* bc, void* bcp, const char* cache_path, u64 key)
{
    R_ASSERT(S_INIT == status);
    if (Vcnt < 4 || Tcnt < 2)
        return false;

    // resident copies + material callback — identical to the monolithic path
    verts_count = Vcnt;
    verts = xr_alloc<Fvector>(verts_count);
    std::memcpy(verts, V, verts_count * sizeof(Fvector));
    tris_count = Tcnt;
    tris = xr_alloc<TRI>(tris_count);
    std::memcpy(tris, T, tris_count * sizeof(TRI));
    if (bc)
        bc(verts, Vcnt, tris, Tcnt, bcp);

    auto cleanup = [&] {
        xr_free(verts);
        verts_count = 0;
        xr_free(tris);
        tris_count = 0;
    };

    TILE_GRID* G = xr_new<TILE_GRID>();
    G->verts = verts;
    G->tris = tris;
    G->verts_count = verts_count;
    G->tris_count = tris_count;
    G->tile_size = kTileSize;
    G->inv_tile = 1.f / kTileSize;
    G->origin_x = aabb.x1 - 1.f;
    G->origin_z = aabb.z1 - 1.f;
    G->nx = std::max(1, (int)std::ceil((aabb.x2 + 1.f - G->origin_x) / kTileSize));
    G->nz = std::max(1, (int)std::ceil((aabb.z2 + 1.f - G->origin_z) / kTileSize));
    const u32 nTiles = G->nx * G->nz;
    if (nTiles > kMaxTiles)
    {
        Msg("![CDB tiles] insane grid %ux%u — falling back to monolith", G->nx, G->nz);
        xr_delete(G);
        cleanup();
        return false;
    }
    G->tiles = std::make_unique<TILE[]>(nTiles);

    // ---- bin tris to tiles (flat prefix layout; a boundary tri lands in every tile it overlaps)
    CTimer timer;
    timer.Start();
    xr_vector<u32> counts(nTiles + 1, 0);
    auto tri_cells = [&](u32 i, int& ix0, int& ix1, int& iz0, int& iz1) {
        const TRI& t = tris[i];
        float x0 = type_max<float>, x1 = type_min<float>, z0 = type_max<float>, z1 = type_min<float>;
        for (u32 k = 0; k < 3; ++k)
        {
            const Fvector& v = verts[t.verts[k]];
            x0 = std::min(x0, v.x);
            x1 = std::max(x1, v.x);
            z0 = std::min(z0, v.z);
            z1 = std::max(z1, v.z);
        }
        ix0 = std::clamp(G->cell_x(x0), 0, (int)G->nx - 1);
        ix1 = std::clamp(G->cell_x(x1), 0, (int)G->nx - 1);
        iz0 = std::clamp(G->cell_z(z0), 0, (int)G->nz - 1);
        iz1 = std::clamp(G->cell_z(z1), 0, (int)G->nz - 1);
    };
    for (u32 i = 0; i < (u32)tris_count; ++i)
    {
        int ix0, ix1, iz0, iz1;
        tri_cells(i, ix0, ix1, iz0, iz1);
        for (int iz = iz0; iz <= iz1; ++iz)
            for (int ix = ix0; ix <= ix1; ++ix)
                ++counts[iz * G->nx + ix + 1];
    }
    for (u32 c = 1; c <= nTiles; ++c)
        counts[c] += counts[c - 1];
    const size_t totalRefs = counts[nTiles];
    xr_vector<u32> refs(totalRefs);
    {
        xr_vector<u32> cursor(counts.begin(), counts.end() - 1);
        for (u32 i = 0; i < (u32)tris_count; ++i)
        {
            int ix0, ix1, iz0, iz1;
            tri_cells(i, ix0, ix1, iz0, iz1);
            for (int iz = iz0; iz <= iz1; ++iz)
                for (int ix = ix0; ix <= ix1; ++ix)
                    refs[cursor[iz * G->nx + ix]++] = i;
        }
    }
    const u32 binMs = timer.GetElapsed_ms();

    // ---- per-tile OPCODE builds, parallel (no mutable globals in the builder)
    timer.Start();
    std::atomic<bool> failed{false};
    xr_vector<std::future<void>> jobs;
    for (u32 c = 0; c < nTiles; ++c)
    {
        if (counts[c + 1] == counts[c])
            continue;
        auto job = [G, &refs, &counts, &failed, c] {
            if (failed.load(std::memory_order_relaxed))
                return;
            xr_vector<u32> ids(refs.begin() + counts[c], refs.begin() + counts[c + 1]);
            if (!build_tile_from_ids(*G, G->tiles[c], ids))
                failed.store(true, std::memory_order_relaxed);
        };
        if (TTAPI)
            jobs.emplace_back(TTAPI->submit(job));
        else
            job();
    }
    for (auto& j : jobs)
        j.wait();
    const u32 buildMs = timer.GetElapsed_ms();

    if (failed.load())
    {
        Msg("![CDB tiles] tile build FAILED — falling back to monolith");
        xr_delete(G);
        cleanup();
        return false;
    }

    // ---- save + reopen as streaming source
    timer.Start();
    if (save_tiled(*G, cache_path, key))
    {
        G->file = fopen(XR_OSPATH(cache_path), "rb");
        Msg("[CDB tiles] baked %ux%u tiles (%u tris, dup %.2f%%): bin %u ms, build %u ms, save %u ms | tree %.1f MB", G->nx, G->nz, (u32)tris_count,
            totalRefs ? 100.f * float(totalRefs - tris_count) / float(tris_count) : 0.f, binMs, buildMs, timer.GetElapsed_ms(),
            G->resident_bytes.load() / (1024.f * 1024.f));
    }
    else
        Msg("![CDB tiles] baked, but save FAILED '%s' — running fully resident (no eviction)", cache_path);

    tiles_ = G;
    status = S_READY;
    return true;
}

bool MODEL::cache_load_tiled(const char* cache_path, u64 key)
{
    R_ASSERT(S_INIT == status);

    FILE* f = fopen(XR_OSPATH(cache_path), "rb");
    if (!f)
        return false;

    TILE_GRID* G = nullptr;
    bool ok = false;
    do
    {
        TiledCacheHeader h;
        if (!read_all(f, &h, sizeof(h)))
            break;
        if (h.magic != kTiledCacheMagic || h.version != kTiledCacheVersion || h.key != key)
            break;
        if (!h.verts_count || !h.tris_count || !h.nx || !h.nz || h.nx * h.nz > kMaxTiles || h.tile_size < 16.f)
            break;

        CTimer _t; _t.Start();
        verts_count = (size_t)h.verts_count;
        tris_count  = (size_t)h.tris_count;
        const size_t vBytes = verts_count * sizeof(Fvector);
        const size_t tBytes = tris_count * sizeof(TRI);

        // Map the file and point the two arrays into it (g_cdb_cache_read_threads
        // 0 = map, >=1 = the old copy on that many readers, kept for the A/B).
        // The header is 4-aligned and so are Fvector and TRI, so the view's own
        // 64 KB alignment is enough for both.
        if (g_cdb_cache_read_threads == 0)
        {
            storage_view = MapCacheStorage(cache_path, sizeof(h), vBytes + tBytes);
            if (!storage_view)
                break;
            verts = (Fvector*)((u8*)storage_view + sizeof(h));
            tris  = (TRI*)((u8*)storage_view + sizeof(h) + vBytes);
            Msg("[load step]   CDB cache map: %u MB verts + %u MB tris mapped in %u ms (warming behind the load)",
                (u32)(vBytes >> 20), (u32)(tBytes >> 20), _t.GetElapsed_ms());
        }
        else
        {
            verts = xr_alloc<Fvector>(verts_count);
            tris  = xr_alloc<TRI>(tris_count);
            const u32 msAlloc = _t.GetElapsed_ms();
            const u32 nrd = (u32)std::min(16, g_cdb_cache_read_threads);
            _t.Start();
            if (!read_parallel(cache_path, sizeof(h), verts, vBytes, nrd))
                break;
            const u32 msVerts = _t.GetElapsed_ms();
            _t.Start();
            if (!read_parallel(cache_path, sizeof(h) + vBytes, tris, tBytes, nrd))
                break;
            Msg("[load step]   CDB cache read: alloc %u | verts %u ms (%u MB) | tris %u ms (%u MB) on %u readers",
                msAlloc, msVerts, (u32)(vBytes >> 20), _t.GetElapsed_ms(), (u32)(tBytes >> 20), nrd);
        }
        if (0 != _fseeki64(f, (s64)(sizeof(h) + vBytes + tBytes), SEEK_SET))
            break;

        const u32 nTiles = h.nx * h.nz;
        xr_vector<TiledDirEntry> dir(nTiles);
        if (!read_all(f, dir.data(), nTiles * sizeof(TiledDirEntry)))
            break;

        G = xr_new<TILE_GRID>();
        G->verts = verts;
        G->tris = tris;
        G->verts_count = verts_count;
        G->tris_count = tris_count;
        G->origin_x = h.origin_x;
        G->origin_z = h.origin_z;
        G->tile_size = h.tile_size;
        G->inv_tile = 1.f / h.tile_size;
        G->nx = h.nx;
        G->nz = h.nz;
        G->tiles = std::make_unique<TILE[]>(nTiles);
        bool sane = true;
        for (u32 i = 0; i < nTiles && sane; ++i)
        {
            TILE& t = G->tiles[i];
            t.file_offset = dir[i].offset;
            t.file_nodes = dir[i].nb_nodes;
            t.bb_min = dir[i].bb_min;
            t.bb_max = dir[i].bb_max;
            if (t.file_nodes && t.file_offset < sizeof(h))
                sane = false;
        }
        if (!sane)
            break;

        G->file = f;
        f = nullptr; // grid owns the handle now
        tiles_ = G;
        G = nullptr;
        status = S_READY;
        ok = true;

    } while (false);

    if (f)
        fclose(f);
    if (!ok)
    {
        xr_delete(G);
        if (storage_view)
        {
            CDB::UnmapStorageView(storage_view);   // mapped, not allocated
            storage_view = nullptr;
            verts = nullptr;
            tris = nullptr;
        }
        else
        {
            xr_free(verts);
            xr_free(tris);
        }
        verts_count = 0;
        tris_count = 0;
        status = S_INIT;
    }
    return ok;
}

namespace CDB
{
void UnmapStorageView(void* view)
{
    if (s_warmer.joinable())
        s_warmer.join();   // it is walking exactly these pages
    if (view)
        UnmapViewOfFile(view);
}
}   // namespace CDB

void MODEL::update_streaming(const Fvector& focus, size_t budget_mb, float bubble_radius, BOOL verbose)
{
    if (tiles_)
        tiles_->update_streaming(focus, budget_mb << 20, bubble_radius, verbose);
}
