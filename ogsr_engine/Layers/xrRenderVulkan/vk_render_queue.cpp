// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_render_queue.h"
#include "vk_profiler.h"   // CountDraw/CountPipelineBind — счётчики были мертвы до 18-09
#include "vk_pipeline_cache.h"
#include "vk_world_material.h"  // WorldMaterial set bind
#include "vk_texture_stream.h"  // TextureStreamer::Touch — LRU heuristic (r_txstream)
#include "vk_env_light.h"       // EnvLight::GetCurrentSet — set 1 (per-frame lighting)
#include "vk_pass_lightcones.h" // SynthCones::Submit — lightplanes-derived beams
#include "vk_Visual.h"
#include "vk_UIPipeline.h"   // g_VkUI_FrameCmd
#include "vk_terrain_cache.h"   // composite-cache capture (terrain material + mesh)

#include "../../xr_3da/device.h"   // RV().vCameraPosition (tess distance factors)
#include "../../xrCDB/Frustum.h"   // CFrustum — optional caster cull in FlushDepth

#include <algorithm>
#include <unordered_set>

// Console cvar at GLOBAL scope (block-scope extern inside namespace VK can
// mangle as VK::* → LNK2001, see vk_pass_skinned.cpp).
extern int ps_r_light_debug;   // r_light_debug — gates the glass-flush triage log
extern int ps_r_ssao_debug;    // r_ssao_debug — gates the statics-normal coverage log below
extern float ps_r_tess, ps_r_tess_max, ps_r_tess_near, ps_r_tess_far, ps_r_tess_height, ps_r_tess_pn;
extern int ps_r_fsinv_split;

namespace VK {

RenderQueue g_RenderQueue;

u64 makeSortKey(u32 stride, u32 tcOffset, bool depthTest, const void* mat, const void* vb, bool wmark, bool tess)
{
    u64 k = 0;
    k |= (u64(wmark     ? 1 : 0)) << 59;   // decals LAST — blend over the opaque world
    k |= (u64(tess      ? 1 : 0)) << 58;   // tessellated materials cluster (one pipeline flip)
    k |= (u64(depthTest ? 1 : 0)) << 56;
    k |= (u64(stride   & 0xFF))   << 48;
    k |= (u64(tcOffset & 0xFF))   << 40;
    // 24-bit material hash (>> 4 to drop alloc-alignment bits).
    k |= ((u64(uintptr_t(mat)) >> 4) & 0xFFFFFFull) << 16;
    // 16-bit VB hash to sub-cluster within a material.
    k |= ((u64(uintptr_t(vb)) >> 4) & 0xFFFFull);
    return k;
}

void RenderQueue::Push(const DrawItem& item)
{
    DrawItem it = item;
    it.hemi = m_SubmitHemi;   // stamp the current submit-hemi (1.0 for statics)
    // Lightplanes carriers register their synthesized beam for Pass_LightCones
    // every frame (the xform tracks moving carriers). The fake sheets themselves
    // are never drawn — the cone replaces them (see FlushGlass).
    if (m_SideEffects && it.lateGlass && it.vis && it.vis->m_SynthBeams.count
        && it.vis->m_pWorldMaterial && it.vis->m_pWorldMaterial->isLitBlend)
        SynthCones::Submit(it.vis, it.xform);
    // Glass panes defer to the LATE translucent flush (Pass_WorldGlass): they
    // blend without z-write, so drawing them in the normal flush let everything
    // rendered after (GPU-world statics, trees, grass, sky) overwrite the
    // blended pixels — glass looked missing or opaque.
    // Water bodies go to their own list: Pass_Water draws them with a dedicated
    // pipeline (analytic waves, Fresnel sky reflection, Beer-Lambert body), so
    // they must not reach the world flush at all — there they were shaded as
    // opaque vert-lit geometry and came out BLACK.
    if (it.lateWater) { m_WaterItems.push_back(it); return; }
    (it.lateGlass ? m_GlassItems : m_Items).push_back(it);
}

// Hierarchy walks double-submit children (parent recursion + own top-level
// entry, ~3.7×): harmless for opaque, fatal for a BLENDED surface — three stacked 0.5 alphas read as
// opaque. One item per unique visual.
void RenderQueue::DedupWater()
{
    if (m_WaterItems.size() < 2) return;
    std::unordered_set<const void*> seen;
    seen.reserve(m_WaterItems.size());
    xr_vector<DrawItem> out;
    out.reserve(m_WaterItems.size());
    for (const DrawItem& it : m_WaterItems)
        if (seen.insert(it.vis).second) out.push_back(it);
    m_WaterItems.swap(out);
}

void RenderQueue::Clear()
{
    m_Items.clear();
    // m_GlassItems intentionally NOT cleared: Clear() runs mid-frame (between the
    // statics and dynamics flushes) while glass accumulates for the late pass.
}

void RenderQueue::ClearGlass()
{
    m_GlassItems.clear();
}

void RenderQueue::FlushGlass(FrameContext& ctx)
{
    if (m_GlassItems.empty()) return;
    // The CPU world walk double-submits hierarchy children (~3.7×).
    // For OPAQUE that's just redundant draws; for BLENDED glass it's STACKED
    // alpha (three 0.6 blends ≈ 0.94 = the pane reads opaque). One per visual.
    {
        // Lightplanes texture-sheet beams (lit-blend) are never drawn —
        // Pass_LightCones draws the REAL volumetric cone from the light instead.
        std::unordered_set<const void*> seen;
        seen.reserve(m_GlassItems.size());
        xr_vector<DrawItem> out;
        out.reserve(m_GlassItems.size());
        for (const DrawItem& it : m_GlassItems) {
            if (it.vis && it.vis->m_pWorldMaterial && it.vis->m_pWorldMaterial->isLitBlend)
                continue;
            if (seen.insert(it.vis).second) out.push_back(it);
        }
        m_GlassItems.swap(out);
    }
    static u32 s_lastLog = 0;
    if (ps_r_light_debug && RV().dwTimeGlobal - s_lastLog > 3000) {
        s_lastLog = RV().dwTimeGlobal;
        string1024 names{ "" };
        u32 shown = 0;
        for (const DrawItem& gi : m_GlassItems) {
            if (shown >= 6) break;
            if (gi.vis && gi.vis->dbg_name.c_str()) {
                xr_strcat(names, gi.vis->dbg_name.c_str());
                xr_strcat(names, "; ");
                ++shown;
            }
        }
        Msg("[VK Glass] late flush: %zu panes [%s]", m_GlassItems.size(), names);
    }
    // Reuse the whole Flush machinery on the glass list: swap it in, sort
    // (groups by pipeline/material), draw, drop the drawn items, restore
    // whatever stale list m_Items held (it was already flushed this frame).
    std::swap(m_Items, m_GlassItems);
    SortByKey();
    const bool tess = m_AllowTess;
    m_AllowTess = false;   // glass dynamics carry model xforms — never tessellate
    Flush(ctx);
    m_AllowTess = tess;
    // The (deduped, sorted) glass list is KEPT: the particles pass re-draws it
    // into the distortion RT for the refraction (r_glass_refr); ClearGlass at
    // the next frame's Pass_World start owns the cleanup.
    std::swap(m_Items, m_GlassItems);
}

void RenderQueue::SortByKey()
{
    std::sort(m_Items.begin(), m_Items.end(),
              [](const DrawItem& a, const DrawItem& b) { return a.sortKey < b.sortKey; });
}

void RenderQueue::Flush(FrameContext& ctx)
{
    if (m_Items.empty()) return;
    if (ctx.cmd == VK_NULL_HANDLE) return;

    VkCommandBuffer cmd = ctx.cmd;

    // Stage mask of the world/terrain push range — must match the layouts'
    // declared range exactly (includes TCS/TES when the device has tess).
    const VkShaderStageFlags kStages = PipelineCache::GetPushStages();

    // World heightmap tessellation (R4 TESS_HM). The per-frame tess block
    // (push offsets 84..116) is owned here: pushed on every layout flip so it
    // survives terrain↔world transitions. tessMax=0 turns the TES into a
    // pass-through and routes items to the flat pipelines below.
    // pnScale (offset 112) is the PN-triangle silhouette-curvature factor: it
    // MUST be pushed here — it lives past the 84-byte VS/FS block, so nothing
    // else writes it. (Was previously uninitialized → garbage curvature warped
    // tessellated props; default r_tess_pn 0 = PN off, heightmap-only.)
    const bool  tessAvail = m_AllowTess && PipelineCache::TessAvailable() && ps_r_tess > 0.5f;
    const float tessMax   = tessAvail ? ps_r_tess_max : 0.0f;
    const Fvector eye     = RV().vCameraPosition;
    struct TessBlock { float tessMax, tessNear, tessFar; float eyeHeight[4]; float pnScale; };
    const TessBlock tessBlock = {
        tessMax, ps_r_tess_near, ps_r_tess_far,
        // R4 amplitude: ComputeDisplacedVertex's `P += N * height * 0.07`.
        { eye.x, eye.y, eye.z, 0.07f * ps_r_tess_height },
        ps_r_tess_pn,
    };
    constexpr u32 kTessOffset = sizeof(Fmatrix) + 5 * sizeof(float);   // 84

    // State tracking: each bind only fires when the next item differs from
    // what's already bound. After a sort by sortKey, runs of identical
    // (pipeline, material, VB, IB) collapse to one bind apiece.
    VkPipeline       lastPipe   = VK_NULL_HANDLE;
    VkDescriptorSet  lastMatSet = VK_NULL_HANDLE;
    float            lastAref   = 999.0f;          // sentinel — first push always fires
    float            lastDetailScale = -999.0f;    // sentinel
    float            lastHemi        = -999.0f;    // sentinel (per-object sky-ambient gate)
    u32              lastStreamID    = 0xFEFEFEFEu;// sentinel (never a real/none id)
    VkBuffer         lastVB     = VK_NULL_HANDLE;
    VkBuffer         lastIB     = VK_NULL_HANDLE;
    VkIndexType      lastIType  = VK_INDEX_TYPE_MAX_ENUM;
    Fmatrix          lastXform;                 // per-item MVP tracker (dynamic objects)
    bool             haveXform  = false;
    // Terrain splatting uses a separate pipeline layout (7-binding set). Push
    // constants are range-compatible, but binds/pushes must target the active
    // layout — track it and force re-emits when it flips.
    VkPipelineLayout lastLayout = VK_NULL_HANDLE;

    u32 nDraw = 0, nPipeBind = 0, nMatBind = 0, nTailPush = 0, nVBBind = 0, nIBBind = 0;
    u32 nTessMat = 0, nTessDraw = 0;   // tessellation diag: opted-in mats / actually-tess draws
    u32 nTerrain = 0, nWmark = 0, nEmis = 0, nAT = 0;   // draw-mix diag (r_fsinv_split)

    for (const DrawItem& it : m_Items)
    {
        auto* fv = static_cast<vkFVisual*>(it.vis);
        if (!fv || !fv->m_mesh.IsValid()) continue;
        if (!fv->m_mesh.p_rm_Vertices || !fv->m_mesh.p_rm_Indices) continue;

        // Resolve material + pick the render path. Terrain (diffuse under
        // "terrain\") uses a separate pipeline + 7-binding splat set + layout;
        // everything else takes the standard lmap/vlit path. Decide first so
        // all pushes/binds below target the correct (active) layout.
        WorldMaterial* mat = fv->m_pWorldMaterial
                              ? fv->m_pWorldMaterial
                              : WorldMaterialCache::GetDefault();

        // Mark the diffuse used this frame for the streamer's LRU heuristic (no-op
        // unless r_txstream is on — Touch self-gates and only takes the lock then).
        if (mat) TextureStreamer::Instance().Touch(mat->tex);

        VkPipeline       pipe   = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkDescriptorSet  set    = VK_NULL_HANDLE;

        // Needed before pipeline selection (atEqual is statics-only) AND by the
        // MVP push below: dynamic visuals carry their own world matrix.
        const bool dynXform = 0 != memcmp(&it.xform, &Fidentity, sizeof(Fmatrix));

        const bool terrain = mat && mat->isTerrain
                          && mat->terrainSet != VK_NULL_HANDLE
                          && PipelineCache::GetTerrainPipeline() != VK_NULL_HANDLE;
        if (terrain) {
            ++nTerrain;
            pipe   = PipelineCache::GetTerrainPipeline();
            layout = PipelineCache::GetTerrainLayout();
            set    = mat->terrainSet;
            // Composite cache (r_terra_cache): remember the terrain material +
            // mesh once so the bake can read mask/heights and probe the VB for
            // the world->uv affine. No-ops after the first capture.
            TerrainCache::OnTerrainMaterial(mat->terrainSet, mat->detailScale);
            if (fv->m_mesh.p_rm_Vertices)
                TerrainCache::OnTerrainMesh(fv->m_mesh.p_rm_Vertices->GetHandle(), fv->m_mesh.vBase,
                                            fv->m_mesh.vStride, fv->m_mesh.tcOffset);
        } else {
            PipelineCache::Key k{};
            k.stride   = fv->m_mesh.vStride;
            k.tcOffset = fv->m_mesh.tcOffset;
            // Sub-layout drives shader variant: tcOffset==24 → lmap (TC1+lightmap),
            // tcOffset==28 → vert-lit (D3DCOLOR + sun mask).
            const bool lmap = (k.tcOffset == 24);
            k.vs        = lmap ? PipelineCache::WorldLmapVS() : PipelineCache::WorldVlitVS();
            k.fs        = lmap ? PipelineCache::WorldLmapFS() : PipelineCache::WorldVlitFS();
            k.depthTest = true;
            k.wmark     = mat && mat->isWmark;   // baked decal: blend + bias + no z-write
            k.emis      = mat && mat->isEmisAdd; // glow/selflight: ADDITIVE + unlit (aref -3)
            // Uber-FS variant (Inc 1): POM bit only for materials with a real `#` height;
            // flat materials bake the POM-off pipeline (no march + no march VGPRs). The
            // frame-global bits (snow/wet/ibl/debug) are stamped in Get().
            k.specMask  = (mat && mat->tessellated) ? (u8)PipelineCache::WS_POM : (u8)0;
            // Heightmap tessellation: only for opted-in materials (bump# in
            // the .thm) whose bounds reach inside the tess range — beyond
            // tessFar the factors would all be 1, so a flat pipeline is free.
            if (tessMax > 0.0f && mat && mat->tessellated && !k.wmark) {
                const Fsphere& s = fv->vis.sphere;
                k.tess = (s.P.distance_to(eye) - s.R) < ps_r_tess_far;
                ++nTessMat;
                if (k.tess) ++nTessDraw;
            }
            // Prepass-covered statics (armed by Pass_World for the static flush;
            // the prepass depth is final for these items):
            //  - r_at_equal: AT items → EQUAL + no write (kills layered AT).
            //  - r_z_prepass: opaque items → LEQUAL, no write (re-enables
            //    early-Z, which the FS's discard + z-write ON had disabled).
            const bool prepassItem = !dynXform && !k.wmark && !k.emis && !k.tess;
            k.atEqual  = m_ATEqual  && prepassItem && mat && mat->alphaRef >= 0.0f;
            k.noZWrite = m_PrepassZ && prepassItem;
            if (k.wmark) ++nWmark;
            else if (k.emis) ++nEmis;
            else if (mat && mat->alphaRef >= 0.0f) ++nAT;
            pipe   = PipelineCache::Get(k);
            layout = PipelineCache::GetLayout();
            set    = mat ? mat->set : VK_NULL_HANDLE;
        }
        if (pipe == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) continue;

        // Layout flip (terrain ↔ standard): a new layout invalidates bound
        // descriptors and may disturb push constants — force MVP + tail re-push
        // and reset pipeline/set/buffer trackers.
        if (layout != lastLayout) {
            lastLayout      = layout;
            haveXform       = false;
            lastAref        = 999.0f;
            lastDetailScale = -999.0f;
            lastStreamID    = 0xFEFEFEFEu;
            lastPipe        = VK_NULL_HANDLE;
            lastMatSet      = VK_NULL_HANDLE;
            lastVB          = VK_NULL_HANDLE;
            lastIB          = VK_NULL_HANDLE;

            // set 1 = per-frame env lighting. The world and terrain layouts differ at
            // set 0, so a layout flip disturbs set 1 too — (re)bind it on every flip.
            VkDescriptorSet envSet = EnvLight::GetCurrentSet();
            if (envSet != VK_NULL_HANDLE)
                VK::Prof::CountDescriptorBind(); vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &envSet, 0, nullptr);

            // Per-frame tess block (tess factors + camera). Constant across the
            // flush; re-pushed per flip because a layout change formally
            // invalidates push-constant state.
            vkCmdPushConstants(cmd, layout, kStages, kTessOffset, sizeof(TessBlock), &tessBlock);
        }

        // Per-item MVP (push-constant offset 0). Dynamic visuals carry their own
        // world matrix in it.xform; level statics submit identity, giving
        // mvp == *viewProj. Push only when the xform changes. (dynXform computed
        // above, before pipeline selection.)
        if (ctx.viewProj && (!haveXform || 0 != memcmp(&it.xform, &lastXform, sizeof(Fmatrix)))) {
            Fmatrix mvp;
            mvp.mul(*ctx.viewProj, it.xform);   // = it.xform · viewProj (model->clip)
            vkCmdPushConstants(cmd, layout, kStages, 0, sizeof(Fmatrix), &mvp);
            // Dynamic visuals also hand the VS the model rows (i, j, c; k = i×j in
            // the shader) so vWorldPos/vNormal leave in WORLD space — without this,
            // fog/cascade-shadow/dyn-lights/wetness read MODEL coords (props "glowed"
            // with the level-origin fog and rotated ones were sun-lit from the wrong
            // side). Written into the tess push region: the dynamics flush never
            // tessellates (SetAllowTess(false)) and statics never read it (their
            // dynHemi tail float stays >= 0, see the encode below).
            if (dynXform) {
                const float rows[9] = { it.xform.i.x, it.xform.i.y, it.xform.i.z,
                                        it.xform.j.x, it.xform.j.y, it.xform.j.z,
                                        it.xform.c.x, it.xform.c.y, it.xform.c.z };
                vkCmdPushConstants(cmd, layout, kStages, kTessOffset, sizeof(rows), rows);
            }
            lastXform = it.xform;
            haveXform = true;
        }

        if (pipe != lastPipe) {
            VK::Prof::CountPipelineBind(); vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            lastPipe = pipe;
            ++nPipeBind;
            lastVB     = VK_NULL_HANDLE;
            lastIB     = VK_NULL_HANDLE;
            lastMatSet = VK_NULL_HANDLE;
        }

        if (set != VK_NULL_HANDLE && set != lastMatSet) {
            VK::Prof::CountDescriptorBind(); vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    layout, 0, 1, &set, 0, nullptr);
            lastMatSet = set;
            ++nMatBind;
        }
        // Per-material/-item tail at offset 72: { alphaRef, detailScale, dynHemi }.
        // dynHemi gates the sky ambient per object (1.0 = statics/open sky). For
        // dynamic (non-identity xform) items it is sign-ENCODED as -(1+hemi): the
        // negative flags "model rows follow" to the VS; the frag decodes -x-1.
        const float aref        = mat ? mat->alphaRef    : -1.0f;
        const float detailScale = mat ? mat->detailScale :  0.0f;
        const float hemiEnc     = dynXform ? -(1.0f + it.hemi) : it.hemi;
        if (aref != lastAref || detailScale != lastDetailScale || hemiEnc != lastHemi) {
            constexpr u32 kTailOffset = sizeof(Fmatrix) + 2 * sizeof(float);
            float tail[3] = { aref, detailScale, hemiEnc };
            vkCmdPushConstants(cmd, layout, kStages, kTailOffset, sizeof(tail), tail);
            lastAref        = aref;
            lastDetailScale = detailScale;
            lastHemi        = hemiEnc;
            ++nTailPush;
        }
        // GPU-feedback stream id of the base diffuse (offset 116, past the tess
        // block). 0xFFFFFFFF = not streamable — the FS skips the report. Tracked
        // separately from the tail: two materials can share aref/scale/hemi.
        const u32 streamID = mat ? mat->streamID : 0xFFFFFFFFu;
        if (streamID != lastStreamID) {
            constexpr u32 kStreamIDOffset = sizeof(Fmatrix) + 13 * sizeof(float);   // 116
            vkCmdPushConstants(cmd, layout, kStages, kStreamIDOffset, sizeof(u32), &streamID);
            lastStreamID = streamID;
        }

        VkBuffer vb = fv->m_mesh.p_rm_Vertices->GetHandle();
        if (vb != lastVB) {
            VkDeviceSize vbOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);
            lastVB = vb;
            ++nVBBind;
        }

        VkBuffer    ib     = fv->m_mesh.p_rm_Indices->GetHandle();
        VkIndexType iType  = fv->m_mesh.iType;
        if (ib != lastIB || iType != lastIType) {
            vkCmdBindIndexBuffer(cmd, ib, 0, iType);
            lastIB    = ib;
            lastIType = iType;
            ++nIBBind;
        }

        const u32 firstIndex = it.iCountOverride ? it.iBaseOverride  : fv->m_mesh.iBase;
        const u32 indexCount = it.iCountOverride ? it.iCountOverride : fv->m_mesh.iCount;
        VK::Prof::CountDraw(1, 0); vkCmdDrawIndexed(cmd, indexCount, 1, firstIndex,
                         (s32)fv->m_mesh.vBase, 0);
        ++nDraw;
    }

    // Draw-mix attribution (r_fsinv_split): what the CPU flush actually consists
    // of. Fires for BOTH the statics and the dynamics flush — tell them apart by
    // items/atEq (statics flush arms atEq when r_at_equal is on).
    if (ps_r_fsinv_split > 0) {
        static u32 s_lastMix = 0;
        if (RV().dwTimeGlobal > s_lastMix + 3000) {
            s_lastMix = RV().dwTimeGlobal;
            Msg("[VK Queue] mix: items=%zu draws=%u terrain=%u wmark=%u emis=%u at=%u atEq=%d",
                m_Items.size(), nDraw, nTerrain, nWmark, nEmis, nAT, (int)m_ATEqual);
        }
    }

    static bool s_diag_done = false;
    if (!s_diag_done) {
        Msg("[VK Queue] Flush: items=%zu draws=%u pipeBinds=%u matBinds=%u tailPush=%u vbBinds=%u ibBinds=%u",
            m_Items.size(), nDraw, nPipeBind, nMatBind, nTailPush, nVBBind, nIBBind);
        Msg("[VK Tess] avail=%s tessMax=%.1f pn=%.2f near=%.1f far=%.1f height=%.2f | this flush: %u tess-mat draws, %u in range (tessellated)",
            (tessMax > 0.f) ? "yes" : "no", tessMax, ps_r_tess_pn, ps_r_tess_near, ps_r_tess_far, ps_r_tess_height,
            nTessMat, nTessDraw);
        s_diag_done = true;
    }
}

void RenderQueue::FlushDepth(VkCommandBuffer cmd, const Fmatrix& lightVP, bool skipAlphaTested,
                            bool alphaTestedOnly, bool displaceTerrain, const CFrustum* cull,
                            bool mvTarget)
{
    if (m_Items.empty() || cmd == VK_NULL_HANDLE) return;
    VkPipelineLayout layoutSolid = PipelineCache::GetDepthLayout();
    VkPipelineLayout layoutAT    = PipelineCache::GetDepthATLayout();
    if (layoutSolid == VK_NULL_HANDLE) return;

    // Push block of the AT variant (must match shadow_depth_at.{vert,frag}).
    struct ATPush { Fmatrix mvp; float uvScale[2]; float aref; float _pad; };

    VkPipeline      lastPipe   = VK_NULL_HANDLE;
    VkPipelineLayout lastLayout = VK_NULL_HANDLE;
    VkDescriptorSet lastMatSet = VK_NULL_HANDLE;
    VkDescriptorSet lastTerrSet0 = VK_NULL_HANDLE;   // terrain material set 0 (uMask read by the TES mud carve)
    float           lastAref   = -999.f;
    VkBuffer    lastVB   = VK_NULL_HANDLE;
    VkBuffer    lastIB   = VK_NULL_HANDLE;
    VkIndexType lastIType = VK_INDEX_TYPE_MAX_ENUM;
    Fmatrix     lastXform; bool haveXform = false;
    u32 nDraw = 0;

    for (const DrawItem& it : m_Items)
    {
        auto* fv = static_cast<vkFVisual*>(it.vis);
        if (!fv || !fv->m_mesh.IsValid()) continue;
        if (!fv->m_mesh.p_rm_Vertices || !fv->m_mesh.p_rm_Indices) continue;

        // Optional cone/face cull: skip casters whose bounding sphere misses the
        // caller's frustum (conservative — touching spheres still draw).
        if (cull) {
            const Fsphere& bs = fv->vis.sphere;
            if (bs.R > 0.f) {
                Fvector c; it.xform.transform_tiny(c, bs.P);
                if (!cull->testSphere_dirty(c, bs.R)) continue;
            }
        }

        // Alpha-tested items go through the AT variant (punch-out silhouette in
        // the depth); without it (shader missing / caller opted out) skip them.
        WorldMaterial* mat = fv->m_pWorldMaterial ? fv->m_pWorldMaterial : WorldMaterialCache::GetDefault();
        if (mat && mat->isWmark) continue;   // baked decals never write depth (prepass/shadows)

        // Terrain in the DEPTH PREPASS: route through the snow-displaced terrain
        // depth pipeline (the SAME world_terrain.vert as the color pass -> identical
        // SnowDisplace -> the prepass depth matches the color -> no z-fight/see-
        // through). Needs the EnvLight set at set 1 (sf_params.w). Only when asked
        // (prepass); shadows/rain pass displaceTerrain=false (undisplaced is fine).
        if (displaceTerrain && mat && mat->isTerrain && mat->terrainSet != VK_NULL_HANDLE) {
            VkPipeline       tpipe = PipelineCache::GetTerrainDepthPipeline(mvTarget);
            VkPipelineLayout tlay  = PipelineCache::GetTerrainLayout();
            VkDescriptorSet  eset  = EnvLight::GetCurrentSet();
            if (tpipe == VK_NULL_HANDLE || tlay == VK_NULL_HANDLE || eset == VK_NULL_HANDLE) continue;
            if (tlay != lastLayout) { lastLayout = tlay; lastMatSet = VK_NULL_HANDLE; lastTerrSet0 = VK_NULL_HANDLE; lastAref = -999.f; haveXform = false; }
            if (tpipe != lastPipe)  { VK::Prof::CountPipelineBind(); vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tpipe); lastPipe = tpipe; lastVB = VK_NULL_HANDLE; lastIB = VK_NULL_HANDLE; }
            if (eset != lastMatSet) { VK::Prof::CountDescriptorBind(); vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tlay, 1, 1, &eset, 0, nullptr); lastMatSet = eset; }   // set 1 = EnvLight
            // Set 0 = terrain material: the tessellation eval samples uMask (soil
            // softness for the mud carve) — unbound set 0 here was a device-lost.
            if (mat->terrainSet != lastTerrSet0) { VK::Prof::CountDescriptorBind(); vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tlay, 0, 1, &mat->terrainSet, 0, nullptr); lastTerrSet0 = mat->terrainSet; }
            struct TPush { Fmatrix mvp; float uv[2]; float aref; float ds; } tp{};
            tp.mvp.mul(lightVP, it.xform);   // terrain xform = identity -> = lightVP (matches color's pc.mvp)
            // uvScale MUST match the color pass (1/1024 statics quant): the TES samples
            // uMask at vUV for the mud carve — zero uvScale carved a DIFFERENT surface
            // in the prepass -> z-fight (flickering black triangles along the trail).
            tp.uv[0] = tp.uv[1] = 1.0f / 1024.0f;
            tp.ds = mat->detailScale;
            vkCmdPushConstants(cmd, tlay, PipelineCache::GetPushStages(), 0, sizeof(TPush), &tp);
            VkBuffer vbT = fv->m_mesh.p_rm_Vertices->GetHandle();
            if (vbT != lastVB) { VkDeviceSize o = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &vbT, &o); lastVB = vbT; }
            VkBuffer ibT = fv->m_mesh.p_rm_Indices->GetHandle();
            if (ibT != lastIB || fv->m_mesh.iType != lastIType) { vkCmdBindIndexBuffer(cmd, ibT, 0, fv->m_mesh.iType); lastIB = ibT; lastIType = fv->m_mesh.iType; }
            const u32 fiT = it.iCountOverride ? it.iBaseOverride  : fv->m_mesh.iBase;
            const u32 icT = it.iCountOverride ? it.iCountOverride : fv->m_mesh.iCount;
            VK::Prof::CountDraw(1, 0); vkCmdDrawIndexed(cmd, icT, 1, fiT, (s32)fv->m_mesh.vBase, 0);
            ++nDraw;
            continue;
        }

        const float aref = mat ? mat->alphaRef : -1.f;
        const bool  at   = aref >= 0.f;
        if (!at && alphaTestedOnly) continue;   // opaque handled by GPU-driven shadow path
        if (at && (skipAlphaTested || layoutAT == VK_NULL_HANDLE
                   || mat->set == VK_NULL_HANDLE)) continue;

        VkPipeline pipe = at ? PipelineCache::GetDepthATPipeline(fv->m_mesh.vStride, fv->m_mesh.tcOffset, mvTarget)
                             : PipelineCache::GetDepthPipeline(fv->m_mesh.vStride, mvTarget);
        if (pipe == VK_NULL_HANDLE) continue;
        VkPipelineLayout layout = at ? layoutAT : layoutSolid;

        if (layout != lastLayout) {   // layout flip disturbs pushes + sets
            lastLayout = layout;
            haveXform  = false;
            lastMatSet = VK_NULL_HANDLE;
            lastAref   = -999.f;
        }
        if (pipe != lastPipe) {
            VK::Prof::CountPipelineBind(); vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            lastPipe = pipe; lastVB = VK_NULL_HANDLE; lastIB = VK_NULL_HANDLE;
        }

        if (at) {
            if (mat->set != lastMatSet) {
                VK::Prof::CountDescriptorBind(); vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &mat->set, 0, nullptr);
                lastMatSet = mat->set;
            }
            if (!haveXform || aref != lastAref || 0 != memcmp(&it.xform, &lastXform, sizeof(Fmatrix))) {
                ATPush push{};
                push.mvp.mul(lightVP, it.xform);
                push.uvScale[0] = 1.0f / 1024.0f;   // level statics' SHORT2 quant
                push.uvScale[1] = 1.0f / 1024.0f;
                push.aref       = aref;
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(push), &push);
                lastXform = it.xform; haveXform = true; lastAref = aref;
            }
        } else if (!haveXform || 0 != memcmp(&it.xform, &lastXform, sizeof(Fmatrix))) {
            Fmatrix lmvp; lmvp.mul(lightVP, it.xform);   // = it.xform · lightVP
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Fmatrix), &lmvp);
            lastXform = it.xform; haveXform = true;
        }

        VkBuffer vb = fv->m_mesh.p_rm_Vertices->GetHandle();
        if (vb != lastVB) { VkDeviceSize o = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &o); lastVB = vb; }

        VkBuffer ib = fv->m_mesh.p_rm_Indices->GetHandle();
        VkIndexType it2 = fv->m_mesh.iType;
        if (ib != lastIB || it2 != lastIType) { vkCmdBindIndexBuffer(cmd, ib, 0, it2); lastIB = ib; lastIType = it2; }

        const u32 firstIndex = it.iCountOverride ? it.iBaseOverride  : fv->m_mesh.iBase;
        const u32 indexCount = it.iCountOverride ? it.iCountOverride : fv->m_mesh.iCount;
        VK::Prof::CountDraw(1, 0); vkCmdDrawIndexed(cmd, indexCount, 1, firstIndex, (s32)fv->m_mesh.vBase, 0);
        ++nDraw;
    }

    static bool s_diagD = false;
    if (!s_diagD) { s_diagD = true; Msg("[VK Shadow] FlushDepth: items=%zu draws=%u", m_Items.size(), nDraw); }
}

// NORMAL G-buffer flush (GTAO statics normals). Mirrors FlushDepth's item walk,
// but binds the normal pipelines and writes the vertex normal into the SSAO
// normal RT instead of depth. Depth is TEST-only here (the pipelines have write
// off), so this stamps normals exactly where the prepass already declared the
// surface visible — the LEQUAL passes on equality because the position math is
// identical (same attribute @0, same mvp).
void RenderQueue::FlushNormals(VkCommandBuffer cmd, const Fmatrix& vp)
{
    if (m_Items.empty() || cmd == VK_NULL_HANDLE) return;
    VkPipelineLayout layoutSolid = PipelineCache::GetNormalLayout();
    VkPipelineLayout layoutAT    = PipelineCache::GetNormalATLayout();
    if (layoutSolid == VK_NULL_HANDLE) return;

    VkPipeline       lastPipe   = VK_NULL_HANDLE;
    VkPipelineLayout lastLayout = VK_NULL_HANDLE;
    VkDescriptorSet  lastMatSet = VK_NULL_HANDLE;
    VkBuffer    lastVB   = VK_NULL_HANDLE;
    VkBuffer    lastIB   = VK_NULL_HANDLE;
    VkIndexType lastIType = VK_INDEX_TYPE_MAX_ENUM;
    Fmatrix     lastXform; bool haveXform = false;
    float       lastAref = -999.f;
    u32 nDraw = 0;

    for (const DrawItem& it : m_Items)
    {
        auto* fv = static_cast<vkFVisual*>(it.vis);
        if (!fv || !fv->m_mesh.IsValid()) continue;
        if (!fv->m_mesh.p_rm_Vertices || !fv->m_mesh.p_rm_Indices) continue;

        WorldMaterial* mat = fv->m_pWorldMaterial ? fv->m_pWorldMaterial : WorldMaterialCache::GetDefault();
        if (mat && mat->isWmark) continue;   // baked decals never laid prepass depth either

        // Terrain: its prepass depth came from the tessellated + snow-displaced
        // pipeline. Re-rastering it flat would sit BEHIND that depth and fail
        // LEQUAL (holes), so leave terrain on the depth-reconstructed normal.
        if (mat && mat->isTerrain) continue;

        const float aref = mat ? mat->alphaRef : -1.f;
        const bool  at   = aref >= 0.f;
        if (at && (layoutAT == VK_NULL_HANDLE || mat->set == VK_NULL_HANDLE)) continue;

        VkPipeline pipe = at ? PipelineCache::GetNormalATPipeline(fv->m_mesh.vStride, fv->m_mesh.tcOffset)
                             : PipelineCache::GetNormalPipeline(fv->m_mesh.vStride);
        if (pipe == VK_NULL_HANDLE) continue;
        VkPipelineLayout layout = at ? layoutAT : layoutSolid;

        if (layout != lastLayout) {   // layout flip disturbs pushes + sets
            lastLayout = layout;
            haveXform  = false;
            lastMatSet = VK_NULL_HANDLE;
            lastAref   = -999.f;
        }
        if (pipe != lastPipe) {
            VK::Prof::CountPipelineBind(); vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            lastPipe = pipe; lastVB = VK_NULL_HANDLE; lastIB = VK_NULL_HANDLE;
        }
        if (at && mat->set != lastMatSet) {
            VK::Prof::CountDescriptorBind(); vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &mat->set, 0, nullptr);
            lastMatSet = mat->set;
        }

        if (!haveXform || aref != lastAref || 0 != memcmp(&it.xform, &lastXform, sizeof(Fmatrix))) {
            // Same identity test the colour flush uses for dynXform: a non-identity
            // xform means a dynamic prop, and the colour pass rotates its normal by
            // these very rows — matching it is what keeps geomN and the GTAO normal
            // the same vector.
            const bool dynXform = 0 != memcmp(&it.xform, &Fidentity, sizeof(Fmatrix));
            PipelineCache::NormalPush push{};
            push.mvp.mul(vp, it.xform);
            push.mi[0] = it.xform.i.x; push.mi[1] = it.xform.i.y; push.mi[2] = it.xform.i.z;
            push.mj[0] = it.xform.j.x; push.mj[1] = it.xform.j.y; push.mj[2] = it.xform.j.z;
            push.mi[3] = 1.0f / 1024.0f;   // uvScale (level statics' SHORT2 quant)
            push.mj[3] = 1.0f / 1024.0f;
            push.misc[0] = aref;
            push.misc[1] = dynXform ? 1.0f : 0.0f;
            vkCmdPushConstants(cmd, layout,
                               at ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                                  : VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(push), &push);
            lastXform = it.xform; haveXform = true; lastAref = aref;
        }

        VkBuffer vb = fv->m_mesh.p_rm_Vertices->GetHandle();
        if (vb != lastVB) { VkDeviceSize o = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &o); lastVB = vb; }

        VkBuffer ib = fv->m_mesh.p_rm_Indices->GetHandle();
        VkIndexType it2 = fv->m_mesh.iType;
        if (ib != lastIB || it2 != lastIType) { vkCmdBindIndexBuffer(cmd, ib, 0, it2); lastIB = ib; lastIType = it2; }

        const u32 firstIndex = it.iCountOverride ? it.iBaseOverride  : fv->m_mesh.iBase;
        const u32 indexCount = it.iCountOverride ? it.iCountOverride : fv->m_mesh.iCount;
        VK::Prof::CountDraw(1, 0); vkCmdDrawIndexed(cmd, indexCount, 1, firstIndex, (s32)fv->m_mesh.vBase, 0);
        ++nDraw;
    }

    // Coverage diag: NOT one-shot. `items=N draws=0` on the very first call is a
    // normal startup reading (the CPU queue can hold only terrain that frame), and
    // a one-shot print froze exactly that misleading snapshot. While r_ssao_debug
    // is on, re-print on the readback cadence so the number tracks what is really
    // being stamped. Pairs with WorldGPU::DrawNormals — most statics live there.
    static u32 s_diagN = 0;
    if (ps_r_ssao_debug && (s_diagN++ % 300) == 0)
        Msg("[VK SSAO] FlushNormals (CPU queue): items=%zu draws=%u", m_Items.size(), nDraw);
}

}  // namespace VK
