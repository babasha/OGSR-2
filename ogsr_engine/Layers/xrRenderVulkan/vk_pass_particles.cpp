// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
extern float ps_r_vol_smoke_inject;   // r_vol_smoke_inject (vk_console_min.cpp, global scope)
extern int ps_r_ptcl_mt, ps_r_core1_prio; // r_ptcl_mt, r_core1_prio (vk_console_min.cpp): billboards filled by a core-1 worker during the other prepares
#include "vk_render_view.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_rendering.h"     // VK::RenderingBuilder
#include "vk_profiler.h"   // TEMP VUID-hunt: VK::Prof::NameImage
#include "vk_pass_particles.h"
#include "vk_color_space.h"   // ColorSpace::LinearizeRGB — smoke media albedo
#include "vk_image.h"      // VK::CreateImage2D / CreateImageView
#include "vk_Particles.h"
#include "vk_pass_world.h"        // DynamicVisuals() / DynVisual
#include "vk_render_queue.h"      // g_RenderQueue.GlassItems() — glass refraction into the distort RT
#include "vk_Visual.h"            // vkFVisual mesh access (glass pane re-draw)
#include "vk_world_material.h"    // WorldMaterial::isEmisAdd — glow halos skip refraction
#include "vk_pass_skinned.h"      // Skinned_RenderGlassDistort — kinematics panes refraction
#include "vk_shaders.h"
#include "vk_texture.h"
#include "vk_buffer.h"
#include "vk_swapchain.h"
#include "vk_scene_color.h"       // HDR scene target format
#include "vk_command_buffer.h"    // CommandManager.GetCurrentFrame() / FRAMES_IN_FLIGHT
#include "../../xrCore/pinned_worker.h"   // sprite textures read off the frame (GetTextureSetAsync)
#include <unordered_set>
#include "vk_gfx_pipeline.h"      // VK::GfxPipelineBuilder
#include "vk_barriers.h"          // ImageBarrier — distort RT layout flips
#include "HW_Vulkan.h"

#include "vk_volumetrics.h"        // VK::Vol — froxel in-scatter probe (Stage-0 smoke lighting) + ProjTerms
#include "../xrRender/FVF.h"
#include "../../xrParticles/psystem.h"  // PAPI::Particle / ParticleManager — Stage-1 smoke media collect
#include "../../xr_3da/fmesh.h"   // MT_PARTICLE_EFFECT / MT_PARTICLE_GROUP
#include "../../xr_3da/device.h"  // RV().mFullTransform_hud2 — HUD-FOV projection

#include <unordered_map>
#include <string>

// Stage-0 volumetric lighting knobs for smoke billboards (global scope — matches
// the C-linkage console symbols the other r_vol_* knobs use). strength 0 = off /
// old look; clamp bounds the per-froxel radiance added so smoke can't blow to white.
extern float ps_r_vol_smoke;
extern float ps_r_vol_smoke_clamp;
// Stage-1 VMS: near-range cutoff for injected smoke media (tied to the terrain detail
// bubble — capped at r__detail_radius so smoke media lives within the grass/terrain
// zone where the froxel grid is fine; far smoke stays billboard-only).
extern float ps_r_vol_smoke_dist;
extern float ps_r_vol_smoke_dist_full;   // inner full-quality radius (volumetric LOD)
extern int   ps_r__detail_radius;
extern float ps_r_glass_refr;            // r_glass_refr — glass refraction (uneven-pane wobble) strength, 0 = off

namespace VK {

namespace {
    constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;

    // Particle vertex = FVF::LIT (24 bytes): vec3 pos, D3DCOLOR, vec2 uv.
    constexpr u32 kVtxStride = sizeof(float) * 3 + sizeof(u32) + sizeof(float) * 2;  // 24

    // Push block shared by the particle VS+FS (and, harmlessly, by the rain/wallmark
    // passes that reuse this layout — they only push the first 64 bytes). Must match
    // the PushConstants block in particle.vert/frag.glsl.
    struct ParticlePush {
        Fmatrix viewProj;        // 64  VS
        float   camPosNear[4];   // 16  VS — xyz cam pos, w = froxel near Z
        float   camDirLogFN[4];  // 16  VS — xyz cam forward, w = log2(far/near)
        float   volParams[4];    // 16  FS — x = probe strength, yz = 1/extent
    };
    static_assert(sizeof(ParticlePush) == 112, "ParticlePush must match the shader push block");

    // Per-frame dynamic vertex ring (host-visible). 4 MB ≈ 174k verts ≈ 29k
    // particles per frame — far above any realistic on-screen count.
    constexpr VkDeviceSize kRingBytes = 4 * 1024 * 1024;
    constexpr u32          kRingVerts = (u32)(kRingBytes / kVtxStride);

    VkShaderModule   s_VS = VK_NULL_HANDLE;
    VkShaderModule   s_FS = VK_NULL_HANDLE;
    VkPipelineLayout s_Layout = VK_NULL_HANDLE;
    VkPipeline       s_Pipelines[PBM_COUNT] = {};   // lazy per blend mode
    VkDescriptorSetLayout s_SetLayout = VK_NULL_HANDLE;
    VkDescriptorPool s_Pool   = VK_NULL_HANDLE;
    VkSampler        s_Sampler = VK_NULL_HANDLE;

    // Set 1 — the froxel in-scatter volume (Stage-0 smoke lighting). One persistent
    // set pointed at Vol's scatter view (eager-created, so valid from init); rebound
    // if Vol ever regenerates. Bound for every particle draw because particle.frag
    // statically references it (the sample itself is gated on volParams.x).
    // Sentinel "no Vol generation bound yet" — != any real Vol::Generation() so the
    // first EnsureVolSet() always writes the descriptor.
    constexpr u32 kVolGenUnbound = 0xFFFFFFFFu;

    VkDescriptorSetLayout s_VolSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet       s_VolSet       = VK_NULL_HANDLE;
    u32                   s_VolBoundGen  = kVolGenUnbound;

    CVulkanBuffer    s_Ring[kFramesInFlight];

    bool             s_Init = false;

    // Heat-haze distortion target (PBM_DISTORT): full-res RGBA8, neutral
    // (0.5, 0.5). Produced and consumed inside the same command buffer every
    // frame, so a single image is safe across frames-in-flight (same argument
    // as the bloom ping-pong). Created lazily on first distort sighting.
    VkImage          s_DistortImg   = VK_NULL_HANDLE;
    VmaAllocation    s_DistortAlloc = VK_NULL_HANDLE;
    VkImageView      s_DistortView  = VK_NULL_HANDLE;
    VkExtent2D       s_DistortExtent{};
    u32              s_DistortGen   = 0;
    bool             s_DistortFirst = true;        // image still UNDEFINED
    VkPipeline       s_DistortPipe  = VK_NULL_HANDLE;
    // Wallmark variants: generic decals = MODULATE2X + wallmark.frag (R4
    // effects_wallmark.s parity); blood decals = alpha blend + the SSS
    // effects_wallmark_blood port (readable red on dark clothing).
    VkShaderModule   s_WallmarkFS   = VK_NULL_HANDLE;
    VkPipeline       s_WallmarkPipe = VK_NULL_HANDLE;
    VkShaderModule   s_BloodFS      = VK_NULL_HANDLE;
    VkPipeline       s_BloodPipe    = VK_NULL_HANDLE;
    // Rain variants: splashes = shape from tex ALPHA (fx_rain rgb is a
    // refraction normal map); drop streaks = fully PROCEDURAL shape (the
    // fx_rain alpha reads ~0 through our loader) — see rain*.frag.glsl.
    VkShaderModule   s_RainFS       = VK_NULL_HANDLE;
    VkPipeline       s_RainPipe     = VK_NULL_HANDLE;
    VkShaderModule   s_RainDropFS   = VK_NULL_HANDLE;
    VkPipeline       s_RainDropPipe = VK_NULL_HANDLE;
    constexpr VkFormat kDistortFormat = VK_FORMAT_R8G8B8A8_UNORM;

    // ---- Glass refraction (r_glass_refr) ------------------------------------
    // The late-glass panes (RenderQueue::GlassItems) are re-drawn into the same
    // distortion RT with a procedural "uneven old glass" wobble — the tonemap
    // then bends whatever is behind the pane. Two pipelines (world stride-32
    // sub-layouts: base UV at offset 24 = lmap, 28 = vert-lit); push = mvp+strength.
    VkPipeline       s_GlassDistortPipe[2] = {};   // [0]=tcOffset 24, [1]=28
    VkPipelineLayout s_GlassDistortLayout  = VK_NULL_HANDLE;
    VkShaderModule   s_GlassVS = VK_NULL_HANDLE, s_GlassFS = VK_NULL_HANDLE;

    struct GlassDistortPush { Fmatrix mvp; float strength; float pad[3]; };

    VkPipeline BuildGlassDistortPipeline(u32 tcOffset)
    {
        if (s_GlassDistortLayout == VK_NULL_HANDLE) {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GlassDistortPush) };
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_GlassDistortLayout) != VK_SUCCESS)
                return VK_NULL_HANDLE;
        }
        if (!s_GlassVS) s_GlassVS = g_ShaderManager->Load("glass_distort.vert.spv");
        if (!s_GlassFS) s_GlassFS = g_ShaderManager->Load("glass_distort.frag.spv");
        if (!s_GlassVS || !s_GlassFS) return VK_NULL_HANDLE;

        // Depth-tested but never written — occluded panes must not warp. Blend is
        // the same straight alpha the haze sprites use.
        return VK::GfxPipelineBuilder(s_GlassDistortLayout)
            .Vert(s_GlassVS).Frag(s_GlassFS)
            .Binding(0, 32)
            .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
            .Attr(1, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset)
            .Depth(true, false)
            .Color(kDistortFormat).BlendAlpha()
            .DepthTarget(Swapchain.m_DepthFormat)
            .Build("Particles glass-distort tcOffset=%u", tcOffset);
    }

    // Re-draw the glass panes into the (already begun) distortion pass.
    void DrawGlassDistort(VkCommandBuffer cmd, const Fmatrix& viewProj)
    {
        if (ps_r_glass_refr <= 0.001f) return;
        const auto& items = g_RenderQueue.GlassItems();
        if (items.empty()) return;

        VkPipeline lastPipe = VK_NULL_HANDLE;
        VkBuffer lastVB = VK_NULL_HANDLE, lastIB = VK_NULL_HANDLE;
        for (const DrawItem& it : items) {
            auto* rv = it.vis;
            if (!rv || (rv->Type != MT_NORMAL && rv->Type != MT_PROGRESSIVE)) continue;
            auto* fv = static_cast<vkFVisual*>(rv);
            if (!fv->m_mesh.p_rm_Vertices || !fv->m_mesh.p_rm_Indices) continue;
            if (fv->m_mesh.vStride != 32) continue;                     // world layouts only
            if (fv->m_pWorldMaterial && (fv->m_pWorldMaterial->isEmisAdd || fv->m_pWorldMaterial->isLitBlend)) continue;   // glow halos / light beams don't refract
            const u32 slot = (fv->m_mesh.tcOffset == 28) ? 1u : 0u;
            if (s_GlassDistortPipe[slot] == VK_NULL_HANDLE)
                s_GlassDistortPipe[slot] = BuildGlassDistortPipeline(fv->m_mesh.tcOffset);
            VkPipeline pipe = s_GlassDistortPipe[slot];
            if (pipe == VK_NULL_HANDLE) continue;

            if (pipe != lastPipe) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                lastPipe = pipe; lastVB = lastIB = VK_NULL_HANDLE;
            }
            GlassDistortPush pc{};
            pc.mvp.mul(viewProj, it.xform);
            pc.strength = ps_r_glass_refr;
            vkCmdPushConstants(cmd, s_GlassDistortLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);

            VkBuffer vb = fv->m_mesh.p_rm_Vertices->GetHandle();
            if (vb != lastVB) { VkDeviceSize off = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off); lastVB = vb; }
            VkBuffer ib = fv->m_mesh.p_rm_Indices->GetHandle();
            if (ib != lastIB) { vkCmdBindIndexBuffer(cmd, ib, 0, fv->m_mesh.iType); lastIB = ib; }
            const u32 firstIndex = it.iCountOverride ? it.iBaseOverride  : fv->m_mesh.iBase;
            const u32 indexCount = it.iCountOverride ? it.iCountOverride : fv->m_mesh.iCount;
            vkCmdDrawIndexed(cmd, indexCount, 1, firstIndex, (s32)fv->m_mesh.vBase, 0);
        }
    }

    void DestroyDistortRT()
    {
        if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
        if (s_DistortView) { vkDestroyImageView(VulkanHW.m_Device, s_DistortView, nullptr); s_DistortView = VK_NULL_HANDLE; }
        if (s_DistortImg)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_DistortImg, s_DistortAlloc); s_DistortImg = VK_NULL_HANDLE; s_DistortAlloc = VK_NULL_HANDLE; }
        s_DistortExtent = {};
        s_DistortFirst  = true;
    }

    bool EnsureDistortRT(VkExtent2D extent)
    {
        if (s_DistortImg && extent.width == s_DistortExtent.width && extent.height == s_DistortExtent.height)
            return true;
        DestroyDistortRT();
        if (!VK::CreateImage2D(kDistortFormat, extent,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                s_DistortImg, s_DistortAlloc, "Particle.Distort"))
            return false;
        s_DistortView = VK::CreateImageView(s_DistortImg, kDistortFormat);
        if (s_DistortView == VK_NULL_HANDLE) {
            DestroyDistortRT();
            return false;
        }
        s_DistortExtent = extent;
        s_DistortFirst  = true;
        ++s_DistortGen;
        Msg("[VK Particles] distort RT ready (%ux%u, gen %u)", extent.width, extent.height, s_DistortGen);
        return true;
    }

    // Sprite texture descriptor cache, keyed by texture name. Many effects share
    // a handful of textures (fire/smoke/sparks), so this bounds the pool.
    struct PTex { CVulkanTexture* tex = nullptr; VkDescriptorSet set = VK_NULL_HANDLE; };
    std::unordered_map<std::string, PTex> s_TexCache;

    void BlendFactors(EParticleBlendMode mode, VkBlendFactor& src, VkBlendFactor& dst)
    {
        switch (mode) {
        case PBM_SET:       src = VK_BLEND_FACTOR_ONE;       dst = VK_BLEND_FACTOR_ZERO;                break;
        case PBM_ADD:       src = VK_BLEND_FACTOR_ONE;       dst = VK_BLEND_FACTOR_ONE;                 break;
        case PBM_MUL:       src = VK_BLEND_FACTOR_DST_COLOR; dst = VK_BLEND_FACTOR_ZERO;                break;
        case PBM_MUL_2X:    src = VK_BLEND_FACTOR_DST_COLOR; dst = VK_BLEND_FACTOR_SRC_COLOR;           break;
        case PBM_ALPHA_ADD: src = VK_BLEND_FACTOR_SRC_ALPHA; dst = VK_BLEND_FACTOR_ONE;                 break;
        case PBM_BLEND:
        default:            src = VK_BLEND_FACTOR_SRC_ALPHA; dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
        }
    }

    // `distortTarget` builds the PBM_DISTORT variant: renders into the RGBA8
    // distortion buffer with standard alpha blending (sprite rg = UV offset
    // around neutral 0.5, alpha = haze mask) instead of the HDR scene target.
    // `fsOverride` swaps the fragment shader (wallmark.frag for decals).
    VkPipeline BuildPipeline(EParticleBlendMode mode, bool distortTarget = false,
                             VkShaderModule fsOverride = VK_NULL_HANDLE)
    {
        VkBlendFactor src, dst;
        if (distortTarget) { src = VK_BLEND_FACTOR_SRC_ALPHA; dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; }
        else               BlendFactors(mode, src, dst);
        VkPipelineColorBlendAttachmentState ba{};
        ba.blendEnable         = VK_TRUE;
        ba.srcColorBlendFactor = src;
        ba.dstColorBlendFactor = dst;
        ba.colorBlendOp        = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.alphaBlendOp        = VK_BLEND_OP_ADD;
        ba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        // Billboards are double-sided (cull NONE, the builder default), occluded by
        // world geometry but transparent, so they test depth and never write it.
        return VK::GfxPipelineBuilder(s_Layout)
            .Vert(s_VS).Frag(fsOverride ? fsOverride : s_FS)
            .Binding(0, kVtxStride)
            .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)    // pos
            .Attr(1, 0, VK_FORMAT_B8G8R8A8_UNORM,   12)   // D3DCOLOR → logical RGBA
            .Attr(2, 0, VK_FORMAT_R32G32_SFLOAT,    16)   // uv
            .Depth(true, false)
            .ColorBlend(distortTarget ? kDistortFormat : VK::SceneColor::Format(), ba)
            .DepthTarget(Swapchain.m_DepthFormat)
            .Build("Particles blend=%d distort=%d", (int)mode, (int)distortTarget);
    }

    // (Re)point set 1 at Vol's scatter volume. Vol::Init runs before ParticlePass_Init
    // and creates the volume eagerly, so the view is valid here; this also rebinds if
    // Vol ever regenerates (Generation() bumps). No-op once bound for the current gen.
    void EnsureVolSet()
    {
        if (!Vol::Ready()) return;
        const u32 gen = Vol::Generation();
        if (s_VolBoundGen == gen) return;
        VkImageView view = Vol::GetScatterView();
        VkSampler   samp = Vol::GetSampler();
        if (!view || !samp || s_VolSet == VK_NULL_HANDLE) return;
        VK::DescriptorWriter(s_VolSet).ImageSampler(0, view, samp).Flush();
        s_VolBoundGen = gen;
    }
}

bool ParticlePass_Init()
{
    if (s_Init) return true;

    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    s_VS = g_ShaderManager->Load("particle.vert.spv");
    s_FS = g_ShaderManager->Load("particle.frag.spv");
    if (s_VS == VK_NULL_HANDLE || s_FS == VK_NULL_HANDLE) {
        Msg("![VK Particles] failed to load particle shaders");
        return false;
    }

    // Set 0: binding 0 = sprite (combined image sampler, FS).
    {
        s_SetLayout = VK::MakeSetLayout({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
                                        VK_SHADER_STAGE_FRAGMENT_BIT, "Particles.Sprite");
        if (s_SetLayout == VK_NULL_HANDLE) return false;
    }

    // Set 1: binding 0 = froxel scatter volume (combined image sampler, FS) — the
    // Stage-0 light probe. Always part of the layout; bound for every particle draw.
    {
        s_VolSetLayout = VK::MakeSetLayout({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
                                           VK_SHADER_STAGE_FRAGMENT_BIT, "Particles.Vol");
        if (s_VolSetLayout == VK_NULL_HANDLE) return false;
    }

    // Pool — one set per unique sprite texture (~256 generous) + 1 for the vol set.
    {
        constexpr u32 kMaxSets = 256 + 1;   // +1 for the persistent vol (set 1) descriptor
        VkDescriptorPoolSize ps{};
        ps.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ps.descriptorCount = kMaxSets;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets       = kMaxSets;
        pci.poolSizeCount = 1;
        pci.pPoolSizes    = &ps;
        if (vkCreateDescriptorPool(VulkanHW.m_Device, &pci, nullptr, &s_Pool) != VK_SUCCESS) {
            Msg("![VK Particles] CreateDescriptorPool failed");
            return false;
        }
    }

    {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.minLod       = 0.0f;
        si.maxLod       = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_Sampler) != VK_SUCCESS) {
            Msg("![VK Particles] CreateSampler failed");
            return false;
        }
    }

    // Pipeline layout: set 0 (sprite) + set 1 (froxel volume) + the ParticlePush block
    // (viewProj VS + froxel basis VS + volParams FS). The rain/wallmark passes share
    // this layout but only push the first 64 bytes (viewProj) and never bind set 1 —
    // valid, since their fragment shaders don't statically use set 1 or the FS bytes.
    {
        VkDescriptorSetLayout sets[2] = { s_SetLayout, s_VolSetLayout };
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pc.offset     = 0;
        pc.size       = sizeof(ParticlePush);
        VkPipelineLayoutCreateInfo plci{};
        plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount         = 2;
        plci.pSetLayouts            = sets;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges    = &pc;
        if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_Layout) != VK_SUCCESS) {
            Msg("![VK Particles] CreatePipelineLayout failed");
            return false;
        }
    }

    // Allocate the persistent vol (set 1) descriptor and point it at Vol's scatter
    // volume (Vol::Init already ran — eager). Re-pointed by EnsureVolSet on regen.
    {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = s_Pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &s_VolSetLayout;
        if (vkAllocateDescriptorSets(VulkanHW.m_Device, &ai, &s_VolSet) != VK_SUCCESS) {
            Msg("![VK Particles] vol set alloc failed");
            return false;
        }
        EnsureVolSet();
    }

    for (u32 i = 0; i < kFramesInFlight; ++i)
        s_Ring[i].Create(kRingBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);

    s_Init = true;
    Msg("[VK Particles] Init OK (ring=%u verts/frame x%u, per-blend pipelines lazy)", kRingVerts, kFramesInFlight);
    return true;
}

void ParticlePass_Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;

    for (auto& kv : s_TexCache) {
        if (kv.second.tex) { kv.second.tex->Destroy(); xr_delete(kv.second.tex); }
    }
    s_TexCache.clear();

    for (u32 i = 0; i < kFramesInFlight; ++i) s_Ring[i].Destroy();

    for (u32 i = 0; i < PBM_COUNT; ++i) {
        if (s_Pipelines[i]) { vkDestroyPipeline(VulkanHW.m_Device, s_Pipelines[i], nullptr); s_Pipelines[i] = VK_NULL_HANDLE; }
    }
    if (s_DistortPipe)  { vkDestroyPipeline(VulkanHW.m_Device, s_DistortPipe, nullptr); s_DistortPipe = VK_NULL_HANDLE; }
    for (u32 i = 0; i < 2; ++i)
        if (s_GlassDistortPipe[i]) { vkDestroyPipeline(VulkanHW.m_Device, s_GlassDistortPipe[i], nullptr); s_GlassDistortPipe[i] = VK_NULL_HANDLE; }
    if (s_GlassDistortLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_GlassDistortLayout, nullptr); s_GlassDistortLayout = VK_NULL_HANDLE; }
    s_GlassVS = s_GlassFS = VK_NULL_HANDLE;
    if (s_WallmarkPipe) { vkDestroyPipeline(VulkanHW.m_Device, s_WallmarkPipe, nullptr); s_WallmarkPipe = VK_NULL_HANDLE; }
    if (s_BloodPipe)    { vkDestroyPipeline(VulkanHW.m_Device, s_BloodPipe, nullptr); s_BloodPipe = VK_NULL_HANDLE; }
    if (s_RainPipe)     { vkDestroyPipeline(VulkanHW.m_Device, s_RainPipe, nullptr); s_RainPipe = VK_NULL_HANDLE; }
    if (s_RainDropPipe) { vkDestroyPipeline(VulkanHW.m_Device, s_RainDropPipe, nullptr); s_RainDropPipe = VK_NULL_HANDLE; }
    s_WallmarkFS = VK_NULL_HANDLE;
    s_BloodFS    = VK_NULL_HANDLE;
    s_RainFS     = VK_NULL_HANDLE;
    s_RainDropFS = VK_NULL_HANDLE;
    DestroyDistortRT();
    if (s_Layout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, s_Layout, nullptr); s_Layout = VK_NULL_HANDLE; }
    if (s_Pool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_Pool, nullptr); s_Pool = VK_NULL_HANDLE; }  // frees s_VolSet too
    s_VolSet = VK_NULL_HANDLE; s_VolBoundGen = kVolGenUnbound;
    if (s_Sampler)   { vkDestroySampler(VulkanHW.m_Device, s_Sampler, nullptr); s_Sampler = VK_NULL_HANDLE; }
    if (s_SetLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_SetLayout, nullptr); s_SetLayout = VK_NULL_HANDLE; }
    if (s_VolSetLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_VolSetLayout, nullptr); s_VolSetLayout = VK_NULL_HANDLE; }
    s_VS = s_FS = VK_NULL_HANDLE;
    s_Init = false;
}

namespace ParticlePass {

bool             Ready()      { return s_Init; }
VkPipelineLayout GetLayout()  { return s_Layout; }

VkPipeline GetPipeline(EParticleBlendMode mode)
{
    if (!s_Init || mode < 0 || mode >= PBM_COUNT) return VK_NULL_HANDLE;
    if (mode == PBM_DISTORT) return VK_NULL_HANDLE;       // own pass — see Pass_Particles phase 3
    if (s_Pipelines[mode] == VK_NULL_HANDLE)
        s_Pipelines[mode] = BuildPipeline(mode);
    return s_Pipelines[mode];
}

VkImageView GetDistortView()      { return s_DistortView; }
u32         DistortGeneration()   { return s_DistortGen; }

VkPipeline GetWallmarkPipeline()
{
    if (!s_Init) return VK_NULL_HANDLE;
    if (s_WallmarkPipe == VK_NULL_HANDLE) {
        if (s_WallmarkFS == VK_NULL_HANDLE && g_ShaderManager)
            s_WallmarkFS = g_ShaderManager->Load("wallmark.frag.spv");
        if (s_WallmarkFS == VK_NULL_HANDLE)
            return GetPipeline(PBM_BLEND);      // shader missing — plain blend fallback
        // MODULATE2X like R4's effects_wallmark.s — wm_* textures are authored
        // for multiply blending (neutral-grey backgrounds, junk alpha).
        s_WallmarkPipe = BuildPipeline(PBM_MUL_2X, /*distortTarget*/ false, s_WallmarkFS);
    }
    return s_WallmarkPipe;
}

VkPipeline GetRainPipeline()
{
    if (!s_Init) return VK_NULL_HANDLE;
    if (s_RainPipe == VK_NULL_HANDLE) {
        if (s_RainFS == VK_NULL_HANDLE && g_ShaderManager)
            s_RainFS = g_ShaderManager->Load("rain.frag.spv");
        if (s_RainFS == VK_NULL_HANDLE)
            return GetPipeline(PBM_BLEND);      // shader missing — plain blend fallback
        // Alpha blend; shape from tex.a only — the fx_rain rgb is a refraction
        // normal map, not a colour sprite (see rain.frag.glsl).
        s_RainPipe = BuildPipeline(PBM_BLEND, /*distortTarget*/ false, s_RainFS);
    }
    return s_RainPipe;
}

VkPipeline GetRainDropPipeline()
{
    if (!s_Init) return VK_NULL_HANDLE;
    if (s_RainDropPipe == VK_NULL_HANDLE) {
        if (s_RainDropFS == VK_NULL_HANDLE && g_ShaderManager)
            s_RainDropFS = g_ShaderManager->Load("rain_drop.frag.spv");
        if (s_RainDropFS == VK_NULL_HANDLE)
            return GetRainPipeline();           // shader missing — tex-alpha fallback
        // ALPHA_ADD: the SSFX drop is refracted-background + hemi lift — its
        // net effect over the backdrop is a faint ADDITIVE glint, not a
        // painted-over streak (alpha blend read as "ливень"/tracers).
        s_RainDropPipe = BuildPipeline(PBM_ALPHA_ADD, /*distortTarget*/ false, s_RainDropFS);
    }
    return s_RainDropPipe;
}

VkPipeline GetBloodWallmarkPipeline()
{
    if (!s_Init) return VK_NULL_HANDLE;
    if (s_BloodPipe == VK_NULL_HANDLE) {
        if (s_BloodFS == VK_NULL_HANDLE && g_ShaderManager)
            s_BloodFS = g_ShaderManager->Load("wallmark_blood.frag.spv");
        if (s_BloodFS == VK_NULL_HANDLE)
            return GetWallmarkPipeline();       // shader missing — generic decal fallback
        s_BloodPipe = BuildPipeline(PBM_BLEND, /*distortTarget*/ false, s_BloodFS);
    }
    return s_BloodPipe;
}

// Two-slot pipeline: the cache is reached from the main thread (extract: particle texture resolve) AND the render thread
// (rain, wallmarks, GPU particles) at the same time; a miss loads a DDS and allocates a descriptor set.
static std::recursive_mutex s_TexMx;

// Normalize legacy particle texture names (matches the DX texture loader):
// defs may carry a comma-separated list (base texture = the FIRST entry)
// and a .tga/.bmp source extension — the cooked asset on disk is .dds
// ('pfx\pfx_flame_01.tga' → 'pfx\pfx_flame_01.dds'). False = no such file.
static bool ResolveSpritePath(const char* texture_name, string_path& full)
{
    string_path nm;
    xr_strcpy(nm, texture_name);
    if (char* comma = strchr(nm, ','))
        *comma = 0;
    if (char* dot = strrchr(nm, '.')) {
        const char* sep1 = strrchr(nm, '\\');
        const char* sep2 = strrchr(nm, '/');
        const char* sep  = (sep1 > sep2) ? sep1 : sep2;
        if (!sep || dot > sep)   // only strip a real extension, not a dotted folder
            *dot = 0;
    }

    // Resolve "$game_textures$\<name>.dds".
    string_path leaf;
    xr_sprintf(leaf, "%s.dds", nm);
    FS.update_path(full, "$game_textures$", leaf);
    return !!FS.exist(full);
}

// Sprite albedo — Colour. Covers both the alpha-blended and the additive (PBM_ADD) effects; additive sprites are
// radiance too, so they must be linear before they are summed into the HDR scene colour. Null = failed.
static CVulkanTexture* LoadSprite(const char* full)
{
    auto* tex = xr_new<CVulkanTexture>();
    if (!tex->LoadDDS(full, /*applyBCSwizzle*/ false, TexStreamClass::UI, TexColorSpace::Color))
        xr_delete(tex);
    return tex;
}

static VkDescriptorSet FinishTextureSet(std::string key, const char* texture_name, CVulkanTexture* tex);

// ⭐First use off the frame. 📏08-10-2026 Switch v237-v245 [rt prof]/[tex late]: the first bullet mark on
// a new material, the first blood, the first sparks/debris of a hit read their .dds from the SD INSIDE the
// render thread's record (Wallmarks + tex.read 4-21 ms, up to five at once). GetTextureSetAsync hands the read
// to a background thread (core 2) and says "pending"; the caller draws nothing of it this frame and asks
// again. The loaded textures get their descriptor sets here, under s_TexMx, on the next call.
static std::unordered_set<std::string> s_TexPending;          // under s_TexMx
static std::mutex s_TexReadyMx;
static xr_vector<std::pair<std::string, CVulkanTexture*>> s_TexReady;   // under s_TexReadyMx

static void DrainReadyTexturesLocked()   // s_TexMx held
{
    xr_vector<std::pair<std::string, CVulkanTexture*>> ready;
    {
        std::lock_guard<std::mutex> lk(s_TexReadyMx);
        if (s_TexReady.empty()) return;
        ready.swap(s_TexReady);
    }
    for (auto& r : ready) {
        s_TexPending.erase(r.first);
        if (s_TexCache.find(r.first) == s_TexCache.end())
            FinishTextureSet(r.first, r.first.c_str(), r.second);
        else if (r.second) { r.second->Destroy(); xr_delete(r.second); }   // a sync load beat it
    }
}

VkDescriptorSet GetTextureSetAsync(const char* texture_name, bool* pending)
{
    if (pending) *pending = false;
    if (!s_Init || !texture_name || !texture_name[0]) return VK_NULL_HANDLE;
    std::lock_guard<std::recursive_mutex> texLock(s_TexMx);
    DrainReadyTexturesLocked();

    std::string key(texture_name);
    auto it = s_TexCache.find(key);
    if (it != s_TexCache.end()) return it->second.set;
    if (pending) *pending = true;
    if (s_TexPending.count(key)) return VK_NULL_HANDLE;

    string_path full;
    if (!ResolveSpritePath(texture_name, full)) {
        if (pending) *pending = false;
        return FinishTextureSet(std::move(key), texture_name, nullptr);   // negative cache, as the sync path
    }
    s_TexPending.insert(key);
    static PinnedWorker* s_worker = new PinnedWorker(2, 0x3B);   // background priority: it waits for the SD
    s_worker->Run([key, path = std::string(full)] {
        CVulkanTexture* tex = LoadSprite(path.c_str());
        std::lock_guard<std::mutex> lk(s_TexReadyMx);
        s_TexReady.emplace_back(key, tex);
    });
    return VK_NULL_HANDLE;
}

VkDescriptorSet GetTextureSet(const char* texture_name)
{
    if (!s_Init || !texture_name || !texture_name[0]) return VK_NULL_HANDLE;
    std::lock_guard<std::recursive_mutex> texLock(s_TexMx);
    DrainReadyTexturesLocked();

    std::string key(texture_name);
    auto it = s_TexCache.find(key);
    if (it != s_TexCache.end()) return it->second.set;

    string_path full;
    CVulkanTexture* tex = ResolveSpritePath(texture_name, full) ? LoadSprite(full) : nullptr;
    return FinishTextureSet(std::move(key), texture_name, tex);
}

// Files the texture (or its absence) in the cache with a descriptor set. s_TexMx held.
static VkDescriptorSet FinishTextureSet(std::string key, const char* texture_name, CVulkanTexture* tex)
{
    PTex entry;
    entry.tex = tex;
    if (!entry.tex) {
        Msg("![VK Particles] sprite texture not found: '%s'", texture_name);
        s_TexCache.emplace(std::move(key), entry);   // negative cache (set = null)
        return VK_NULL_HANDLE;
    }

    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = s_Pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &s_SetLayout;
    if (vkAllocateDescriptorSets(VulkanHW.m_Device, &ai, &entry.set) != VK_SUCCESS) {
        Msg("![VK Particles] descriptor pool exhausted for '%s'", texture_name);
        entry.tex->Destroy(); xr_delete(entry.tex);
        entry.tex = nullptr; entry.set = VK_NULL_HANDLE;
        s_TexCache.emplace(std::move(key), entry);
        return VK_NULL_HANDLE;
    }

    VK::DescriptorWriter(entry.set).ImageSampler(0, entry.tex->GetView(), s_Sampler).Flush();

    VkDescriptorSet set = entry.set;
    s_TexCache.emplace(std::move(key), entry);
    return set;
}

VkImageView GetTextureView(const char* texture_name)
{
    if (!s_Init || !texture_name || !texture_name[0]) return VK_NULL_HANDLE;
    std::lock_guard<std::recursive_mutex> texLock(s_TexMx);
    GetTextureSet(texture_name);                      // ensure loaded + cached
    auto it = s_TexCache.find(std::string(texture_name));
    if (it != s_TexCache.end() && it->second.tex)
        return it->second.tex->GetView();
    return VK_NULL_HANDLE;
}

}  // namespace ParticlePass


void CollectSmokeParticles(xr_vector<VK::Vol::SmokeParticle>& out)
{
    // World-phase alpha smoke (PBM_BLEND) only — additive fire/sparks/muzzle are
    // self-emissive (not media), HUD smoke uses a different projection. Same flatten
    // as Pass_Particles. Each live PAPI particle → a media splat sample.
    //
    // Two-zone LOD: the exp-Z froxel grid is fine near the camera but coarse far away
    // (distant smoke = blobby muddy clouds + wasted splat). FULL volumetric quality only
    // inside dist_full (~12 m); from there density fades linearly to 0 by smoke_dist
    // (capped at the terrain detail bubble r__detail_radius); beyond that the billboard
    // alone represents it (still lit by the Stage-0 probe). So the pretty volumetric smoke
    // is the near body, the muddy far blobs never form, and the expensive full-footprint
    // splats concentrate near the camera (far footprints shrink with cell size anyway).
    const Fvector& eye    = RV().vCameraPosition;
    const Fvector& camDir = RV().vCameraDirection;   // forward — drop the behind-camera hemisphere
    const float maxD      = _min(ps_r_vol_smoke_dist, float(_max(ps_r__detail_radius, 1)));
    const float maxDSqr   = maxD * maxD;
    const float distFull  = _min(ps_r_vol_smoke_dist_full, maxD);   // full-quality inner radius
    const float fadeRange = _max(maxD - distFull, 0.001f);

    static xr_vector<vkCParticleEffect*> collect;
    for (const DynVisual& d : DynamicVisuals()) {
        if (!d.vis) continue;
        const u32 t = d.vis->Type;
        if (t != MT_PARTICLE_EFFECT && t != MT_PARTICLE_GROUP) continue;
        collect.clear();
        static_cast<vkParticleVisual*>(d.vis)->CollectEffects(collect);
        for (vkCParticleEffect* e : collect) {
            if (!e || e->GetBlendMode() != PBM_BLEND || e->GetHudMode()) continue;

            PAPI::Particle* particles = nullptr;
            u32 p_cnt = 0;
            PAPI::ParticleManager()->GetParticles(e->GetHandleEffect(), particles, p_cnt);
            if (!particles || !p_cnt) continue;

            const bool xform = e->m_RT_Flags.is(vkCParticleEffect::flRT_XFORM);
            for (u32 i = 0; i < p_cnt; ++i) {
                const PAPI::Particle& m = particles[i];
                if (m.colorA <= 0.003f) continue;            // invisible → contributes no media

                Fvector wp;
                if (xform) e->m_XFORM.transform_tiny(wp, m.pos);
                else       wp.set(m.pos.x, m.pos.y, m.pos.z);

                // Behind the camera → can never land in a view-frustum froxel (the splat
                // would reject it anyway); cull here so it's not uploaded/splatted at all.
                Fvector rel; rel.sub(wp, eye);
                if (rel.dotproduct(camDir) <= 0.0f) continue;
                const float d2 = eye.distance_to_sqr(wp);
                if (d2 > maxDSqr) continue;                  // outside the detail bubble → billboard only
                float dens = m.colorA;
                const float dd = _sqrt(d2);
                if (dd > distFull) dens *= _max(1.0f - (dd - distFull) / fadeRange, 0.0f);   // LOD fade → billboard
                if (dens <= 0.003f) continue;

                VK::Vol::SmokeParticle sp;
                sp.pos[0] = wp.x; sp.pos[1] = wp.y; sp.pos[2] = wp.z;
                sp.radius = (m.size.x + m.size.y) * 0.25f;   // avg billboard half-extent
                // Same decode as the sprite path (vk_ParticleEffect): this colour becomes
                // participating-media albedo in the froxel volume, so it has to be in the
                // same space as the sun/ambient radiance it scatters. [3] is density.
                sp.color[0] = m.colorR; sp.color[1] = m.colorG; sp.color[2] = m.colorB; sp.color[3] = dens;
                ColorSpace::LinearizeRGB(sp.color);
                out.push_back(sp);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// RENDER_THREAD_PLAN step 7 -- EXTRACT HALF OF THE PARTICLE PASS.
// Everything that touches the live PAPI arrays / effect objects (collect, texture resolve, BuildVertices)
// runs here, from the PREPARE phase on the main thread, and fills the frame's ring slot. Pass_Particles
// below only binds and draws what this left in PF(). PF().frame tags the frame: a record that finds a
// different tag (prepares not registered yet, or skipped) draws nothing, as a frame without particles.
// ---------------------------------------------------------------------------
namespace {
// blend = e->GetBlendMode() taken at extract: the record side must not touch the live effect object (the game may free or
// rewrite it while the frame is still being recorded).
struct PDraw { vkCParticleEffect* e; VkDescriptorSet set; u32 vtxStart, vcount; u32 blend; };
struct ParticleFrame {
    xr_vector<PDraw> world, hud, distort;
    u32  vtxUsed = 0;
    u32  frame   = u32(-1);
    bool valid   = false;
};
// Two-slot pipeline: one set per frame slot (extract of frame N+1 fills its own while record of frame N draws its).
struct ParticleSlot {
    ParticleFrame pf;
    xr_vector<VK::Vol::SmokeParticle> smoke;
    u32 smokeTag = u32(-1);
};
RT::PerSlot<ParticleSlot> s_partSlots;
inline ParticleFrame& PF() { return s_partSlots.cur().pf; }
inline xr_vector<VK::Vol::SmokeParticle>& SmokeFrame() { return s_partSlots.cur().smoke; }
inline u32& SmokeTag() { return s_partSlots.cur().smokeTag; }
}  // namespace

const xr_vector<VK::Vol::SmokeParticle>& SmokeParticlesFrame()
{
    static const xr_vector<VK::Vol::SmokeParticle> s_none;
    return (SmokeTag() == RV().frameIndex) ? SmokeFrame() : s_none;
}

// r_ptcl_mt: the billboards of one frame, planned by Particles_Extract and filled by a core-1 worker + the main thread
struct PtclJob { vkCParticleEffect* e; u32 first, cap; xr_vector<PDraw>* out; u32 idx; };
struct PtclFill
{
    xr_vector<PtclJob> jobs;
    std::atomic<u32> next{0};
    u8* base = nullptr;
    CVulkanBuffer* ring = nullptr;
    u32 vtxUsed = 0;
    bool pending = false, worker = false;
};
static PtclFill s_fill;
static PinnedWorker* s_ptclWorker = nullptr;

static void PtclDrain()
{
    PtclFill& f = s_fill;
    const u32 n = (u32)f.jobs.size();
    for (u32 i = f.next.fetch_add(1, std::memory_order_relaxed); i < n; i = f.next.fetch_add(1, std::memory_order_relaxed))
    {
        PtclJob& j = f.jobs[i];
        const u32 got = j.e->BuildVertices((FVF::LIT*)(f.base + (size_t)j.first * kVtxStride), j.cap);
        (*j.out)[j.idx].vcount = got;   // <= cap; vkCmdDraw of 0 vertices draws nothing
    }
}

void Particles_ExtractFinish();

void Particles_Extract()
{
    RT_ASSERT_MAIN();
    if (s_fill.pending)   // a frame whose finish never ran: close its ring before this one plans
        Particles_ExtractFinish();
    // Stage-1 VMS smoke media: same gate the World pass used (the froxel grid's own Wanted/Ready are record-side).
    SmokeFrame().clear();
    SmokeTag() = RV().frameIndex;
    // 11-10-2026: and only while the froxel volume is on at all (its only reader, vk_pass_world, is behind
    // Vol::Wanted too). The Switch preset has r_vol 0 with the inject at its default 1: every smoke particle
    // was gathered and colour-converted (3 powf) on the main thread for nothing -- ~15 % of Particles_Extract.
    if (ps_r_vol_smoke_inject > 0.0f && VK::Vol::Wanted())
        CollectSmokeParticles(SmokeFrame());

    PF().valid = false;
    PF().world.clear(); PF().hud.clear(); PF().distort.clear(); PF().vtxUsed = 0;
    if (!s_Init) return;
    const bool glassRefr = ps_r_glass_refr > 0.001f;
    if (DynamicVisuals().empty() && HudVisuals().empty() && s_DistortImg == VK_NULL_HANDLE && !glassRefr) return;

    static xr_vector<vkCParticleEffect*> s_world, s_hud, s_distort, s_collect;
    s_world.clear(); s_hud.clear(); s_distort.clear();
    auto collectList = [&](const xr_vector<DynVisual>& list) {
        for (const DynVisual& d : list) {
            if (!d.vis) continue;
            const u32 t = d.vis->Type;
            if (t != MT_PARTICLE_EFFECT && t != MT_PARTICLE_GROUP) continue;
            s_collect.clear();
            static_cast<vkParticleVisual*>(d.vis)->CollectEffects(s_collect);
            for (vkCParticleEffect* e : s_collect) {
                if (!e) continue;
                // GPU-particle off-screen emission gate: "the game submitted
                // this effect to render" — hidden 1st-person attachments never
                // land here and must not emit on the GPU path either.
                e->m_GpuLastSubmitT = RV().fTimeGlobal;
                if (e->GetBlendMode() == PBM_DISTORT) s_distort.push_back(e);
                else if (e->GetHudMode())             s_hud.push_back(e);
                else                                  s_world.push_back(e);
            }
        }
    };
    collectList(DynamicVisuals());
    collectList(HudVisuals());
    const bool runDistort = !s_distort.empty() || s_DistortImg != VK_NULL_HANDLE || glassRefr;
    if (s_world.empty() && s_hud.empty() && !runDistort) return;

    const u32 slot = RV().ringSlot;
    CVulkanBuffer& ring = s_Ring[slot];
    if (!ring.IsValid()) return;
    u8* base = (u8*)ring.Map();
    if (!base) return;

    u32 vtxUsed = 0;
    if (ps_r_ptcl_mt > 0)
    {
        // 📏12-10-2026 Switch, a 12-bandit fight: the billboards were ~0.8 ms of the main thread (4.7 %) while core 1's
        // render thread sat waiting for it. The main thread only plans here -- every effect gets its slice of the ring
        // in the same order as before (= draw order), sized by PlannedVerts -- and hands the filling to a worker on
        // core 1, then goes on with the other prepares (bones, pre-skin, rain...). Particles_ExtractFinish (the last
        // prepare) takes over whatever the worker has not started, waits for it and closes the ring. BuildVertices only
        // reads the effect and writes its own slice; RV() on the worker is the extract slot, as on the main thread.
        PtclFill& f = s_fill;
        f.jobs.clear();
        auto plan = [&](xr_vector<vkCParticleEffect*>& list, xr_vector<PDraw>& out) {
            for (vkCParticleEffect* e : list) {
                VkDescriptorSet set = e->ResolveTextureSet();
                if (set == VK_NULL_HANDLE) continue;           // missing texture
                if (vtxUsed + 6 > kRingVerts) break;           // ring full
                const u32 want = e->PlannedVerts();
                if (want == 0) continue;
                const u32 cap = std::min(want, (kRingVerts - vtxUsed) / 6 * 6);
                if (cap == 0) break;
                f.jobs.push_back({ e, vtxUsed, cap, &out, (u32)out.size() });
                out.push_back({ e, set, vtxUsed, cap, (u32)e->GetBlendMode() });
                vtxUsed += cap;
            }
        };
        plan(s_world, PF().world);        // ring order = draw order of the three phases, as before
        plan(s_hud, PF().hud);
        plan(s_distort, PF().distort);

        f.base = base;
        f.ring = &ring;
        f.vtxUsed = vtxUsed;
        f.next.store(0, std::memory_order_relaxed);
        f.pending = true;
        f.worker = !f.jobs.empty();
        if (f.worker)
        {
            if (!s_ptclWorker)
                s_ptclWorker = new PinnedWorker(1); // core 1, 0x2E: below the render thread (0x2D) it shares the core with
            s_ptclWorker->SetPrio(ps_r_core1_prio);    // the main thread waits for it: not below the render thread on core 1
            s_ptclWorker->Run([] { PtclDrain(); });
        }
        return;   // Particles_ExtractFinish
    }
    auto build = [&](xr_vector<vkCParticleEffect*>& list, xr_vector<PDraw>& out) {
        for (vkCParticleEffect* e : list) {
            VkDescriptorSet set = e->ResolveTextureSet();
            if (set == VK_NULL_HANDLE) continue;           // missing texture
            if (vtxUsed >= kRingVerts) break;              // ring full
            const u32 avail = kRingVerts - vtxUsed;
            FVF::LIT* dst = (FVF::LIT*)(base + (size_t)vtxUsed * kVtxStride);
            const u32 vcount = e->BuildVertices(dst, avail);
            if (vcount == 0) continue;
            out.push_back({ e, set, vtxUsed, vcount, (u32)e->GetBlendMode() });
            vtxUsed += vcount;
        }
    };
    build(s_world, PF().world);        // ring order = draw order of the three phases, as before
    build(s_hud, PF().hud);
    build(s_distort, PF().distort);
    ring.Flush();
    ring.Unmap();
    PF().vtxUsed = vtxUsed;
    PF().frame   = RV().frameIndex;
    PF().valid   = true;
}

// The last prepare (CRender_Vulkan.cpp RegisterPrepare): r_ptcl_mt's filling, finished.
void Particles_ExtractFinish()
{
    RT_ASSERT_MAIN();
    PtclFill& f = s_fill;
    if (!f.pending)
        return;
    f.pending = false;
    PtclDrain();                 // whatever the worker has not started yet
    if (f.worker)
        s_ptclWorker->Wait();
    f.ring->Flush();
    f.ring->Unmap();
    PF().vtxUsed = f.vtxUsed;
    PF().frame   = RV().frameIndex;
    PF().valid   = true;
}

void Pass_Particles(FrameContext& ctx)
{
    if (!s_Init)                   return;
    if (ctx.cmd == VK_NULL_HANDLE) return;
    // Built by Particles_Extract() (PREPARE phase) for THIS frame, or nothing to draw.
    if (!PF().valid || PF().frame != RV().frameIndex) return;
    // Glass refraction needs the distort phase even with no particle around.
    const bool glassRefr = ps_r_glass_refr > 0.001f;
    // Once the distort RT exists it must be refreshed (cleared) EVERY frame --
    // otherwise the tonemap would re-apply last frame's frozen haze.
    const bool runDistort = !PF().distort.empty() || s_DistortImg != VK_NULL_HANDLE || glassRefr;
    if (PF().world.empty() && PF().hud.empty() && !runDistort) return;

    VkCommandBuffer cmd = ctx.cmd;
    const u32 slot = CommandManager.GetCurrentFrame();
    CVulkanBuffer& ring = s_Ring[slot];
    if (!ring.IsValid()) return;

    VkPipeline      lastPipe = VK_NULL_HANDLE;
    VkDescriptorSet lastSet  = VK_NULL_HANDLE;
    const u32 vtxUsed = PF().vtxUsed;
    u32 nDraw = 0, nHud = 0, nDistort = 0;

    // ---- Stage-0 volumetric smoke lighting setup -----------------------------
    // Build the froxel camera basis the VS uses to map each vertex to its froxel.
    // The probe (drawList's per-effect volParams.x) is enabled only for the WORLD
    // phase, only when r_vol actually ran this frame (scatter is valid + SHADER_READ),
    // and only on alpha-blended smoke — see drawList. The basis is built from the
    // scene camera even for the HUD phase (which pushes strength 0, so it's unused).
    EnsureVolSet();
    const float smokeStrength = (Vol::Ready() && Vol::Wanted() && Vol::ProbeAvailableToGraphics())
                              ? ps_r_vol_smoke : 0.0f;

    ParticlePush push{};
    {
        const Vol::GridZParams gz = Vol::GetGridZ();
        const Fvector& eye = RV().vCameraPosition;
        Fvector dir; dir.set(0.f, 0.f, 1.f);
        if (ctx.viewProj) dir = DeriveProjTerms(*ctx.viewProj).dir;
        push.camPosNear[0]  = eye.x; push.camPosNear[1]  = eye.y; push.camPosNear[2]  = eye.z; push.camPosNear[3]  = gz.nearZ;
        push.camDirLogFN[0] = dir.x; push.camDirLogFN[1] = dir.y; push.camDirLogFN[2] = dir.z; push.camDirLogFN[3] = gz.logFarNear;
        push.volParams[1]   = ctx.extent.width  ? 1.0f / float(ctx.extent.width)  : 0.0f;
        push.volParams[2]   = ctx.extent.height ? 1.0f / float(ctx.extent.height) : 0.0f;
        push.volParams[3]   = ps_r_vol_smoke_clamp;
    }

    // Shared draw loop: builds billboards into the ring at the running offset and
    // issues one draw per effect. `vpMat` is the phase's view-projection; `probe`
    // is the Stage-0 light-probe strength, applied ONLY to alpha-blended smoke
    // (PBM_BLEND) — additive fire/sparks/muzzle are self-emissive and would
    // over-glow, so everything else pushes strength 0. The push carries viewProj,
    // so it's re-issued whenever the strength changes (and always on the first
    // drawable effect). `forcedPipe` overrides the per-blend pipeline (the distort
    // phase renders every effect with one pipeline).
    // Shared draw loop over the extract-built draws. `vpMat` is the phase's view-projection; `probe`
    // is the Stage-0 light-probe strength, applied ONLY to alpha-blended smoke (PBM_BLEND) -- additive
    // fire/sparks/muzzle are self-emissive and would over-glow, so everything else pushes strength 0.
    // The push carries viewProj, so it's re-issued whenever the strength changes (and always on the
    // first drawable effect). `forcedPipe` overrides the per-blend pipeline (the distort phase renders
    // every effect with one pipeline).
    auto drawList = [&](const xr_vector<PDraw>& list, VkPipeline forcedPipe,
                        u32& counter, const Fmatrix& vpMat, float probe) {
        push.viewProj = vpMat;
        float lastStrength = -1.0f;   // != any real strength -> push on first drawable effect
        for (const PDraw& pd : list) {
            VkPipeline pipe = forcedPipe ? forcedPipe : ParticlePass::GetPipeline((EParticleBlendMode)pd.blend);
            if (pipe == VK_NULL_HANDLE) continue;          // build failure

            const float strength = (pd.blend == PBM_BLEND) ? probe : 0.0f;
            if (strength != lastStrength) {
                push.volParams[0] = strength;
                vkCmdPushConstants(cmd, s_Layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(ParticlePush), &push);
                lastStrength = strength;
            }

            if (pipe != lastPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); lastPipe = pipe; }
            if (pd.set != lastSet) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_Layout, 0, 1, &pd.set, 0, nullptr); lastSet = pd.set; }

            vkCmdDraw(cmd, pd.vcount, 1, pd.vtxStart, 0);
            ++counter;
        }
    };

    VkBuffer vbuf = ring.GetHandle();
    VkDeviceSize voff = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &voff);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_Layout, 1, 1, &s_VolSet, 0, nullptr);

    VkRect2D sc{ {}, ctx.extent };

    // ---- Phases 1+2: scene colour + world depth, no depth write -------------
    if (!PF().world.empty() || !PF().hud.empty()) {
        VK::RenderingBuilder(ctx.extent)
            .Color(ctx.colorView)
            .Depth(ctx.depthView, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE)
            .Begin(cmd);

        // Kept explicit (not BeginFlipped): the HUD pass below re-submits this
        // same viewport with a compressed depth range.
        VkViewport vp{};
        vp.x = 0.0f; vp.y = (float)ctx.extent.height;
        vp.width = (float)ctx.extent.width; vp.height = -(float)ctx.extent.height;
        vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        if (!PF().world.empty() && ctx.viewProj) {
            drawList(PF().world, VK_NULL_HANDLE, nDraw, *ctx.viewProj, smokeStrength);   // alpha smoke catches the froxel light
        }

        if (!PF().hud.empty()) {
            // R4 HUD-mode particles: positions stay world-space, but the
            // projection switches to the HUD FOV (mFullTransform_hud2 = real
            // view × HUD-FOV proj) and the depth range compresses to
            // [0, 0.02] (rmNear) — in front of the world, depth-tested only
            // against the HUD weapon itself.
            vp.minDepth = 0.0f; vp.maxDepth = 0.02f;
            vkCmdSetViewport(cmd, 0, 1, &vp);
            // HUD smoke: no froxel probe (different projection/depth) → strength 0.
            drawList(PF().hud, VK_NULL_HANDLE, nHud, RV().mFullTransform_hud2, 0.0f);
        }

        vkCmdEndRendering(cmd);
    }

    // ---- Phase 3: distortion buffer (heat haze) ------------------------------
    if (runDistort && EnsureDistortRT(ctx.extent)) {
        if (s_DistortPipe == VK_NULL_HANDLE)
            s_DistortPipe = BuildPipeline(PBM_BLEND, /*distortTarget*/ true);

        // UNDEFINED on the first use; SHADER_READ (tonemap sampled it) after.
        ImageBarrier(cmd, s_DistortImg,
                     s_DistortFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        s_DistortFirst = false;

        // CLEAR to neutral (= no offset); world depth LOADed for the test only,
        // so haze behind walls does not bleed through.
        VK::RenderingBuilder(ctx.extent)
            .ColorClear(s_DistortView, VkClearColorValue{ { 0.5f, 0.5f, 0.5f, 0.0f } })
            .Depth(ctx.depthView, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE)
            .BeginFlipped(cmd);

        if (!PF().distort.empty() && ctx.viewProj) {
            // distort writes UV offsets, not colour — no probe → strength 0.
            drawList(PF().distort, s_DistortPipe, nDistort, *ctx.viewProj, 0.0f);
        }

        // Glass refraction: re-draw the late-glass panes as procedural wobble
        // (r_glass_refr) — the tonemap bends the scene behind them. Both halves:
        // world-path panes (windows/doors/vehicles) + kinematics (cabinet doors).
        if (ps_r_glass_refr > 0.001f && ctx.viewProj) {
            DrawGlassDistort(cmd, *ctx.viewProj);
            Skinned_RenderGlassDistort(cmd, *ctx.viewProj, ps_r_glass_refr);
        }

        vkCmdEndRendering(cmd);
        ImageBarrier(cmd, s_DistortImg,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    static bool s_diag = false;
    if (!s_diag && (nDraw + nHud + nDistort)) {
        Msg("[VK Particles] first frame drawn: world=%u hud=%u distort=%u verts=%u", nDraw, nHud, nDistort, vtxUsed);
        s_diag = true;
    }
    static bool s_diagHud = false;
    if (!s_diagHud && nHud) { Msg("[VK Particles] first HUD effect drawn (hud draws=%u)", nHud); s_diagHud = true; }
    static bool s_diagDist = false;
    if (!s_diagDist && nDistort) { Msg("[VK Particles] first DISTORT effect drawn (draws=%u)", nDistort); s_diagDist = true; }
}

}  // namespace VK
