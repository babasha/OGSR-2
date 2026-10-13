#include "StdAfx.h"

// ============================================================================
// Tiled-CDB stress/soak harness (run with `xrEngine_VK.exe -cdb_stress`).
// Self-contained: synthesizes a city-like mesh, then loops
//   bake | cache-load -> MT queries + eviction/reload churn -> destroy
// and cross-checks tiled query results against the monolithic tree.
// Exists to convict/exonerate the tiled path on heap-corruption reports
// without needing in-game repro (the 16-07 unload crash investigation).
// ============================================================================

#include "xrCDB.h"
#include "xrCDB_tiled.h"

#include <random>
#include <thread>
#include <atomic>

namespace
{
void synth_mesh(xr_vector<Fvector>& V, xr_vector<CDB::TRI>& T, u32 grid, float world)
{
    // rippled heightfield
    V.resize((size_t)grid * grid);
    const float step = world / float(grid - 1);
    for (u32 z = 0; z < grid; ++z)
        for (u32 x = 0; x < grid; ++x)
        {
            float h = 2.f * std::sin(x * 0.05f) + 1.5f * std::cos(z * 0.07f) + 0.3f * std::sin(x * 0.71f + z * 0.53f);
            V[(size_t)z * grid + x].set(x * step, h, z * step);
        }
    T.reserve((size_t)(grid - 1) * (grid - 1) * 2);
    for (u32 z = 0; z + 1 < grid; ++z)
        for (u32 x = 0; x + 1 < grid; ++x)
        {
            const u32 a = z * grid + x, b = a + 1, c = a + grid, d = c + 1;
            CDB::TRI t1;
            t1.verts[0] = a;
            t1.verts[1] = b;
            t1.verts[2] = c;
            t1.dummy = (x * 31 + z) & 0x3FFF;
            CDB::TRI t2;
            t2.verts[0] = b;
            t2.verts[1] = d;
            t2.verts[2] = c;
            t2.dummy = (x * 17 + z * 3) & 0x3FFF;
            T.push_back(t1);
            T.push_back(t2);
        }
}

struct rng_t
{
    std::mt19937 g;
    explicit rng_t(u32 seed) : g(seed) {}
    float uf(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g); }
    u32 ui(u32 lo, u32 hi) { return std::uniform_int_distribution<u32>(lo, hi)(g); }
};

// collect sorted unique hit ids
void ids_of(CDB::COLLIDER& c, xr_vector<int>& out)
{
    out.clear();
    for (CDB::RESULT* r = c.r_begin(); r != c.r_end(); ++r)
        out.push_back(r->id);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}
} // namespace

void run_cdb_tiled_stress(const char* cmd)
{
    u32 cycles = 40;
    const bool realData = nullptr != strstr(cmd, "-cdb_stress_real");
    if (const char* p = strstr(cmd, "-cdb_stress "))
        if (1 != sscanf(p + xr_strlen("-cdb_stress "), "%u", &cycles))
            cycles = 40;

    xr_vector<Fvector> V;
    xr_vector<CDB::TRI> T;
    u64 monoKey = 0;
    string_path monoCache{};
    if (realData)
    {
        // pull verts/tris out of the game's monolithic CDB cache (real 61M-tri data)
        string_path dir;
        FS.update_path(dir, "$app_data_root$", "");
        string_path pattern;
        xr_sprintf(pattern, "%scdb_*.cdb", dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (INVALID_HANDLE_VALUE == h)
        {
            Msg("![CDB stress] no cdb_*.cdb in '%s' — run the game once with cdb_tiles 0 first", dir);
            return;
        }
        u64 bestSize = 0;
        do // biggest cache = the level we actually care about (Pripyat, 61M tris)
        {
            const u64 sz = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (sz > bestSize)
            {
                bestSize = sz;
                xr_sprintf(monoCache, "%s%s", dir, fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);

#pragma pack(push, 4)
        struct MonoHeader
        {
            u32 magic, version;
            u64 key, verts_count, tris_count;
            u32 nb_nodes, model_code;
        };
#pragma pack(pop)
        FILE* f = fopen(monoCache, "rb");
        MonoHeader mh;
        if (!f || 1 != fread(&mh, sizeof(mh), 1, f))
        {
            Msg("![CDB stress] can't read '%s'", monoCache);
            if (f)
                fclose(f);
            return;
        }
        monoKey = mh.key;
        V.resize((size_t)mh.verts_count);
        T.resize((size_t)mh.tris_count);
        const bool ok = 1 == fread(V.data(), V.size() * sizeof(Fvector), 1, f) && 1 == fread(T.data(), T.size() * sizeof(CDB::TRI), 1, f);
        fclose(f);
        if (!ok)
        {
            Msg("![CDB stress] short read '%s'", monoCache);
            return;
        }
        cycles = std::min(cycles, 4u);
        Msg("[CDB stress] REAL data from '%s'", monoCache);
    }
    else
        synth_mesh(V, T, 1024, 1500.f);

    Fbox aabb;
    aabb.invalidate();
    for (const Fvector& v : V)
        aabb.modify(v);
    const float world = std::max(aabb.x2 - aabb.x1, aabb.z2 - aabb.z1);
    Msg("[CDB stress] mesh: %zu verts, %zu tris, %u cycles, world %.0f m", V.size(), T.size(), cycles, world);

    string_path cachePath;
    xr_strcpy(cachePath, "cdb_stress_tile_cache.bin");
    remove(cachePath);

    // reference monolith for correctness cross-check
    CDB::MODEL mono;
    if (realData)
    {
        if (!mono.cache_load(monoCache, monoKey))
        {
            Msg("![CDB stress] monolith cache_load failed");
            return;
        }
    }
    else
        mono.build(V.data(), V.size(), T.data(), T.size());
    Msg("[CDB stress] monolith reference ready");

    std::atomic<u32> mismatches{0};

    for (u32 cycle = 0; cycle < cycles; ++cycle)
    {
        CDB::MODEL tiled;
        const bool fromCache = (cycle & 1) && tiled.cache_load_tiled(cachePath, 0xC0FFEEull);
        if (!fromCache)
        {
            if (!tiled.build_tiled(V.data(), V.size(), T.data(), T.size(), aabb, nullptr, nullptr, cachePath, 0xC0FFEEull))
            {
                Msg("![CDB stress] build_tiled FAILED at cycle %u", cycle);
                return;
            }
        }

        // correctness sweep (single-threaded, deterministic per cycle)
        {
            rng_t r(1234 + cycle);
            CDB::COLLIDER ct, cm;
            xr_vector<int> it, im;
            for (u32 q = 0; q < 400; ++q)
            {
                Fvector s{r.uf(aabb.x1 - 100, aabb.x2 + 100), r.uf(aabb.y1 - 20, aabb.y2 + 40), r.uf(aabb.z1 - 100, aabb.z2 + 100)};
                Fvector d{r.uf(-1, 1), r.uf(-1, 1), r.uf(-1, 1)};
                if (d.magnitude() < 0.05f)
                    d.set(0, -1, 0);
                d.normalize();
                const float range = r.uf(1, 2000);

                ct.ray_query(CDB::OPT_ONLYNEAREST | CDB::OPT_CULL, &tiled, s, d, range);
                cm.ray_query(CDB::OPT_ONLYNEAREST | CDB::OPT_CULL, &mono, s, d, range);
                if (ct.r_count() != cm.r_count() || (ct.r_count() && ct.r_begin()->id != cm.r_begin()->id && !fsimilar(ct.r_begin()->range, cm.r_begin()->range, 0.01f)))
                    ++mismatches;

                ct.ray_query(0, &tiled, s, d, range);
                cm.ray_query(0, &mono, s, d, range);
                ids_of(ct, it);
                ids_of(cm, im);
                if (it != im)
                    ++mismatches;

                const Fvector bc{r.uf(aabb.x1, aabb.x2), r.uf(aabb.y1, aabb.y2), r.uf(aabb.z1, aabb.z2)};
                const Fvector bd{r.uf(0.2f, 8), r.uf(0.2f, 8), r.uf(0.2f, 8)};
                ct.box_query(CDB::OPT_FULL_TEST, &tiled, bc, bd);
                cm.box_query(CDB::OPT_FULL_TEST, &mono, bc, bd);
                ids_of(ct, it);
                ids_of(cm, im);
                if (it != im)
                    ++mismatches;
            }
        }

        // MT churn: workers hammer a window around a MOVING focus. Tiles the
        // focus leaves behind go cold (worker window follows), get LRU-evicted
        // while the new window sync-reloads under fire — evict/load/query all
        // concurrent, the amplified version of walking across the level.
        std::atomic<bool> stop{false};
        std::atomic<float> fx{0.f}, fz{0.f};
        auto worker = [&](u32 seed) {
            rng_t r(seed);
            CDB::COLLIDER c;
            while (!stop.load(std::memory_order_relaxed))
            {
                const float cx = fx.load(std::memory_order_relaxed), cz = fz.load(std::memory_order_relaxed);
                Fvector s{cx + r.uf(-200, 200), r.uf(aabb.y1, aabb.y2), cz + r.uf(-200, 200)};
                if (r.ui(0, 3) == 0)
                {
                    Fvector d{r.uf(-1, 1), r.uf(-0.5f, 0.1f), r.uf(-1, 1)};
                    if (d.magnitude() < 0.05f)
                        d.set(0, -1, 0);
                    d.normalize();
                    c.ray_query(CDB::OPT_ONLYNEAREST | CDB::OPT_CULL, &tiled, s, d, r.uf(1, 1200));
                }
                else
                    c.box_query(0, &tiled, s, Fvector{r.uf(0.2f, 2), r.uf(0.2f, 2), r.uf(0.2f, 2)});
            }
        };
        xr_vector<std::thread> pool;
        for (u32 w = 0; w < 6; ++w)
            pool.emplace_back(worker, 777 * (cycle + 1) + w);

        // focus sweeps diagonally back and forth; tiny budget + tiny bubble so
        // the trail behind actually crosses the eviction protect window
        const size_t budgetMB = realData ? 256 : 8; // tiny on purpose — force churn
        for (u32 tick = 0; tick < 1500; ++tick)
        {
            const float t = (tick % 500) / 500.f;
            const float sweep = (tick / 500) & 1 ? 1.f - t : t;
            fx.store(aabb.x1 + sweep * (aabb.x2 - aabb.x1), std::memory_order_relaxed);
            fz.store(aabb.z1 + sweep * (aabb.z2 - aabb.z1), std::memory_order_relaxed);
            tiled.update_streaming(Fvector{fx.load(), 0, fz.load()}, budgetMB, 128.f, FALSE);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        stop = true;
        for (auto& th : pool)
            th.join();

        CDB::TILE_GRID* G = tiled.tiled();
        Msg("[CDB stress] cycle %u (%s): resident %.1f MB, loads %u (sync %u), evictions %u, rebuilds %u, mismatches %u", cycle, fromCache ? "cache" : "bake",
            G->resident_bytes.load() / (1024.f * 1024.f), G->stat_loads.load(), G->stat_sync_loads.load(), G->stat_evictions.load(), G->stat_rebuilds.load(),
            mismatches.load());
        // tiled destroyed here — dtor under just-stopped-thread conditions
    }

    remove(cachePath);
    if (mismatches.load())
        Msg("![CDB stress] FAILED: %u mismatches", mismatches.load());
    else
        Msg("[CDB stress] PASSED: %u cycles clean", cycles);
}
