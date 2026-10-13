// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - WATER pass. See vk_pass_water.h for the whole story of why
// the game's water was black and where the seams are.

#include "stdafx.h"
#include "vk_env_snapshot.h"
#include "vk_render_view.h"
#include "vk_rendering.h"          // VK::RenderingBuilder
#include "vk_pass_water.h"
#include "vk_render_queue.h"       // g_RenderQueue - the WATER list
#include "vk_Visual.h"             // vkFVisual / m_mesh
#include "vk_world_material.h"     // WorldMaterial::isWater
#include "vk_swapchain.h"          // Swapchain.m_DepthView (the sampled descriptor)
#include "vk_framegraph.h"         // SceneDepthToSampled/ToAttachment — tracked scene-depth invariant
#include "vk_scene_color.h"        // HDR scene target format
#include "vk_image.h"              // CreateImage2D / CreateImageView (the scene copy)
#include "vk_shaders.h"            // g_ShaderManager (SPIR-V loader)
#include "vk_buffer.h"           // CVulkanBuffer - the per-frame tuning UBO
#include "vk_shadow.h"             // ShadowMap::GetSampler (clamped point sampler)
#include "vk_barriers.h"           // ImageBarrier (depth â sampled and back)
#include "vk_command_buffer.h"     // CommandManager.GetCurrentFrame()
#include "vk_env_light.h"          // EnvLight::GetSetLayout / GetCurrentSet (set 1)
#include "vk_pipeline_cache.h"     // PipelineCache::TessAvailable
#include "vk_gfx_pipeline.h"       // VK::GfxPipelineBuilder
#include "vk_descriptors.h"        // VK::DescriptorWriter / MakeSetLayout
#include "vk_compute_util.h"       // VK::MakePipelineLayout
#include "vk_profiler.h"           // Prof zones
#include "vk_water_ripple.h"     // WaterRipple - the interactive heightfield sim
#include "vk_water_fft.h"        // WaterFFT - the Tessendorf spectral wave field
#include "vk_pass_skinned.h"      // Skinned_CollectFeet - who is standing in the water
#include "HW_Vulkan.h"
#include "../../xr_3da/device.h"           // RV().fTimeGlobal
#include "../../xr_3da/IGame_Level.h"      // g_pGameLevel - rebuild the level water map on level change
#include "../../xr_3da/IGame_Persistent.h" // ES() - wind
#include <algorithm>
#include <cmath>
#include <cstring>   // std::memcmp / memcpy (identity-xform check, UBO upload)

// ---- cvars (vk_console_min.cpp) -------------------------------------------
// Namespace is `r_wtr_*`, deliberately NOT `r_water_*`: that family already
// belongs to the parked shallow-water PUDDLE simulation (vk_water_sim.cpp), and
// two unrelated things answering to one prefix is how a tuning session ends up
// turning the wrong knob for twenty minutes.
extern int   ps_r_wtr;
extern int   ps_r_wtr_lean;       // Switch twin: water_lean.frag, no tess / refraction / grid
extern int   ps_r_wtr_debug;
extern float ps_r_wtr_wave;
extern float ps_r_wtr_calm;        // what a dead calm still leaves of that wave
extern float ps_r_wtr_scale;
extern float ps_r_wtr_speed;
extern float ps_r_wtr_murk;
extern float ps_r_wtr_refl;
extern float ps_r_wtr_glint;
extern float ps_r_wtr_detail;
extern float ps_r_wtr_rough;
extern float ps_r_wtr_shore;
extern float ps_r_wtr_foam;
extern float ps_r_wtr_foam_w;
extern float ps_r_wtr_micro;
extern float ps_r_wtr_wind;
extern Fvector3 ps_r_wtr_color;
extern int   ps_r_wtr_sim;         // interactive ripple sim master
extern float ps_r_wtr_sim_slope;   // how much of the sim reaches the NORMAL
extern float ps_r_wtr_sim_height;  // ...and the DISPLACEMENT
extern int   ps_r_wtr_sim_pools;   // draw the pool mask that bounds the ripple field
extern int   ps_r_wtr_grid;        // quads per side of the dense near-field mesh (0 = off)
extern int   ps_r_wtr_sim_lid;     // 1 = lid comes from the collision model, not this rasterizer
extern float ps_r_wtr_step;        // splat strength of a footstep (m)
extern float ps_r_wtr_bow;         // bow wave: water shoved ahead of a moving body
extern float ps_r_wtr_wet;         // shore wetness (gates building the level water map)
extern int   ps_r_wtr_occl;        // gate the refraction snapshot on last frame's occlusion query
extern float ps_r_wtr_tess;        // max tessellation factor (0 = flat, no tess)
extern float ps_r_wtr_tess_near;
extern float ps_r_wtr_tess_far;
extern float ps_r_wtr_disp;        // vertical displacement gain (m per unit height)
extern float ps_r_wtr_refract;     // bottom refraction offset
extern float ps_r_wtr_caustic;     // caustic strength (0 = off)
extern float ps_r_wtr_caustic_p;   // caustic sharpness (exponent)
extern float ps_r_wtr_swash;       // METRES the run-up climbs above the still line
extern float ps_r_wtr_swash_t;     // seconds between arriving crests
extern float ps_r_wtr_surf;        // whitewater strength (0 = no shore break)
extern float ps_r_wtr_surf_h;      // offshore height of the arriving wave (m)
extern float ps_r_wtr_surf_len;    // metres between crests
extern float ps_r_wtr_film;        // metres of water in the run-up sheet
extern float ps_r_wtr_fetch;       // how literally to take the size of a pool (0 = every pool is open sea)
extern float ps_r_wtr_shelter;     // how much a roof overhead calms the water (0 = roofs do nothing)
extern float ps_r_wtr_murk_still;  // murk multiplier for sheltered (stagnant) water
// SPECTRAL WAVES (vk_water_fft). The module owns the rest of the r_wtr_fft_*
// family; these four are the ones the SHADERS need and so travel in the UBO.
extern float ps_r_wtr_fft_gain;    // how much of the transformed field to use (0 = none)
extern float ps_r_wtr_fft_foam;    // whitewater gain on the Jacobian
extern float ps_r_wtr_fft_fetch;   // body span (m) at which the spectral field reaches full strength
extern float ps_r_wtr_fft_slope;   // slope gain — shading response, independent of displacement

namespace VK {

namespace {

constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;

// Push carries the matrix plus the one value that is genuinely PER-SURFACE and
// therefore cannot live in the UBO: the fetch (the span of this body of water).
// The tuning block itself outgrew the 128-byte push guarantee when the shoreline
// work landed and moved to a per-frame UBO; 64 + 16 still fits under it.
struct WaterPush {
    Fmatrix mvp;
    float   basin[4];   // x = fetch in metres (0 = open water), yzw reserved
};
static_assert(sizeof(WaterPush) == 80, "water push: matrix + per-surface basin");

// Must match the WaterParams block in water.frag.glsl (std140: all vec4).
struct WaterParams {
    float p0[4];   // time, wave slope, base freq, speed
    float p1[4];   // wind.x, wind.z, extinction k, rain
    float p2[4];   // murk rgb, reflection strength
    float p3[4];   // detail fade dist, base roughness, glitter, debug
    float p4[4];   // shore fade (m), foam strength, foam width (m), micro slope
    float p5[4];   // tessMax, tessNear (m), tessFar (m), displacement gain
    float p6[4];   // sim on, tile origin X, tile origin Z, tile size (m)
    float p7[4];   // sim slope gain, sim height gain, 1/texels, reserved
    float p8[4];   // camera world position (the TESC needs it and has no light UBO)
    float p9[4];   // refraction strength, caustic strength, caustic exponent, scene-copy valid
    float p10[4];  // run-up (m), shelter, murk still, seconds between crests
    float p11[4];  // shore break: crest spacing (m), whitewater, wave height (m), enable
    // SPECTRAL WAVES (vk_water_fft). p12.xyz = the three cascade tile sizes in
    // metres — the shaders turn one world XZ into three sets of tile coordinates
    // and need all three; .w = how much of the transformed field to use.
    float p12[4];
    // x = enabled (0 = the analytic swell carries everything, as before),
    // y = whitewater gain for the Jacobian foam, z = the body span at which the
    // spectral field reaches full strength (m), w = slope gain.
    float p13[4];
    float rainVP[16];  // sky-occlusion map view-proj - the TESE cannot include light_ubo
};

bool                  s_inited   = false;
bool                  s_failed   = false;
VkDescriptorSetLayout s_setLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_pool      = VK_NULL_HANDLE;
VkDescriptorSet       s_set[kFramesInFlight]{};
VkPipelineLayout      s_layout    = VK_NULL_HANDLE;
VkPipeline            s_pipe[2]   = {};      // [0] = tcOffset 24 (lmap), [1] = 28 (vert-lit)
VkPipeline            s_pipeLean[2] = {};    // same split, r_wtr_lean
VkShaderModule        s_vs = VK_NULL_HANDLE, s_fs = VK_NULL_HANDLE;
VkShaderModule        s_fsLean = VK_NULL_HANDLE;   // water.frag -DWATER_LEAN (optional)
VkShaderModule        s_tcs = VK_NULL_HANDLE, s_tes = VK_NULL_HANDLE;
// NEAR-FIELD GRID: the mesh this level's water does not have. See
// water_grid.vert.glsl - no vertex buffer, no tessellation, drawn straight from
// gl_VertexIndex over the ripple tile.
VkShaderModule        s_gridVS   = VK_NULL_HANDLE;
VkPipeline            s_gridPipe = VK_NULL_HANDLE;
// Pool mask (see BuildMaskPipeline): its own tiny pipeline and layout, because
// it shares neither descriptor sets nor push layout with the surface pass.
VkShaderModule        s_maskVS = VK_NULL_HANDLE, s_maskFS = VK_NULL_HANDLE;
// WATER_MASK_SURF twin: same shader, plus the second target the shore wetness
// runs on (live surface + ground). A separate module and pipeline rather than one
// that always writes location 1, because the LEVEL-wide map has one attachment
// and a 2048Â² second target would be 16 MB of nothing.
VkShaderModule        s_maskSurfFS   = VK_NULL_HANDLE;
VkPipeline            s_maskSurfPipe = VK_NULL_HANDLE;
bool                  s_surfFirst    = true;   // live-surface image still UNDEFINED
VkPipelineLayout      s_maskLayout = VK_NULL_HANDLE;
VkPipeline            s_maskPipe   = VK_NULL_HANDLE;
bool                  s_maskFirst  = true;   // mask image still UNDEFINED
// set 0 for the mask pipelines: the tuning UBO and nothing else - see Init.
VkDescriptorSetLayout s_maskUboSetLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_maskUboPool      = VK_NULL_HANDLE;
VkDescriptorSet       s_maskUboSet[kFramesInFlight]{};

// Push block shared by both mask pipelines and both passes that drive them.
// â  Must match `PC` in water_mask.{vert,frag}.glsl: vec4 @0, mat4 @16, vec4 @80.
struct MaskPush {
    float   tile[4];   // xy = origin (world XZ), z = 1/size (m), w = STRICT
    Fmatrix rainVP;
    float   basin[4];  // x = fetch (m), y = metres per unit ortho depth,
                       // z = how far above the still line the ground map is trusted
};
static_assert(sizeof(MaskPush) == 96, "push block must stay in step with water_mask.*.glsl");
// LID pass: the level's opaque statics, drawn top-down, keeping the LOWEST
// surface above the water (MIN blend). Its own set (the water mask, sampled) and
// its own pipeline.
VkShaderModule        s_lidVS = VK_NULL_HANDLE, s_lidFS = VK_NULL_HANDLE;
VkPipelineLayout      s_lidLayout = VK_NULL_HANDLE;
VkPipeline            s_lidPipe   = VK_NULL_HANDLE;
VkDescriptorSetLayout s_lidSetLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_lidPool   = VK_NULL_HANDLE;
VkDescriptorSet       s_lidSet    = VK_NULL_HANDLE;
bool                  s_lidFirst  = true;
bool                  s_tess = false;      // device has tessellation AND the stages loaded
// Per-frame-in-flight tuning UBO (host-visible, written once per frame).
CVulkanBuffer         s_ubo[kFramesInFlight];
// Half-res copy of the HDR scene, taken between the opaque world and the water
// draw: the bottom as something we can SAMPLE (and therefore bend, tint and
// light with caustics) instead of merely blending over.
VkImage               s_refImg   = VK_NULL_HANDLE;
VmaAllocation         s_refAlloc = VK_NULL_HANDLE;
VkImageView           s_refView  = VK_NULL_HANDLE;
VkExtent2D            s_refExt{};
VkSampler             s_refSamp  = VK_NULL_HANDLE;
bool                  s_refFirst = true;

// ---- IS ANY WATER PIXEL ACTUALLY ON SCREEN? -------------------------------
// The two surfaces this level ships are horizontal plates ~1 km across sitting
// 16 m BELOW the road; their bounding spheres reach the frustum from almost
// anywhere, so frustum culling never rejects them and the pass ran in full on a
// forest track with no water in sight. What that cost was not the draw (0.02 ms,
// its fragments lose the depth test) but the REFRACTION SNAPSHOT above it: a blit
// of the entire 2560x1440 HDR target down to half res, plus two layout
// transitions on the frame's main colour attachment, every frame, so that water
// which is not there can sample a riverbed that is not there.
//
// The honest test for "is any of it visible" is the one the hardware already
// does: an OCCLUSION QUERY around the draw itself. It costs nothing, needs no
// pyramid of our own, and answers about the real tessellated surface rather than
// a bounding box. It is read ONE FRAME LATE by design - the alternative is a
// stall, and water appearing or disappearing takes many frames of camera motion.
// The cost of being wrong for one frame is a surface refracting a snapshot one
// frame old.
// The DRAW is never skipped, only the snapshot: skipping the draw would kill the
// signal that decides when to resume.
VkQueryPool           s_occlPool = VK_NULL_HANDLE;
bool                  s_occlHas[kFramesInFlight]{};   // slot holds a query from an earlier frame
bool                  s_waterSeen = true;             // optimistic: frame one takes the snapshot
u32                   s_occlSamples = 0;              // last reading, for the diagnostic line
u32                   s_occlFrames = 0;               // frames since the last report
u32                   s_occlBlits  = 0;               // ...of those, ones that took the snapshot
u32                   s_occlLogAt  = 0;               // next report, on the REAL clock

// (Re)create the scene copy at half the render resolution. Returns false when
// refraction is unavailable - the shader then falls back to the plain blend.
bool EnsureRefract(VkExtent2D scene)
{
    const VkExtent2D want{ std::max(scene.width / 2u, 1u), std::max(scene.height / 2u, 1u) };
    if (s_refImg != VK_NULL_HANDLE && want.width == s_refExt.width && want.height == s_refExt.height)
        return true;
    if (s_refView) { vkDestroyImageView(VulkanHW.m_Device, s_refView, nullptr); s_refView = VK_NULL_HANDLE; }
    if (s_refImg)  { vmaDestroyImage(VulkanHW.m_Allocator, s_refImg, s_refAlloc); s_refImg = VK_NULL_HANDLE; }
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!CreateImage2D(VK::SceneColor::Format(), want, usage, s_refImg, s_refAlloc, "WaterRefract"))
        return false;
    s_refView = CreateImageView(s_refImg, VK::SceneColor::Format());
    s_refExt = want; s_refFirst = true;
    if (s_refSamp == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_refSamp);
    }
    Msg("[VK Water] refraction copy %ux%u", want.width, want.height);
    return s_refView != VK_NULL_HANDLE;
}

// The world vertex is stride 32: position at 0, packed normal (D3DCOLOR BGRA)
// at 12, base UV (SHORT2) at 24 or 28 depending on the sub-layout. One shader
// serves both - only this attribute offset differs.
// `grid` builds the NEAR-FIELD variant: the dense camera-locked mesh from
// water_grid.vert. It takes no vertex buffer at all (every position comes out of
// gl_VertexIndex) and no tessellation stages - the grid is already finer than any
// factor the hardware would give on this level's 200 m triangles, and displacing
// in the vertex stage saves two stages per frame.
// `lean` builds the r_wtr_lean twin: the -DWATER_LEAN fragment stage and no
// tessellation, so the level polygons go straight through water.vert.
VkPipeline BuildPipeline(u32 tcOffset, bool grid = false, bool lean = false)
{
    const bool tess = s_tess && !grid && !lean;

    // PREMULTIPLIED alpha: the shader emits the light the layer ADDS, and alpha
    // is how much of the bottom it hides. out = src + dst·(1-src.a).
    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable         = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp        = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.alphaBlendOp        = VK_BLEND_OP_ADD;
    ba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    // Two-sided like every other world pipeline (cull NONE is the builder's
    // default): X-Ray winding does not agree with normals, and you can stand in
    // a flooded basement looking UP at the surface from below.
    // No depth attachment at all — the FS does the LEQUAL test against the
    // sampled depth (see water.frag.glsl).
    VK::GfxPipelineBuilder b(s_layout);
    b.Vert(grid ? s_gridVS : s_vs).Frag(lean ? s_fsLean : s_fs);
    if (!grid) {
        b.Binding(0, 32)
         .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
         .Attr(1, 0, VK_FORMAT_R8G8B8A8_UNORM,   12)
         .Attr(2, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset);
    }
    // With tessellation the input assembly feeds PATCHES, not triangles: the
    // control stage takes 3 control points and the evaluation stage emits the
    // subdivided surface.
    if (tess)
        b.Tess(s_tcs, s_tes);
    return b.ColorBlend(VK::SceneColor::Format(), ba)
            .Build("Water tcOffset=%u grid=%d lean=%d", tcOffset, (int)grid, (int)lean);
}

// ---------------------------------------------------------------------------
// POOL MASK pipeline. Draws the water surfaces from straight above into the
// ripple sim's tile, writing surface HEIGHT (see water_mask.*.glsl). Tiny: no
// depth, no descriptors, one vec4 of push, and the stock levels put every water
// polygon in a handful of visuals.
// ---------------------------------------------------------------------------
VkPipeline BuildMaskPipeline(bool surf)
{
    VkShaderModule fs = surf ? s_maskSurfFS : s_maskFS;
    if (s_maskVS == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    // MAX, not overwrite: where two pools stack in plan view the TOP surface is
    // the one a wave can travel on, and the clear value is far below any level.
    // The second target takes MAX for the same reason on .r (the live surface),
    // and it costs nothing on .g - the ground is a fact about the column, so every
    // sheet over it writes the same number.
    VkPipelineColorBlendAttachmentState ba[2]{};
    ba[0].blendEnable         = VK_TRUE;
    ba[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba[0].colorBlendOp        = VK_BLEND_OP_MAX;
    ba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba[0].alphaBlendOp        = VK_BLEND_OP_MAX;
    ba[0].colorWriteMask      = VK_COLOR_COMPONENT_R_BIT;
    ba[1] = ba[0];
    ba[1].colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                              | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VK::GfxPipelineBuilder b(s_maskLayout);
    b.Vert(s_maskVS).Frag(fs)
     .Binding(0, 32)
     .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)   // position only
     .ColorBlend(VK_FORMAT_R32_SFLOAT, ba[0]);
    if (surf)
        b.ColorBlend(VK_FORMAT_R32G32B32A32_SFLOAT, ba[1]);
    return b.Build("Water pool-mask surf=%d", (int)surf);
}

// Same shape as the mask pipeline, but MIN-blended and reading the water mask:
// it keeps the lowest surface ABOVE the water, which is the headroom test.
VkPipeline BuildLidPipeline()
{
    if (s_lidVS == VK_NULL_HANDLE || s_lidFS == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable         = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.colorBlendOp        = VK_BLEND_OP_MIN;      // the LOWEST lid wins
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.alphaBlendOp        = VK_BLEND_OP_MIN;
    ba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT;

    return VK::GfxPipelineBuilder(s_lidLayout)
        .Vert(s_lidVS).Frag(s_lidFS)
        .Binding(0, 32)
        .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
        .ColorBlend(VK_FORMAT_R32_SFLOAT, ba)
        .Build("Water lid");
}

bool Init()
{
    if (s_inited) return !s_failed;
    s_inited = true;

    if (!g_ShaderManager) { s_failed = true; return false; }

    // Occlusion query pool: one slot per frame in flight, read a frame late.
    // Non-precise (boolean) is all the gate needs and is core Vulkan - no feature
    // to check. A failure here is not fatal: s_waterSeen stays true and the pass
    // behaves exactly as it did before this existed.
    {
        VkQueryPoolCreateInfo qi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType  = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = kFramesInFlight;
        if (vkCreateQueryPool(VulkanHW.m_Device, &qi, nullptr, &s_occlPool) != VK_SUCCESS) {
            Msg("![VK Water] occlusion query pool create failed - the refraction snapshot stays unconditional");
            s_occlPool = VK_NULL_HANDLE;
        }
    }
    s_vs = g_ShaderManager->Load("water.vert.spv");
    s_fs = g_ShaderManager->Load("water.frag.spv");
    if (s_vs == VK_NULL_HANDLE || s_fs == VK_NULL_HANDLE) {
        Msg("![VK Water] water.{vert,frag}.spv missing - water bodies stay unrendered");
        s_failed = true; return false;
    }
    // The near-field grid's vertex stage. Optional in the same sense as
    // tessellation: without it the water is drawn by the level's own polygons
    // alone, which is what it has always been.
    // The lean twin. Optional: without it r_wtr_lean falls back to the full shader.
    s_fsLean = g_ShaderManager->Load("water_lean.frag.spv");
    Msg("[VK Water] lean twin: %s", s_fsLean ? "loaded (r_wtr_lean)" : "OFF (water_lean.frag.spv missing)");
    s_gridVS = g_ShaderManager->Load("water_grid.vert.spv");
    Msg("[VK Water] near-field grid: %s", s_gridVS ? "shader loaded (r_wtr_grid)"
                                                   : "OFF (water_grid.vert.spv missing)");

    // Tessellation is what turns the wave from a lighting trick into geometry.
    // Optional: a device without the feature (or a build without the stages)
    // keeps the flat surface and the per-pixel normal, which still looks like
    // water - it just has no silhouette.
    if (PipelineCache::TessAvailable()) {
        s_tcs = g_ShaderManager->Load("water.tesc.spv");
        s_tes = g_ShaderManager->Load("water.tese.spv");
        s_tess = (s_tcs != VK_NULL_HANDLE && s_tes != VK_NULL_HANDLE);
    }
    Msg("[VK Water] tessellation: %s", s_tess ? "on (displaced surface)" : "OFF (flat surface, normals only)");

    // Lid map: needs its own descriptor set (it SAMPLES the water mask it is
    // paired with) and its own pipeline.
    s_lidVS = g_ShaderManager->Load("water_lid.vert.spv");
    s_lidFS = g_ShaderManager->Load("water_lid.frag.spv");
    if (s_lidVS != VK_NULL_HANDLE && s_lidFS != VK_NULL_HANDLE) {
        if (VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER }, 1,
                                   s_lidSetLayout, s_lidPool, &s_lidSet,
                                   VK_SHADER_STAGE_FRAGMENT_BIT, "Water.Lid")) {
            s_lidLayout = VK::MakePipelineLayout({ s_lidSetLayout }, sizeof(float) * 4 + sizeof(Fmatrix),
                                                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
            if (s_lidLayout != VK_NULL_HANDLE)
                s_lidPipe = BuildLidPipeline();
        }
    }
    Msg("[VK Water] lid map: %s", s_lidPipe ? "on (headroom tells a puddle from a buried sheet)"
                                            : "OFF (mask alone; buried water counts as a pool)");

    // Set 0: binding 0 = scene depth (the bottom behind the surface),
    //        binding 1 = the tuning UBO.
    // The tuning UBO and the ripple texture are read by the TESSELLATION stages
    // too (they displace by the same field the FS shades with), so both are
    // visible to TESC/TESE as well as FRAGMENT.
    // â  VERTEX too, since the near-field grid displaces in its VERTEX stage: it
    // reads the tuning UBO for the wave and the ripple field for the interactive
    // part. A binding the shader uses but the layout does not name for that stage
    // is undefined behaviour that happens to work on some drivers.
    const VkShaderStageFlags kFieldStages = VK_SHADER_STAGE_FRAGMENT_BIT
                                          | VK_SHADER_STAGE_VERTEX_BIT
                                          | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT
                                          | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    // ⚠ SIZE THIS WITH THE BINDINGS — and the pool, and the writes below. This
    // project has a scar here: w[5] on a five-entry array wrote past its end and
    // CLevel::Render took an SEH fault every frame the water drew.
    VkDescriptorSetLayoutBinding b[9]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorCount = 1; b[1].stageFlags = kFieldStages;
    b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[2].descriptorCount = 1; b[2].stageFlags = kFieldStages;
    b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;   // the scene copy
    b[4].binding = 4; b[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // Pool mask: r_wtr_debug 7 reads it, and so does the near-field grid's VERTEX
    // stage - it is where the grid gets its surface height per texel.
    b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;
    // Live surface + GROUND. The shore break is a function of the still water
    // DEPTH, and this is where the ground under each tile texel lives - read by
    // the grid's vertex stage (it displaces the crest) and by the fragment stage
    // (it shades the whitewater), which is why both stages are named.
    b[5].binding = 5; b[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[5].descriptorCount = 1; b[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;
    // SPECTRAL WAVE FIELD (vk_water_fft), as 2D ARRAYS - one layer per cascade.
    // 6 = (Dx, h, Dz, foam), 7 = (dh/dx, dh/dz, Jacobian, -). kFieldStages, all
    // four of them: the near-field grid displaces in its VERTEX stage, the TESE
    // displaces the level sheets, and the FS shades by the slope - and they have
    // to agree about where the wave is or the lighting slides off the geometry.
    b[6].binding = 6; b[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[6].descriptorCount = 1; b[6].stageFlags = kFieldStages;
    b[7].binding = 7; b[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[7].descriptorCount = 1; b[7].stageFlags = kFieldStages;
    // LOCAL FETCH (water_fetch.comp): how big the body of water under each column
    // actually is. kFieldStages, all four, for the reason 6/7 are: every stage
    // that evaluates the wave has to agree about how big it is, and they evaluate
    // it in the vertex stage (the near-field grid), the TESE (the level sheets)
    // and the FS (the shading).
    b[8].binding = 8; b[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[8].descriptorCount = 1; b[8].stageFlags = kFieldStages;
    VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    slci.bindingCount = 9; slci.pBindings = b;
    if (vkCreateDescriptorSetLayout(VulkanHW.m_Device, &slci, nullptr, &s_setLayout) != VK_SUCCESS) {
        s_failed = true; return false;
    }

    for (u32 i = 0; i < kFramesInFlight; ++i)
        s_ubo[i].Create(sizeof(WaterParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);

    VkDescriptorPoolSize ps[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight * 8 },   // depth, ripple, scene copy, mask, surf, fft disp, fft deriv, local fetch
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight },
    };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = kFramesInFlight; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(VulkanHW.m_Device, &pci, nullptr, &s_pool) != VK_SUCCESS) {
        s_failed = true; return false;
    }
    VkDescriptorSetLayout layouts[kFramesInFlight];
    for (u32 i = 0; i < kFramesInFlight; ++i) layouts[i] = s_setLayout;
    VkDescriptorSetAllocateInfo dai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = s_pool; dai.descriptorSetCount = kFramesInFlight; dai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(VulkanHW.m_Device, &dai, s_set) != VK_SUCCESS) {
        s_failed = true; return false;
    }

    // ---- pool mask --------------------------------------------------------
    // Optional in the same sense as tessellation: if it is missing the ripple sim
    // simply runs unbounded, the way it did before pools existed.
    //
    // Set 0 is JUST the tuning UBO, and it has to be its own layout for two
    // reasons. The water set carries the pool mask itself at binding 4 - the very
    // image this pass has bound as a COLOR ATTACHMENT - and it did not exist yet:
    // this block used to run before s_setLayout was created and handed a NULL
    // handle to vkCreatePipelineLayout, which only went unnoticed because nothing
    // ever bound set 0. The WATER_MASK_SURF variant reads the wave from it, so
    // both had to be put right.
    //
    // Set 1 = the shared EnvLight set: the ground-height map the fragment stage
    // tests against, and the rain map the shelter query needs.
    s_maskVS     = g_ShaderManager->Load("water_mask.vert.spv");
    s_maskFS     = g_ShaderManager->Load("water_mask.frag.spv");
    s_maskSurfFS = g_ShaderManager->Load("water_mask_surf.frag.spv");
    // The spectral field has to EXIST before this set is written: these descriptors
    // are baked ONCE, below, and the first WaterFFT::Dispatch - which is what would
    // otherwise create the images - has not run yet. Without this the set would hold
    // a VK_NULL_HANDLE view for the rest of the session.
    //
    // And it is a REQUIREMENT, not a nicety: the mask shader names bindings 6/7
    // unconditionally, so a set with those slots unwritten is invalid to bind at
    // all. Missing .spv here means an inconsistent deploy (these ship together);
    // say so plainly rather than binding something that merely looks like a dummy.
    const bool fftReady = WaterFFT::EnsureCreated()
                       && WaterFFT::DispView() != VK_NULL_HANDLE
                       && WaterFFT::DerivView() != VK_NULL_HANDLE;
    if (s_maskVS != VK_NULL_HANDLE && s_maskFS != VK_NULL_HANDLE && fftReady) {
        // ⚠ SIZE THE ARRAY WITH THE BINDINGS - and the pool, and the writes. Three
        // stack smashes on record in this file alone.
        VkDescriptorSetLayoutBinding mb[4]{};
        mb[0].binding = 1; mb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        mb[0].descriptorCount = 1; mb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // 6/7 = the spectral displacement and derivative arrays, the same slots
        // they occupy in the water set. This pass writes the LIVE SURFACE the
        // wetness and the sim's contact test read, so it has to know about the
        // same wave the surface is drawn with.
        mb[1].binding = 6; mb[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mb[1].descriptorCount = 1; mb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        mb[2] = mb[1]; mb[2].binding = 7;
        // 8 = the LOCAL FETCH map. This pass writes the live surface the wetness
        // record runs on, so it has to size the wave the same way the surface
        // does — otherwise the dark band and the water that made it disagree,
        // which is the defect this pass keeps being about. It reads the map one
        // frame late (it is recorded before the step that builds it); a pool does
        // not change size in 16 ms.
        mb[3] = mb[1]; mb[3].binding = 8;
        VkDescriptorSetLayoutCreateInfo mslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        mslci.bindingCount = 4; mslci.pBindings = mb;
        VkDescriptorPoolSize mps[2] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight * 3 },
        };
        VkDescriptorPoolCreateInfo mpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        mpci.maxSets = kFramesInFlight; mpci.poolSizeCount = 2; mpci.pPoolSizes = mps;
        if (vkCreateDescriptorSetLayout(VulkanHW.m_Device, &mslci, nullptr, &s_maskUboSetLayout) == VK_SUCCESS &&
            vkCreateDescriptorPool(VulkanHW.m_Device, &mpci, nullptr, &s_maskUboPool) == VK_SUCCESS) {
            VkDescriptorSetLayout mlay[kFramesInFlight];
            for (u32 i = 0; i < kFramesInFlight; ++i) mlay[i] = s_maskUboSetLayout;
            VkDescriptorSetAllocateInfo mdai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            mdai.descriptorPool = s_maskUboPool; mdai.descriptorSetCount = kFramesInFlight;
            mdai.pSetLayouts = mlay;
            if (vkAllocateDescriptorSets(VulkanHW.m_Device, &mdai, s_maskUboSet) == VK_SUCCESS) {
                for (u32 i = 0; i < kFramesInFlight; ++i) {
                    // Binding 8 is refreshed every frame just before this set is
                    // bound (WaterRipple may not have created its images yet).
                    // Seeded here anyway: a set with an unwritten binding is
                    // invalid to BIND, not merely to read.
                    const bool haveF = WaterRipple::FetchView() != VK_NULL_HANDLE;
                    VK::DescriptorWriter(s_maskUboSet[i])
                        .UniformBuffer(1, s_ubo[i].GetHandle(), sizeof(WaterParams))
                        .ImageSampler (6, WaterFFT::DispView(),  WaterFFT::GetSampler(), VK_IMAGE_LAYOUT_GENERAL)
                        .ImageSampler (7, WaterFFT::DerivView(), WaterFFT::GetSampler(), VK_IMAGE_LAYOUT_GENERAL)
                        .ImageSampler (8, haveF ? WaterRipple::FetchView() : Swapchain.m_DepthView,
                                       WaterRipple::GetPointSampler() ? WaterRipple::GetPointSampler()
                                                                      : ShadowMap::GetSampler(),
                                       haveF ? VK_IMAGE_LAYOUT_GENERAL
                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                        .Flush();
                }
            }
        }
        VkDescriptorSetLayout msets[2] = { s_maskUboSetLayout, EnvLight::GetSetLayout() };
        VkPushConstantRange mpcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                  0, sizeof(MaskPush) };
        VkPipelineLayoutCreateInfo mplci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        mplci.setLayoutCount = 2; mplci.pSetLayouts = msets;
        mplci.pushConstantRangeCount = 1; mplci.pPushConstantRanges = &mpcr;
        if (msets[0] != VK_NULL_HANDLE && msets[1] != VK_NULL_HANDLE &&
            vkCreatePipelineLayout(VulkanHW.m_Device, &mplci, nullptr, &s_maskLayout) == VK_SUCCESS) {
            s_maskPipe = BuildMaskPipeline(false);
            if (s_maskSurfFS != VK_NULL_HANDLE) s_maskSurfPipe = BuildMaskPipeline(true);
        }
    }
    Msg("[VK Water] pool mask: %s", s_maskPipe ? "on (puddles are separate bodies)"
                                               : "OFF (one unbounded ripple field)");
    if (!fftReady)
        Msg("![VK Water] pool mask OFF because the SPECTRAL FIELD did not create - "
            "this pass reads it at bindings 6/7 now. Check that water_fft_*.comp.spv are deployed "
            "alongside water_mask.frag.spv; they are one set.");
    Msg("[VK Water] live surface + ground: %s",
        s_maskSurfPipe ? "on (wetness follows the wave that is actually drawn)"
                       : "OFF (shore wetness falls back to the still waterline)");

    // set 0 = ours (depth), set 1 = the shared EnvLight set - the water FS
    // includes light_ubo/env_common/vsm_sample verbatim, which all declare
    // set 1, so the lighting, sky cubes, shadow maps and rain map come for free.
    VkDescriptorSetLayout sets[2] = { s_setLayout, EnvLight::GetSetLayout() };
    if (sets[1] == VK_NULL_HANDLE) {
        Msg("![VK Water] EnvLight set layout not ready - water disabled");
        s_failed = true; return false;
    }
    // The FS is in the range now: it reads `basin` (per-surface, so not UBO
    // material) even though the matrix means nothing to it.
    VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
                             | VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(WaterPush) };
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 2; plci.pSetLayouts = sets;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_layout) != VK_SUCCESS) {
        s_failed = true; return false;
    }

    Msg("[VK Water] init OK");
    return true;
}

// Statics captured for the lid map, in world space. Kept between frames so the
// pass has them after Pass_World has cleared the queue.
xr_vector<DrawItem> s_lidStatics;

// FETCH of one water surface, in metres: how much water the wind has to work on.
// Ocean-wave growth scales with it, and more sharply, a wave whose length does
// not fit in the basin cannot exist there at all - which is the whole reason a
// flooded cellar was simulating surf.
//
// Geometric mean of the bounding box in XZ, NOT the smaller side: a 3x200 m
// drainage channel is not a 3-metre puddle, waves run ALONG it. The box is the
// visual's own, and this pass only draws identity-transform statics, so it is
// already in world space.
float BasinFetch(vkFVisual* fv)
{
    if (ps_r_wtr_fetch <= 0.f || !fv) return 0.f;   // 0 = open water, old behaviour
    const Fbox& bb = fv->vis.box;
    const float sx = std::max(bb.max.x - bb.min.x, 0.f);
    const float sz = std::max(bb.max.z - bb.min.z, 0.f);
    return std::sqrt(sx * sz) * ps_r_wtr_fetch;
}

}  // anon namespace

// ---- LEVEL-WIDE WATER HEIGHT MAP -------------------------------------------
// Wetness was a property of WHERE THE PLAYER STOOD, not of the world: it lived on
// the ripple tile, 32 m of camera-following window, and the band it uncovered
// started dry. So a shore twenty metres off was bone dry until you walked up to
// it and watched it soak - the wetness arrived because the WINDOW did.
//
// This map is the opposite: rasterized ONCE per level over all the water there
// is, never scrolled, never accumulated. Every shore is wet from the first frame
// you can see it, including ones you have never visited.
//
// â­ It can be coarse. The sharpness of the wet band comes from the HEIGHT test
// per pixel, not from this map's resolution - the map only answers "is there
// water at this XZ, and at what level". And the plan-view outline it quantizes
// is not the visible waterline anyway: the sheet runs on UNDER the bank, so the
// edge you see is drawn by the height comparison, metres away from this outline.
namespace {
constexpr u32   kLvlTexels = 2048;
constexpr float kLvlSpan   = 4096.f;   // 2 m/texel - see the note above on why coarse is fine
VkImage       s_lvl      = VK_NULL_HANDLE;
VmaAllocation s_lvlAlloc = nullptr;
VkImageView   s_lvlView  = VK_NULL_HANDLE;
float         s_lvlOX = 0.f, s_lvlOZ = 0.f, s_lvlSize = 0.f;
bool          s_lvlBuilt = false;
const void*   s_lvlLevel = nullptr;
// Water visuals already rasterized into the map. The render queue holds only the
// water VISIBLE THIS FRAME, so a single build captures whatever happened to be on
// screen at the time - which is how the first version came out anchored around
// the spawn while the player stood in a river 400 m outside it, seeing wetness
// "creep in from the side" as the local tile filled in behind the missing map.
// Water is static, so the map only ever GAINS coverage: draw each visual once,
// never clear, and the map converges on the whole level as you walk it.
}

VkImageView Water_LevelMapView()   { return s_lvlView; }
float       Water_LevelMapOriginX(){ return s_lvlOX; }
float       Water_LevelMapOriginZ(){ return s_lvlOZ; }
float       Water_LevelMapSize()   { return s_lvlSize; }
bool        Water_LevelMapReady()  { return s_lvlBuilt && s_lvlView != VK_NULL_HANDLE; }

void Water_CaptureLidStatics()
{
    // The collision-model lid needs no geometry list at all - and this copies the
    // whole render queue every frame, so skipping it is the point, not a detail.
    if (!ps_r_wtr || !ps_r_wtr_sim || !ps_r_wtr_sim_pools || ps_r_wtr_sim_lid) { s_lidStatics.clear(); return; }
    if (!g_RenderQueue.HasWater())                         { s_lidStatics.clear(); return; }
    s_lidStatics.assign(g_RenderQueue.Items().begin(), g_RenderQueue.Items().end());
}

void Pass_Water(FrameContext& ctx)
{
    if (ps_r_wtr <= 0)                  return;
    if (ctx.cmd == VK_NULL_HANDLE)      return;
    if (!g_RenderQueue.HasWater())      return;
    if (!Init())                        return;

    // One item per visual: the CPU world walk double-submits hierarchy children,
    // which for a BLENDED surface would stack alpha into an opaque sheet.
    g_RenderQueue.DedupWater();
    const auto& items = g_RenderQueue.WaterItems();
    if (items.empty()) return;

    VkCommandBuffer cmd = ctx.cmd;
    const int zone = VK::Prof::ZoneBegin(cmd, "Water");

    // Hoisted above the pool mask, which now reads the tuning UBO for the wave.
    // The UBO for THIS frame is filled further down but on the CPU, at record
    // time - so by submission the mask draw sees this frame's numbers, not the
    // previous frame's, and the wet line cannot lag the wave that made it.
    const u32 slot = CommandManager.GetCurrentFrame() % kFramesInFlight;

    // ---- feed the ripple sim ----------------------------------------------
    // Anything standing in the water writes into the heightfield. The waves
    // then propagate, reflect and interfere on their own - which is the whole
    // reason the sim exists next to the analytic swell.
    if (ps_r_wtr_sim) {
        float rainForSim = 0.f;
        if (g_pGamePersistent && ES().CurrentEnv)
            rainForSim = std::clamp(ES().CurrentEnv->rain_density, 0.f, 1.f);
        // Water planes are horizontal; each visual's bounding sphere gives its
        // surface height and extent, which is all the test needs.
        struct Plane { float x, z, y, r; };
        static xr_vector<Plane> planes;
        planes.clear();
        for (const DrawItem& it : items) {
            auto* fv = static_cast<vkFVisual*>(it.vis);
            if (!fv) continue;
            planes.push_back({ fv->vis.sphere.P.x, fv->vis.sphere.P.z, fv->vis.sphere.P.y, fv->vis.sphere.R });
        }

        static xr_vector<Fvector> feet;
        feet.clear();
        {   // the player: two prints either side of the camera, like the snow deform
            const Fvector& camp = RV().vCameraPosition;
            Fvector fwd = RV().vCameraDirection; fwd.y = 0.f;
            if (fwd.square_magnitude() > 1e-4f) fwd.normalize(); else fwd.set(0.f, 0.f, 1.f);
            const Fvector perp = { -fwd.z, 0.f, fwd.x };
            feet.push_back({ camp.x - perp.x * 0.15f, camp.y - 1.7f, camp.z - perp.z * 0.15f });
            feet.push_back({ camp.x + perp.x * 0.15f, camp.y - 1.7f, camp.z + perp.z * 0.15f });
        }
        Skinned_CollectFeet(feet, 24);   // NPCs / third-person body

        // â  The player's two feet are the CAMERA Â± a perpendicular 15 cm, so
        // TURNING THE MOUSE swings them along a 30 cm arc - far enough to clear
        // the "has it moved" filter below. Standing still and looking around
        // therefore hammered the water at 6-19 splats/second (measured, with the
        // actor not walking at all). Gate the player's feet on the CAMERA having
        // moved, which is what "the player took a step" actually means.
        static Fvector s_lastCam{ 0.f, 0.f, 0.f };
        const Fvector& camNow = RV().vCameraPosition;
        const float camMoved2 = (camNow.x - s_lastCam.x) * (camNow.x - s_lastCam.x)
                              + (camNow.z - s_lastCam.z) * (camNow.z - s_lastCam.z);
        // â  ...but XZ ALONE misses the one move players actually test water with.
        // A jump is pure Y, so it cleared no gate at all and landing in a puddle
        // wrote NOTHING into the field - "I jump in the water and there are no
        // waves" was literally true, and no amount of tuning the wave equation
        // was ever going to fix it. Turning the mouse still cannot fire: rotation
        // moves the camera's DIRECTION, not its position, so Y stays put.
        const float camMovedY = _abs(camNow.y - s_lastCam.y);
        const bool playerWalking = camMoved2 > 0.06f * 0.06f || camMovedY > 0.06f;
        // A landing should hit harder than a footfall - that is what the eye
        // expects from a jump, and it is free: we already know how fast the
        // camera is descending.
        const float impact = 1.f + std::clamp(camMovedY / _max(RV().fTimeDelta, 0.001f) / 4.f, 0.f, 3.f);
        if (playerWalking) s_lastCam = camNow;

        // A splat fired every frame at the same spot is a static dent, not a
        // ripple: hold each source back until it has MOVED, so a walking NPC
        // leaves a train of rings and a standing one leaves nothing.
        struct Recent { float x, z, t; };
        static xr_vector<Recent> recent;
        const float now = RV().fTimeGlobal;
        for (auto it2 = recent.begin(); it2 != recent.end(); )
            if (now - it2->t > 0.5f) it2 = recent.erase(it2); else ++it2;

        // WHO is actually writing into the field? "Waves appear in the far
        // puddles" has three possible authors - your feet, an NPC's feet, and
        // rain - and they are fixed in three different places, so guessing which
        // one costs a round trip. The first two feet are the player's.
        static u32 s_spPlayer = 0, s_spNpc = 0, s_spRain = 0;
        static float s_spLog = 0.f;
        u32 footIdx = 0;
        for (const Fvector& f : feet) {
            const bool isPlayer = (footIdx++ < 2);
            if (isPlayer && !playerWalking) continue;   // standing and turning is not a step
            float py = 0.f; bool inWater = false;
            for (const Plane& pl : planes) {
                const float dx = f.x - pl.x, dz = f.z - pl.z;
                if (dx * dx + dz * dz > pl.r * pl.r) continue;
                if (f.y - pl.y > 0.6f || pl.y - f.y > 2.5f) continue;   // above the surface / far under it
                py = pl.y; inWater = true; break;
            }
            if (!inWater) continue;
            bool tooSoon = false;
            for (const Recent& r : recent) {
                const float dx = f.x - r.x, dz = f.z - r.z;
                if (dx * dx + dz * dz < 0.22f * 0.22f) { tooSoon = true; break; }
            }
            if (tooSoon) continue;
            recent.push_back({ f.x, f.z, now });
            // 35 cm, not 70: a footprint is not a metre and a half across, and a
            // wide bell reaches over the rim into the NEXT puddle even when the
            // step itself landed in this one.
            // f.y is the FOOT - the step compares it against the surface height
            // the mask holds at that texel, which is the only place that number
            // is trustworthy. The CPU sphere test above cannot do it.
            WaterRipple::Splat(f.x, f.z, 0.35f, -ps_r_wtr_step * (isPlayer ? impact : 1.f), f.y);
            if (isPlayer) ++s_spPlayer; else ++s_spNpc;
        }

        // ---- BOW WAVE ------------------------------------------------------
        // Walking forward produced nothing ahead of the player, and two separate
        // things were behind that. One is correct and stays: at ~3 m/s through
        // 12 cm of water the Froude number is about 3, so no wave CAN run out in
        // front - a wading man makes a V-wake, not a ring, and that is what
        // supercritical flow looks like.
        //
        // The other was a real gap. What you see ahead of someone wading is not a
        // propagating wave at all, it is water SHOVED ASIDE by the body - a mound
        // that travels with them. Every source here was a symmetric dip: we
        // modelled the hole a foot punches and never modelled the push, so there
        // was nothing to shove. A positive lobe just ahead of the travel
        // direction supplies it; together with the trough at the feet that is a
        // displacement, which is what a body in water actually is.
        static Fvector s_prevCam{ 0.f, 0.f, 0.f };
        static Fvector s_lastBow{ 0.f, 0.f, 0.f };
        static bool    s_haveCam = false;
        Fvector vel; vel.sub(camNow, s_prevCam); vel.y = 0.f;
        s_prevCam = camNow;
        const float speed = s_haveCam ? vel.magnitude() / _max(RV().fTimeDelta, 0.001f) : 0.f;
        s_haveCam = true;
        if (ps_r_wtr_bow > 0.f && speed > 0.6f && speed < 20.f) {   // 20: teleports are not swimming
            Fvector dir = vel; dir.y = 0.f; dir.normalize_safe();
            const float bx = camNow.x + dir.x * 0.45f;   // just ahead of the body
            const float bz = camNow.z + dir.z * 0.45f;
            const float mx = bx - s_lastBow.x, mz = bz - s_lastBow.z;
            // Same spacing discipline as the feet: a source re-applied every frame
            // at the same spot is a static bump being pumped, not a wave.
            if (mx * mx + mz * mz > 0.20f * 0.20f) {
                s_lastBow.set(bx, 0.f, bz);
                // No CPU water test on purpose - the mask gate inside the step is
                // the authority on where water is, and it already drops splats
                // that land dry. That lesson cost a round trip once already.
                WaterRipple::Splat(bx, bz, 0.55f,
                                   ps_r_wtr_bow * ps_r_wtr_step * std::clamp(speed / 3.0f, 0.f, 2.f),
                                   camNow.y - 1.7f);   // the body's feet, same test as a step
            }
        }

        // Rain: a handful of drops per frame, scaled by the weather. The
        // analytic ring layer in the FS still does the fine stipple; these are
        // the ones that leave a wave behind them.
        // Rate it by TIME, not by frame. Six drops PER FRAME is ~700 a second at
        // 100 fps: it emptied the whole per-frame splat budget every frame and
        // the footsteps never made it into the sim at all - measured as 2000+
        // splats per 3 s in the [VK Ripple] counter, which is what "I walk in
        // the water and nothing happens" actually was.
        if (rainForSim > 0.05f && WaterRipple::Ready()) {
            static u32   seed = 12345u;
            static float rainAccum = 0.f;
            rainAccum += rainForSim * 18.f * std::clamp(RV().fTimeDelta, 0.f, 0.1f);
            const int n = std::min((int)rainAccum, 6);
            rainAccum -= float(n);
            const float o0 = WaterRipple::OriginX(), o1 = WaterRipple::OriginZ();
            const float sz = WaterRipple::SizeMetres();
            for (int i = 0; i < n; ++i) {
                seed = seed * 1664525u + 1013904223u;
                const float u = float((seed >> 8) & 0xFFFF) / 65535.f;
                seed = seed * 1664525u + 1013904223u;
                const float v = float((seed >> 8) & 0xFFFF) / 65535.f;
                // Rain falls onto whatever surface is there - it has no source
                // height to check against, so it opts out of the test.
                WaterRipple::Splat(o0 + u * sz, o1 + v * sz, 0.16f, -0.012f * rainForSim,
                                   WaterRipple::kNoHeightTest);
                ++s_spRain;
            }
        }

        if (RV().fTimeGlobal - s_spLog > 3.f) {
            if (s_spPlayer || s_spNpc || s_spRain)
                Msg("[VK Water] splats/3s: player=%u npc=%u rain=%u (rain_density=%.2f)",
                    s_spPlayer, s_spNpc, s_spRain, rainForSim);
            s_spPlayer = s_spNpc = s_spRain = 0;
            s_spLog = RV().fTimeGlobal;
        }
    }
    // ---- spectral wave field -----------------------------------------------
    // ⚠ BEFORE THE POOL MASK, and that ordering is load-bearing. The mask pass
    // SAMPLES this field now (it writes the live surface the wetness and the sim's
    // contact test read), and it is a render pass — so the compute has to be both
    // recorded earlier and finished earlier. Left where it used to sit, after the
    // mask, frame one would have sampled an image still in UNDEFINED layout that
    // nothing had written: not a stale value, a genuinely undefined read.
    //
    // Outside any render pass on purpose. Its closing barrier is COMPUTE ->
    // ALL_GRAPHICS, so it covers the mask draw as well as the water draw.
    const int zFft = VK::Prof::ZoneBegin(cmd, "Wtr/FFT");
    WaterFFT::Dispatch(cmd);
    VK::Prof::ZoneEnd(cmd, zFft);

    const int zMask = VK::Prof::ZoneBegin(cmd, "Wtr/Mask");
    // ---- pool mask ---------------------------------------------------------
    // Draw the water surfaces from straight above into the sim's tile, so the
    // wave equation knows where each body of water ENDS. Without it the field is
    // one unbroken sheet: a splash in one puddle crossed the dry floor and came
    // up in every other puddle within the tile.
    bool maskOK = false, surfDrawn = false;
    VkDescriptorSet envSetMask = EnvLight::GetCurrentSet();
    if (s_maskPipe != VK_NULL_HANDLE && ps_r_wtr_sim && ps_r_wtr_sim_pools) {
        WaterRipple::PrepareTile();          // settle the anchor BEFORE rasterizing into it
        const VkImage mimg = WaterRipple::MaskImage();
        const u32     mtex = WaterRipple::MaskTexels();
        // The second target only exists when its pipeline and its image both do;
        // without it the mask behaves exactly as it did before, and the sim's
        // contact test falls back to the still waterline it already had.
        const VkImage simg = WaterRipple::SurfImage();
        // Deliberately NOT gated on r_wtr_wet. The step reports "no live surface"
        // when this is off, and that report has to mean a real failure - one more
        // condition here and the log would cry wolf every time someone turned the
        // wetness knob down. The cost is one extra 512Â² write in a pass that draws
        // a handful of visuals.
        const bool    surfOn = (s_maskSurfPipe != VK_NULL_HANDLE) && (simg != VK_NULL_HANDLE)
                            && (s_maskUboSet[slot] != VK_NULL_HANDLE);
        if (mimg != VK_NULL_HANDLE && mtex) {
            ImageBarrier(cmd, mimg,
                         s_maskFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            s_maskFirst = false;
            if (surfOn) {
                ImageBarrier(cmd, simg,
                             s_surfFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                s_surfFirst = false;
            }

            // All three heights start at the dry sentinel: "no water drawn here",
            // "no ground known here" and "no still level here" are the three things
            // the readers have to be able to tell from a real height, and a zero
            // would read as sea level.
            const float dry = WaterRipple::MaskDryValue();
            VK::RenderingBuilder mrb(mtex, mtex);
            mrb.ColorClear(WaterRipple::MaskView(), VkClearColorValue{ { dry, 0.f, 0.f, 0.f } });
            if (surfOn)
                mrb.ColorClear(WaterRipple::SurfView(), VkClearColorValue{ { dry, dry, dry, 0.f } });
            mrb.Begin(cmd);

            // Plain (unflipped) viewport: the mask is indexed by the compute
            // shader in texel space, so row 0 must be the tile's first row.
            VkViewport mvp{ 0.f, 0.f, float(mtex), float(mtex), 0.f, 1.f };
            vkCmdSetViewport(cmd, 0, 1, &mvp);
            VkRect2D msc{ {}, { mtex, mtex } };
            vkCmdSetScissor(cmd, 0, 1, &msc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, surfOn ? s_maskSurfPipe : s_maskPipe);

            if (envSetMask != VK_NULL_HANDLE)
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_maskLayout, 1, 1, &envSetMask, 0, nullptr);
            if (s_maskUboSet[slot] != VK_NULL_HANDLE) {
                // Point the local-fetch slot at the real map now that WaterRipple
                // has certainly created it. Per frame in flight, so this set is
                // never updated while a submitted frame is still reading it — the
                // same discipline the water set below uses.
                if (WaterRipple::FetchView() != VK_NULL_HANDLE && WaterRipple::GetPointSampler())
                    VK::DescriptorWriter(s_maskUboSet[slot])
                        .ImageSampler(8, WaterRipple::FetchView(), WaterRipple::GetPointSampler(),
                                      VK_IMAGE_LAYOUT_GENERAL)
                        .Flush();
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_maskLayout, 0, 1, &s_maskUboSet[slot], 0, nullptr);
            }

            MaskPush mpush{};
            mpush.tile[0] = WaterRipple::OriginX();
            mpush.tile[1] = WaterRipple::OriginZ();
            mpush.tile[2] = 1.f / std::max(WaterRipple::SizeMetres(), 0.001f);
            mpush.rainVP  = ShadowMap::GetRainVP();
            // Metres per unit of ortho depth - how a sampled ground depth becomes
            // a world height. Read from the map's own constants rather than
            // written down here, because a hard-coded 349 would rot silently.
            mpush.basin[1] = ShadowMap::RainZFar() - ShadowMap::RainZNear();
            // How far above the still line the height map is still believed to be
            // GROUND. Above that it is a hillside over a buried sheet or a cellar
            // ceiling, and the step falls back to its own downward ray. Tied to the
            // wave, because the wave is what decides how high water can plausibly
            // reach: a band that does not cover the swash would clip the run-up,
            // one that covers a whole storey would soak basements. Both terms are
            // metres - the crest of the swell, and how far a surge climbs past it.
            mpush.basin[2] = std::max(0.6f, 1.5f * (ps_r_wtr_wave + ps_r_wtr_swash));

            VkBuffer lastVB = VK_NULL_HANDLE, lastIB = VK_NULL_HANDLE;
            VkIndexType lastIT = VK_INDEX_TYPE_MAX_ENUM;
            for (const DrawItem& it : items) {
                auto* rv = it.vis;
                if (!rv || (rv->Type != MT_NORMAL && rv->Type != MT_PROGRESSIVE)) continue;
                auto* fv = static_cast<vkFVisual*>(rv);
                if (!fv->m_mesh.IsValid() || !fv->m_mesh.p_rm_Vertices || !fv->m_mesh.p_rm_Indices) continue;
                if (fv->m_mesh.vStride != 32) continue;
                if (0 != std::memcmp(&it.xform, &Fidentity, sizeof(Fmatrix))) continue;
                // FETCH is per body of water and the swell is gated by it, so it
                // has to be pushed per visual - the same number the surface shader
                // gets, or a cellar's wet band would breathe to the marsh's swell.
                mpush.basin[0] = BasinFetch(fv);
                vkCmdPushConstants(cmd, s_maskLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(mpush), &mpush);
                VkBuffer vb = fv->m_mesh.p_rm_Vertices->GetHandle();
                if (vb != lastVB) { VkDeviceSize off = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off); lastVB = vb; }
                VkBuffer    ib    = fv->m_mesh.p_rm_Indices->GetHandle();
                VkIndexType iType = fv->m_mesh.iType;
                if (ib != lastIB || iType != lastIT) { vkCmdBindIndexBuffer(cmd, ib, 0, iType); lastIB = ib; lastIT = iType; }
                const u32 firstIndex = it.iCountOverride ? it.iBaseOverride  : fv->m_mesh.iBase;
                const u32 indexCount = it.iCountOverride ? it.iCountOverride : fv->m_mesh.iCount;
                vkCmdDrawIndexed(cmd, indexCount, 1, firstIndex, (s32)fv->m_mesh.vBase, 0);
            }
            vkCmdEndRendering(cmd);
            ImageBarrier(cmd, mimg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            // Straight back to GENERAL: the step reads it as a storage image, and
            // that is the layout its descriptor was written with.
            if (surfOn)
                ImageBarrier(cmd, simg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            maskOK    = true;
            surfDrawn = surfOn;

            // ---- LID map -----------------------------------------------------
            // The level's opaque statics, same projection, keeping the LOWEST
            // surface above the water. Without it the mask alone says a cellar
            // floor is one 31 m pool (the audit drew exactly that disc): from
            // above, the nearest thing over a basement is the building's roof,
            // so the sheet running under the floor looks wide open.
            // ...but only when the CPU is not building it from the collision
            // model instead (r_wtr_sim_lid). The two write the same image, and
            // the rasterizer would clear what the ray-traced grid just uploaded.
            const VkImage limg = WaterRipple::LidImage();
            if (!ps_r_wtr_sim_lid && s_lidPipe != VK_NULL_HANDLE && limg != VK_NULL_HANDLE) {
                VkDescriptorImageInfo mii{};
                // â  ShadowMap::GetSampler is LINEAR despite this note - see WaterRipple::GetPointSampler.
                // Was meant to be POINT: R32_SFLOAT is not required to support linear
                // filtering, and the mask is a height field where interpolating
                // across the dry border would invent surfaces that are not there.
                mii.sampler     = ShadowMap::GetSampler();
                mii.imageView   = WaterRipple::MaskView();
                mii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                VK::DescriptorWriter(s_lidSet)
                    .Image(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mii)
                    .Flush();

                ImageBarrier(cmd, limg,
                             s_lidFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                s_lidFirst = false;

                // CLEAR to the "nothing overhead" sentinel.
                VK::RenderingBuilder(mtex, mtex)
                    .ColorClear(WaterRipple::LidView(), VkClearColorValue{ { WaterRipple::LidNoneValue(), 0.f, 0.f, 0.f } })
                    .Begin(cmd);
                vkCmdSetViewport(cmd, 0, 1, &mvp);
                vkCmdSetScissor(cmd, 0, 1, &msc);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_lidPipe);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_lidLayout, 0, 1, &s_lidSet, 0, nullptr);
                struct { float tile[4]; Fmatrix model; } lpush{};
                std::memcpy(lpush.tile, mpush.tile, sizeof(lpush.tile));

                // Only what overlaps the tile: a 64 m window over a level's worth
                // of statics is a small fraction of them, and the rest would be
                // draw calls landing entirely outside the scissor.
                const float tileMinX = WaterRipple::OriginX(), tileMinZ = WaterRipple::OriginZ();
                const float tileMax  = WaterRipple::SizeMetres();
                u32 lidDrawn = 0;
                lastVB = lastIB = VK_NULL_HANDLE; lastIT = VK_INDEX_TYPE_MAX_ENUM;
                for (const DrawItem& it : s_lidStatics) {
                    auto* rv = it.vis;
                    if (!rv || (rv->Type != MT_NORMAL && rv->Type != MT_PROGRESSIVE)) continue;
                    auto* fv = static_cast<vkFVisual*>(rv);
                    if (!fv->m_mesh.IsValid() || !fv->m_mesh.p_rm_Vertices || !fv->m_mesh.p_rm_Indices) continue;
                    if (fv->m_mesh.vStride != 32) continue;
                    // Transform the visual's box before culling - an instanced
                    // building's box is in OBJECT space, and testing it raw
                    // rejects exactly the floors that do the hiding.
                    Fbox bb = fv->vis.box;
                    if (0 != std::memcmp(&it.xform, &Fidentity, sizeof(Fmatrix))) {
                        Fbox src = bb; bb.invalidate();
                        Fvector corner;
                        for (int ci = 0; ci < 8; ++ci) {
                            corner.set((ci & 1) ? src.max.x : src.min.x,
                                       (ci & 2) ? src.max.y : src.min.y,
                                       (ci & 4) ? src.max.z : src.min.z);
                            Fvector w; it.xform.transform_tiny(w, corner);
                            bb.modify(w);
                        }
                    }
                    if (bb.max.x < tileMinX || bb.min.x > tileMinX + tileMax) continue;
                    if (bb.max.z < tileMinZ || bb.min.z > tileMinZ + tileMax) continue;
                    lpush.model = it.xform;
                    vkCmdPushConstants(cmd, s_lidLayout,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(lpush), &lpush);
                    VkBuffer vb = fv->m_mesh.p_rm_Vertices->GetHandle();
                    if (vb != lastVB) { VkDeviceSize off = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off); lastVB = vb; }
                    VkBuffer    ib    = fv->m_mesh.p_rm_Indices->GetHandle();
                    VkIndexType iType = fv->m_mesh.iType;
                    if (ib != lastIB || iType != lastIT) { vkCmdBindIndexBuffer(cmd, ib, 0, iType); lastIB = ib; lastIT = iType; }
                    const u32 firstIndex = it.iCountOverride ? it.iBaseOverride  : fv->m_mesh.iBase;
                    const u32 indexCount = it.iCountOverride ? it.iCountOverride : fv->m_mesh.iCount;
                    vkCmdDrawIndexed(cmd, indexCount, 1, firstIndex, (s32)fv->m_mesh.vBase, 0);
                    ++lidDrawn;
                }
                vkCmdEndRendering(cmd);
                ImageBarrier(cmd, limg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
                static bool s_lidLog = false;
                if (!s_lidLog) { s_lidLog = true;
                    Msg("[VK Water] lid map: %u static(s) overlap the %.0f m tile", lidDrawn, tileMax); }
            }
            ImageBarrier(cmd, mimg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        }
    }
    WaterRipple::SetMaskValid(maskOK);
    WaterRipple::SetSurfValid(maskOK && surfDrawn);

    // ---- LEVEL-WIDE water height map, built once -------------------------
    if (s_maskPipe != VK_NULL_HANDLE && (const void*)g_pGameLevel != s_lvlLevel) {
        s_lvlLevel = (const void*)g_pGameLevel;
        s_lvlBuilt = false;
    }
    // Redraw a WINDOW around the camera every few frames. Not "each visual once":
    // the mask shader rejects water with no headroom above it (the test that keeps
    // a sheet buried under a bank out of the map), and that test reads the ground
    // map - which only exists within ~Â±75 m of the camera. Drawn in one go at load,
    // the check was inert almost everywhere, and a second sheet running under the
    // hillside came out marked as standing water metres above the river.
    //
    // MAX blend makes a wrong write PERMANENT, so the rule is simple: never write
    // outside the region where the test can actually be evaluated. The map then
    // fills in correctly as you walk, the same way the lid rays do.
    static u32 s_lvlLastFrame = 0;
    if (s_maskPipe != VK_NULL_HANDLE && ps_r_wtr_wet > 0.f
        && (!s_lvlBuilt || RV().frameIndex - s_lvlLastFrame >= 4)) {
        s_lvlLastFrame = RV().frameIndex;
        static xr_vector<const DrawItem*> fresh;
        fresh.clear();
        for (const DrawItem& it : items) {
            if (!it.vis) continue;
            if (0 != std::memcmp(&it.xform, &Fidentity, sizeof(Fmatrix))) continue;
            fresh.push_back(&it);
        }
        if (!fresh.empty()) {
            // FIXED extent, centred on where the player first met water. Derived
            // from the visuals' bounds it followed whatever was on screen and left
            // the player outside the map entirely; a fixed square cannot do that.
            if (!s_lvlBuilt) {
                s_lvlSize = kLvlSpan;
                const float mpt = kLvlSpan / float(kLvlTexels);
                s_lvlOX = std::floor((RV().vCameraPosition.x - 0.5f * kLvlSpan) / mpt) * mpt;
                s_lvlOZ = std::floor((RV().vCameraPosition.z - 0.5f * kLvlSpan) / mpt) * mpt;
            }

            // SCISSOR: only where the ground map is valid (~Â±75 m of the camera);
            // 70 m keeps a margin. Computed BEFORE the pass so an empty window
            // skips the whole thing instead of opening one to draw nothing.
            const float mptL = s_lvlSize / float(kLvlTexels);
            s32 sx0 = (s32)std::floor((RV().vCameraPosition.x - 70.f - s_lvlOX) / mptL);
            s32 sz0 = (s32)std::floor((RV().vCameraPosition.z - 70.f - s_lvlOZ) / mptL);
            s32 sx1 = (s32)std::ceil ((RV().vCameraPosition.x + 70.f - s_lvlOX) / mptL);
            s32 sz1 = (s32)std::ceil ((RV().vCameraPosition.z + 70.f - s_lvlOZ) / mptL);
            sx0 = _max(sx0, 0); sz0 = _max(sz0, 0);
            sx1 = _min(sx1, (s32)kLvlTexels); sz1 = _min(sz1, (s32)kLvlTexels);

            bool ok = (sx1 > sx0 && sz1 > sz0) && (s_lvl != VK_NULL_HANDLE);
            if (!ok && sx1 > sx0 && sz1 > sz0 && s_lvl == VK_NULL_HANDLE) {
                ok = CreateImage2D(VK_FORMAT_R32_SFLOAT, { kLvlTexels, kLvlTexels },
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   s_lvl, s_lvlAlloc, "WaterLevelMap");
                if (ok) { s_lvlView = CreateImageView(s_lvl, VK_FORMAT_R32_SFLOAT); ok = (s_lvlView != VK_NULL_HANDLE); }
            }
            if (ok) {
                // CLEAR the first time, LOAD afterwards - the map accumulates.
                ImageBarrier(cmd, s_lvl,
                             s_lvlBuilt ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                VK::RenderingBuilder lrb(kLvlTexels, kLvlTexels);
                lrb.ColorClear(s_lvlView, VkClearColorValue{ { WaterRipple::MaskDryValue(), 0.f, 0.f, 0.f } });
                if (s_lvlBuilt) lrb.SetColorLoad(VK_ATTACHMENT_LOAD_OP_LOAD);   // the map accumulates
                lrb.Begin(cmd);
                VkViewport lvp{ 0.f, 0.f, float(kLvlTexels), float(kLvlTexels), 0.f, 1.f };
                vkCmdSetViewport(cmd, 0, 1, &lvp);
                VkRect2D lsc{ { sx0, sz0 }, { u32(sx1 - sx0), u32(sz1 - sz0) } };
                vkCmdSetScissor(cmd, 0, 1, &lsc);
                // The PLAIN variant: one target. The level map is 2048Â² and the
                // live surface it would carry means nothing at 2 m per texel - the
                // swash is centimetres and this map answers "is there a sheet here,
                // at what height", which does not move.
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_maskPipe);
                if (envSetMask != VK_NULL_HANDLE)
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_maskLayout, 1, 1, &envSetMask, 0, nullptr);
                MaskPush lpush{};
                lpush.tile[0] = s_lvlOX; lpush.tile[1] = s_lvlOZ;
                lpush.tile[2] = 1.f / _max(s_lvlSize, 0.001f);
                lpush.tile[3] = 1.f;   // STRICT: reject any water with ground over it
                lpush.rainVP  = ShadowMap::GetRainVP();
                lpush.basin[1] = ShadowMap::RainZFar() - ShadowMap::RainZNear();
                vkCmdPushConstants(cmd, s_maskLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(lpush), &lpush);
                VkBuffer lvb = VK_NULL_HANDLE, lib = VK_NULL_HANDLE;
                VkIndexType lit = VK_INDEX_TYPE_MAX_ENUM;
                u32 drawn = 0;
                for (const DrawItem* pit : fresh) {
                    const DrawItem& it = *pit;
                    auto* rv = it.vis;
                    if (rv->Type != MT_NORMAL && rv->Type != MT_PROGRESSIVE) continue;
                    auto* fv = static_cast<vkFVisual*>(rv);
                    if (!fv->m_mesh.IsValid() || !fv->m_mesh.p_rm_Vertices || !fv->m_mesh.p_rm_Indices) continue;
                    if (fv->m_mesh.vStride != 32) continue;
                    VkBuffer vb = fv->m_mesh.p_rm_Vertices->GetHandle();
                    if (vb != lvb) { VkDeviceSize off = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off); lvb = vb; }
                    VkBuffer    ib = fv->m_mesh.p_rm_Indices->GetHandle();
                    VkIndexType iT = fv->m_mesh.iType;
                    if (ib != lib || iT != lit) { vkCmdBindIndexBuffer(cmd, ib, 0, iT); lib = ib; lit = iT; }
                    const u32 fi = it.iCountOverride ? it.iBaseOverride  : fv->m_mesh.iBase;
                    const u32 ic = it.iCountOverride ? it.iCountOverride : fv->m_mesh.iCount;
                    vkCmdDrawIndexed(cmd, ic, 1, fi, (s32)fv->m_mesh.vBase, 0);
                    ++drawn;
                }
                vkCmdEndRendering(cmd);
                ImageBarrier(cmd, s_lvl, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                const bool first = !s_lvlBuilt;
                s_lvlBuilt = true;
                if (first)
                    Msg("[VK Water] level water map: %.0f m across at %.2f m/texel, origin (%.0f, %.0f); filled in a %d x %d texel window around the camera, %u surface(s)",
                        s_lvlSize, s_lvlSize / float(kLvlTexels), s_lvlOX, s_lvlOZ,
                        sx1 - sx0, sz1 - sz0, drawn);
            }
        }
    }

    // OUTSIDE any render pass on purpose - this is a compute dispatch, and the
    // graphics stages below sample what it writes.
    // Stays HERE, unlike the spectral field above: this one CONSUMES the pool mask
    // the pass has just rasterized, so it cannot move ahead of it.
    VK::Prof::ZoneEnd(cmd, zMask);
    const int zRip = VK::Prof::ZoneBegin(cmd, "Wtr/Ripple");
    WaterRipple::Dispatch(cmd);
    VK::Prof::ZoneEnd(cmd, zRip);

    // One-shot census. Answers the question the next tier depends on: is there
    // enough geometry to displace real waves into, or must the surface stay a
    // per-pixel normal field? (Stock ponds are a few huge quads - hence the
    // analytic slope in the FS rather than vertex displacement.)
    static bool s_census = false;
    if (!s_census) {
        s_census = true;
        u32 tris = 0, verts = 0;
        for (const DrawItem& it : items) {
            auto* fv = static_cast<vkFVisual*>(it.vis);
            if (!fv) continue;
            tris  += fv->m_mesh.iCount / 3;
            verts += fv->m_mesh.vCount;
        }
        Msg("[VK Water] census: %zu water visual(s), %u tris, %u verts - %s",
            items.size(), tris, verts,
            (verts > 4096) ? "dense enough for vertex waves later" : "too coarse for vertex waves (per-pixel slope it is)");
        for (const DrawItem& it : items) {
            auto* fv = static_cast<vkFVisual*>(it.vis);
            if (!fv) continue;
            // Span and fetch alongside the mesh stats: "why is this water
            // heaving / why is it dead" is answered by which basin the surface
            // belongs to, and a surface that turns out to bundle a cellar
            // together with a lake shows up here as one implausibly large box.
            const Fbox& bb = fv->vis.box;
            Msg("[VK Water]   '%s' tris=%u verts=%u stride=%u tc=%u sphere=(%.1f,%.1f,%.1f) r=%.1f "
                "span=%.1fx%.1f m y=%.1f fetch=%.1f m",
                fv->dbg_name.c_str() ? fv->dbg_name.c_str() : "(unnamed)",
                fv->m_mesh.iCount / 3, fv->m_mesh.vCount, fv->m_mesh.vStride, fv->m_mesh.tcOffset,
                fv->vis.sphere.P.x, fv->vis.sphere.P.y, fv->vis.sphere.P.z, fv->vis.sphere.R,
                bb.max.x - bb.min.x, bb.max.z - bb.min.z, bb.min.y, BasinFetch(fv));
        }
    }

    // ---- snapshot the bottom ----------------------------------------------
    // Everything opaque plus the sky is already in the HDR target; blit it down
    // to half res so the fragment stage can sample the riverbed at a bent UV.
    // Has to happen HERE, before the water draws into that same target - a
    // moment later and the water would be refracting itself.
    // ---- did any water pixel survive, kFramesInFlight frames ago? ---------
    // The slot about to be reused was written that many frames back and its fence
    // has already been waited on at frame begin, so the result is there - asked
    // for WITHOUT the wait bit all the same, because a profiler must never be able
    // to stall the thing it measures. No answer yet (a fresh pool, a level change)
    // reads as "visible", which is the safe side: an unnecessary snapshot costs a
    // fraction of a millisecond, a missing one shows a stale riverbed.
    // Outside any render pass: the mask/lid passes above have ended, and
    // vkCmdResetQueryPool may not appear inside one.
    if (s_occlPool != VK_NULL_HANDLE) {
        if (s_occlHas[slot]) {
            u32 res[2] = { 0, 0 };   // [0] = samples, [1] = availability
            if (vkGetQueryPoolResults(VulkanHW.m_Device, s_occlPool, slot, 1,
                                      sizeof(res), res, sizeof(u32),
                                      VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS && res[1]) {
                s_occlSamples = res[0];
                s_waterSeen   = (res[0] != 0);
            }
        }
        vkCmdResetQueryPool(cmd, s_occlPool, slot, 1);
        s_occlHas[slot] = false;
    }

    const int zRef = VK::Prof::ZoneBegin(cmd, "Wtr/Refract");
    // The lean FS never samples the copy, so it never asks for one.
    const bool lean = ps_r_wtr_lean && s_fsLean != VK_NULL_HANDLE;
    const bool refractWanted = !lean && ps_r_wtr_refract > 0.0001f && EnsureRefract(ctx.extent);
    // r_wtr_occl 0 restores the unconditional snapshot - the A/B for this gate.
    const bool gateSkips = ps_r_wtr_occl != 0 && s_occlPool != VK_NULL_HANDLE && !s_waterSeen;
    // WARNING: TAKING a snapshot and HAVING one are different questions, and conflating
    // them is a visible bug rather than a slow frame. The shader's "is there a
    // bottom to sample" flag (p9[3]) and the descriptor below must key off the
    // second: the target keeps last frame's content, so a skipped frame refracts a
    // snapshot one frame old - which is the whole design - whereas keying off the
    // first would drop refraction entirely for the frame water comes into view and
    // pop the surface's whole look for it.
    // The FIRST frame after the target is created always blits: there is nothing
    // in it yet, and a black riverbed is not a stale one.
    const bool refractBlit = refractWanted && (s_refFirst || !gateSkips);
    if (refractBlit) {
        // The scene COLOUR leaves its frame-wide attachment layout here, and this is
        // the only place in the frame it does — so it goes through the tracker for
        // the same reason the depth flips do: the src scope becomes the real prior
        // access (whatever last drew into the HDR target) rather than the old
        // layout's canonical one, and the tracked layout stays true for the derived
        // inter-pass barrier. g_FrameGraph.Color() IS ctx.colorImage (both are
        // SceneColor's hdrImage — see CRender::Begin).
        VK::g_FrameGraph.Color().Require(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                         VK_ACCESS_2_TRANSFER_READ_BIT);
        ImageBarrier(cmd, s_refImg,
                     s_refFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        s_refFirst = false;
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstSubresource = blit.srcSubresource;
        blit.srcOffsets[1] = { (s32)ctx.extent.width, (s32)ctx.extent.height, 1 };
        blit.dstOffsets[1] = { (s32)s_refExt.width,   (s32)s_refExt.height,   1 };
        vkCmdBlitImage(cmd, ctx.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s_refImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        ImageBarrier(cmd, s_refImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // Back to the frame-wide invariant: the water draw writes into it next.
        VK::g_FrameGraph.Color().Require(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                                         | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    }
    // s_refFirst is cleared by the blit above, and the blit is unconditional on the
    // frame it would still be set - so this is exactly "the target holds a real
    // snapshot, this frame's or an earlier one's".
    const bool refractOn = refractWanted && !s_refFirst;

    // Scene depth: attachment â sampled for this pass, restored at the end. The
    // frame-wide invariant is DEPTH_ATTACHMENT_OPTIMAL, so both ends are ours.
    // Through g_FrameGraph.Depth(), not a hand-placed barrier: the src scope then
    // comes from what really touched the depth, and the tracked layout stays true
    // (the derived inter-pass barrier reads it — see vk_framegraph.h).
    // Paired with SceneDepthEndSample below — the pair is correct on both paths:
    // under the derived barrier the boundary already handed us SHADER_READ (we
    // declared Sampled), so this no-ops; on the conservative path it does the flip.
    SceneDepthToSampled(cmd);   // water.frag samples uSceneDepth; nothing earlier does

    {
        VkDescriptorImageInfo ii{};
        ii.sampler     = ShadowMap::GetSampler();     // clamp-to-edge, point
        ii.imageView   = Swapchain.m_DepthView;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // Ripple field. Until the sim has run there is nothing to point at, so
        // the slot borrows the depth image - a valid descriptor the shader never
        // reads, because p6.x gates every sample of it.
        VkDescriptorImageInfo ri{};
        ri.sampler     = WaterRipple::GetSampler() ? WaterRipple::GetSampler() : ShadowMap::GetSampler();
        ri.imageView   = WaterRipple::Ready() ? WaterRipple::GetView() : Swapchain.m_DepthView;
        ri.imageLayout = WaterRipple::Ready() ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorBufferInfo bi{};
        bi.buffer = s_ubo[slot].GetHandle(); bi.offset = 0; bi.range = sizeof(WaterParams);
        VkDescriptorImageInfo fi{};
        fi.sampler     = s_refSamp ? s_refSamp : ShadowMap::GetSampler();
        fi.imageView   = (refractOn && s_refView) ? s_refView : Swapchain.m_DepthView;
        fi.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // â FIVE, not four. The pool-mask descriptor (binding 4) was added below without
        // growing this array: `w[4] = w[3]` then wrote a whole VkWriteDescriptorSet past
        // the end of it and vkUpdateDescriptorSets was handed 5 entries, the last one
        // living in whatever the stack had there. Every frame the water drew, CLevel::Render
        // took an SEH fault - and xrGame is built without /EH, so the frame registrar
        // swallowed it as `threw [non-std]` and just SKIPPED the level render. The picture
        // froze while the engine cheerfully reported 56 fps.
        // The writer's capacity is checked on every append, so the failure mode the
        // comment above describes cannot recur.
        VK::DescriptorWriter dw(s_set[slot]);
        dw.Image (0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ii)
          .Buffer(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         bi)
          .Image (2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ri)
          .Image (3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fi);
        // Pool mask - r_wtr_debug 7, and the near-field grid's height source.
        // Borrows the depth image (a valid descriptor the shader never reads)
        // until the mask exists.
        // â  POINT. ShadowMap::GetSampler is documented as POINT and is LINEAR, and
        // this image is a height field with a -10000 "dry" sentinel: filtered, a
        // texel next to a shore comes back as a surface halfway to minus ten
        // kilometres, and the grid vertex there goes with it.
        VkDescriptorImageInfo mi{};
        mi.sampler     = WaterRipple::GetPointSampler() ? WaterRipple::GetPointSampler()
                                                        : ShadowMap::GetSampler();
        mi.imageView   = WaterRipple::MaskView() ? WaterRipple::MaskView() : Swapchain.m_DepthView;
        mi.imageLayout = WaterRipple::MaskView() ? VK_IMAGE_LAYOUT_GENERAL
                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dw.Image(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mi);
        // Live surface + ground — the shore break's depth field. Same point
        // sampler and the same dummy-until-it-exists rule as the mask.
        VkDescriptorImageInfo si{};
        si.sampler     = mi.sampler;
        si.imageView   = WaterRipple::SurfView() ? WaterRipple::SurfView() : Swapchain.m_DepthView;
        si.imageLayout = WaterRipple::SurfView() ? VK_IMAGE_LAYOUT_GENERAL
                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dw.Image(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, si);
        // Spectral field. ⚠ No dummy fallback here, unlike every optional slot
        // above: these are sampler2DArray and the stand-in the others use (the
        // scene depth view) is a plain 2D view — binding it would be a view-TYPE
        // mismatch, which is a different and much less forgiving kind of wrong.
        // WaterFFT therefore allocates and clears its images whether or not the
        // feature is enabled, and these two views are always real. p13.x is what
        // actually turns the sampling off.
        VkDescriptorImageInfo di{}, gi{};
        di.sampler     = WaterFFT::GetSampler();
        di.imageView   = WaterFFT::DispView();
        di.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        gi = di; gi.imageView = WaterFFT::DerivView();
        dw.Image(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, di)
          .Image(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, gi);
        // Local fetch — how big this column's pool actually is. Same point sampler
        // and the same dummy-until-it-exists rule as the mask: the value is metres
        // with 0 for "no answer", and filtering that would blend a real basin size
        // with a sentinel into a basin that is neither.
        // Initialise the map here rather than relying on the sim's Dispatch: the draw
        // below samples it every frame, Dispatch does not run with r_wtr_sim 0, and the
        // dummy this used to fall back to (the scene DEPTH view) is bound as
        // SHADER_READ_ONLY while the depth image is in DEPTH_ATTACHMENT — which is its
        // own violation (VUID-vkCmdDrawIndexed-imageLayout-00344). One clear removes
        // both the undefined read and the need for the fallback.
        WaterRipple::EnsureFetchInitialized(cmd);
        VkDescriptorImageInfo lf{};
        lf.sampler     = mi.sampler;
        lf.imageView   = WaterRipple::FetchView() ? WaterRipple::FetchView() : Swapchain.m_DepthView;
        lf.imageLayout = WaterRipple::FetchView() ? VK_IMAGE_LAYOUT_GENERAL
                                                  : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dw.Image(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, lf)
          .Flush();
    }

    // Colour only (LOAD/STORE) - no depth attachment, see the header.
    // BeginFlipped = the same X-Ray D3D-style flipped viewport every other scene
    // pass uses.
    VK::Prof::ZoneEnd(cmd, zRef);
    const int zDraw = VK::Prof::ZoneBegin(cmd, "Wtr/Draw");
    VK::RenderingBuilder(ctx.extent).Color(ctx.colorView).BeginFlipped(cmd);
    // Counts the fragments of the real surface (and of the near-field grid below
    // it) that pass the depth test. Must sit INSIDE the rendering instance, and
    // the pool slot was reset outside it a moment ago.
    if (s_occlPool != VK_NULL_HANDLE) { vkCmdBeginQuery(cmd, s_occlPool, slot, 0); s_occlHas[slot] = true; }

    // ---- per-frame parameters ---------------------------------------------
    // Wind steers the wave train and scales its amplitude, so a storm actually
    // whips the ponds up; m_fWaterIntensity is the weather config's own
    // per-hour water knob (the R2 water shader's wave scale) - reused verbatim
    // so existing weather content still means something here.
    float windX = 1.0f, windZ = 0.0f;
    float windStr = 0.35f, rain = 0.0f, waterIntensity = 1.0f;
    if (g_pGamePersistent) {
        auto& env = ES();
        if (env.CurrentEnv) {
            const float a = env.CurrentEnv->wind_direction;
            windX = std::cos(a); windZ = std::sin(a);          // XZ, same convention as the grass
            windStr = std::clamp(env.CurrentEnv->wind_velocity * 0.001f, 0.0f, 1.0f);
            rain    = std::clamp(env.CurrentEnv->rain_density, 0.0f, 1.0f);
            waterIntensity = std::max(env.CurrentEnv->m_fWaterIntensity, 0.0f);
        }
    }
    {
        const float m = std::sqrt(windX * windX + windZ * windZ);
        if (m < 1e-3f) { windX = 1.0f; windZ = 0.0f; }
        else           { windX /= m;   windZ /= m; }
    }

    // Wind drives the SWELL, and it must be able to drive it UP: the first cut
    // only ever scaled the amplitude DOWN (0.45..1.0) and multiplied that by the
    // weather's m_fWaterIntensity (which is routinely ~0.3), so a calm hour got
    // ~16% of the configured wave and the water read as dead flat. Now calm is a
    // real floor and a storm is a real multiplier; r_wtr_wind scales how much of
    // that the weather is allowed to do (0 = ignore the wind entirely).
    const float windGain = 1.0f + ps_r_wtr_wind * (2.2f * windStr - 0.55f);
    // The weather knob is a modifier, not a veto - most of the way toward 1.0.
    const float wiGain   = 0.65f + 0.35f * std::clamp(waterIntensity, 0.0f, 2.0f);

    // ---- near-field grid: is there a sheet under us to draw one over? ------
    // It needs the pool mask (its height source, and the only thing that knows
    // where water actually is at 12.5 cm) and one reference height for the texels
    // the mask calls dry, so the mesh does not tear at a shoreline. The nearest
    // sheet in plan view supplies both that height and the basin's fetch.
    //
    // Only sheets overlapping the grid's own tile count. The grid is rasterized
    // over exactly the ripple tile (WaterRipple's camera-anchored square), so a
    // body outside it cannot lie under the mesh, and picking it as the reference
    // is meaningless. Cheap guard only — it does NOT decide coverage: a water
    // BOUNDING BOX is a poor proxy for water (Cordon's river is a 148 m ribbon
    // whose box swallows the whole newbie village), so a tile can overlap the box
    // and still hold no water at all. Whether the grid actually draws anything is
    // settled per quad against the pool mask in water_grid.vert — see the note
    // there about the village that came out knee-deep.
    int   gridN     = 0;
    float gridFetch = 0.f, gridRefY = -1e9f;
    {
        const float tileX0 = WaterRipple::OriginX();       // settled by PrepareTile above
        const float tileZ0 = WaterRipple::OriginZ();
        const float tileSz = WaterRipple::SizeMetres();
        float best = 1e30f;
        for (const DrawItem& it : items) {
            auto* fv = static_cast<vkFVisual*>(it.vis);
            if (!fv || !fv->m_mesh.IsValid()) continue;
            if (0 != std::memcmp(&it.xform, &Fidentity, sizeof(Fmatrix))) continue;
            const Fbox& bb = fv->vis.box;
            // Plan-view overlap with the tile the grid actually covers.
            if (bb.max.x < tileX0 || bb.min.x > tileX0 + tileSz ||
                bb.max.z < tileZ0 || bb.min.z > tileZ0 + tileSz)
                continue;
            const float dx = std::max(std::max(bb.min.x - RV().vCameraPosition.x,
                                               RV().vCameraPosition.x - bb.max.x), 0.f);
            const float dz = std::max(std::max(bb.min.z - RV().vCameraPosition.z,
                                               RV().vCameraPosition.z - bb.max.z), 0.f);
            const float d  = dx * dx + dz * dz;
            if (d < best) { best = d; gridRefY = bb.min.y; gridFetch = BasinFetch(fv); }
        }
    }
    const bool gridOn = !lean && (ps_r_wtr_grid > 0) && (s_gridVS != VK_NULL_HANDLE)
                     && maskOK && WaterRipple::Ready() && (gridRefY > -1e8f);
    if (gridOn) gridN = std::clamp(ps_r_wtr_grid, 16, 384);

    WaterParams up{};
    up.p0[0] = std::fmod(RV().fTimeGlobal, 6283.185f);            // radians-safe wrap
    // â  MEASURED, not guessed: with wind_velocity 0 (which is most of the day in
    // this game's weather) the old floor of 0.35 put the swell at SEVEN CENTIMETRES
    // - the [VK Water] wind line printed 0.071 - spread across nine octaves from
    // 28 m down to 0.9 m. Seven centimetres of that, on a sheet whose triangles are
    // two hundred metres across, is not a wave; it is a mirror with a texture on
    // it, and no amount of shading was going to rescue it. r_wtr_calm is the floor:
    // what a dead calm still leaves of the configured wave.
    up.p0[1] = ps_r_wtr_wave * std::max(windGain, std::clamp(ps_r_wtr_calm, 0.f, 1.f)) * wiGain;
    up.p0[2] = ps_r_wtr_scale;
    up.p0[3] = ps_r_wtr_speed;
    up.p1[0] = windX;   up.p1[1] = windZ;
    up.p1[2] = ps_r_wtr_murk;
    up.p1[3] = rain;
    up.p2[0] = ps_r_wtr_color.x; up.p2[1] = ps_r_wtr_color.y; up.p2[2] = ps_r_wtr_color.z;
    up.p2[3] = ps_r_wtr_refl;
    up.p3[0] = ps_r_wtr_detail;
    up.p3[1] = ps_r_wtr_rough;
    up.p3[2] = ps_r_wtr_glint;
    up.p3[3] = float(ps_r_wtr_debug);
    up.p4[0] = ps_r_wtr_shore;
    up.p4[1] = ps_r_wtr_foam;
    up.p4[2] = ps_r_wtr_foam_w;
    up.p4[3] = ps_r_wtr_micro;
    // Tessellation + displacement. tessMax <= 1 (or no tess stages) leaves the
    // surface flat and the wave a per-pixel normal, which is still water - just
    // without a silhouette.
    up.p5[0] = s_tess ? ps_r_wtr_tess : 0.f;
    up.p5[1] = ps_r_wtr_tess_near;
    up.p5[2] = ps_r_wtr_tess_far;
    up.p5[3] = s_tess ? ps_r_wtr_disp : 0.f;
    const bool simOn = ps_r_wtr_sim && WaterRipple::Ready();
    up.p6[0] = simOn ? 1.f : 0.f;
    up.p6[1] = WaterRipple::OriginX();
    up.p6[2] = WaterRipple::OriginZ();
    up.p6[3] = WaterRipple::SizeMetres();
    up.p7[0] = ps_r_wtr_sim_slope;
    up.p7[1] = ps_r_wtr_sim_height;
    up.p7[2] = 1.f / float(std::max(WaterRipple::Texels(), 1u));
    up.p7[3] = gridRefY;    // the grid's fallback height where the mask says dry
    up.p8[0] = RV().vCameraPosition.x;
    up.p8[1] = RV().vCameraPosition.y;
    up.p8[2] = RV().vCameraPosition.z;
    up.p9[0] = ps_r_wtr_refract;
    up.p9[1] = ps_r_wtr_caustic;
    up.p9[2] = std::max(ps_r_wtr_caustic_p, 0.1f);
    up.p9[3] = refractOn ? 1.f : 0.f;   // the shader's "do I have a bottom to sample" gate
    up.p10[0] = ps_r_wtr_swash;                     // METRES of run-up, not a gain
    up.p10[1] = ps_r_wtr_shelter;
    up.p10[2] = std::max(ps_r_wtr_murk_still, 1.f);
    up.p10[3] = std::max(ps_r_wtr_swash_t, 1.f);    // seconds between crests
    // SHORE BREAK. Needs the live-surface map (its depth field) and the grid that
    // has the vertices to carry a crest; without either it stays off and the
    // wind-phased swash carries the shoreline as before.
    up.p11[0] = std::max(ps_r_wtr_surf_len, 2.f);   // metres between crests
    up.p11[1] = std::max(ps_r_wtr_surf, 0.f);       // whitewater strength
    up.p11[2] = std::max(ps_r_wtr_surf_h, 0.f);     // offshore wave height (m)
    // ⚠ NOT gated on r_wtr_surf. This field is the FILM, and the film is what keeps
    // the run-up a couple of centimetres deep instead of a slab of opaque water
    // standing on the bank — it has nothing to do with whether the water is white.
    // Tying the two together meant that turning the whitewater off (which is now
    // the default) silently brought the wall back, because the depth fade fell
    // through to `depthV + swash` with no cap on it.
    //
    // So: p11.w = "the depth field is available, and here is the film thickness",
    // p11.y = "and this is how white the broken water gets", which may be zero.
    // The waterline still arrives and drains with r_wtr_surf 0; only the foam goes.
    up.p11[3] = surfDrawn ? std::max(ps_r_wtr_film, 0.01f) : 0.f;

    // SPECTRAL WAVES. The three cascade sizes come from the module rather than
    // being recomputed here — one owner for the number that decides where a tile
    // repeats, or the shader would sample a grid the compute stage never wrote.
    for (u32 c = 0; c < 3; ++c)
        up.p12[c] = (c < WaterFFT::kCascades) ? WaterFFT::CascadeLen(c) : 0.f;
    up.p12[3] = std::max(ps_r_wtr_fft_gain, 0.f);
    // ⭐ Ready(), not the cvar. The field is only worth sampling once a full
    // transform has landed in it, and on the frame the switch is flipped it has
    // not. Gating on the cvar alone would sample a cleared image and read as "the
    // wave vanished for one frame" — a flicker with no cause in the code you'd
    // then be reading.
    up.p13[0] = WaterFFT::Ready() ? 1.f : 0.f;
    up.p13[1] = std::max(ps_r_wtr_fft_foam, 0.f);
    up.p13[2] = std::max(ps_r_wtr_fft_fetch, 1.f);
    up.p13[3] = std::max(ps_r_wtr_fft_slope, 0.f);
    // Same matrix the wetness and the ambient gate query the sky-occlusion map
    // with. The map itself is set 1 / binding 9; only the matrix has to be
    // copied, because the tessellation stages have no light UBO to read it from.
    std::memcpy(up.rainVP, &ShadowMap::GetRainVP(), sizeof(up.rainVP));
    if (void* dst = s_ubo[slot].Map()) { std::memcpy(dst, &up, sizeof(up)); s_ubo[slot].Unmap(); }

    WaterPush push{};

    VkDescriptorSet envSet = EnvLight::GetCurrentSet();
    VkPipeline lastPipe = VK_NULL_HANDLE;
    VkBuffer   lastVB   = VK_NULL_HANDLE, lastIB = VK_NULL_HANDLE;
    VkIndexType lastIT  = VK_INDEX_TYPE_MAX_ENUM;
    u32 drawn = 0, skippedXform = 0;

    for (const DrawItem& it : items) {
        auto* rv = it.vis;
        if (!rv || (rv->Type != MT_NORMAL && rv->Type != MT_PROGRESSIVE)) continue;
        auto* fv = static_cast<vkFVisual*>(rv);
        if (!fv->m_mesh.IsValid() || !fv->m_mesh.p_rm_Vertices || !fv->m_mesh.p_rm_Indices) continue;
        if (fv->m_mesh.vStride != 32) continue;   // world layouts only
        // The VS treats the raw position as world space (level statics submit
        // identity). A dynamic water visual would need its model rows pushed -
        // no stock content has one, so count it rather than draw it wrong.
        if (0 != std::memcmp(&it.xform, &Fidentity, sizeof(Fmatrix))) { ++skippedXform; continue; }

        const u32 s = (fv->m_mesh.tcOffset == 28) ? 1u : 0u;
        VkPipeline* slotPipe = lean ? &s_pipeLean[s] : &s_pipe[s];
        if (*slotPipe == VK_NULL_HANDLE) *slotPipe = BuildPipeline(fv->m_mesh.tcOffset, false, lean);
        VkPipeline pipe = *slotPipe;
        if (pipe == VK_NULL_HANDLE) continue;

        if (pipe != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            lastPipe = pipe; lastVB = lastIB = VK_NULL_HANDLE;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 1, &s_set[slot], 0, nullptr);
            if (envSet != VK_NULL_HANDLE)
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 1, 1, &envSet, 0, nullptr);
        }

        // FETCH - per surface, which is why it rides the push and not the UBO.
        // The wave field used to be a function of world XZ alone, so a flooded
        // cellar got the same 28-metre swell as the Cordon pond and heaved like
        // surf. A wave needs room: the shader keeps only the octaves whose
        // wavelength fits in this span several times over.
        push.mvp = *ctx.viewProj;
        push.basin[0] = BasinFetch(fv);
        push.basin[1] = 0.f;                          // these are the LEVEL polygons
        // ...and they must get out of the near-field grid's way where it draws,
        // or the two premultiplied layers stack and the near water comes out twice
        // as opaque. See the cross-fade at the end of water.frag.
        push.basin[3] = gridOn ? 1.f : 0.f;
        vkCmdPushConstants(cmd, s_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
                           | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(WaterPush), &push);

        VkBuffer vb = fv->m_mesh.p_rm_Vertices->GetHandle();
        if (vb != lastVB) {
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
            lastVB = vb;
        }
        VkBuffer    ib    = fv->m_mesh.p_rm_Indices->GetHandle();
        VkIndexType iType = fv->m_mesh.iType;
        if (ib != lastIB || iType != lastIT) {
            vkCmdBindIndexBuffer(cmd, ib, 0, iType);
            lastIB = ib; lastIT = iType;
        }
        const u32 firstIndex = it.iCountOverride ? it.iBaseOverride  : fv->m_mesh.iBase;
        const u32 indexCount = it.iCountOverride ? it.iCountOverride : fv->m_mesh.iCount;
        vkCmdDrawIndexed(cmd, indexCount, 1, firstIndex, (s32)fv->m_mesh.vBase, 0);
        ++drawn;
    }

    // ---- NEAR-FIELD GRID ---------------------------------------------------
    // The mesh the level does not have. Everything above draws water whose
    // triangles are up to two hundred metres across, which no amount of hardware
    // tessellation (capped at 64 per edge) can subdivide below ~3 m - so the wave
    // had nowhere to exist as geometry. This draws the near 64 m again on a grid
    // fine enough to hold one, taking its height from the pool mask and displacing
    // in the vertex stage. No buffers: every position comes out of gl_VertexIndex.
    if (gridOn) {
        if (s_gridPipe == VK_NULL_HANDLE) s_gridPipe = BuildPipeline(28, true);
        if (s_gridPipe != VK_NULL_HANDLE) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_gridPipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 1, &s_set[slot], 0, nullptr);
            if (envSet != VK_NULL_HANDLE)
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 1, 1, &envSet, 0, nullptr);
            WaterPush gp{};
            gp.mvp = *ctx.viewProj;
            // The fetch of the body the camera is standing in - the grid is one
            // draw over one tile, so it gets one basin, and the near one is the
            // only one that can be under it.
            gp.basin[0] = gridFetch;
            gp.basin[1] = 1.f;                       // "I am the grid"
            gp.basin[2] = float(gridN);              // quads per side
            vkCmdPushConstants(cmd, s_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
                               | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(WaterPush), &gp);
            vkCmdDraw(cmd, u32(gridN) * u32(gridN) * 6u, 1, 0, 0);
        }
    }

    if (s_occlPool != VK_NULL_HANDLE && s_occlHas[slot]) vkCmdEndQuery(cmd, s_occlPool, slot);
    vkCmdEndRendering(cmd);
    VK::Prof::ZoneEnd(cmd, zDraw);

    // Nothing here needs the attachment layout back — only the passes after us do,
    // and the derived barrier asks for what each of them declared. So this DEFERS
    // under r_fg_barriers: the light cones sample the same depth next and ride this
    // window instead of opening their own. See vk_framegraph.h.
    SceneDepthEndSample(cmd);
    // WARNING: COUNTS, NOT A SNAPSHOT OF THE MOMENT. A throttled line that prints the
    // current state answers "was water visible when the timer fired", which is not
    // the question - the question is what fraction of frames paid for the copy.
    ++s_occlFrames;
    if (refractBlit) ++s_occlBlits;
    if (RV().dwTimeContinual >= s_occlLogAt) {
        if (s_occlLogAt != 0 && s_occlFrames)
            Msg("[VK Water] refraction snapshot taken on %u of %u frames (%.0f%%) | last query %u samples, gate %s",
                s_occlBlits, s_occlFrames, 100.0 * double(s_occlBlits) / double(s_occlFrames),
                s_occlSamples, ps_r_wtr_occl ? "on" : "off (r_wtr_occl 0)");
        s_occlLogAt  = RV().dwTimeContinual + 5000;
        s_occlFrames = 0;
        s_occlBlits  = 0;
    }

    VK::Prof::ZoneEnd(cmd, zone);

    static bool s_once = false;
    if (!s_once) {
        s_once = true;
        Msg("[VK Water] first flush: %u surface(s) drawn, %u skipped (non-identity xform)", drawn, skippedXform);
        // The wave amplitude that actually reached the shader, next to what the
        // weather asked for. "The water is dead flat" is nearly always one of
        // these three being ~0, and guessing which cost a round trip once.
        Msg("[VK Water] wind vel=%.0f (str=%.2f) dir=(%.2f,%.2f) waterIntensity=%.2f -> swell=%.3f micro=%.3f",
            windStr * 1000.f, windStr, windX, windZ, waterIntensity, up.p0[1], up.p4[3]);
    }
}

void Water_Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    WaterFFT::Destroy();
    if (s_refView)  { vkDestroyImageView(VulkanHW.m_Device, s_refView, nullptr); s_refView = VK_NULL_HANDLE; }
    if (s_refImg)   { vmaDestroyImage(VulkanHW.m_Allocator, s_refImg, s_refAlloc); s_refImg = VK_NULL_HANDLE; }
    if (s_refSamp)  { vkDestroySampler(VulkanHW.m_Device, s_refSamp, nullptr); s_refSamp = VK_NULL_HANDLE; }
    if (s_occlPool) { vkDestroyQueryPool(VulkanHW.m_Device, s_occlPool, nullptr); s_occlPool = VK_NULL_HANDLE; }
    for (bool& h : s_occlHas) h = false;
    s_waterSeen = true;
    s_refExt = VkExtent2D{}; s_refFirst = true;
    for (auto& b : s_ubo) b.Destroy();
    for (auto& p : s_pipe)
        if (p) { vkDestroyPipeline(VulkanHW.m_Device, p, nullptr); p = VK_NULL_HANDLE; }
    for (auto& p : s_pipeLean)
        if (p) { vkDestroyPipeline(VulkanHW.m_Device, p, nullptr); p = VK_NULL_HANDLE; }
    s_fsLean = VK_NULL_HANDLE;
    if (s_gridPipe) { vkDestroyPipeline(VulkanHW.m_Device, s_gridPipe, nullptr); s_gridPipe = VK_NULL_HANDLE; }
    s_gridVS = VK_NULL_HANDLE;
    if (s_layout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, s_layout, nullptr);    s_layout = VK_NULL_HANDLE; }
    if (s_maskPipe)     { vkDestroyPipeline(VulkanHW.m_Device, s_maskPipe, nullptr);         s_maskPipe = VK_NULL_HANDLE; }
    if (s_maskSurfPipe) { vkDestroyPipeline(VulkanHW.m_Device, s_maskSurfPipe, nullptr);     s_maskSurfPipe = VK_NULL_HANDLE; }
    if (s_maskLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_maskLayout, nullptr);   s_maskLayout = VK_NULL_HANDLE; }
    if (s_maskUboPool) { vkDestroyDescriptorPool(VulkanHW.m_Device, s_maskUboPool, nullptr); s_maskUboPool = VK_NULL_HANDLE; }
    if (s_maskUboSetLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_maskUboSetLayout, nullptr); s_maskUboSetLayout = VK_NULL_HANDLE; }
    for (auto& ms : s_maskUboSet) ms = VK_NULL_HANDLE;
    s_maskVS = s_maskFS = s_maskSurfFS = VK_NULL_HANDLE; s_maskFirst = true; s_surfFirst = true;
    if (s_pool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr);      s_pool = VK_NULL_HANDLE; }
    if (s_setLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    for (auto& s : s_set) s = VK_NULL_HANDLE;
    s_inited = false; s_failed = false;
}

}  // namespace VK
