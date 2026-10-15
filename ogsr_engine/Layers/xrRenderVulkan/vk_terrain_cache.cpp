// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// TERRAIN COMPOSITE CACHE - see vk_terrain_cache.h. Modeled on vk_deform
// (camera-anchored compute-baked field on the EnvLight set). Two extra pieces:
//  - the WORLD -> terrain-base-UV affine is not knowable from CPU data (level
//    VBs keep no CPU copy) -> derived ONCE per level by vkCmdCopyBuffer-ing a
//    few terrain vertices out of the level VB (created with TRANSFER_SRC) into
//    a host staging buffer and least-squares solving u,v = A*world.xz + b;
//  - the bake reads the terrain MATERIAL set (mask + 4 heights) at set 0, so
//    the terrain set layout carries VK_SHADER_STAGE_COMPUTE_BIT now.

#include "stdafx.h"
#include "vk_env_snapshot.h"
#include "vk_render_view.h"
#include "vk_rendering.h"          // VK::RenderingBuilder
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_terrain_cache.h"
#include "vk_image.h"            // VK::CreateImage / CreateImageView
#include "vk_compute_util.h"     // VK::MakePipelineLayout / CreateComputePipeline
#include "vk_shaders.h"          // g_ShaderManager
#include "vk_world_material.h"   // GetTerrainSetLayout()
#include "vk_gfx_pipeline.h"     // VK::GfxPipelineBuilder
#include "vk_profiler.h"         // VK::Prof::NameImage
#include "vk_env_light.h"        // EnvLight::TerrainChOff — per-level SSFX channel offsets
#include "vk_Visual.h"           // vkFVisual/vkFHierrarhyVisual — TerrainMask bake walks Visuals[]
#include "vk_barriers.h"         // VK::ImageBarrier — TerrainMask bake layout transitions
#include "vk_command_buffer.h"   // CommandManager.BeginImmediate — one-shot bake submit
#include "CRender_Vulkan.h"      // RImplementation.Visuals
#include "vk_world_gpu.h"        // WorldGPU::LeafVisuals — shared load-time leaf walk
#include "../../xr_3da/IGame_Persistent.h" // ES()
#include "../../xr_3da/Environment.h"      // CEnvDescriptorMixer::sun_dir (horizon bake azimuth)

#include <unordered_set>

extern int ps_r_terra_cache;     // r_terra_cache (vk_console_min.cpp)
extern int ps_r_terra_cone;      // r_terra_cone: bake+march the cone-step channel
extern int ps_r_terra_horizon;   // r_terra_horizon: bake the sun-horizon channel (needs cone pass)
extern float ps_r_terra_blend;   // r_terra_blend: detail-blend depth (0 = GAMMA plain-mask cross-fade)

namespace VK { namespace TerrainCache {

namespace {
    bool s_inited = false, s_failed = false;

    constexpr u32   kSize   = 2048;   // 2048^2: R16F 8MB + RGBA8 16MB
    constexpr float kHalf   = 24.f;   // +/-24 m window (2.3 cm/texel); POM range ~19 m fits
    constexpr float kRebake = 4.f;    // re-bake when the camera drifts this far from the baked centre
    // Per-level SSFX channel offsets now come from EnvLight::TerrainChOff()
    // (terrain_details.ltx) — the same values the shaders read as L.ch_off, so
    // the baked cache and the direct march stay in agreement.
    constexpr float kUVScale  = 1.0f / 1024.0f;   // statics SHORT2 tc quantization (matches draw pushes)

    struct Buf { VkImage img = VK_NULL_HANDLE; VmaAllocation alloc = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
    Buf s_height;    // RG16F: .r composite height, .g cone-step ratio (cone bake pass)
    Buf s_weights;   // RGBA8 blend weights

    // Height max-pyramid for the cone bake's conservative ring search
    // (kSize/2 base, 3 mips = block sizes 2/4/8 texels). Bake-internal only:
    // lives in GENERAL layout forever, never bound to the frag shaders.
    constexpr u32 kPyrMips = 3;
    Buf         s_pyr;                                       // view = ALL mips (texelFetch source)
    VkImageView s_pyrMip[kPyrMips] = {};                     // per-mip storage views
    bool        s_pyrLayoutInit = false;                     // UNDEFINED -> GENERAL done

    VkSampler             s_sampler    = VK_NULL_HANDLE;
    VkDescriptorSetLayout s_setLayout  = VK_NULL_HANDLE;   // set 1: storage images + pyramid sampler
    VkDescriptorPool      s_pool       = VK_NULL_HANDLE;
    VkDescriptorSet       s_set        = VK_NULL_HANDLE;
    VkPipelineLayout      s_pipeLayout = VK_NULL_HANDLE;
    VkPipeline            s_pipe       = VK_NULL_HANDLE;
    VkPipeline            s_pipeMip    = VK_NULL_HANDLE;   // terrain_cache_mip.comp (max-reduce)
    VkPipeline            s_pipeCone   = VK_NULL_HANDLE;   // terrain_cache_cone.comp (cone ratio -> .g)

    struct PushCS {
        float originExtent[4];   // x,y origin XZ; z extent (m); w height-tap lod
        float uvU[4];            // u = dot(uvU.xy, wxz) + uvU.z
        float uvV[4];
        float chOff[4];
        float dsPad[4];          // x = detailScale
        float tmask[4];          // baked world-space mask affine (mask-less maps); z==0 = off
    };

    // Captured terrain draw (first terrain item seen after level load).
    VkDescriptorSet s_terrainSet  = VK_NULL_HANDLE;
    bool            s_multiTerrain = false;   // >1 terrain material -> v1 cache off (would march the wrong heights)
    float           s_detailScale = 64.f;
    VkBuffer        s_meshVB      = VK_NULL_HANDLE;
    u32             s_meshVBase = 0, s_meshStride = 0, s_meshTCOff = 0;

    // Affine derivation state.
    constexpr u32  kProbeVerts = 16;
    VkBuffer       s_stage      = VK_NULL_HANDLE;
    VmaAllocation  s_stageAlloc = VK_NULL_HANDLE;
    u8*            s_stageMap   = nullptr;
    u32            s_copyFrame  = 0;      // RV().frameIndex when the copy was recorded (0 = none)
    bool           s_affineOk   = false;  // uv = A*wxz + b solved
    float          s_uvU[3], s_uvV[3];    // u = uvU[0]*x + uvU[1]*z + uvU[2]

    // Bake state.
    bool  s_baked  = false;     // images hold a valid bake (layout = SHADER_READ)
    bool  s_bakedCone = false;  // the current bake includes the cone channel (.g)
    bool  s_bakedHor  = false;  // the current bake includes the sun-horizon channel (.b)
    float s_bakedBlend = -1.f;  // r_terra_blend the weights were baked with
    float s_bakedCX = 0.f, s_bakedCZ = 0.f;   // baked window centre
    float s_sunU = 1.f, s_sunV = 0.f;         // baked horizon azimuth (cache-space, toward sun)
    // Re-bake when the sun azimuth drifts past ~2.5 deg from the baked horizon
    // direction (game-time sun moves slowly; the smoothstep penumbra hides it).
    constexpr float kSunCos = 0.999f;

    // Compute -> compute "everything written is visible" between the bake /
    // mip-reduce / cone dispatches (all images stay GENERAL) → VK::ComputeBarrier.

    bool createImage(VkFormat fmt, const char* name, Buf& out, u32 size = kSize, u32 mips = 1)
    {
        VK::ImageDesc d;
        d.format = fmt;
        d.extent = { size, size, 1 };
        d.mips   = mips;
        d.usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        d.name   = name;
        if (!VK::CreateImage(d, out.img, out.alloc)) return false;
        out.view = VK::CreateImageView(out.img, fmt, VK_IMAGE_VIEW_TYPE_2D,
                                       VK_IMAGE_ASPECT_COLOR_BIT, 0, mips);
        return out.view != VK_NULL_HANDLE;
    }

    void destroyBuf(Buf& b)
    {
        if (b.view) { vkDestroyImageView(VulkanHW.m_Device, b.view, nullptr); b.view = VK_NULL_HANDLE; }
        if (b.img)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, b.img, b.alloc); b.img = VK_NULL_HANDLE; }
    }

    // Least squares u = a*x + b*z + c over the probed vertices. Returns false on
    // a degenerate (collinear) probe set.
    bool solveAffine(const float* X, const float* Z, const float* U, u32 n, float out[3])
    {
        double Sx = 0, Sz = 0, Su = 0, Sxx = 0, Szz = 0, Sxz = 0, Sxu = 0, Szu = 0;
        for (u32 i = 0; i < n; ++i) {
            Sx += X[i]; Sz += Z[i]; Su += U[i];
            Sxx += double(X[i]) * X[i]; Szz += double(Z[i]) * Z[i];
            Sxz += double(X[i]) * Z[i];
            Sxu += double(X[i]) * U[i]; Szu += double(Z[i]) * U[i];
        }
        const double N = double(n);
        // Normal equations, 3x3 symmetric [Sxx Sxz Sx; Sxz Szz Sz; Sx Sz N].
        const double m[3][3] = { { Sxx, Sxz, Sx }, { Sxz, Szz, Sz }, { Sx, Sz, N } };
        const double r[3]    = { Sxu, Szu, Su };
        const double det = m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])
                         - m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])
                         + m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
        if (fabs(det) < 1e-9) return false;
        auto det3 = [&](int col) {
            double t[3][3];
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) t[i][j] = (j == col) ? r[i] : m[i][j];
            return t[0][0]*(t[1][1]*t[2][2]-t[1][2]*t[2][1])
                 - t[0][1]*(t[1][0]*t[2][2]-t[1][2]*t[2][0])
                 + t[0][2]*(t[1][0]*t[2][1]-t[1][1]*t[2][0]);
        };
        out[0] = float(det3(0) / det); out[1] = float(det3(1) / det); out[2] = float(det3(2) / det);
        return true;
    }

    // Parse the staged vertices and solve the affine map.
    void tryResolveAffine()
    {
        float X[kProbeVerts], Z[kProbeVerts], U[kProbeVerts], V[kProbeVerts];
        u32 n = 0;
        for (u32 i = 0; i < kProbeVerts; ++i) {
            const u8* v = s_stageMap + size_t(i) * s_meshStride;
            const float* pos = (const float*)v;
            const s16*   tc  = (const s16*)(v + s_meshTCOff);
            X[n] = pos[0]; Z[n] = pos[2];
            U[n] = float(tc[0]) * kUVScale;
            V[n] = float(tc[1]) * kUVScale;
            ++n;
        }
        float uvU[3], uvV[3];
        if (!solveAffine(X, Z, U, n, uvU) || !solveAffine(X, Z, V, n, uvV)) {
            Msg("![VK TerraCache] affine solve degenerate — cache disabled this level");
            s_copyFrame = u32(-1);   // don't retry
            return;
        }
        // Residual check: reject non-planar mappings (would corrupt the bake).
        float maxErr = 0.f;
        for (u32 i = 0; i < n; ++i) {
            maxErr = _max(maxErr, fabsf(uvU[0]*X[i] + uvU[1]*Z[i] + uvU[2] - U[i]));
            maxErr = _max(maxErr, fabsf(uvV[0]*X[i] + uvV[1]*Z[i] + uvV[2] - V[i]));
        }
        if (maxErr > 0.002f) {   // ~2 base-texels at 1024 quant
            Msg("![VK TerraCache] terrain uv not planar (err %.5f) — cache disabled this level", maxErr);
            s_copyFrame = u32(-1);
            return;
        }
        memcpy(s_uvU, uvU, sizeof(uvU)); memcpy(s_uvV, uvV, sizeof(uvV));
        s_affineOk = true;
        Msg("[VK TerraCache] uv affine: u=(%.6f,%.6f,%.3f) v=(%.6f,%.6f,%.3f) err=%.5f",
            uvU[0], uvU[1], uvU[2], uvV[0], uvV[1], uvV[2], maxErr);
    }
}

bool Init()
{
    if (s_inited) return true;
    if (s_failed) return false;
    if (!g_ShaderManager) return false;

    if (!createImage(VK_FORMAT_R16G16B16A16_SFLOAT, "TerraCache.Height", s_height) ||
        !createImage(VK_FORMAT_R8G8B8A8_UNORM, "TerraCache.Weights", s_weights) ||
        !createImage(VK_FORMAT_R16_SFLOAT, "TerraCache.HPyramid", s_pyr, kSize / 2, kPyrMips)) {
        Msg("![VK TerraCache] image create failed"); s_failed = true; return false;
    }
    for (u32 m = 0; m < kPyrMips; ++m) {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = s_pyr.img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_R16_SFLOAT;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1 };
        if (vkCreateImageView(VulkanHW.m_Device, &vci, nullptr, &s_pyrMip[m]) != VK_SUCCESS) {
            Msg("![VK TerraCache] pyramid mip view failed"); s_failed = true; return false;
        }
    }

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_sampler) != VK_SUCCESS) {
        Msg("![VK TerraCache] sampler create failed"); s_failed = true; return false;
    }

    // Staging buffer for the vertex probe (host-visible, mapped).
    {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = 64ull * kProbeVerts;   // stride <= 64
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (VK::Vram::CreateBuffer(VulkanHW.m_Allocator, &bci, &aci, &s_stage, &s_stageAlloc, &info) != VK_SUCCESS) {
            Msg("![VK TerraCache] staging create failed"); s_failed = true; return false;
        }
        s_stageMap = (u8*)info.pMappedData;
    }

    // Set 1: b0 height (rw storage), b1 weights, b2 pyramid sampled (texelFetch),
    // b3..b5 pyramid mip storage views (max-reduce targets).
    constexpr auto kImg = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (!VK::MakeDescriptorSets({ kImg, kImg,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 2 = pyramid, sampled
                                  kImg, kImg, kImg },
                                1, s_setLayout, s_pool, &s_set,
                                VK_SHADER_STAGE_COMPUTE_BIT, "TerraCache")) {
        s_failed = true; return false;
    }
    const VkDescriptorImageInfo di[6] = {
        { VK_NULL_HANDLE, s_height.view,  VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, s_weights.view, VK_IMAGE_LAYOUT_GENERAL },
        { s_sampler,      s_pyr.view,     VK_IMAGE_LAYOUT_GENERAL },   // bake-internal: stays GENERAL
        { VK_NULL_HANDLE, s_pyrMip[0],    VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, s_pyrMip[1],    VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, s_pyrMip[2],    VK_IMAGE_LAYOUT_GENERAL },
    };
    VK::DescriptorWriter w(s_set);
    for (u32 i = 0; i < 6; ++i)
        w.Image(i, (i == 2) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, di[i]);
    w.Flush();

    // Pipeline layout: set 0 = terrain material (mask + heights), set 1 = outputs.
    VkDescriptorSetLayout tset = WorldMaterialCache::GetTerrainSetLayout();
    if (tset == VK_NULL_HANDLE) { Msg("![VK TerraCache] terrain set layout missing"); s_failed = true; return false; }
    s_pipeLayout = VK::MakePipelineLayout({ tset, s_setLayout }, sizeof(PushCS));

    const char* csName[3] = { "terrain_cache.comp.spv", "terrain_cache_mip.comp.spv", "terrain_cache_cone.comp.spv" };
    VkPipeline* csPipe[3] = { &s_pipe, &s_pipeMip, &s_pipeCone };
    for (u32 i = 0; i < 3; ++i) {
        VkShaderModule cs = g_ShaderManager->Load(csName[i]);
        if (cs == VK_NULL_HANDLE) { Msg("![VK TerraCache] %s missing", csName[i]); s_failed = true; return false; }
        *csPipe[i] = VK::CreateComputePipeline(cs, s_pipeLayout, csName[i]);
        if (*csPipe[i] == VK_NULL_HANDLE) { s_failed = true; return false; }
    }

    s_inited = true;
    Msg("[VK TerraCache] init: %ux%u window ±%.0fm (%.1f cm/texel)", kSize, kSize, kHalf, 200.f * kHalf / kSize);
    return true;
}

void Destroy()
{
    if (!s_inited && !s_failed) return;
    if (s_pipe)       { vkDestroyPipeline(VulkanHW.m_Device, s_pipe, nullptr); s_pipe = VK_NULL_HANDLE; }
    if (s_pipeMip)    { vkDestroyPipeline(VulkanHW.m_Device, s_pipeMip, nullptr); s_pipeMip = VK_NULL_HANDLE; }
    if (s_pipeCone)   { vkDestroyPipeline(VulkanHW.m_Device, s_pipeCone, nullptr); s_pipeCone = VK_NULL_HANDLE; }
    if (s_pipeLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_pipeLayout, nullptr); s_pipeLayout = VK_NULL_HANDLE; }
    if (s_pool)       { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; s_set = VK_NULL_HANDLE; }
    if (s_setLayout)  { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    if (s_sampler)    { vkDestroySampler(VulkanHW.m_Device, s_sampler, nullptr); s_sampler = VK_NULL_HANDLE; }
    if (s_stage)      { VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, s_stage, s_stageAlloc); s_stage = VK_NULL_HANDLE; s_stageMap = nullptr; }
    destroyBuf(s_height);
    destroyBuf(s_weights);
    for (u32 m = 0; m < kPyrMips; ++m)
        if (s_pyrMip[m]) { vkDestroyImageView(VulkanHW.m_Device, s_pyrMip[m], nullptr); s_pyrMip[m] = VK_NULL_HANDLE; }
    destroyBuf(s_pyr);
    s_pyrLayoutInit = false;
    OnLevelUnload();
    s_inited = false; s_failed = false;
}

void OnLevelUnload()
{
    s_terrainSet   = VK_NULL_HANDLE;
    s_multiTerrain = false;
    s_meshVB       = VK_NULL_HANDLE;
    s_copyFrame    = 0;
    s_affineOk     = false;
    s_baked        = false;
}

void OnTerrainMaterial(VkDescriptorSet terrainSet, float detailScale)
{
    if (s_terrainSet == terrainSet) return;
    if (s_terrainSet != VK_NULL_HANDLE) {
        // Second DIFFERENT terrain material on this level: the single v1 cache
        // would serve the wrong heights to it (the live flag is global). Off.
        if (!s_multiTerrain) Msg("![VK TerraCache] multiple terrain materials — cache disabled this level");
        s_multiTerrain = true;
        return;
    }
    s_terrainSet  = terrainSet;
    s_detailScale = detailScale;
}

void OnTerrainMesh(VkBuffer vb, u32 vBase, u32 stride, u32 tcOffset)
{
    if (s_meshVB != VK_NULL_HANDLE || vb == VK_NULL_HANDLE) return;
    if (stride == 0 || stride > 64 || tcOffset + 4 > stride) return;
    s_meshVB = vb; s_meshVBase = vBase; s_meshStride = stride; s_meshTCOff = tcOffset;
}

void RecaptureMesh(VkBuffer vb, u32 vBase)
{
    if (s_meshVB == VK_NULL_HANDLE || vb == VK_NULL_HANDLE) return;
    s_meshVB = vb; s_meshVBase = vBase;
    if (!s_affineOk) s_copyFrame = 0;   // probe not resolved yet — re-arm on the new buffer
}

void Update(VkCommandBuffer cmd)
{
    if (!ps_r_terra_cache) return;
    // Multi-terrain levels: the v1 cache is single-material — honor the "cache
    // disabled this level" promise. Without this gate the probe/bake happily ran
    // with the FIRST material's set + mesh (wrong heights/weights on every other
    // terrain region). Latent until 17-07: the capture used to point at the
    // page-repacked cluster VB, the probe read garbage and never resolved, so
    // the missing gate was invisible.
    if (s_multiTerrain) return;
    if (!s_inited && !Init()) return;

    // Stage 1: probe the terrain vertices for the uv affine (once per level).
    if (!s_affineOk && s_meshVB != VK_NULL_HANDLE && s_copyFrame != u32(-1)) {
        if (s_copyFrame == 0) {
            VkBufferCopy bc{ VkDeviceSize(s_meshVBase) * s_meshStride, 0, VkDeviceSize(kProbeVerts) * s_meshStride };
            // VB is consumed by VERTEX_INPUT this frame too — full barrier around the tiny copy.
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            vkCmdCopyBuffer(cmd, s_meshVB, s_stage, 1, &bc);
            VkMemoryBarrier mb2{ VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb2, 0, nullptr, 0, nullptr);
            s_copyFrame = RV().frameIndex;
            return;
        }
        if (RV().frameIndex > s_copyFrame + 3) {   // GPU surely done (fenced frames in flight)
            tryResolveAffine();
            if (!s_affineOk) return;
        } else return;
    }
    if (!s_affineOk || s_terrainSet == VK_NULL_HANDLE) return;

    // Sun azimuth for the horizon channel (cache u = world x, v = world z; the
    // bake grid is world-axis-aligned). Near-vertical sun -> keep the previous
    // azimuth (self-shadows vanish geometrically at high sun anyway).
    const bool wantHor = ps_r_terra_cone && ps_r_terra_horizon;
    float sunU = s_sunU, sunV = s_sunV;
    if (wantHor && g_pGamePersistent) {
        if (auto* E = ES().CurrentEnv) {
            const float sx = -E->sun_dir.x, sz = -E->sun_dir.z;   // toward the sun
            const float len = sqrtf(sx * sx + sz * sz);
            if (len > 0.05f) { sunU = sx / len; sunV = sz / len; }
        }
    }

    // Stage 2: (re)bake when the camera leaves the inner window, a channel
    // cvar was toggled (the baked channels must match what the shaders expect)
    // or the sun azimuth drifted from the baked horizon direction.
    const Fvector eye = RV().vCameraPosition;
    if (s_baked && s_bakedCone == !!ps_r_terra_cone && s_bakedHor == wantHor
        && (!wantHor || (sunU * s_sunU + sunV * s_sunV) > kSunCos)
        && fabsf(ps_r_terra_blend - s_bakedBlend) < 1e-4f
        && fabsf(eye.x - s_bakedCX) < kRebake && fabsf(eye.z - s_bakedCZ) < kRebake) return;

    const float texel = (2.f * kHalf) / float(kSize);
    const float cx = floorf(eye.x / texel) * texel;   // texel snap: stable content across re-bakes
    const float cz = floorf(eye.z / texel) * texel;

    const VkImageLayout oldL = s_baked ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    const VkPipelineStageFlags2 kConsumers = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    const VkAccessFlags2 priorRead = s_baked ? VK_ACCESS_2_SHADER_READ_BIT : 0;
    VK::ImageBarrier(cmd, s_height.img,  oldL, VK_IMAGE_LAYOUT_GENERAL,
                     kConsumers, priorRead, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    VK::ImageBarrier(cmd, s_weights.img, oldL, VK_IMAGE_LAYOUT_GENERAL,
                     kConsumers, priorRead, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    PushCS pc{};
    pc.originExtent[0] = cx - kHalf;
    pc.originExtent[1] = cz - kHalf;
    pc.originExtent[2] = 2.f * kHalf;
    // Height-tap lod: cache texel (m) vs native detail-height texel (m).
    // detail tile world size = 1 / (|d(duv)/d(world)|) ~ 1/(affine scale * ds).
    const float duvPerM = _max(fabsf(s_uvU[0]) + fabsf(s_uvU[1]), 1e-6f) * s_detailScale;
    const float hTexel  = 1.f / (duvPerM * 2048.f);   // heights are ~2048^2
    pc.originExtent[3] = _max(0.f, log2f(texel / _max(hTexel, 1e-6f)));
    memcpy(pc.uvU, s_uvU, sizeof(s_uvU));
    memcpy(pc.uvV, s_uvV, sizeof(s_uvV));
    memcpy(pc.chOff, EnvLight::TerrainChOff(), 4 * sizeof(float));
    pc.dsPad[0] = s_detailScale;
    pc.dsPad[1] = ps_r_terra_blend;   // 0 = plain mask blend (GAMMA), >0 = Mishkinis depth
    // Mask-less maps: the bake samples the BAKED world-space mask exactly like
    // terrainMaskUV() in the fragment shaders (zeros on real-mask maps -> `uv`).
    TerrainMask::GetParams(pc.tmask);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe);
    VkDescriptorSet sets[2] = { s_terrainSet, s_set };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeLayout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, s_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (kSize + 7) / 8, (kSize + 7) / 8, 1);

    // CONE-STEP channel (r_terra_cone): max-reduce the fresh height into the
    // pyramid, then bake the conservative cone ratio into height .g. Skipped
    // entirely when off - the bake left .g = 0 and the shaders take the
    // fixed-layer path (tcache_params.y stays 0 via EnvLight).
    if (ps_r_terra_cone) {
        if (!s_pyrLayoutInit) {
            VK::ImageBarrier(cmd, s_pyr.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, kPyrMips);
            s_pyrLayoutInit = true;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeMip);
        for (u32 lvl = 0; lvl < kPyrMips; ++lvl) {
            VK::ComputeBarrier(cmd);   // prev writes (bake / prev mip) -> this reduce
            float mp[4] = { float(lvl), 0.f, 0.f, 0.f };
            vkCmdPushConstants(cmd, s_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mp), mp);
            const u32 msz = (kSize / 2) >> lvl;
            vkCmdDispatch(cmd, (msz + 7) / 8, (msz + 7) / 8, 1);
        }
        VK::ComputeBarrier(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeCone);
        const float sunPC[4] = { sunU, sunV, wantHor ? 1.f : 0.f, 0.f };
        vkCmdPushConstants(cmd, s_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sunPC), sunPC);
        vkCmdDispatch(cmd, (kSize + 7) / 8, (kSize + 7) / 8, 1);
    }

    VK::ImageBarrier(cmd, s_height.img,  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, kConsumers, VK_ACCESS_2_SHADER_READ_BIT);
    VK::ImageBarrier(cmd, s_weights.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, kConsumers, VK_ACCESS_2_SHADER_READ_BIT);

    if (!s_baked)
        Msg("[VK TerraCache] first bake at (%.1f, %.1f), lod %.2f, cone %d hor %d", cx, cz, pc.originExtent[3], ps_r_terra_cone, int(wantHor));
    s_baked      = true;
    s_bakedCone  = !!ps_r_terra_cone;
    s_bakedHor   = wantHor;
    s_bakedBlend = ps_r_terra_blend;
    s_sunU = sunU; s_sunV = sunV;
    s_bakedCX = cx; s_bakedCZ = cz;
}

bool Live()        { return s_inited && s_baked && s_affineOk && !s_multiTerrain && ps_r_terra_cache; }
bool ConeLive()    { return s_bakedCone && Live(); }
bool HorizonLive() { return s_bakedHor && ConeLive(); }

void GetXform(float out_xform[4], float out_offs[2])
{
    // duv -> cacheUV:  world = invA * (duv/ds - b);  cacheUV = (world - origin)/extent.
    // Compose into cuv = M*duv + c.
    const float ds  = _max(s_detailScale, 1e-6f);
    const float a = s_uvU[0], b = s_uvU[1], c = s_uvU[2];
    const float d = s_uvV[0], e = s_uvV[1], f = s_uvV[2];
    const float det = a * e - b * d;
    if (fabsf(det) < 1e-12f) { out_xform[0] = out_xform[1] = out_xform[2] = out_xform[3] = 0.f; out_offs[0] = out_offs[1] = 0.f; return; }
    // inv of [a b; d e]
    const float ia =  e / det, ib = -b / det, ic = -d / det, id = a / det;
    const float ext = 2.f * kHalf;
    const float ox  = s_bakedCX - kHalf, oz = s_bakedCZ - kHalf;
    // world.x = ia*(u - c) + ib*(v - f); world.z = ic*(u - c) + id*(v - f); u = duv.x/ds ...
    // cuv.x = (world.x - ox)/ext -> row for duv:
    out_xform[0] = ia / (ds * ext);   // du coefficient for cuv.x
    out_xform[1] = ib / (ds * ext);   // dv coefficient for cuv.x
    out_xform[2] = ic / (ds * ext);
    out_xform[3] = id / (ds * ext);
    out_offs[0] = (-(ia * c + ib * f) - ox) / ext;
    out_offs[1] = (-(ic * c + id * f) - oz) / ext;
}

VkImageView HeightView()  { return s_baked ? s_height.view  : VK_NULL_HANDLE; }
VkImageView WeightsView() { return s_baked ? s_weights.view : VK_NULL_HANDLE; }
VkSampler   Sampler()     { return s_sampler; }

}  // namespace TerrainCache

// ============================================================================
// TERRAIN SPLAT-MASK BAKE — see vk_terrain_cache.h. One top-down pass over the
// level's mask-less terrain geometry at load end; each region draws its one-hot
// channel color; the low-res RT sampled bilinearly = the soft splat mask the
// map author never painted.
// ============================================================================
namespace TerrainMask {

namespace {
    constexpr u32      kMaskSize = 1024;   // ~1-2 m/texel on 1-2 km maps = metre-scale seams
    constexpr VkFormat kFormat   = VK_FORMAT_R8G8B8A8_UNORM;

    VkImage          s_img    = VK_NULL_HANDLE;
    VmaAllocation    s_alloc  = VK_NULL_HANDLE;
    VkImageView      s_view   = VK_NULL_HANDLE;
    VkPipelineLayout s_layout = VK_NULL_HANDLE;
    VkPipeline       s_pipe   = VK_NULL_HANDLE;
    u32              s_pipeStride = 0;      // vertex stride the pipeline was built for
    bool             s_active = false;
    float            s_params[4] = { 0, 0, 0, 0 };

    struct Push { float rect[4]; float color[4]; };   // rect = (ox, oz, 1/sx, 1/sz)

    struct BakeItem
    {
        VkBuffer    vb, ib;
        VkIndexType iType;
        u32         vBase, stride, iBase, iCount;
        u8          channel;
    };

    // Terrain-region filter over the shared leaf index (WorldGPU::LeafVisuals):
    // the container recursion and the by-object dedup this used to do itself now
    // happen once for the whole load phase. Same order, so the bake is unchanged.
    void Collect(vkFVisual* fv, xr_vector<BakeItem>& out, Fbox& bounds)
    {
        if (!fv || !fv->m_pWorldMaterial) return;
        WorldMaterial* m = fv->m_pWorldMaterial;
        if (!m->isTerrain || m->terrainChannel == 255) return;   // real-mask terrain: never baked
        if (!fv->m_mesh.IsValid() || !fv->m_mesh.p_rm_Vertices || !fv->m_mesh.p_rm_Indices) return;

        BakeItem it{};
        it.vb      = fv->m_mesh.p_rm_Vertices->GetHandle();
        it.ib      = fv->m_mesh.p_rm_Indices->GetHandle();
        it.iType   = fv->m_mesh.iType;
        it.vBase   = fv->m_mesh.vBase;
        it.stride  = fv->m_mesh.vStride;
        it.iBase   = fv->m_mesh.iBase;
        it.iCount  = fv->m_mesh.iCount;
        it.channel = m->terrainChannel;
        out.push_back(it);
        bounds.merge(fv->vis.box);
    }

    bool EnsurePipeline(u32 stride)
    {
        if (s_pipe != VK_NULL_HANDLE && s_pipeStride == stride) return true;
        if (s_pipe != VK_NULL_HANDLE) { vkDestroyPipeline(VulkanHW.m_Device, s_pipe, nullptr); s_pipe = VK_NULL_HANDLE; }

        VkShaderModule vs = g_ShaderManager ? g_ShaderManager->Load("terrain_mask_bake.vert.spv") : VK_NULL_HANDLE;
        VkShaderModule fs = g_ShaderManager ? g_ShaderManager->Load("terrain_mask_bake.frag.spv") : VK_NULL_HANDLE;
        if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
            Msg("![VK TerrainMask] terrain_mask_bake.{vert,frag}.spv missing — bake disabled");
            return false;
        }

        if (s_layout == VK_NULL_HANDLE) {
            VkPushConstantRange pc{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
            VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            lci.pushConstantRangeCount = 1;
            lci.pPushConstantRanges    = &pc;
            if (vkCreatePipelineLayout(VulkanHW.m_Device, &lci, nullptr, &s_layout) != VK_SUCCESS) {
                Msg("![VK TerrainMask] pipeline layout create failed");
                return false;
            }
        }

        // Top-down projection — winding is irrelevant, so no culling. Viewport and
        // scissor are dynamic (Bake sets them to the full mask before drawing).
        s_pipe = VK::GfxPipelineBuilder(s_layout)
            .Vert(vs).Frag(fs)
            .Binding(0, stride)
            .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)   // position
            .Color(kFormat)
            .Build("TerrainMask stride=%u", stride);
        if (s_pipe == VK_NULL_HANDLE)
            return false;
        s_pipeStride = stride;
        return true;
    }
}

void BakeIfNeeded()
{
    s_active = false;
    s_params[0] = s_params[1] = s_params[2] = s_params[3] = 0.f;

    // Sub-timers (`[load step]`): this pass cost 499 ms of a 19.5 s load and the
    // split between the visual walk, pipeline creation and the GPU draw was unknown.
    CTimer _p; _p.Start();
    float msCollect = 0, msPipe = 0, msRecord = 0, msSubmit = 0;

    // Collect the level's mask-less terrain draws (real-mask maps collect nothing).
    xr_vector<BakeItem> items;
    Fbox bounds; bounds.invalidate();
    const xr_vector<vkFVisual*>& leaves = VK::WorldGPU::LeafVisuals();
    for (vkFVisual* fv : leaves)
        Collect(fv, items, bounds);
    msCollect = _p.GetElapsed_ms_total();
    if (items.empty()) return;

    // All entries share the statics vertex layout; a mixed-stride level would
    // need per-stride pipelines — split on it if it ever shows up.
    _p.Start();
    const u32 stride = items[0].stride;
    for (const BakeItem& it : items)
        if (it.stride != stride) {
            Msg("![VK TerrainMask] mixed vertex strides (%u vs %u) — bake skipped (one-hot masks kept)", stride, it.stride);
            return;
        }
    if (!EnsurePipeline(stride)) return;

    if (s_img == VK_NULL_HANDLE) {
        if (!CreateImage2D(kFormat, { kMaskSize, kMaskSize },
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           s_img, s_alloc, "terrain_mask_bake"))
            return;
        s_view = CreateImageView(s_img, kFormat);
        if (s_view == VK_NULL_HANDLE) return;
    }

    // World rect, padded ~2 texels so bilinear taps at the rim stay inside.
    const float pad = 2.0f * (bounds.max.x - bounds.min.x + bounds.max.z - bounds.min.z) * 0.5f / (float)kMaskSize;
    const float ox = bounds.min.x - pad, oz = bounds.min.z - pad;
    const float sx = (bounds.max.x + pad) - ox, sz = (bounds.max.z + pad) - oz;
    if (sx <= 1.f || sz <= 1.f) return;

    msPipe = _p.GetElapsed_ms_total();
    _p.Start();

    VkCommandBuffer cmd = CommandManager.BeginImmediate();
    if (cmd == VK_NULL_HANDLE) return;

    ImageBarrier(cmd, s_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Uncovered texels clear to the earth channel. BeginPlain sets the full
    // unflipped viewport/scissor the pipeline needs (it is dynamic-viewport).
    RenderingBuilder(kMaskSize, kMaskSize)
        .ColorClear(s_view, VkClearColorValue{ { 0.f, 0.f, 1.f, 0.f } })
        .BeginPlain(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipe);
    static const float kOneHot[4][4] = {
        { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 },
    };
    VkBuffer    lastVB = VK_NULL_HANDLE, lastIB = VK_NULL_HANDLE;
    VkIndexType lastIT = VK_INDEX_TYPE_MAX_ENUM;
    for (const BakeItem& it : items) {
        Push p{};
        p.rect[0] = ox; p.rect[1] = oz; p.rect[2] = 1.f / sx; p.rect[3] = 1.f / sz;
        memcpy(p.color, kOneHot[it.channel & 3], sizeof(p.color));
        vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
        if (it.vb != lastVB) { VkDeviceSize z = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &it.vb, &z); lastVB = it.vb; }
        if (it.ib != lastIB || it.iType != lastIT) { vkCmdBindIndexBuffer(cmd, it.ib, 0, it.iType); lastIB = it.ib; lastIT = it.iType; }
        vkCmdDrawIndexed(cmd, it.iCount, 1, it.iBase, (s32)it.vBase, 0);
    }

    vkCmdEndRendering(cmd);
    ImageBarrier(cmd, s_img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    msRecord = _p.GetElapsed_ms_total();
    _p.Start();
    CommandManager.EndAndSubmitImmediate(cmd);
    msSubmit = _p.GetElapsed_ms_total();

    // Point every mask-less terrain material's binding 1 at the bake and expose
    // the world->UV affine to the shaders (L.tmask_params).
    WorldMaterialCache::RebindTerrainMasks(s_view);
    s_params[0] = ox; s_params[1] = oz; s_params[2] = 1.f / sx; s_params[3] = 1.f / sz;
    s_active = true;
    Msg("[VK TerrainMask] baked %zu terrain region draw(s) into %ux%u mask, world rect (%.0f, %.0f)+(%.0f x %.0f)",
        items.size(), kMaskSize, kMaskSize, ox, oz, sx, sz);
    Msg("[load step]   TerrainMask: collect %.0f (%u leaves -> %u draws) | pipeline+image %.0f | record %.0f | submit+wait %.0f ms",
        msCollect, (u32)leaves.size(), (u32)items.size(), msPipe, msRecord, msSubmit);
}

void OnLevelUnload()
{
    s_active = false;
    s_params[0] = s_params[1] = s_params[2] = s_params[3] = 0.f;
}

void Destroy()
{
    OnLevelUnload();
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_pipe   != VK_NULL_HANDLE) { vkDestroyPipeline(VulkanHW.m_Device, s_pipe, nullptr); s_pipe = VK_NULL_HANDLE; }
    if (s_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_layout, nullptr); s_layout = VK_NULL_HANDLE; }
    if (s_view   != VK_NULL_HANDLE) { vkDestroyImageView(VulkanHW.m_Device, s_view, nullptr); s_view = VK_NULL_HANDLE; }
    if (s_img    != VK_NULL_HANDLE) { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_img, s_alloc); s_img = VK_NULL_HANDLE; s_alloc = VK_NULL_HANDLE; }
}

bool Active() { return s_active; }

void GetParams(float out4[4])
{
    out4[0] = s_params[0]; out4[1] = s_params[1];
    out4[2] = s_active ? s_params[2] : 0.f;
    out4[3] = s_params[3];
}

}}  // namespace VK::TerrainMask
