// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Per-frame draw queue.
//
// Phase 3 splits "scene assembled this DrawItem" from "GPU executed it".
// Visuals call Push() during traversal; Pass_World sorts and Flushes once.
// Flush is the only place that issues Vulkan draw/bind calls — all
// state-tracking (pipeline, VB, IB, material) lives there so we can collapse
// adjacent items that share state.

#pragma once
#include "vk_core.h"
#include "vk_pass_context.h"

class vkRender_Visual;
class CFrustum;

namespace VK {

struct DrawItem
{
    vkRender_Visual* vis     = nullptr;
    Fmatrix          xform   = {};      // world matrix (identity for level statics)
    float            lod     = 0.0f;
    u64              sortKey = 0;       // see makeSortKey() in vk_render_queue.cpp

    // Optional draw-range override. FProgressive picks a LOD-specific slice
    // of its own IB; iCountOverride!=0 tells Flush to use these instead of
    // the visual's full m_mesh.iBase/iCount. firstIndex passed to
    // vkCmdDrawIndexed becomes iBaseOverride (already includes m_mesh.iBase
    // + sw_offsets[lod]).
    u32              iBaseOverride  = 0;
    u32              iCountOverride = 0;

    // Sky-ambient occlusion for dynamic objects (1.0 = open sky / statics).
    // Stamped by Push() from the queue's current submit-hemi; Flush feeds it to
    // the world shader so indoor dynamics don't glow. See CRender::add_Visual.
    float            hemi           = 1.0f;

    // Translucent glass pane (WorldMaterial::isGlass): Push routes it to the
    // LATE glass list, drawn by Pass_WorldGlass after the whole opaque world.
    bool             lateGlass      = false;

    // Water body (WorldMaterial::isWater): Push routes it to the WATER list,
    // drawn by Pass_Water (own pipeline + shader) after the opaque world + sky.
    // Checked BEFORE lateGlass in Push — water is never also glass, but the
    // routing must be unambiguous.
    bool             lateWater      = false;
};

class RenderQueue
{
public:
    void Push(const DrawItem& item);
    // Raw append for REPLAYING items captured from a prior Push (rain AT-caster
    // cache): skips the hemi stamp and glass routing — the stored item already
    // went through them. Items appended in pre-sorted order need no SortByKey.
    void PushPresorted(const DrawItem& item) { m_Items.push_back(item); }
    void Clear();
    void SortByKey();
    void Flush(FrameContext& ctx);

    // Late translucent (glass) list — accumulated by Push across the statics AND
    // dynamics flushes, drawn once by Pass_WorldGlass after the opaque world
    // (GPU statics/trees/grass/sky), cleared there. ClearGlass() at frame start.
    void FlushGlass(FrameContext& ctx);
    void ClearGlass();
    bool HasGlass() const { return !m_GlassItems.empty(); }
    // Post-FlushGlass view (deduped): the particles pass re-draws these panes
    // into the heat-haze distortion RT = glass refraction (r_glass_refr).
    const xr_vector<DrawItem>& GlassItems() const { return m_GlassItems; }

    // WATER list — same lifetime discipline as the glass one (accumulated by
    // Push across the statics/dynamics flushes, consumed by Pass_Water, cleared
    // at the next frame's Pass_World start). Pass_Water owns its own draw loop
    // (its pipeline is not a PipelineCache world variant), so there is no
    // FlushWater here — it reads WaterItems() directly.
    void ClearWater() { m_WaterItems.clear(); }
    bool HasWater() const { return !m_WaterItems.empty(); }
    const xr_vector<DrawItem>& WaterItems() const { return m_WaterItems; }
    // Hierarchy double-submit dedup (see FlushGlass's note — a blended surface
    // drawn three times stacks its alpha into an opaque one). Called by Pass_Water.
    void DedupWater();

    // Sky-ambient occlusion stamped onto every subsequently-pushed item (until
    // changed). Set per dynamic object before its Submit; reset to 1.0 for
    // statics. Lets one shared queue carry per-object hemi without touching the
    // many Submit() overrides.
    void SetSubmitHemi(float h) { m_SubmitHemi = h; }

    // Off for a queue filled on a worker thread (the far shadow map's caster collect): Push then touches nothing outside
    // the queue -- no lightplanes cone registration (SynthCones is render-thread state). Default on.
    void SetSideEffects(bool b) { m_SideEffects = b; }

    // Heightmap tessellation is statics-only: dynamic visuals carry a model
    // xform, but the world VS emits MODEL-space vWorldPos, so the TES distance
    // factor (vs the camera) would be garbage for them. Pass_World flips this
    // off for the dynamic-visuals flush. Default on.
    void SetAllowTess(bool b) { m_AllowTess = b; }

    // AT statics color optimization (r_at_equal): Pass_World arms this for the
    // STATIC flush only (the prepass wrote these items' final depth), so aref>=0
    // items draw with depth EQUAL + no write. Dynamics are guarded per-item too
    // (non-identity xform), but keep it off for any flush whose items are not in
    // the depth prepass. Default off.
    void SetATEqual(bool b) { m_ATEqual = b; }

    // r_z_prepass companion (armed for the same static flush): opaque statics
    // drop the redundant color-pass depth write so the discard-bearing uber-FS
    // keeps EARLY-Z (discard + z-write ON = late-Z on real HW). Default off.
    void SetPrepassZ(bool b) { m_PrepassZ = b; }

    // Depth-only flush: binds PipelineCache depth pipelines (by stride) and
    // pushes mvp = item.xform · vp per item. No materials/colour — position
    // only. Used by the sun/spot shadow casters AND the camera depth prepass;
    // the prepass passes skipAlphaTested=true (a position-only shader can't
    // discard, so punch-out materials would poke opaque holes in the depth).
    // alphaTestedOnly=true skips OPAQUE casters (they're rendered by the
    // GPU-driven compute-cull + indirect path, vk_shadow_gpu) — the CPU queue
    // then draws only the alpha-tested cutout casters the GPU path can't.
    // cull (optional): per-item bounding-sphere vs frustum test — spot tiles /
    // point-cube faces collect their queue by range-SPHERE, but the actual
    // cone/face frustum is a small slice of it; this skips the rest.
    // mvTarget: this flush is the CAMERA depth prepass, whose scope carries the
    // RG16F motion attachment (r_mv_prepass) — bind the pipeline twins that declare
    // it. The call site has to say so: nothing in the argument list distinguishes
    // the camera prepass from a shadow target, and binding the wrong twin inside a
    // dynamic-rendering scope is a validation error, not a wrong picture.
    void FlushDepth(VkCommandBuffer cmd, const Fmatrix& vp, bool skipAlphaTested = false,
                    bool alphaTestedOnly = false, bool displaceTerrain = false,
                    const CFrustum* cull = nullptr, bool mvTarget = false);

    // NORMAL G-buffer flush for GTAO: the same statics this queue just laid into
    // the depth prepass, re-rastered into the SSAO normal RT with depth TEST-only
    // (LEQUAL, no write). Gives GTAO the vertex normal the colour pass shades
    // with instead of a depth-derivative guess — see world_normal.vert.
    // Terrain is deliberately NOT handled here: its prepass depth comes from the
    // tessellated + snow-displaced pipeline, so an untessellated re-raster would
    // fail LEQUAL and leave holes. Terrain keeps the depth-reconstructed normal
    // until it gets its own normal pipeline.
    void FlushNormals(VkCommandBuffer cmd, const Fmatrix& vp);

    size_t Size() const { return m_Items.size(); }

    // Read-only view of the queued items (diagnostics / GPU-driven overlap analysis).
    const xr_vector<DrawItem>& Items() const { return m_Items; }

private:
    xr_vector<DrawItem> m_Items;
    xr_vector<DrawItem> m_GlassItems;   // late translucent panes (see FlushGlass)
    xr_vector<DrawItem> m_WaterItems;   // water bodies (see Pass_Water)
    float               m_SubmitHemi = 1.0f;
    bool                m_AllowTess  = true;
    bool                m_ATEqual    = false;   // see SetATEqual
    bool                m_PrepassZ   = false;   // see SetPrepassZ
    bool                m_SideEffects = true;   // see SetSideEffects
};

extern RenderQueue g_RenderQueue;

// Encode a 64-bit sort key from the bits that change pipeline/material/VB
// binds. High bits change the rarest binds, low bits change cheapest —
// items sharing a prefix collapse into a single bind in Flush.
//
// Layout (MSB → LSB):
//   [59]      wmark — baked level decal (1 bit, sorts LAST: blends over opaque)
//   [58]      tess — heightmap-tessellated material (clusters tess pipelines)
//   [56]      depthTest                (1 bit)
//   [48..55]  stride                   (8 bits)
//   [40..47]  tcOffset                 (8 bits)
//   [16..39]  material descriptor hash (24 bits) — clusters by descriptor set
//   [0..15]   VB pointer hash          (16 bits) — sub-clusters by VB
u64 makeSortKey(u32 stride, u32 tcOffset, bool depthTest, const void* mat, const void* vb, bool wmark = false, bool tess = false);

}  // namespace VK
