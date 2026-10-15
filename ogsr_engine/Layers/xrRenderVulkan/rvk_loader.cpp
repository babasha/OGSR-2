// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — level loading for OGSR.
//
// Stripped from monolith's rvk_loader.cpp down to MVP scope: shaders, geometry
// buffers, sliding-window LOD items, and visuals (the four chunks that have
// to land for the engine to consider the level loaded). Subsystems we have
// not ported yet — sectors/portals, lights, wallmarks, HOM, ShadowManager,
// RTGI, 3D fluid, details — are left as no-ops; their entry points exist so
// the engine's call order is preserved, they just do nothing.

#include "stdafx.h"
#include "xr_platform_seh.h"
#include "CRender_Vulkan.h"
#include "vk_Visual.h"
#include "vk_ModelPool.h"
#include "vk_buffer.h"
#include "vk_buffer_pool.h"
#include "vk_d3d_compat.h"
#include "vk_shader.h"          // g_VulkanShaderManager
#include "vk_DetailManager.h"   // VK::CDetailManager (grass)
#include "vk_TreeManager.h"     // VK::CTreeManager (trees)
#include "vk_LODManager.h"      // VK::CLODManager (LOD imposters)
#include "vk_shadow_gpu.h"      // VK::ShadowGPU (GPU-driven sun shadow casters)
#include "vk_world_gpu.h"       // VK::WorldGPU (GPU-driven world forward pass)
#include "vk_vsm.h"             // VK::VSM::InvalidateCache (world-anchored page cache vs level change)
#include "vk_terrain_cache.h"   // TerrainCache::OnLevelUnload (forget captured terrain)
#include "vk_world_material.h"  // VK::WorldMaterialCache::SetLevelTag (per-level lightmap namespacing)
#include "vk_texture_stream.h"   // TextureStreamer::SetLoadNonTexReserve (geometry's share of VRAM)
#include "HW_Vulkan.h"          // VulkanHW (vkDeviceWaitIdle in level_Unload)
#include "vk_command_buffer.h"  // CommandManager.FlushUploadsAndWait (drain async uploads)
#include "vk_render_queue.h"    // g_RenderQueue (holds raw visual pointers across frames)
#include "vk_pass_world.h"
#include "vk_hom.h"           // nfs10: VK::Hom::Load / Unload      // DynamicVisuals() / HudVisuals()
#include "vk_parallel.h"        // VK::ParallelChunks (r_vis_warm)
#include "vk_rt_scene.h"        // VK::RTScene::OnLevelUnload (the RT bubble points into the cform)
#include "vk_rt_dyn.h"          // VK::RTDyn::OnLevelUnload (per-frame skeleton BLAS over the pre-skin pool)
#include "vk_ddgi.h"            // VK::DDGI::OnLevelUnload (the probe field described that world)
#include "vk_gi_froxel.h"       // VK::GiFroxel::OnLevelUnload (its froxel copy described it too)
#include "vk_pass_gather.h"     // VK::GatherPass::OnLevelUnload (its history described it too)
#include "vk_wrc.h"             // VK::WRC::OnLevelUnload (its cells describe the level that is going away)
#include "vk_clk.h"              // VK::ClkToMs — one calibrated rdtsc->ms for every load counter

#include "../../xr_3da/x_ray.h"             // pApp
#include "../../xr_3da/xrLevel.h"           // fsL_SHADERS / fsL_VB / fsL_IB / fsL_VISUALS / fsL_SWIS
#include "../../xr_3da/IGame_Persistent.h"  // g_pGamePersistent
#include "../../xr_3da/IGame_Level.h"       // g_pGameLevel
#include "../../xr_3da/fmesh.h"             // ogf_header, OGF_HEADER, MT_*
#include "../../xrCore/stream_reader.h"
#include "vk_pass_shadow.h"                 // VK::SunShadow_WaitWorker (level_Unload)
#include "vk_wallmarks.h"                   // VK::Wallmarks::Prewarm (level_Load)

// OGSR has no dedicated-server build path here; geometry loading runs
// unconditionally. (Monolith gates these on `g_dedicated_server`.)

// ============================================================================
// Load-step timing
// ============================================================================
// The engine's [load phase] markers lump everything after LoadVisuals into ONE
// '(render)' bucket - 7526 ms on pripyat_full - which is too coarse to act on:
// that bucket covers eight independent build passes, each an O(visual count)
// walk over 450k visuals. These sub-timers split it per pass and print one
// table at the end of the load so the log stays readable. Same shape as the
// existing [CDB tiles] bin/build/write split.
// ============================================================================
namespace
{
static constexpr u32 kMaxLoadSteps = 16;
static constexpr u32 kVisTypes     = 16;   // MT_* fit in 0..12, rest bucketed

struct load_steps
{
    struct entry { const char* name; float ms; };

    entry  m_steps[kMaxLoadSteps] = {};
    u32    m_count                = 0;
    CTimer m_timer;

    load_steps() { m_timer.Start(); }

    // Close the step that just ended and start the next one.
    void end(const char* name)
    {
        if (m_count < kMaxLoadSteps)
            m_steps[m_count++] = { name, m_timer.GetElapsed_ms_total() };
        m_timer.Start();
    }

    void dump(const char* title) const
    {
        float total = 0.f;
        for (u32 i = 0; i < m_count; ++i)
            total += m_steps[i].ms;

        Msg("[load step] %s: %.0f ms total", title, total);
        for (u32 i = 0; i < m_count; ++i)
            Msg("[load step]   %-24s %8.1f ms  %5.1f%%", m_steps[i].name, m_steps[i].ms,
                total > 0.f ? 100.f * m_steps[i].ms / total : 0.f);
    }
};
}

// Per-visual Load split (header/texture/geometry/fastpath), defined in vk_Visual.cpp,
// and the material-cache split inside it, defined in vk_world_material.cpp.
// FS open-source snapshot, taken as the level's render phase begins (see the
// dump at the end of LoadVisuals).
static FS_OpenStats s_fsAtLevelStart;

static u64   s_logLinesAtStart = 0, s_logClkAtStart = 0, s_logWriteClkAtStart = 0;

namespace VK { namespace VisualProf { void Dump(); } }
namespace VK { namespace UploadProf { void Dump(); } }

namespace VK { namespace WorldMaterialCache { void ProfDump(); } }
namespace VK { namespace TexLoadProf { void Dump(); } }   // read/repack/create/upload (vk_texture.cpp)

// ============================================================================
// Level Loading
// ============================================================================
void CRender::level_Load(IReader* fs)
{
    R_ASSERT(0 != g_pGameLevel);
    R_ASSERT(!b_loaded);

    Msg("[Vulkan] CRender::level_Load() started");
    pApp->LoadBegin();

    // Open the texture streamer's per-level bracket (budget snapshot + demotion tally
    // + end-of-load residency report). Mirrors r4_loader.cpp:35. The matching
    // DeferredLoad(FALSE)/ResourcesDeferredUpload closing it come from the shared
    // level-start path (Level_network_start_client). Level textures are created just
    // below (the "Loading N level shaders" pass), so this must precede them.
    Device.m_pRender->DeferredLoad(TRUE);

    // Namespace the material cache's lightmap keys by THIS level ($level$ is
    // already mounted here): lmap names repeat across levels, and the cache
    // survives level changes — without the tag the new level binds the previous
    // level's lightmaps ("baked" light/dark patches that ignore the sun).
    {
        string_path lp;
        FS.update_path(lp, "$level$", "");
        VK::WorldMaterialCache::SetLevelTag(lp);
    }

    IReader* chunk;

    // Open the span the rdtsc->ms ratio is measured over (vk_clk.h). Everything
    // that counts ticks during this load converts with the SAME constant, and by
    // dump time the span is seconds long, so the ratio is not a sampling guess.
    VK::ClkAnchor();

    s_fsAtLevelStart = FS_GetOpenStats();
    LogProfGet(s_logLinesAtStart, s_logClkAtStart, s_logWriteClkAtStart);

    // ----- Shaders -----------------------------------------------------------
    g_pGamePersistent->LoadTitle("");
    {
        chunk = fs->open_chunk(fsL_SHADERS);
        R_ASSERT2(chunk, "Level doesn't built correctly - no shaders chunk");

        u32 count = chunk->r_u32();
        Msg("[Vulkan] Loading %u level shaders", count);
        Shaders.resize(count, nullptr);

        for (u32 i = 0; i < count; i++)
        {
            LPCSTR n = LPCSTR(chunk->pointer());
            chunk->skip_stringZ();

            if (!g_VulkanShaderManager) continue;

            if (0 == n[0]) {
                Shaders[i] = g_VulkanShaderManager->GetDefaultShader();
                continue;
            }

            string512 n_sh, n_tlist;
            xr_strcpy(n_sh, n);
            n_tlist[0] = 0;
            if (LPSTR delim = strchr(n_sh, '/')) {
                *delim = 0;
                xr_strcpy(n_tlist, delim + 1);
            }

            Shaders[i] = g_VulkanShaderManager->CreateShader(n_sh, n_tlist);
            if (!Shaders[i])
                Shaders[i] = g_VulkanShaderManager->GetDefaultShader();
        }
        chunk->close();

        if (!g_VulkanShaderManager)
            Msg("![Vulkan] g_VulkanShaderManager not initialized — visuals will use nullptr shaders");
        Msg("[Vulkan] Loaded %u shaders", (u32)Shaders.size());
    }

    // ----- Texture prefetch (starts here, joined after the visual walk) -------
    // This table IS the level's texture set: world visuals take their diffuse and
    // lightmap from it (vk_Visual::LoadTexture), and it is complete right now —
    // three seconds of geometry loading before the walk that consumes it. So the
    // ~2460 .dds opens go to a worker pool instead of the single loader thread,
    // overlapping with the geometry read that follows. Byte cache only: the walk
    // below still decides what actually becomes a texture (see StartTexturePrefetch).
    {
        xr_vector<shared_str> bases, lmaps;
        bases.reserve(Shaders.size());
        lmaps.reserve(Shaders.size());
        for (VK::CVulkanShader* sh : Shaders) {
            if (!sh) continue;
            if (sh->m_TexDiffuse.size() > 0) bases.push_back(sh->m_TexDiffuse);
            if (sh->m_TexLmap.size()    > 0) lmaps.push_back(sh->m_TexLmap);
        }
        // level.geom leads: LoadBuffers below reads it sequentially through a 1 MB
        // mapped window on this thread and spends ~2.6 s faulting its pages in.
        string_path geomPath;
        FS.update_path(geomPath, "$level$", "level.geom");
        // ...and its SIZE is what the level's vertex+index buffers will cost in VRAM
        // (1869 MB on Pripyat, within a megabyte of the file). The texture budget has
        // to know that before the first texture is planned, because the textures are
        // now built by workers that run ahead of this read — see SetLoadNonTexReserve.
        // The FS record, not r_open: opening it here would materialize (and for an
        // archived level, decompress) 1.9 GB for a number the directory already has.
        if (const CLocatorAPI::file* gf = FS.exist(geomPath))
            VK::TextureStreamer::Instance().SetLoadNonTexReserve((VkDeviceSize)gf->size_real);
        VK::WorldMaterialCache::StartTexturePrefetch(std::move(bases), std::move(lmaps), geomPath);

    }


    // ----- Geometry buffers + LOD sliding-window items ----------------------
    g_pGamePersistent->LoadTitle("");
    if (!VK::g_BufferPool)
        VK::g_BufferPool = xr_new<VK::CBufferPool>();
    {
        CStreamReader* geom = FS.rs_open("$level$", "level.geom");
        R_ASSERT2(geom, "level.geom not found");
        LoadBuffers(geom, FALSE);
        LoadSWIs(geom);
        FS.r_close(geom);

        // level.geomx (fast-path shadow geometry) is deliberately NOT loaded:
        // vkFVisual::LoadFastPath is a stub, the extended pool registers nowhere
        // and nothing binds it — it was hundreds of MB of dead VRAM per level
        // (shadows draw the cluster-LOD / meshlet paths instead). xVB/xIB stay
        // empty; release_bufs() on unload handles that fine.
    }

    g_pGamePersistent->LoadTitle("");
    {
        // The phase total has always been bigger than the sum of the per-visual
        // timers, and the gap was blamed on the walk's own overhead. Everything
        // around the walk is timed here instead of assumed: opening fsL_VISUALS
        // decompresses the whole blob before a single visual is parsed, and the
        // prefetch join sits after it.
        // From here the loading thread is the only consumer of textures, and the
        // geometry stage above is done with the upload ring: the prefetch workers
        // can stop parking bytes and start building textures (r_tex_materialize 2).
        VK::TexPrefetch::EnableMaterialize();

        CTimer _tOpen; _tOpen.Start();
        chunk = fs->open_chunk(fsL_VISUALS);
        const float openMs = _tOpen.GetElapsed_ms_total();
        R_ASSERT2(chunk, "Level has no visuals");
        Msg("[load step]   visuals chunk open: %.0f ms (%.1f MB blob)", openMs, float(chunk->length()) / (1024.f * 1024.f));

        LoadVisuals(chunk);
        // Timed on its own: the visuals chunk is a decompressed blob, and freeing
        // one of those has been the hidden tail of a phase before (see the texture
        // close). If this shows up, it moves off the loader thread too.
        CTimer _tClose; _tClose.Start();
        chunk->close();
        Msg("[load step]   visuals chunk close: %.0f ms", _tClose.GetElapsed_ms_total());
    }
    // Every material this level will ever ask for has been asked for by now — join
    // the workers and free anything the walk never claimed.
    {
        CTimer _tStop; _tStop.Start();
        VK::TexPrefetch::Stop();
        Msg("[load step]   prefetch join: %.0f ms", _tStop.GetElapsed_ms_total());
    }

    // ----- Subsystem hooks (currently no-op stubs) --------------------------
    g_pGamePersistent->LoadTitle("");
    load_steps _steps;   // splits this fourth '(render)' phase, see load_steps above
    LoadSectors(fs);
    LoadLights(fs);
    Load3DFluid();
    _steps.end("sectors+lights+fluid");

    // ----- Grass / detail objects ------------------------------------------
    // Session A scope: open level.details, bake heightmap from collision tris,
    // upload slot+obj SSBOs. No rendering yet — Sessions B/C add the compute
    // generator + render pipeline. Safe to skip on levels with no level.details.
    if (!Details) Details = xr_new<VK::CDetailManager>();
    Details->Load();
    _steps.end("Details::Load");

    // ----- GPU-driven world forward pass -----------------------------------
    // Extract opaque/AT static world meshes grouped by material for compute-cull
    // + indirect draw (moves per-object CPU submission off the CPU so detail-rich
    // levels stop hitting the draw-call wall). See vk_pass_world / vk_world_gpu.
    // Runs BEFORE ShadowGPU so the caster build can report cluster-shadow
    // coverage (casters outside the WorldGPU set, WorldGPU::InSet).

    VK::WorldGPU::Build();
    _steps.end("WorldGPU::Build");

    // ----- Pool compaction (Stage B increment (б)) --------------------------
    // Free the nVB/nIB slices of cluster-repacked meshes (their draw data lives
    // in the ClusterStream page pools). MUST run after WorldGPU::Build (needs
    // the cluster/repack refs) and BEFORE Trees->Build / ShadowGPU::Build —
    // both snapshot pool handles + offsets from m_mesh at Build, so building
    // them after the swap means they see the compacted state. The upload drain
    // guarantees the pools' initial data is on the GPU before the copy.
    CommandManager.FlushUploadsAndWait();
    _steps.end("upload drain #1");
    VK::WorldGPU::CompactPools();
    _steps.end("WorldGPU::CompactPools");

    // ----- Trees ------------------------------------------------------------
    // GPU-driven indirect path: walks Visuals[] for MT_TREE_ST/PM, builds
    // metadata + transforms SSBOs. Must run AFTER LoadVisuals (Visuals[]
    // populated) and AFTER CompactPools (snapshots pool handles/offsets +
    // reads back meshlet/hull geometry from the pools).
    if (!Trees) Trees = xr_new<VK::CTreeManager>();
    Trees->Build();
    _steps.end("Trees::Build");

    // ----- GPU-driven sun shadow casters -----------------------------------
    // Extract opaque static casters (shared VB/IB pools, world-space) into a
    // meta SSBO + indirect/count buffers for per-cascade compute cull. Must run
    // AFTER LoadVisuals (Visuals[] populated). Alpha-tested casters stay on the
    // CPU FlushDepth path. See vk_pass_shadow / vk_shadow_gpu.
    VK::ShadowGPU::Build();
    _steps.end("ShadowGPU::Build");

    // ----- LOD imposters ----------------------------------------------------
    // FLOD billboard facets (MT_LOD) for distant foliage density. Must run after
    // LoadVisuals so the vkFLOD objects exist.
    if (!LODs) LODs = xr_new<VK::CLODManager>();
    LODs->Build();
    _steps.end("LODs::Build");

    // ----- Terrain splat-mask bake (mask-less maps) -------------------------
    // Community maps regionalize terrain by LEVEL SHADER and ship no `_mask` —
    // bake the mask from their own region geometry (soft seams). No-op on maps
    // with an authored mask. Must run AFTER LoadVisuals + material creation.
    VK::TerrainMask::BakeIfNeeded();
    _steps.end("TerrainMask::Bake");

    // Last consumer of the shared leaf index is done — drop it. Dynamic objects
    // register their own visuals from here on, so a kept list would be stale.
    VK::WorldGPU::ReleaseLeafVisuals();

    pApp->LoadEnd();
    _steps.end("LoadEnd (stat_memory)");

    // Drain all async vertex/index uploads staged during load before the first
    // frame renders — one wait instead of a stall per buffer, and frees the ring.
    CommandManager.FlushUploadsAndWait();
    _steps.end("upload drain #2");
    _steps.dump("post-visuals build");

    // What this load spent writing its own log. Not a phase: it is a syscall per
    // line taken on whichever thread called Msg, so it hides inside every phase at
    // once. XROS_LOG_FLUSH=0 keeps the flush for '!' lines and every 64th line.
    {
        u64 lines = 0, clkT = 0, clkW = 0;
        LogProfGet(lines, clkT, clkW);
        Msg("[load step]   log: %llu lines this load, %.0f ms (%.0f ms of it write+flush)",
            (unsigned long long)(lines - s_logLinesAtStart),
            VK::ClkToMs() * float(clkT - s_logClkAtStart), VK::ClkToMs() * float(clkW - s_logWriteClkAtStart));
    }

    VK::Hom::Load();   // nfs10: $level$/level.hom (R1/R4 occlusion map)
    VK::Wallmarks::Prewarm();   // the first bullet hole / blood / rain otherwise builds its pipelines mid-fight
    b_loaded = TRUE;
    Msg("[Vulkan] level_Load() complete: %u visuals", (u32)Visuals.size());
}

// ============================================================================
// Level Unloading
// ============================================================================
void CRender::level_Unload()
{
    RT_WaitIdle();   // RENDER_THREAD_PLAN step 12: a frame handed to the render thread must finish first
    extern void NxOccWaitRays();
    NxOccWaitRays();  // r_dyn_occ_async: the ray worker reads the static model the level is about to free
    VK::SunShadow_WaitWorker();   // r_shadow_far_async: the far-map collect walks the visuals about to be freed
    if (0 == g_pGameLevel) return;
    if (!b_loaded) return;

    Msg("[Vulkan] CRender::level_Unload()");

    VK::Hom::Unload();   // nfs10

    // The RT bubble was cut out of THIS level's cform, which ObjectSpace is about
    // to drop, and a collect job may still be walking it on a worker.
    VK::RTDyn::OnLevelUnload();
    VK::RTScene::OnLevelUnload();
    // The probe field measured that world; on the next level every value in it
    // would be a confident lie, so wipe rather than let hysteresis fade it out.
    VK::DDGI::OnLevelUnload();
    // ...and so did the gather's history, which is reprojected every frame.
    VK::GatherPass::OnLevelUnload();
    // ...and every cell of the world radiance cache, which unlike a probe answers
    // with FULL confidence: a stale one would be authoritative radiance for a wall
    // that no longer exists.
    VK::WRC::OnLevelUnload();
    // ...and the froxel copy of the field, which is resampled out of the probes and
    // is therefore exactly as stale as they are.
    VK::GiFroxel::OnLevelUnload();

    // Every render queue holds RAW vkRender_Visual pointers collected during the
    // last frame, and the glass/water lists are kept deliberately across frames.
    // The visuals below are about to be deleted, so whatever is still queued is a
    // use-after-free with no owner left to notice.
    VK::g_RenderQueue.Clear();
    VK::g_RenderQueue.ClearGlass();
    VK::g_RenderQueue.ClearWater();
    VK::ClearAllVisualLists();   // every frame slot (this runs behind RT_WaitIdle)

    // Wait for in-flight GPU work — VMA can't free buffers the GPU is reading.
    if (VulkanHW.m_Device != VK_NULL_HANDLE)
        VK_DeviceWaitIdleLocked();

    // Visuals
    for (IRenderVisual* iv : Visuals)
    {
        if (!iv) continue;
        vkRender_Visual* v = static_cast<vkRender_Visual*>(iv);
        v->Release();
        xr_delete(v);
    }
    Visuals.clear();

    // Sliding window items
    for (FSlideWindowItem& swi : SWIs)
        if (swi.sw) { xr_free(swi.sw); swi.sw = nullptr; }
    SWIs.clear();

    Shaders.clear();

    auto release_bufs = [](xr_vector<VK::CVulkanBuffer*>& v)
    {
        for (VK::CVulkanBuffer* b : v)
            if (b) { b->Destroy(); xr_delete(b); }
        v.clear();
    };
    release_bufs(nVB); release_bufs(xVB);
    release_bufs(nIB); release_bufs(xIB);
    nVB_Strides.clear();
    xVB_Strides.clear();

    if (Models) Models->ClearPool(true);

    if (Details) {
        Details->Unload();
        xr_delete(Details);
    }

    if (Trees) {
        Trees->Destroy();
        xr_delete(Trees);
    }

    // GPU-driven shadow casters borrow the shared VB/IB pool handles — release
    // before those pools (below). Device is already idle (vkDeviceWaitIdle above).
    VK::ShadowGPU::Destroy();
    VK::WorldGPU::Destroy();

    // VSM's toroidal page cache is WORLD-anchored — the next level reuses the
    // same coordinates, so resident pages would keep serving THIS level's depth
    // (stale light/dark page squares on the new level's walls). Mark it empty;
    // the first frame re-renders the atlas from the new casters.
    VK::VSM::InvalidateCache();

    // Terrain composite cache captured THIS level's terrain material/mesh/affine —
    // forget them (the next level's first terrain draw re-captures + re-bakes).
    VK::TerrainCache::OnLevelUnload();
    VK::TerrainMask::OnLevelUnload();

    if (LODs) {
        LODs->Destroy();
        xr_delete(LODs);
    }

    if (VK::g_BufferPool) {
        xr_delete(VK::g_BufferPool);
        VK::g_BufferPool = nullptr;
    }

    b_loaded = FALSE;
    Msg("[Vulkan] level_Unload() complete");
}

// ============================================================================
// LoadBuffers — VB/IB pools from level.geom / level.geomx.
// ============================================================================
// Stage `size` bytes straight from the reader's mapped window into `dst`.
//
// What this replaces: the loader used to xr_alloc the whole buffer, r() the file
// into it, upload THAT, and free it -- so every byte of level.geom crossed the
// CPU twice and landed in freshly committed pages whose first touch costs a
// demand-zero fault per 4 KB. Measured 1869 MB at 2.7 GB/s on a machine whose
// memcpy does 14-20. Uploading out of the window skips the heap copy entirely;
// the ring copy that remains is the one that was always there.
//
// Falls back to the old shape for readers that do not map (archive entries).

// Is the stage bound by the copy, or by the pages arriving? A window is a fresh
// MapViewOfFile every megabyte, and a fresh mapping means a fault per 4 KB on the
// loading thread even when the file is already in the cache -- the page is
// resident, the PTE is not. Measured (4 copy helpers, pages touched ahead): touch
// 465 ms, after which the copy runs at 13.7 GB/s instead of 5.9 -- i.e. two thirds
// of the stage was faults, not bandwidth. Which is why the source below is the
// prefetch's view when there is one.
extern int ps_r_geom_lead;
#include <thread>
extern int ps_r_vis_warm;
static volatile u32 s_visWarmSink = 0;

// One mismatch is enough to stop trusting the lead view for the rest of the level.
static bool s_leadDistrusted = false;

static void StageFromStream(VK::CVulkanBuffer* dst, CStreamReader* fs, size_t bytes,
                            const u8* lead, u64 leadSize)
{
    size_t done = 0;
    while (done < bytes)
    {
        const u8* p = fs->window_pointer();
        const size_t avail = fs->window_avail();
        if (!p || !avail)
        {
            xr_vector<u8> scratch(bytes - done);
            fs->r(scratch.data(), scratch.size());
            dst->Upload(scratch.data(), scratch.size(), done);
            return;
        }
        const size_t n = _min(avail, bytes - done);

        // Same bytes, other view: the prefetch workers have already faulted these
        // pages into the process, and reading them through our own window would
        // fault every one of them again on this thread. The guard is cheap next to
        // a megabyte copy and the failure it prevents is silent wrong geometry.
        const u8* src = p;
        if (lead && !s_leadDistrusted && n >= 64) {
            const size_t abs = fs->abs_pos();
            if (abs != size_t(-1) && u64(abs) + n <= leadSize) {
                const u8* alt = lead + abs;
                if (memcmp(alt, p, 64) == 0 && memcmp(alt + n - 64, p + n - 64, 64) == 0) {
                    src = alt;
                } else {
                    s_leadDistrusted = true;
                    Msg("![Vulkan] geom: lead view disagrees with the stream at %zu — staging from the window", abs);
                }
            }
        }

        dst->Upload(src, n, done);
        fs->advance((std::ptrdiff_t)n);
        done += n;
    }
}

void CRender::LoadBuffers(CStreamReader* base_fs, BOOL _alternative)
{
    VK::Vram::Scope _vram_scope("Geom/Pools");
    R_ASSERT2(base_fs, "Could not load geometry - file not found");

    xr_vector<VK::CVulkanBuffer*>& _VB = _alternative ? xVB : nVB;
    xr_vector<VK::CVulkanBuffer*>& _IB = _alternative ? xIB : nIB;
    xr_vector<u32>& _Strides           = _alternative ? xVB_Strides : nVB_Strides;

    Msg("[Vulkan] Loading %s geometry buffers", _alternative ? "extended" : "normal");

    // Read vs. create+upload split. This pass is 3503 ms on pripyat_full, whose
    // level.geom is 1.96 GB (= ~560 MB/s, well under NVMe) - and the two halves
    // need different fixes: the read side is disk I/O plus one full copy into the
    // heap (which the staging ring then copies AGAIN), the GPU side is VMA create
    // plus ring backpressure. Measure them apart before touching either.
    CTimer tPart;
    float  readMs = 0.f, gpuMs = 0.f;
    s_leadDistrusted = false;

    // The texture prefetch mapped this same file whole and its workers are walking
    // it front to back faulting every page in. Staging out of THAT view costs the
    // loader no faults at all; out of its own window it costs one per 4 KB.
    u64 leadSize = 0;
    const u8* lead = nullptr;
    {
        string_path geomPath;
        FS.update_path(geomPath, "$level$", _alternative ? "level.geomx" : "level.geom");
        if (ps_r_geom_lead) lead = VK::TexPrefetch::LeadView(geomPath, leadSize);
    }
    u64    vbBytes = 0, ibBytes = 0;
    const FS_StreamStats streamAtStart = FS_GetStreamStats();


    // ----- Vertex buffers ---------------------------------------------------
    {
        CStreamReader* fs = base_fs->open_chunk(fsL_VB);
        R_ASSERT2(fs, "fsL_VB chunk not found");

        const u32 count = fs->r_u32();
        _VB.resize(count);
        _Strides.resize(count);

        const u32 declBytes = (MAXD3DDECLLENGTH + 1) * sizeof(D3DVERTEXELEMENT9);
        D3DVERTEXELEMENT9* dcl = (D3DVERTEXELEMENT9*)_alloca(declBytes);

        for (u32 i = 0; i < count; i++)
        {
            fs->r(dcl, declBytes);
            fs->advance(-(int)declBytes);

            const u32 dcl_len = VK_GetDeclLength(dcl) + 1;
            fs->advance(dcl_len * sizeof(D3DVERTEXELEMENT9));

            const u32 vCount = fs->r_u32();
            const u32 vSize  = VK_GetDeclVertexSize(dcl, 0);
            _Strides[i] = vSize;

            // TEXCOORD0 offset — buffer pool uses it to keep stride-32 lightmapped
            // (tcOff=24) and vertex-lit (tcOff=28) layouts on separate pipelines.
            u32 tcOffset = 24;
            for (u32 e = 0; e + 1 < dcl_len; e++) {
                if (dcl[e].Usage == D3DDECLUSAGE_TEXCOORD && dcl[e].UsageIndex == 0) {
                    tcOffset = dcl[e].Offset;
                    break;
                }
            }

            vbBytes += u64(vCount) * vSize;
            tPart.Start();

            VkBufferUsageFlags vbUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT |   // CTreeManager meshlet readback (positions)
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;  // RT world gather (vk_rt_world) reads the pool by address
            _VB[i] = xr_new<VK::CVulkanBuffer>();
            _VB[i]->Create(vCount * vSize, vbUsage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
            gpuMs += tPart.GetElapsed_ms_total();
            tPart.Start();
            StageFromStream(_VB[i], fs, size_t(vCount) * vSize, lead, leadSize);
            readMs += tPart.GetElapsed_ms_total();
            tPart.Start();

            // Only the normal pool publishes to g_BufferPool — extended (geomx)
            // shares VB slot ids and would overwrite the registrations.
            if (VK::g_BufferPool && !_alternative)
                VK::g_BufferPool->RegisterVertexBuffer(i, _VB[i], vSize, tcOffset);

            gpuMs += tPart.GetElapsed_ms_total();
        }
        fs->close();
    }

    // ----- Index buffers ----------------------------------------------------
    {
        CStreamReader* fs = base_fs->open_chunk(fsL_IB);
        R_ASSERT2(fs, "fsL_IB chunk not found");

        const u32 count = fs->r_u32();
        _IB.resize(count);

        for (u32 i = 0; i < count; i++)
        {
            const u32 iCount = fs->r_u32();
            const u32 iSize  = iCount * sizeof(u16);

            ibBytes += iSize;
            tPart.Start();

            VkBufferUsageFlags ibUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT |   // CTreeManager meshlet readback (indices)
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;  // RT world gather (vk_rt_world) reads the pool by address
            _IB[i] = xr_new<VK::CVulkanBuffer>();
            _IB[i]->Create(iSize, ibUsage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
            gpuMs += tPart.GetElapsed_ms_total();
            tPart.Start();
            StageFromStream(_IB[i], fs, iSize, lead, leadSize);
            readMs += tPart.GetElapsed_ms_total();
            tPart.Start();

            if (VK::g_BufferPool && !_alternative)
                VK::g_BufferPool->RegisterIndexBuffer(i, _IB[i], VK_INDEX_TYPE_UINT16);

            gpuMs += tPart.GetElapsed_ms_total();
        }
        fs->close();
    }

    const float totalMB = float(vbBytes + ibBytes) / (1024.f * 1024.f);
    Msg("[load step]   geom %s: stage %.0f ms (%.0f MB/s) | create %.0f ms | VB %.1f MB + IB %.1f MB = %.1f MB",
        _alternative ? "extended" : "normal", readMs, readMs > 0.f ? totalMB * 1000.f / readMs : 0.f, gpuMs,
        float(vbBytes) / (1024.f * 1024.f), float(ibBytes) / (1024.f * 1024.f), totalMB);

    Msg("[load step]   geom source: %s", lead ? (s_leadDistrusted ? "own sliding window - the lead view disagreed"
                                                                 : "prefetch lead view - no page faults on this thread")
                                              : (ps_r_geom_lead ? "own sliding window - no prefetch lead"
                                                                : "own sliding window - r_geom_lead 0"));


    // What that stage actually was. The bytes are consumed IN PLACE now (see
    // StageFromStream), so copy-out here counts only the ring copy; a low MB/s
    // is the window's own paging, not a heap hop.
    {
        const FS_StreamStats now = FS_GetStreamStats();
        const u64 maps = now.maps - streamAtStart.maps;
        const u64 cb   = now.copy_bytes - streamAtStart.copy_bytes;
        const double cus = double(now.copy_us - streamAtStart.copy_us);
        Msg("[load step]   geom stream: window %u MB | %llu remaps (%.0f ms) | copy-out %llu MB in %.0f ms (%.0f MB/s)",
            (u32)(FS_StreamWindowSize() >> 20), (unsigned long long)maps,
            double(now.map_us - streamAtStart.map_us) / 1000.0,
            (unsigned long long)(cb >> 20), cus / 1000.0,
            cus > 0.0 ? double(cb >> 20) * 1e6 / cus : 0.0);
    }

    // Ring split for the geometry alone. The phase total above is the whole
    // StageFromStream loop; this says how much of it is the host copy and how much
    // is the GPU making us wait -- and it leaves the texture phase's own dump (end
    // of LoadVisuals) covering nothing but textures.
    VK::UploadProf::Dump();

}

// ============================================================================
// LoadVisuals — fsL_VISUALS chunk; one vkRender_Visual per OGF entry.
// ============================================================================
void CRender::LoadVisuals(IReader* fs)
{
    Msg("[Vulkan] Loading visuals...");

    // Per-type split: WHICH MT_ type dominates decides what a cooked (flat,
    // memcpy-able) format has to replace first, so bucket the cost by H.type instead
    // of guessing.
    // In rdtsc, not chrono: GetElapsed_ms_total truncates to whole MICROSECONDS, and
    // a visual takes about two and a half of them. Half a microsecond lost 450k
    // times is 225 ms that this table used to hand to a phantom "loop overhead"
    // (measured: chrono sum 935 ms vs rdtsc 1202 for the same walk).
    u64    typeClk[kVisTypes]   = {};
    u32    typeCount[kVisTypes] = {};

    IReader* chunk = nullptr;
    u32 index = 0;
    ogf_header H;

    // Warm the blob alongside the walk. The visuals chunk is a span of the mapped
    // level file, so the walk's first touch of every 4 KB is a fault on this thread
    // -- the same shape the geometry stage had (see the lead view). Sixteen workers
    // sweep it front to back in 28 ms and stay far ahead of a walk that takes 1100,
    // so this is started, not waited for; the join is after the loop, before the
    // caller frees the blob.
    std::thread warmer;
    if (ps_r_vis_warm) {
        const u8*    base = (const u8*)fs->begin();
        const size_t len  = fs->length();
        warmer = std::thread([base, len] {
            const int mbs = int((len + (1u << 20) - 1) >> 20);
            VK::ParallelChunks(mbs, [base, len](int lo, int hi) {
                u32 sink = 0;
                for (int c = lo; c < hi; ++c) {
                    const size_t o0 = size_t(c) << 20;
                    const size_t o1 = _min(len, o0 + (1u << 20));
                    for (size_t o = o0; o < o1; o += 4096) sink += base[o];
                }
                s_visWarmSink += sink;
            });
        });
    }

    // The walk as a whole, against the sum of the per-type timers inside it. What is
    // left is the loop, and the obvious suspect -- a heap IReader per visual from
    // open_chunk -- is NOT it: keeping that reader on the stack was A/B'd in one
    // binary and moved 330 ms by 5. The rdtsc marks below say where it really goes
    // (the chrono pair around Create+Load already costs ~60 ns of it).
    CTimer tWalk;
    tWalk.Start();

    // The body, split three ways. The per-type table says Create+Load costs 1080 ms
    // while the four counters INSIDE V->Load add up to 873 — so 200 ms sit either in
    // the allocation, in the vector, or in nothing at all, and only marks can say
    // which. Two extra rdtsc per visual is ~9 ms; the answer is worth that.
    u64 clkCreate = 0, clkLoad = 0, clkPush = 0;
    u64 clkAll  = CPU::GetCLK();

    while ((chunk = fs->open_chunk(index)) != nullptr)
    {
        chunk->r_chunk_safe(OGF_HEADER, &H, sizeof(H));

        const u64 clkVis0 = CPU::GetCLK();
        vkRender_Visual* V = vkVisual_Create(H.type);
        const u64 clkVisC = CPU::GetCLK();
        clkCreate += clkVisC - clkVis0;
        if (V) {
            V->Load(nullptr, chunk, 0);
            const u64 clkVisL = CPU::GetCLK();
            clkLoad += clkVisL - clkVisC;
            Visuals.push_back(V);
            clkPush += CPU::GetCLK() - clkVisL;
        } else {
            Msg("![Vulkan] Visual #%u: unknown type %u — using dummy", index, H.type);
            Visuals.push_back(vkVisual_CreateDummy());
        }

        const u32 t = (H.type < kVisTypes) ? u32(H.type) : (kVisTypes - 1);
        typeClk[t] += CPU::GetCLK() - clkVis0;
        typeCount[t]++;

        chunk->close();
        index++;
    }
    clkAll = CPU::GetCLK() - clkAll;
    const float walkMs = tWalk.GetElapsed_ms_total();
    if (warmer.joinable()) warmer.join();   // before the caller frees the blob
    Msg("[Vulkan] Loaded %u visuals", index);

    {
        static const char* kTypeNames[kVisTypes] = {
            "MT_NORMAL",         "MT_HIERRARHY",       "MT_PROGRESSIVE",     "MT_SKELETON_ANIM",
            "MT_SKEL_GEOMDEF_PM","MT_SKEL_GEOMDEF_ST", "MT_LOD",             "MT_TREE_ST",
            "MT_PARTICLE_EFFECT","MT_PARTICLE_GROUP",  "MT_SKELETON_RIGID",  "MT_TREE_PM",
            "MT_3DFLUIDVOLUME",  "MT_13",              "MT_14",              "MT_other"};

        // rdtsc -> ms. One constant for every counter in this load (vk_clk.h),
        // measured against QPC over the whole level load. The walk also derives its
        // own from its wall clock; the two are printed against each other because a
        // disagreement here would misprice EVERY table below, and this instrument
        // has lied before.
        const float k     = VK::ClkToMs();
        const float kWalk = clkAll ? walkMs / float(clkAll) : 0.f;
        if (kWalk > 0.f && k > 0.f)
            Msg("[load step]   clk calib: %.3f GHz global vs %.3f GHz from this walk (%+.1f%%)",
                1e-6f / k, 1e-6f / kWalk, 100.f * (kWalk - k) / k);

        for (u32 t = 0; t < kVisTypes; ++t)
            if (typeCount[t])
                Msg("[load step]   visuals %-20s %8u x %6.1f us = %8.1f ms", kTypeNames[t], typeCount[t],
                    1000.f * k * float(typeClk[t]) / float(typeCount[t]), k * float(typeClk[t]));

        u64 sumClk = 0;
        for (u32 t = 0; t < kVisTypes; ++t) sumClk += typeClk[t];
        Msg("[load step]   visuals walk: %.0f ms = %.0f ms in Create+Load + %.0f ms of loop (%u iterations)",
            walkMs, k * float(sumClk), walkMs - k * float(sumClk), index);

        Msg("[load step]   visuals walk body: vkVisual_Create %.0f | V->Load %.0f | Visuals.push_back %.0f ms",
            k * float(clkCreate), k * float(clkLoad), k * float(clkPush));

        VK::VisualProf::Dump();               // header/texture/geometry/fastpath split (vk_Visual.cpp)
        VK::WorldMaterialCache::ProfDump();   // probe/find/create split inside LoadTexture's one call
        VK::TexLoadProf::Dump();              // and what a cache MISS actually spends its 4.2 ms on
        VK::UploadProf::Dump();               // ring-wrap drain vs host memcpy vs recording


        // Where the load's file opens actually went. "open/read" above is one number
        // covering three unrelated costs — a mapped loose file, a mapped archive
        // entry, and a whole-file LZO decompress — and only the third is worth
        // moving off the loader thread. Delta over the whole render phase, so it
        // also counts what the prefetch workers opened.
        const FS_OpenStats now = FS_GetOpenStats();
        Msg("[load step]   FS opens this phase: loose %u (%llu MB, %.0f ms) | archive-stored %u (%llu MB, %.0f ms) | archive-LZO %u (%llu MB, %.0f ms)",
            (u32)(now.loose_n - s_fsAtLevelStart.loose_n),
            (unsigned long long)((now.loose_b - s_fsAtLevelStart.loose_b) >> 20),
            (now.loose_us - s_fsAtLevelStart.loose_us) / 1000.0,
            (u32)(now.packed_n - s_fsAtLevelStart.packed_n),
            (unsigned long long)((now.packed_b - s_fsAtLevelStart.packed_b) >> 20),
            (now.packed_us - s_fsAtLevelStart.packed_us) / 1000.0,
            (u32)(now.compr_n - s_fsAtLevelStart.compr_n),
            (unsigned long long)((now.compr_b - s_fsAtLevelStart.compr_b) >> 20),
            (now.compr_us - s_fsAtLevelStart.compr_us) / 1000.0);

    }
}

// ============================================================================
// LoadSWIs — sliding-window items for progressive (LOD) meshes.
// ============================================================================
void CRender::LoadSWIs(CStreamReader* base_fs)
{
    if (!base_fs->find_chunk(fsL_SWIS))
        return;

    CStreamReader* fs = base_fs->open_chunk(fsL_SWIS);
    const u32 item_count = fs->r_u32();
    Msg("[Vulkan] Loading %u sliding window items", item_count);

    for (FSlideWindowItem& swi : SWIs)
        if (swi.sw) { xr_free(swi.sw); swi.sw = nullptr; }
    SWIs.clear();

    SWIs.resize(item_count);
    for (u32 c = 0; c < item_count; c++)
    {
        FSlideWindowItem& swi = SWIs[c];
        swi.reserved[0] = fs->r_u32();
        swi.reserved[1] = fs->r_u32();
        swi.reserved[2] = fs->r_u32();
        swi.reserved[3] = fs->r_u32();
        swi.count       = fs->r_u32();
        swi.sw          = xr_alloc<FSlideWindow>(swi.count);
        fs->r(swi.sw, sizeof(FSlideWindow) * swi.count);
    }

    fs->close();
}

// Resolve a pooled sliding-window item (OGF_SWICONTAINER references this by id).
// Used by vkFTreeVisual_PM / vkFProgressive for per-LOD index ranges.
FSlideWindowItem* CRender::getSWI(int id)
{
    return &SWIs.at(id);
}

// ============================================================================
// Stubs for subsystems we have not ported yet. Engine call order is preserved
// so when the real impls land they just replace the body.
// ============================================================================
void CRender::LoadSectors(IReader* /*fs*/) {}
void CRender::LoadLights (IReader* /*fs*/) {}
void CRender::Load3DFluid()                {}
