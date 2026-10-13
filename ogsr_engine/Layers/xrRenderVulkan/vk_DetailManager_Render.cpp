// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — CDetailManager Session B render path.
//
// Per-frame flow:
//   1. PrepareFrame() — fill m_GfxConstants + m_Frame from camera/wind/density.
//   2. Clear VisibleSSBO + AtomicCounters; pre-fill IndirectCmdBuf with the
//      static {indexCount, firstIndex, vertexOffset, firstInstance} per type.
//   3. Barrier transfer→compute.
//   4. Bind gen compute pipeline; push 208 B; dispatch (groupCount, 1, 1).
//   5. Barrier compute→transfer; per-type vkCmdCopyBuffer of atomic count
//      into the corresponding indirect cmd's instanceCount field.
//   6. Barrier transfer→{vertex_input | indirect_draw_read}.
//   7. Begin dynamic-rendering pass (color = swapchain image LOAD,
//      depth = swapchain depth LOAD).
//   8. Per type: bind descriptor (diffuse), bind VBs (binding 0 = obj VB,
//      binding 1 = VisibleSSBO at section offset), bind IB,
//      vkCmdDrawIndexedIndirect(indirect, i*20, 1, 20).
//   9. End rendering; transition color back to TRANSFER_DST so Pass_Sky and
//      UI can still read the layout we left behind.

#include "stdafx.h"
#include "vk_env_snapshot.h"
#include "vk_render_view.h"
#include "vk_DetailManager.h"
#include "vk_color_space.h"   // ColorSpace::LinearizeRGB — env colours are authored sRGB
#include "vk_pass_context.h"
#include "vk_swapchain.h"
#include "vk_env_light.h"               // VK::EnvLight::GetCurrentSet — set 1 (shadow lookup)
#include "vk_cull.h"                    // VK::ExtractFrustumPlanes (shared with TreeManager)
#include "vk_profiler.h"                // VK_CPU_PROBE — per-frame CPU attribution
#include "HW_Vulkan.h"
#include "../xrRender/DetailFormat.h"   // DO_NO_WAVING

// Console cvar at GLOBAL scope: a block-scope extern inside namespace VK declares VK::*
// (MSVC resolves it to the global anyway; GCC/Clang follow the standard and fail at link).
extern int ps_r_profiler;

#include "../../xr_3da/IGame_Persistent.h"
#include "../../xr_3da/IGame_Level.h"           // g_pGameLevel, CurrentEntity, Objects
#include "../../xr_3da/xr_object.h"             // CObject Position/Radius/getVisible
#include "../../xr_3da/xr_object_list.h"        // o_count / o_get_by_iterator
#include "../../xr_3da/Environment.h"

// Console-tunable knobs from xrRender shared console. Owned by
// vk_console_min.cpp; we just read them per frame.
extern int   ps_r__detail_radius;       // metres, default 100, range 70..300
extern float ps_current_detail_density; // 0..1, default 0.6 (lower = denser)
extern float ps_r_grass_thin;            // nfs25 r_grass_thin: full-density radius, metres (0 = off)
extern float ps_current_detail_scale;   // r__detail_scale, 0.7..1.5 — per-item size multiplier
extern float ps_r_sun_boost;            // r_sun_boost — global sun multiplier (see vk_env_light)
extern float ps_r_grass_aref;           // r_grass_aref — grass alpha-test cutoff (lower = fatter blades)
extern float ps_r_grass_asharp;         // r_grass_asharp — mip alpha compensation (far grass keeps coverage)
extern int   ps_r_grass_nowave;         // r_grass_nowave — respect DO_NO_WAVING (1) or wind everything (0)
extern float ps_r_sun_grass_dist;       // r_sun_grass_dist — sun grass-shadow reach (sizes the caster set)
#ifdef __SWITCH__
extern "C" int g_xr_nx_diag;           // nfs32 device.cpp: 151 = grass through a 1x1 scissor, 152 = old R1 grass FS
#endif
extern int ps_r_sun_grass, ps_r_spot_grass, ps_r_point_grass, ps_r_vsm, ps_r_vsm_grass;   // nfs33: caster consumers
extern int ps_r_grass_gen;   // r_grass_gen (vk_console_min.cpp): 0 full generator (PC), 1 lean, 2 lean2, 3 tuft cache
extern int ps_r_grass_r1;    // r_grass_r1  (vk_console_min.cpp): 0 full grass FS (PC), 1 R1 FS, 2 R1 per vertex
extern int   ps_r_grass_gi_inst;        // r_grass_gi_inst — 1 = probe field once per TUFT (gen compute) instead of ~16 atlas taps per fragment

namespace VK
{

// ----- Frustum plane extraction (D3D-style row-major mat4) ------------------
// Delegates to the shared VK::ExtractFrustumPlanes (vk_cull.h). Kept as a member
// so existing call sites and the header declaration are unchanged.
void CDetailManager::ExtractFrustumPlanes(const Fmatrix& m, Fvector4 planes[6]) const
{
    VK::ExtractFrustumPlanes(m, planes);
}

// RENDER_THREAD_PLAN step 8 -- see the header. Same walk PrepareFrame did, on the extract side.
void CDetailManager::ExtractInteractors()
{
    const Fvector eye = RV().vCameraPosition;
    m_Interactors.cur().v[0].set(eye.x, eye.y, eye.z, 1.2f);
    for (u32 i = 1; i < MAX_GRASS_INTERACTORS; ++i)
        m_Interactors.cur().v[i].set(0.0f, 0.0f, 0.0f, 0.0f);
    if (g_pGameLevel) {
        constexpr float kMaxDistSq = 15.0f * 15.0f;
        CObject* player = g_pGameLevel->CurrentEntity();

        struct Slot { float distSq; CObject* obj; } closest[3] = {
            { kMaxDistSq, nullptr }, { kMaxDistSq, nullptr }, { kMaxDistSq, nullptr }
        };

        const u32 oc = g_pGameLevel->Objects.o_count();
        for (u32 i = 0; i < oc; ++i) {
            CObject* O = g_pGameLevel->Objects.o_get_by_iterator(i);
            if (!O || O == player || !O->getVisible()) continue;
            const Fvector p = O->Position();
            const float dx = p.x - eye.x, dz = p.z - eye.z;
            const float d2 = dx * dx + dz * dz;
            if (d2 >= closest[2].distSq) continue;
            // Insertion-sort into the 3-slot ladder.
            if (d2 < closest[0].distSq) {
                closest[2] = closest[1]; closest[1] = closest[0];
                closest[0] = { d2, O };
            } else if (d2 < closest[1].distSq) {
                closest[2] = closest[1];
                closest[1] = { d2, O };
            } else {
                closest[2] = { d2, O };
            }
        }

        for (int c = 0; c < 3; ++c) {
            if (!closest[c].obj) continue;
            Fvector p = closest[c].obj->Position();
            float r = closest[c].obj->Radius();
            r = _min(_max(r, 0.5f), 2.0f);
            m_Interactors.cur().v[1 + c].set(p.x, p.y, p.z, r);
        }
    }
    m_Interactors.cur().frame = RV().frameIndex;
}

// ----- PrepareFrame ---------------------------------------------------------
void CDetailManager::PrepareFrame(const VK::FrameContext& ctx)
{
    m_Frame.valid = false;
    if (!m_bCreated || !m_GenPipeline || !m_GfxPipeline) return;
    if (objects.empty()) return;
    if (!ctx.viewProj) return;

    // Roll wind history for the MV overlay: m_GfxConstants still holds LAST frame's
    // wind here (it is overwritten with this frame's values below), which is exactly
    // the previous pose the grass MV pass must reproject to capture the sway delta.
    if (m_MvWindPrevValid) {
        m_MvWindParamsPrev = m_GfxConstants.wind_params;
        m_MvWindAnimPrev   = m_GfxConstants.wind_anim;
    }

    // Camera position from device transform (world inverse-transformed origin).
    // Approx: read it from RV().vCameraPosition for now — engine sets that
    // global before Render(). (TODO: thread it through FrameContext explicitly.)
    const Fvector eye = RV().vCameraPosition;

    // Density / d_size: read from `r__detail_density` console var. Lower
    // density = bigger d_size = more grass per slot. Clamped 2..10 so the
    // shader's posPerSlot stays in a sane range.
    const float density = _max(ps_current_detail_density, 0.1f);
    m_Frame.d_size      = u32(_max(2.0f, _min(10.0f, ceilf(dm_slot_size / density))));
    m_Frame.posPerSlot  = (m_Frame.d_size + 1) * (m_Frame.d_size + 1);

    // Slot range driven by `r__detail_radius` cvar (metres). Default 100m.
    // `r__detail_radius 0` disables grass entirely (totalPositions=0).
    const float gen_radius = float(_max(ps_r__detail_radius, 0));
    if (gen_radius < 1.0f) {
        m_Frame.totalPositions = 0;
        m_Frame.valid          = false;
        return;
    }
    const int gen_half  = int(floorf(gen_radius / dm_slot_size));
    const int slot_eye_x = int(floorf(eye.x / dm_slot_size + 0.5f));
    const int slot_eye_z = int(floorf(eye.z / dm_slot_size + 0.5f));
    m_Frame.slotMinSX  = slot_eye_x - gen_half;
    m_Frame.slotMinSZ  = slot_eye_z - gen_half;
    m_Frame.slotCountX = gen_half * 2 + 1;
    m_Frame.slotCountZ = gen_half * 2 + 1;

    m_Frame.totalPositions = u32(m_Frame.slotCountX) * u32(m_Frame.slotCountZ) * m_Frame.posPerSlot;
    m_Frame.groupCount     = (m_Frame.totalPositions + 255) / 256;

    // Refresh GenUBO — written every frame because slot range / camera change.
    if (m_GenUBO && m_GenUBO->IsMapped()) {
        DetailGenUBO* u = (DetailGenUBO*)m_GenUBO->m_Mapped;
        u->hmOriginX     = m_HMOriginX;
        u->hmOriginZ     = m_HMOriginZ;
        u->hmInvScaleX   = (m_HMWorldSizeX > 0.0f) ? (1.0f / m_HMWorldSizeX) : 0.0f;
        u->hmInvScaleZ   = (m_HMWorldSizeZ > 0.0f) ? (1.0f / m_HMWorldSizeZ) : 0.0f;
        u->totalPositions= m_Frame.totalPositions;
        u->numObjTypes   = u32(objects.size());
        u->outputCapacity= m_OutputCapacity;   // live (auto-grown) capacity
        u->posPerSlot    = m_Frame.posPerSlot;
        u->dtOffsX       = float(dtH.offs_x);
        u->dtOffsZ       = float(dtH.offs_z);
        u->dtSizeX       = float(dtH.size_x);
        u->dtSizeZ       = float(dtH.size_z);
        u->slotSize      = dm_slot_size;
        // The gen shader multiplies every item's scale by this — the original
        // engine does `Item.scale *= ps_current_detail_scale` in Decompress
        // (DetailManager_Decompress.cpp:189). It was hardcoded 1.0 here, so the
        // user's r__detail_scale (1.2 in user.ltx) was ignored and ALL grass /
        // bushes / flowers rendered ~17% smaller than R4 — "не пышно".
        u->detailHeight  = ps_current_detail_scale;
        u->hmWidth       = m_HeightmapW;
        u->hmHeight      = m_HeightmapH;
        m_GenUBO->Flush();
    }

    // Graphics push constants
    m_GfxConstants.mViewProj = *ctx.viewProj;
    m_time_pos += ctx.dt;

    // Env-driven wind:
    //   - direction taken from `Environment().CurrentEnv->wind_direction` (radians),
    //   - amplitude/speed lerped between `swing_desc[0]` (normal) and
    //     `swing_desc[1]` (fast) by `wind_velocity`-derived strength factor.
    // Engine internal `wind_velocity` is NOT a real m/s — it sits in tens-to-
    // hundreds, scaled `*0.001` everywhere it's used (Environment.cpp:490).
    // Multiplying that into the shader's displacement amplitude (which is in
    // metres) yanked grass 10+ m per wave → "rubber-band stretching" look.
    Fvector  wind_dir{};      wind_dir.set(0.7f, 0.0f, 0.7f);   // fallback SE
    float    wind_strength = 0.0f;                             // 0..1 lerp factor
    float    windDirAngle  = 0.0f;                             // SSFX: raw wind_direction (radians)
    float    windVel       = 0.0f;                             // SSFX: raw wind_velocity (engine units)
    Fvector3 windAnim{};                                       // SSFX: accumulated wind drift (Environment.wind_anim)
    // Env lighting for the grass (colorize the baked sun scalar to match the
    // world ground). Neutral fallback when env isn't up yet. (The hemi colour
    // is NOT pushed any more — no shader ever read it; the ambient side comes
    // from the sky SH + L.ambient the fragment already samples.)
    m_GfxConstants.vSunColor.set(0.6f, 0.6f, 0.6f, 0.0f);
    if (g_pGamePersistent) {
        auto& env = ES();
        windAnim = env.wind_anim;   // shared accumulator (R4/SSFX): drifts with wind dir·velocity·dt
        if (env.CurrentEnv) {
            const float a = env.CurrentEnv->wind_direction;
            windDirAngle = a;
            windVel      = env.CurrentEnv->wind_velocity;
            wind_dir.set(_cos(a), 0.0f, _sin(a));
            // Map `wind_velocity` (engine units, typically 0..1000) into a
            // 0..1 lerp factor. *0.001 matches Environment.cpp's own scale,
            // then clamp.
            wind_strength = clampr(env.CurrentEnv->wind_velocity * 0.001f, 0.0f, 1.0f);
            m_GfxConstants.vSunColor.set(env.CurrentEnv->sun_color.x, env.CurrentEnv->sun_color.y, env.CurrentEnv->sun_color.z, 0.0f);
        }
    }
    // Linearise before the boost, same rule and same order as the LightUBO fill in
    // vk_env_light — grass bypasses that UBO and gets its env colours by push
    // constant, so it needs its own conversion or the blades would stay gamma-lit
    // while the ground under them went linear. (.w is a pad here, not colour.)
    VK::ColorSpace::LinearizeRGB(m_GfxConstants.vSunColor);

    // The same global sun boost the world receives — vk_env_light premultiplies
    // it into the LightUBO; the grass sun colour travels via push constants.
    m_GfxConstants.vSunColor.mul(ps_r_sun_boost);

    // SSFX flow-map wind (screenspace_wind.h). The shader builds the dir vector +
    // speed from these; drift comes from the shared Environment.wind_anim
    // accumulator. Defaults match the SSFX "10 - Wind" MCM (grass animspeed 9.5,
    // turbulence 1.4, push 1.5, wave 0.4; min wind speed 0.1).
    m_GfxConstants.wind_params.set(windDirAngle, windVel, 0.0f, 1.0f);   // .w = wind scale, overridden per detail type in the draw loop (DO_NO_WAVING → 0)
    m_GfxConstants.wsetup_grass.set(9.5f, 1.4f, 1.5f, 0.4f);
    m_GfxConstants.wind_anim.set(windAnim.x, windAnim.y, windAnim.z, 0.1f);
    m_GfxConstants.vConsts.set(ps_r_grass_aref, ps_r_grass_asharp, 0.0f, 0.0f);   // x = grass alpha cutoff, y = mip alpha sharpen; zw spare (sun.y/floor were never read)

    // First frame: seed prev == cur so the grass MV pass reports zero sway motion
    // (nothing to reproject against yet).
    if (!m_MvWindPrevValid) {
        m_MvWindParamsPrev = m_GfxConstants.wind_params;
        m_MvWindAnimPrev   = m_GfxConstants.wind_anim;
        m_MvWindPrevValid  = true;
    }

    // Character interaction: vInteractors[0] = player, [1..3] = nearest 3 NPCs within 15 m (walked in
    // ExtractInteractors, PREPARE phase). Empty slots have radius=0 so the shader skips them.
    if (m_Interactors.cur().frame == RV().frameIndex) {
        for (u32 i = 0; i < MAX_GRASS_INTERACTORS; ++i)
            m_GfxConstants.vInteractors[i] = m_Interactors.cur().v[i];
    } else {
        m_GfxConstants.vInteractors[0].set(eye.x, eye.y, eye.z, 1.2f);
        for (u32 i = 1; i < MAX_GRASS_INTERACTORS; ++i)
            m_GfxConstants.vInteractors[i].set(0.0f, 0.0f, 0.0f, 0.0f);
    }

    m_Frame.valid = true;
}

// ----- Render ---------------------------------------------------------------
// ============================================================================
// BuildHZB — max-reduce the scene depth buffer into the Hi-Z mip chain.
//
// Runs at the top of Render(): depth already holds Pass_World statics/dynamics,
// so grass occludes against this frame's world. Transitions depth to a sampled
// layout for the build and restores DEPTH_ATTACHMENT_OPTIMAL before the grass
// draw / Pass_Sky. No-op (safe fallback to the white dummy) when the pipeline
// failed to build.
// ============================================================================
bool CDetailManager::HZBPrevUsable() const
{
    return HZBReady() && m_HZBBuiltFrame != 0xFFFFFFFFu
        && m_HZBBuiltFrame + 1 == RV().frameIndex
        && m_HZBDepthExtent.width  == Swapchain.m_DepthExtent.width
        && m_HZBDepthExtent.height == Swapchain.m_DepthExtent.height;
}

void CDetailManager::BuildHZB(VK::FrameContext& ctx)
{
    if (Swapchain.m_DepthImage == VK_NULL_HANDLE) return;

    // The HZB caches a view of the scene depth (m_DepthSampleView) and is sized to
    // depth/2. When the depth is resized (DLSS render<display, or a window resize) that
    // view dangles at the freed image AND the HZB dims no longer match — sampling it
    // faults the GPU (DEVICE_LOST). Rebuild against the current depth extent. Rare
    // (only on a resolution change); DestroyHZB waits idle + recreates the descriptors.
    if (m_HZBImage != VK_NULL_HANDLE &&
        (m_HZBDepthExtent.width  != Swapchain.m_DepthExtent.width ||
         m_HZBDepthExtent.height != Swapchain.m_DepthExtent.height)) {
        DestroyHZB();
        CreateHZB(Swapchain.m_DepthExtent.width, Swapchain.m_DepthExtent.height);
        // The gen/caster sets' binding 6 still points at the DESTROYED old HZB view
        // — sampling it reads garbage and the gen shader culls all grass past
        // HZB_NEAR_SKIP (the "grass only in a 25 m bubble" bug under DLSS, where the
        // render-res depth differs from the swapchain extent CreateHZB used at init).
        UpdateGenDescriptors();
    }

    if (m_HZBPipeline == VK_NULL_HANDLE || m_HZBImage == VK_NULL_HANDLE) return;
    if (m_HZBDescSets.size() != m_HZBMipCount) return;

    // Build the pyramid at most once per frame. With r_hzb_cull, Pass_World builds
    // it from the PREPASS depth (before its color pass) for WorldGPU::CullColor;
    // grass Render then calls BuildHZB again at the top of Render() → this guard
    // makes the second call a no-op (otherwise the depth is max-reduced twice and,
    // worse, grass would rebuild from post-color depth, undoing the early build).
    if (m_HZBBuiltFrame == RV().frameIndex) return;
    m_HZBBuiltFrame = RV().frameIndex;

    const VkCommandBuffer cmd = ctx.cmd;
    auto mipDim = [](u32 base, u32 lvl) { const u32 v = base >> lvl; return v ? v : 1u; };

    // WAR: order the previous frame's HZB reads (grass gen sample at gen binding 6
    // + WorldGPU::CullColor's occlusion sample) before this frame's max-reduce
    // writes — the pyramid is single-buffered across frames in flight. (Grass's own
    // Render() WAR covers its buffers but now runs after this earlier build.)
    {
        VkMemoryBarrier war{};
        war.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        war.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        war.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &war, 0, nullptr, 0, nullptr);
    }

    // 1) depth DEPTH_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL (compute sample).
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = Swapchain.m_DepthImage;
        b.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        b.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // 2) Per-mip max-reduce dispatch. Global compute→compute barrier between
    //    passes serializes each mip's writes before the next mip (and the gen
    //    dispatch) reads them.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_HZBPipeline);
    for (u32 m = 0; m < m_HZBMipCount; ++m) {
        const u32 dstW = mipDim(m_HZBWidth, m);
        const u32 dstH = mipDim(m_HZBHeight, m);

        HZBBuildPush pc{};
        if (m == 0) {
            pc.srcW = s32(m_HZBDepthExtent.width);
            pc.srcH = s32(m_HZBDepthExtent.height);
            pc.srcMip = 0u;
            pc.isFirstPass = 1u;
        } else {
            pc.srcW = s32(mipDim(m_HZBWidth, m - 1));
            pc.srcH = s32(mipDim(m_HZBHeight, m - 1));
            pc.srcMip = m - 1u;
            pc.isFirstPass = 0u;
        }
        pc.dstW = s32(dstW);
        pc.dstH = s32(dstH);
        pc.dstMip = m;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_HZBPipelineLayout,
                                0, 1, &m_HZBDescSets[m], 0, nullptr);
        vkCmdPushConstants(cmd, m_HZBPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (dstW + 7u) / 8u, (dstH + 7u) / 8u, 1u);

        VkMemoryBarrier mb{};
        mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    // 3) depth back → DEPTH_ATTACHMENT_OPTIMAL for the grass draw + Pass_Sky.
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = Swapchain.m_DepthImage;
        b.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        b.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }
}

// r_grass_gen clamped to what was built at level load (each step falls back to the one below), plus the nx windows.
int CDetailManager::GenMode() const
{
    int m = ps_r_grass_gen;
#ifdef __SWITCH__
    if (g_xr_nx_diag == 171) m = 0;                    // the full generator (+ HZB)
    else if (g_xr_nx_diag == 200 && m == 2) m = 1;     // the nfs34 lean instead of lean2
    else if (g_xr_nx_diag == 211) m = 3;               // the tuft cache
#endif
    if (m >= 3 && !(m_CacheBakePipeline && m_CacheCullPipeline && m_GenPipelineLean2)) m = 2;
    if (m == 2 && m_GenPipelineLean2 == VK_NULL_HANDLE) m = 1;
    if (m == 1 && m_GenPipelineLean == VK_NULL_HANDLE) m = 0;
    return m < 0 ? 0 : (m > 3 ? 3 : m);
}

// nfs41: run the generation half of Render now (frame start, no render pass open) -- the Grass pass then only draws.
void CDetailManager::GenerateEarly(VK::FrameContext& ctx)
{
    // Only the generators that need no depth (no grass HZB: lean2 / the cache) can run before the world draws.
    if (GenMode() < 2) return;
#ifdef __SWITCH__
    if (g_xr_nx_diag == 170) return;   // nx window: lean + HZB -- the pyramid needs this frame's depth
#endif
    m_GenOnly = true;
    Render(ctx);
    m_GenOnly = false;
    static bool s_logged = false;
    if (!s_logged && m_GenEarlyFrame == RV().frameIndex) { s_logged = true; Msg("[VK Grass] generation at the frame start: ON"); }
}

void CDetailManager::Render(VK::FrameContext& ctx)
{
    VK_CPU_PROBE("GrassCPU");
    if (!m_GenOnly && m_GenEarlyFrame == RV().frameIndex) {   // nfs41: generated at the frame start -> draw only
        const u32 nObjE = u32(objects.size());
        if (nObjE && m_Frame.valid && ctx.cmd != VK_NULL_HANDLE && m_VisibleSSBO && m_IndirectCmdBuf)
            RenderDraw(ctx, nObjE, m_OutputCapacity / _max(nObjE, 1u));
        return;
    }
    // Apply a pending output-buffer GROW before anything this frame touches it.
    // Rare event (demand exceeded the per-type section → grass holes): wait the
    // GPU idle (in-flight frames still read the old buffer), swap the buffer,
    // rewrite the gen descriptors. One hitch per growth step, then never again
    // at this demand level — a modder can pour any density and the buffer follows.
    // NOTE: nothing recorded earlier THIS frame reads m_VisibleSSBO (shadow/VSM
    // passes read the CASTER set) — the grass draw + MV overlay bind it below.
    if (m_PendingCapacity > m_OutputCapacity && m_VisibleSSBO) {
        const u32 newCap = _min(m_PendingCapacity, GPU_OUTPUT_CAP_MAX);
        m_PendingCapacity = 0;
        if (newCap > m_OutputCapacity) {
            VK_DeviceWaitIdleLocked();
            m_VisibleSSBO->Destroy();
            m_VisibleSSBO->Create(u64(newCap) * sizeof(DetailInstance),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT  |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
            m_OutputCapacity = newCap;
            UpdateGenDescriptors();
            Msg("[VK Grass] output buffer GROWN to %u instances (%.1f MB), per-type section %u",
                m_OutputCapacity, u64(m_OutputCapacity) * sizeof(DetailInstance) / (1024.0f * 1024.0f),
                m_OutputCapacity / _max(u32(objects.size()), 1u));
        }
    }

    PrepareFrame(ctx);
    if (!m_Frame.valid) return;
    if (ctx.cmd == VK_NULL_HANDLE) return;
    if (m_VisibleSSBO == nullptr || m_IndirectCmdBuf == nullptr || m_AtomicCounters == nullptr) return;

    const VkCommandBuffer cmd = ctx.cmd;

    // Frames in flight (VK_FRAMES_IN_FLIGHT = 3) share ONE set of grass GPU
    // buffers (VisibleSSBO / AtomicCounters / IndirectCmdBuf / HZB mips). The
    // previous frame's GPU work may still be reading them (instance vertex
    // fetch, indirect fetch, gen-compute HZB reads) when this frame starts
    // overwriting — a write-after-read hazard that shows up as flickering
    // grass patches while the camera moves (the slot window shifts, so the
    // overwritten data no longer matches what frame N-1 meant to draw).
    // WAR needs only an execution dependency: order all prior-frame reads
    // before this frame's transfer/compute writes.
    // VERTEX_SHADER is in the src scope because the VSM grass page pass (earlier
    // THIS frame) PULLS the caster instance rows from m_CasterSSBO as a storage
    // buffer in the vertex shader (pair compaction dropped the instance-rate
    // attributes, which VERTEX_INPUT used to cover) — without it the gen below
    // overwrites the rows mid-read and tufts cast garbage/cut shadows.
    {
        VkMemoryBarrier b{};
        b.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        // TRANSFER in src: the PREVIOUS frame's diagnostics copy (counters →
        // m_OverflowRB) must finish before THIS frame's counter fill zeroes the
        // source — without it the readback raced the clear and reported ~0 usage.
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &b, 0, nullptr, 0, nullptr);
    }

    // Build the Hi-Z occlusion pyramid from this frame's depth before the gen
    // compute samples it (binding 6). Restores depth to DEPTH_ATTACHMENT_OPTIMAL.
    // nfs33 (all platforms): the CASTER set has no reader unless a grass shadow consumer is on -- the spot beam /
    // sun cascades / point cubes (DrawGrassSpotCasters, GrassCull: r_spot/sun/point_grass) or the VSM grass pages
    // (r_vsm && r_vsm_grass). With none of them the second full generator dispatch, its counter fill and its copy are
    // pure waste (Switch: -1.16 ms). A consumer switched on reads one stale frame, as all grass casters already do.
    // r_grass_gen (GenMode): 0 = full generator + grass HZB (PC), >= 1 = lean (no grass pyramid: the reduced
    // generators skip the HZB test, nfs34 measured the pyramid at 0.64 ms on the Switch).
    int  genMode  = GenMode();
    bool nxHzb    = genMode == 0;
    bool nxCaster = ps_r_sun_grass || ps_r_spot_grass || ps_r_point_grass || (ps_r_vsm && ps_r_vsm_grass);
    bool nxFreeze = false;
    bool hzbBuild = true;
#ifdef __SWITCH__
    // nx windows: 160 forces the casters (A/B), 161 skips the HZB build (the generator then reads last frame's
    // pyramid), 162 freezes the visible set (no reset, no dispatch, no count copy: last frame's instances are drawn
    // again -- with the camera still, the same picture), 170 = the lean generator WITH the pyramid.
    if (g_xr_nx_diag == 160) nxCaster = true;
    nxFreeze = g_xr_nx_diag == 162;
    if (g_xr_nx_diag == 170 && genMode >= 1) { genMode = 1; nxHzb = true; }
    if (g_xr_nx_diag == 161) hzbBuild = false;
    {
        static int s_lastCaster = -1;
        if (int(nxCaster) != s_lastCaster) {
            s_lastCaster = int(nxCaster);
            Msg("[nx grass] casters %s (sun_grass %d spot_grass %d point_grass %d vsm %d vsm_grass %d diag %d)",
                nxCaster ? "GENERATED" : "skipped", ps_r_sun_grass, ps_r_spot_grass, ps_r_point_grass, ps_r_vsm,
                ps_r_vsm_grass, g_xr_nx_diag);
        }
    }
#endif
    const bool nxLeanGen = genMode >= 1;
    if (nxHzb && hzbBuild)
        BuildHZB(ctx);
    const u32 nObj = u32(objects.size());
    const u32 sectionSize = m_OutputCapacity / _max(nObj, 1u);

    // -------------------------------------------------------------------
    // 1) Reset atomic counters only. We DON'T clear VisibleSSBO — the gen
    // shader's atomic-append path only writes the first `count` slots per
    // type, and the indirect draw's instanceCount = `count` so old data
    // beyond `count` is never sampled. Skipping the 96 MB fill saves a
    // huge chunk of memory bandwidth per frame.
    //
    // IndirectCmdBuf was pre-filled in CreateGpuBuffers; only instanceCount
    // (offset 4 within each 20 B record) gets patched per frame by the
    // atomic→indirect copy below.
    // -------------------------------------------------------------------
    // nfs35 (r_grass_gen >= 1): counters reset / published by detail_counters.comp instead of fill + copy (casters
    // keep the old path). Measured 0.00 ms on the Switch -- kept with the reduced generators, never on the full one.
    bool nxNoXfer = nxLeanGen && !nxCaster && m_CountersPipeline != VK_NULL_HANDLE;
#ifdef __SWITCH__
    if (g_xr_nx_diag == 180) nxNoXfer = false;   // nx window: the old fill + copy + readback
#endif
    auto countersDispatch = [&](u32 mode) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CountersPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_GenPipelineLayout, 0, 1, &m_GenDescSet, 0, nullptr);
        const u32 pcv[2] = { mode, nObj };
        vkCmdPushConstants(cmd, m_GenPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcv), pcv);
        vkCmdDispatch(cmd, 1, 1, 1);
    };
    if (!nxFreeze && nxNoXfer)
        countersDispatch(0u);
    else if (!nxFreeze)
        vkCmdFillBuffer(cmd, m_AtomicCounters->GetHandle(), 0, VK_WHOLE_SIZE, 0u);
    if (m_CasterAtomic && nxCaster)
        vkCmdFillBuffer(cmd, m_CasterAtomic->GetHandle(), 0, VK_WHOLE_SIZE, 0u);

    // Barrier transfer-write → compute-read/write.
    {
        VkMemoryBarrier b{};
        b.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;   // nfs35: or the counters reset
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }

    // -------------------------------------------------------------------
    // 2) Gen compute dispatch.
    // -------------------------------------------------------------------
    DetailGenPushConstants gpc{};
    gpc.viewProj   = *ctx.viewProj;
    ExtractFrustumPlanes(gpc.viewProj, gpc.frustumPlanes);
    gpc.cameraPos.set(RV().vCameraPosition.x, RV().vCameraPosition.y, RV().vCameraPosition.z, m_time_pos);
    // Fade params: start² | limit² | range² | density. Limit = current
    // gen radius; start = 60% of limit so grass fades over the last ~40m.
    {
        const float limit = float(_max(ps_r__detail_radius, 1));
        const float start = limit * 0.6f;
        gpc.fadeParams.set(start * start, limit * limit,
                           (limit * limit - start * start),
                           ps_current_detail_density);
    }
    gpc.slotMinSX  = m_Frame.slotMinSX;
    gpc.slotMinSZ  = m_Frame.slotMinSZ;
    gpc.slotCountX = m_Frame.slotCountX;
    gpc.slotCountZ = m_Frame.slotCountZ;

    // z = vertical focal scale P11 = 1/tan(fovY/2), the term the shared HZB
    // footprint needs (hzb_test.glsl). Same expression as the tree/LOD/world cull
    // sites — grass used to omit it entirely and under-sample its own footprint.
    const float hzbFocal = 1.f / _max(0.05f, tanf(deg2rad(RV().fFOV) * 0.5f));
    gpc.casterParams.set(0.f, 0.f, nxHzb ? hzbFocal : 0.f, _max(ps_r_grass_thin, 0.f));   // visible pass: frustum+HZB cull as usual; nfs25: w = thinning radius

    // r_grass_gen: 0 = full generator, 1 = nfs34 lean (slot pre-cull, no per-tuft GI, subgroup append), 2 = nfs38
    // lean2 (lean without local memory / HZB code), 3 = nfs40 tuft cache (falls back to lean2). Lean/lean2 never
    // fill the per-tuft GI field: they are meant with r_grass_r1 >= 1, whose shaders do not read it.
    VkPipeline genPipe = (genMode >= 2) ? m_GenPipelineLean2 : (genMode == 1) ? m_GenPipelineLean : m_GenPipeline;
    // nfs40 (r_grass_gen 3): the R1-style tuft cache instead of the generator -- slots entering the W x W window are
    // baked once (CPU diff of the toroidal cell grid -> bake list), every frame only the cached tufts are culled. Same
    // tufts as lean2; falls back to it with thinning on or for the caster pass. nfs40: 0.14 ms SLOWER than lean2.
    const bool nxCache = genMode == 3 && ps_r_grass_thin <= 0.f && !nxCaster
                      && m_Frame.slotCountX == m_Frame.slotCountZ
                      && CacheEnsure(u32(m_Frame.slotCountX), m_Frame.posPerSlot);
#ifdef __SWITCH__
    {
        static int s_lastCache = -1;
        if (int(nxCache) != s_lastCache) { s_lastCache = int(nxCache); Msg("[nx grass] tuft cache %s", nxCache ? "ON" : "off (lean2)"); }
    }
#endif
    if (nxCache && !nxFreeze) {
        const s32 W = m_Frame.slotCountX;
        const u32 cells = u32(W) * u32(W);
        const u32 region = (m_CacheRing++ % 4u)   /* = kCacheRing in vk_DetailManager.cpp */ * cells;
        u32* dst = m_CacheBakePtr + size_t(region) * 4u;
        u32 n = 0;
        for (s32 lz = 0; lz < W; ++lz)
            for (s32 lx = 0; lx < W; ++lx) {
                const s32 sx = m_Frame.slotMinSX + lx, sz = m_Frame.slotMinSZ + lz;
                const u32 c  = u32(((sx % W) + W) % W) + u32(((sz % W) + W) % W) * u32(W);
                if (m_CacheCellCoord[c * 2u] == sx && m_CacheCellCoord[c * 2u + 1u] == sz) continue;
                m_CacheCellCoord[c * 2u] = sx; m_CacheCellCoord[c * 2u + 1u] = sz;
                dst[n * 4u + 0u] = c; dst[n * 4u + 1u] = u32(sx); dst[n * 4u + 2u] = u32(sz); dst[n * 4u + 3u] = 0u;
                ++n;
            }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CachePipelineLayout, 0, 1, &m_GenDescSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CachePipelineLayout, 2, 1, &m_CacheDescSet, 0, nullptr);
        if (n) {
            m_CacheBakeList->Flush();
            DetailGenPushConstants bpc = gpc;
            bpc.slotMinSX = s32(region); bpc.slotMinSZ = s32(n); bpc.slotCountX = W; bpc.slotCountZ = W;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CacheBakePipeline);
            vkCmdPushConstants(cmd, m_CachePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bpc), &bpc);
            vkCmdDispatch(cmd, (n * m_Frame.posPerSlot + 255u) / 256u, 1, 1);
            VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &b, 0, nullptr, 0, nullptr);
#ifdef __SWITCH__
            static u32 s_bakeLog = 0;
            if (n > 200u || (++s_bakeLog % 120u) == 1u) Msg("[nx grass] tuft cache baked %u slots", n);
#endif
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CacheCullPipeline);
        vkCmdPushConstants(cmd, m_CachePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(gpc), &gpc);
        vkCmdDispatch(cmd, m_Frame.groupCount, 1, 1);
    }
    if (!nxCache) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, genPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_GenPipelineLayout,
                            0, 1, &m_GenDescSet, 0, nullptr);
    // set 1 = shared EnvLight (L + the DDGI probe field): the generator samples
    // the field once per tuft into the instance record (r_grass_gi_inst). Bound
    // once — the caster dispatch below rebinds only set 0 on the same layout, so
    // this stays. Updated by Pass_World, which runs before us (same guarantee
    // the forward draw's env bind already leans on).
    if (VkDescriptorSet envSetC = VK::EnvLight::GetCurrentSet())
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_GenPipelineLayout,
                                1, 1, &envSetC, 0, nullptr);
    vkCmdPushConstants(cmd, m_GenPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(gpc), &gpc);
    if (!nxFreeze)
        vkCmdDispatch(cmd, m_Frame.groupCount, 1, 1);
    }   // nfs40: !nxCache

    // CASTER pass: same generator, distance-only cull (no frustum/HZB) into the
    // caster buffers — the shadow passes (spot beam / sun cascades / point cubes
    // / VSM grass) need blades that are OUTSIDE the camera frustum too: walk
    // toward a wall and the campfire grass behind you still dapples it. Radius =
    // the farthest a consumer reaches (sun grass dist; near lights sit < 40 m +
    // ~15 m range), capped by the visual fade limit.
    if (nxCaster && m_CasterDescSet != VK_NULL_HANDLE && m_CasterSSBO && m_CasterAtomic && m_CasterIndirectBuf) {
        DetailGenPushConstants cpc = gpc;
        const float lim = float(_max(ps_r__detail_radius, 1));
        const float cr  = _min(_max(55.f, ps_r_sun_grass_dist), lim);
        cpc.casterParams.set(cr * cr, float(GPU_CASTER_CAPACITY), hzbFocal, 0.f);   // focal unused here (caster pass skips HZB) — kept consistent so enabling it later can't read a zero
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_GenPipelineLayout,
                                0, 1, &m_CasterDescSet, 0, nullptr);
        vkCmdPushConstants(cmd, m_GenPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(cpc), &cpc);
        vkCmdDispatch(cmd, m_Frame.groupCount, 1, 1);
    }

    // Barrier compute-write → transfer-read (atomics → indirect copy).
    {
        VkMemoryBarrier b{};
        b.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT
                        | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;   // nfs35: or the publish dispatch
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }

    // -------------------------------------------------------------------
    // 3) Atomic-count → indirect.instanceCount per type.
    //    instanceCount field is at byte offset 4 within the 20-byte cmd.
    // -------------------------------------------------------------------
    if (nObj > 0 && !nxFreeze && nxNoXfer) {
        countersDispatch(1u);
    } else if (nObj > 0 && !nxFreeze) {
        xr_vector<VkBufferCopy> copies(nObj);
        for (u32 i = 0; i < nObj; ++i) {
            copies[i].srcOffset = i * sizeof(u32);
            copies[i].dstOffset = i * sizeof(VkDrawIndexedIndirectCommand) + 4;
            copies[i].size      = sizeof(u32);
        }
        vkCmdCopyBuffer(cmd, m_AtomicCounters->GetHandle(),
                        m_IndirectCmdBuf->GetHandle(),
                        nObj, copies.data());
        if (nxCaster && m_CasterAtomic && m_CasterIndirectBuf)   // caster counters → caster indirect
            vkCmdCopyBuffer(cmd, m_CasterAtomic->GetHandle(),
                            m_CasterIndirectBuf->GetHandle(),
                            nObj, copies.data());
    }

    // Usage / overflow diagnostics + AUTO-GROW. The host mirror holds
    // counters[0..63] = per-type used and [64..127] = per-type dropped
    // (a few frames stale — fine). A non-zero drop = grass holes; request a
    // grow to observed demand + 25% headroom (applied at next Render start).
    if (m_OverflowRB && m_OverflowPtr) {
        u32 usedTotal = 0, needSection = 0, topType = 0, dropped = 0;
        for (u32 i = 0; i < nObj; ++i) {
            const u32 need = m_OverflowPtr[i] + m_OverflowPtr[GPU_MAX_OBJ_TYPES + i];
            usedTotal += m_OverflowPtr[i];
            dropped   += m_OverflowPtr[GPU_MAX_OBJ_TYPES + i];
            if (need > needSection) { needSection = need; topType = i; }
        }
        if (dropped && m_OutputCapacity < GPU_OUTPUT_CAP_MAX) {
            const u64 want = u64(needSection) * 5u / 4u * nObj;             // demand + 25% headroom
            m_PendingCapacity = _max(m_PendingCapacity, u32(_min(want, (u64)GPU_OUTPUT_CAP_MAX)));
        }
        if (dropped && RV().dwTimeGlobal > m_OverflowLastLog + 2000) {
            m_OverflowLastLog = RV().dwTimeGlobal;
            Msg("![VK Grass] section OVERFLOW: %u instances/frame dropped (top type %u needs %u, section=%u) — %s",
                dropped, topType, needSection, sectionSize,
                m_OutputCapacity < GPU_OUTPUT_CAP_MAX ? "growing buffer" : "AT HARD CAP, lower density/radius");
        }
        // Live consumption line for tuning/modding (r_profiler cadence, 5 s).
        if (ps_r_profiler > 0 && RV().dwTimeGlobal > m_UsageLastLog + 5000) {
            m_UsageLastLog = RV().dwTimeGlobal;
            Msg("[VK Grass] usage: %u/%u instances (%.0f%%), top type %u: %u/%u section, %u types",
                usedTotal, m_OutputCapacity, 100.0f * usedTotal / _max(m_OutputCapacity, 1u),
                topType, m_OverflowPtr[topType], sectionSize, nObj);
        }
        VkBufferCopy oc{ 0, 0, (VkDeviceSize)(2u * GPU_MAX_OBJ_TYPES) * sizeof(u32) };
        if (!nxNoXfer || (RV().frameIndex & 7u) == 0)   // nfs35: the diagnostics readback every 8th frame
            vkCmdCopyBuffer(cmd, m_AtomicCounters->GetHandle(), m_OverflowRB->GetHandle(), 1, &oc);
#ifdef __SWITCH__
        {   // nfs34: how many tufts the frame actually draws (readback a few frames old), and with which generator
            static u32 s_nxGrassLog = 0;
            if ((++s_nxGrassLog % 600) == 1) {
                u32 nonEmpty = 0;
                for (u32 i = 0; i < nObj; ++i) if (m_OverflowPtr[i]) ++nonEmpty;
                Msg("[nx grass] instances %u (%u of %u types) | positions %u in %u slots | gen %s hzb %d | radius %d",
                    usedTotal, nonEmpty, nObj, m_Frame.totalPositions, m_Frame.slotCountX * m_Frame.slotCountZ,
                    nxLeanGen ? "lean" : "v2", int(nxHzb), ps_r__detail_radius);
            }
        }
#endif
    }

    // Barrier: VisibleSSBO ↦ vertex-input read; IndirectCmdBuf ↦ indirect read.
    {
        VkMemoryBarrier b{};
        b.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            0, 1, &b, 0, nullptr, 0, nullptr);
    }

    if (m_GenOnly) { m_GenEarlyFrame = RV().frameIndex; return; }   // nfs41: early generation ends here
    RenderDraw(ctx, nObj, sectionSize);
}

// nfs41: the draw half of Render (unchanged), callable after an early generation.
void CDetailManager::RenderDraw(VK::FrameContext& ctx, u32 nObj, u32 sectionSize)
{
    const VkCommandBuffer cmd = ctx.cmd;
    // -------------------------------------------------------------------
    // 4) Begin dynamic-rendering pass (load color + depth from world pass).
    //    Single-layout convention: color is already COLOR_ATTACHMENT and depth
    //    DEPTH_ATTACHMENT for the whole frame; ExecutePasses inserted the
    //    inter-pass barrier before us. No swapchain layout transition here.
    //    (The compute→vertex/indirect barrier above is our own and stays.)
    //    Shared overlay begin (depth STORE — kept for sky) — see vk_pass_context.h.
    // -------------------------------------------------------------------
    VK::BeginOverlayRendering(cmd, ctx);

    // r_grass_gi_inst 1 (default): the FS that reads the per-tuft field from the
    // instance record instead of walking the probe atlases per fragment. Both
    // pipelines share layout/VS/resources, so the cvar flips live mid-run — the
    // only A/B this renderer accepts.
    VkPipeline gfxPipe = (ps_r_grass_gi_inst != 0 && m_GfxPipelineInst != VK_NULL_HANDLE)
                       ? m_GfxPipelineInst : m_GfxPipeline;
    // r_grass_r1 (Switch preset): 1 = nfs22 R1 grass FS, 2 = nfs32 R1V (light/fade/fog per vertex, falls back to 1).
    // Live: both twins share layout/vertex input/resources with the pair above.
    int grassR1 = ps_r_grass_r1;
#ifdef __SWITCH__
    if (g_xr_nx_diag == 152 && grassR1 >= 2) grassR1 = 1;   // nx window: the old R1 FS
    if (g_xr_nx_diag == 151) { const VkRect2D one{ { 0, 0 }, { 1u, 1u } }; vkCmdSetScissor(cmd, 0, 1, &one); }
#endif
    if (grassR1 >= 1 && m_GfxPipelineR1 != VK_NULL_HANDLE) gfxPipe = m_GfxPipelineR1;
    if (grassR1 >= 2 && m_GfxPipelineR1V != VK_NULL_HANDLE) gfxPipe = m_GfxPipelineR1V;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gfxPipe);
    vkCmdPushConstants(cmd, m_GfxPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(m_GfxConstants), &m_GfxConstants);

    // set 1 = shared env lighting (sun_vp + sun shadow map) — updated by Pass_World.
    if (VkDescriptorSet envSet = VK::EnvLight::GetCurrentSet())
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_GfxPipelineLayout, 1, 1, &envSet, 0, nullptr);

    // Per-type: bind diffuse, bind VBs, bind IB, draw indirect.
    for (u32 i = 0; i < nObj; ++i) {
        const VK::CDetail* obj = objects[i];
        if (!obj || !obj->m_VertexBuffer || !obj->m_IndexBuffer) continue;

        if (i < m_GfxDescSets.size() && m_GfxDescSets[i] != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_GfxPipelineLayout, 0, 1, &m_GfxDescSets[i], 0, nullptr);
        }

        // Per-detail-type wind scale: DO_NO_WAVING models (tiny shoots in asphalt)
        // stay static like R4 (push 0 into wind_params.w); normal grass waves (1).
        const float windScale = (ps_r_grass_nowave && (obj->m_Flags & DO_NO_WAVING)) ? 0.0f : 1.0f;
        // ⚠Stages must match the DECLARED range, not the stage that happens to read
        // the field. This layout declares ONE range covering VS+FS (vk_DetailManager.cpp),
        // so a partial update naming only VERTEX is illegal even though only the vertex
        // shader reads wind_params — VUID-vkCmdPushConstants-offset-01796, caught 16-08.
        // (The MV pass below looks identical but is NOT the same case: its layout
        // declares a VERTEX-only range, so VERTEX-only there is correct.)
        vkCmdPushConstants(cmd, m_GfxPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           offsetof(DetailGfxPushConstants, wind_params) + 3u * sizeof(float),
                           sizeof(float), &windScale);

        VkBuffer vbs[2] = { obj->m_VertexBuffer->GetHandle(), m_VisibleSSBO->GetHandle() };
        VkDeviceSize off[2] = { 0, u64(i) * u64(sectionSize) * sizeof(DetailInstance) };
        vkCmdBindVertexBuffers(cmd, 0, 2, vbs, off);
        vkCmdBindIndexBuffer(cmd, obj->m_IndexBuffer->GetHandle(), 0, VK_INDEX_TYPE_UINT16);

        vkCmdDrawIndexedIndirect(cmd,
            m_IndirectCmdBuf->GetHandle(),
            i * sizeof(VkDrawIndexedIndirectCommand),
            1, sizeof(VkDrawIndexedIndirectCommand));
    }

    vkCmdEndRendering(cmd);
    // No exit transition: image stays COLOR_ATTACHMENT for Pass_Sky / UI.

    // Diagnostic — once.
    static bool s_diag = false;
    if (!s_diag) {
        Msg("[VK Grass] First Render: types=%u sectionSize=%u totalPositions=%u groupCount=%u",
            nObj, sectionSize, m_Frame.totalPositions, m_Frame.groupCount);
        Msg("[VK Grass] Tunables: r__detail_radius=%d  r__detail_density=%.2f  posPerSlot=%u",
            ps_r__detail_radius, ps_current_detail_density, m_Frame.posPerSlot);
        s_diag = true;
    }
}

// ---- Wind-sway motion-vector overlay ----------------------------------------
// Re-draws THIS frame's visible grass (same VisibleSSBO + indirect the forward
// pass filled) into the MV target, reprojecting the cur AND prev wind pose so the
// sway carries true screen motion. Runs INSIDE MotionVec::ExecuteDynamic's render
// pass (MV target + scene depth already bound, viewport set). Depth-tests LEQUAL
// (no write) against the grass depth the forward pass wrote — same VP + wind + bias
// → bit-match. No PrepareFrame here (the forward Render already ran it this frame;
// re-running would clobber the prev-wind roll).
void CDetailManager::RenderMotion(const VK::FrameContext& ctx, const Fmatrix& curVP, const Fmatrix& prevVP,
                                  float jitterNdcX, float jitterNdcY)
{
    if (!m_Frame.valid) return;                       // grass didn't draw this frame
    if (ctx.cmd == VK_NULL_HANDLE) return;
    if (m_VisibleSSBO == nullptr || m_IndirectCmdBuf == nullptr) return;

    CreateMotionPipeline();                           // lazy — needs m_GfxDescLayout (built by Load)
    if (m_MotionPipeline == VK_NULL_HANDLE) return;

    const VkCommandBuffer cmd = ctx.cmd;
    const u32 nObj = u32(objects.size());
    const u32 sectionSize = m_OutputCapacity / _max(nObj, 1u);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_MotionPipeline);

    DetailMotionPushConstants pc{};
    pc.curVP            = curVP;
    pc.prevVP           = prevVP;
    pc.wind_params      = m_GfxConstants.wind_params;    // .w patched per-type below
    pc.wsetup_grass     = m_GfxConstants.wsetup_grass;
    pc.wind_anim        = m_GfxConstants.wind_anim;
    pc.wind_params_prev = m_MvWindParamsPrev;
    pc.wind_anim_prev   = m_MvWindAnimPrev;
    pc.jitter.set(jitterNdcX, jitterNdcY, 0.0f, 0.0f);   // re-applied to gl_Position (jitter-free MV)
    vkCmdPushConstants(cmd, m_MotionPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

    for (u32 i = 0; i < nObj; ++i) {
        const VK::CDetail* obj = objects[i];
        if (!obj || !obj->m_VertexBuffer || !obj->m_IndexBuffer) continue;

        if (i < m_GfxDescSets.size() && m_GfxDescSets[i] != VK_NULL_HANDLE)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_MotionPipelineLayout, 0, 1, &m_GfxDescSets[i], 0, nullptr);

        // Same per-type wind scale as the forward pass (DO_NO_WAVING → 0 → the tiny
        // static shoots write pure camera MV, matching their static forward pose).
        const float windScale = (ps_r_grass_nowave && (obj->m_Flags & DO_NO_WAVING)) ? 0.0f : 1.0f;
        vkCmdPushConstants(cmd, m_MotionPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           offsetof(DetailMotionPushConstants, wind_params) + 3u * sizeof(float),
                           sizeof(float), &windScale);

        VkBuffer vbs[2] = { obj->m_VertexBuffer->GetHandle(), m_VisibleSSBO->GetHandle() };
        VkDeviceSize off[2] = { 0, u64(i) * u64(sectionSize) * sizeof(DetailInstance) };
        vkCmdBindVertexBuffers(cmd, 0, 2, vbs, off);
        vkCmdBindIndexBuffer(cmd, obj->m_IndexBuffer->GetHandle(), 0, VK_INDEX_TYPE_UINT16);

        vkCmdDrawIndexedIndirect(cmd, m_IndirectCmdBuf->GetHandle(),
                                 i * sizeof(VkDrawIndexedIndirectCommand),
                                 1, sizeof(VkDrawIndexedIndirectCommand));
    }

    static bool s_diagMv = false;
    if (!s_diagMv) { s_diagMv = true; Msg("[VK Grass] first wind-sway MV render: types=%u", nObj); }
}

// ---- VSM grass-shadow caster access (read-only; see vk_vsm.cpp) -------------
// Shadow passes read the CASTER set (distance-only cull — blades behind the
// camera still cast); fall back to the visible set if the caster one is absent.
VkBuffer CDetailManager::Vsm_VisibleSSBO() const {
    if (m_CasterSSBO && m_CasterIndirectBuf) return m_CasterSSBO->GetHandle();
    return m_VisibleSSBO ? m_VisibleSSBO->GetHandle() : VK_NULL_HANDLE;
}
VkBuffer CDetailManager::Vsm_IndirectBuf() const {
    if (m_CasterSSBO && m_CasterIndirectBuf) return m_CasterIndirectBuf->GetHandle();
    return m_IndirectCmdBuf ? m_IndirectCmdBuf->GetHandle() : VK_NULL_HANDLE;
}
u32      CDetailManager::Vsm_TypeCount()   const { return u32(objects.size()); }
u32      CDetailManager::Vsm_SectionSize() const {
    const u32 cap = (m_CasterSSBO && m_CasterIndirectBuf) ? GPU_CASTER_CAPACITY : m_OutputCapacity;
    return cap / _max(u32(objects.size()), 1u);
}
u32      CDetailManager::Vsm_VertexStride() const { return (u32)sizeof(VK::CDetail::Vertex); }

bool CDetailManager::Vsm_TypeMesh(u32 i, VkBuffer& vb, VkBuffer& ib, u32& indexCount) const
{
    if (i >= objects.size()) return false;
    const VK::CDetail* obj = objects[i];
    if (!obj || !obj->m_VertexBuffer || !obj->m_IndexBuffer || obj->m_IndexCount == 0) return false;
    vb = obj->m_VertexBuffer->GetHandle();
    ib = obj->m_IndexBuffer->GetHandle();
    indexCount = obj->m_IndexCount;
    return true;
}

VkDescriptorSetLayout CDetailManager::Vsm_GfxSetLayout() const { return m_GfxDescLayout; }
VkDescriptorSet       CDetailManager::Vsm_TypeDiffuseSet(u32 i) const { return (i < m_GfxDescSets.size()) ? m_GfxDescSets[i] : VK_NULL_HANDLE; }

void CDetailManager::Vsm_WindPush(Fvector4& wind, Fvector4& wsetup, Fvector4& anim) const
{
    wind   = m_GfxConstants.wind_params;
    wsetup = m_GfxConstants.wsetup_grass;
    anim   = m_GfxConstants.wind_anim;
}

float CDetailManager::Vsm_TypeWindScale(u32 i) const
{
    if (i >= objects.size() || !objects[i]) return 1.f;
    return (ps_r_grass_nowave && (objects[i]->m_Flags & DO_NO_WAVING)) ? 0.f : 1.f;
}

}  // namespace VK
