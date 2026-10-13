// xrRenderVulkan - Snow (and mud) DEFORMATION texture (compute).
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// A PERSISTENT, WORLD-ANCHORED (toroidal) top-down compression field. Unlike the rain
// box (±75 m @ 1024² = 14.6 cm/texel = too coarse for footprints), this has its OWN
// dense field (±kHalf window @ kSize) so prints are sharp. Feet stamp depressions; the
// field decays slowly so prints last ~r_snow_deform_time seconds. Each texel maps to a
// FIXED world XZ (mod kWorld), so camera movement needs NO reprojection — only the thin
// leading strip of texels that just scrolled in (their world identity flipped) is
// cleared. The compute runs IN PLACE on one image (no scratch / no copy-back). Read by
// snow_displace.glsl (EnvLight binding 20, uv = worldXZ/kWorld, REPEAT wrap, windowed to
// ±kHalf of the eye). See deform_stamp.comp.glsl.

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_profiler.h"   // TEMP VUID-hunt: VK::Prof::NameImage
#include "vk_deform.h"
#include "vk_image.h"      // VK::CreateImage2D / CreateImageView
#include "vk_barriers.h"   // VK::ImageBarrier (explicit stage/access overload)
#include "vk_compute_util.h" // VK::MakePipelineLayout / CreateComputePipeline
#include "vk_shaders.h"         // g_ShaderManager
#include "vk_pipeline_cache.h"  // shared pipeline cache object
#include "vk_shadow.h"          // rain map view/sampler/VP + RainEyeY (terrain-height gate)
#include "vk_command_buffer.h"  // CommandManager.GetCurrentFrame() (per-slot stamp UBO)

extern float ps_r_snow_deform_time;   // r_snow_deform_time — print lifetime (sec)
extern float ps_r_snow_berm;          // r_snow_berm — displaced-snow berm height (fraction of dent depth)
extern float ps_r_snow_rough;         // r_snow_rough — per-print/edge imperfection strength

namespace VK { namespace Deform {

namespace {
    bool          s_inited = false, s_failed = false, s_first = true;
    constexpr u32   kSize = 2048;     // 2048² R16F (~8 MB each) — sharp prints
    constexpr float kHalf = 28.f;     // ±28 m around the camera (~2.7 cm/texel — enough for boot TREAD bars)
    constexpr float kEyeUp = 100.f;
    constexpr float kZNear = 1.f;
    constexpr float kZFar  = 350.f;
    Fmatrix       s_vp;   // uploaded to EnvLight deform_vp; unused by the toroidal sample path (kept for the UBO field)

    struct Buf { VkImage img = VK_NULL_HANDLE; VmaAllocation alloc = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
    Buf s_state;   // the single persistent press field — TOROIDAL, updated IN PLACE (no scratch/copy)

    VkSampler             s_sampler    = VK_NULL_HANDLE;
    VkDescriptorSetLayout s_setLayout  = VK_NULL_HANDLE;
    VkDescriptorPool      s_pool       = VK_NULL_HANDLE;
    VkDescriptorSet       s_set        = VK_NULL_HANDLE;
    VkPipelineLayout      s_pipeLayout = VK_NULL_HANDLE;
    VkPipeline            s_pipe       = VK_NULL_HANDLE;

    // Stamps live in a BUFFER (not push constants) so there's no per-frame count cap —
    // many NPC feet + items all stamp the same frame. Per-in-flight-slot region (dynamic
    // UBO offset) so the CPU never overwrites a region the GPU is still reading.
    constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;
    constexpr u32 kMaxStamps = 64;
    struct StampsUBO {
        float sPos[kMaxStamps][4];    // xyz = world pos, w = radius (m)
        float sPar[kMaxStamps][4];    // x = press strength (1 foot .. ~0.34 item)
    };
    constexpr VkDeviceSize kStampStride = (sizeof(StampsUBO) + 255) & ~VkDeviceSize(255);
    VkBuffer       s_stampBuf    = VK_NULL_HANDLE;
    VmaAllocation  s_stampAlloc  = VK_NULL_HANDLE;
    u8*            s_stampMapped = nullptr;

    struct PushConstants {
        Fmatrix rainVP;               // world -> rain ndc (terrain-height gate)
        float   p0[4];                // kSize, decay (dt/life), kWorld (m), stampCount
        float   p1[4];                // xy = movement dir (world XZ), z = berm max (frac), w = ground gate (m)
        float   p2[4];                // x = rain eyeY, y = rain zRange, z = rough, w = unused
        float   p3[4];                // xy = cam window min (camXZ-kHalf), zw = prev cam window min
    };

    float s_prevCamX = 0.f, s_prevCamZ = 0.f;
    bool  s_haveCam  = false;

    // Cache gate: the field is player-centred and must reproject/decay every frame it
    // holds prints — but once every print has decayed it is flat ZERO, and reprojecting
    // or decaying zero stays zero. Track how long prints may still be visible so we can
    // skip the whole dispatch when the field is guaranteed empty (nobody stepped).
    float s_contentUntil = 0.f;   // RV().fTimeGlobal until which the field may hold visible prints
    bool  s_skipped      = false; // last Dispatch skipped GPU work -> resume uses a fresh window (prev is stale)

    bool createImage(VkImageUsageFlags usage, Buf& out)
    {
        if (!VK::CreateImage2D(VK_FORMAT_R16_SFLOAT, { kSize, kSize }, usage,
                out.img, out.alloc, "Deform.Field"))
            return false;
        out.view = VK::CreateImageView(out.img, VK_FORMAT_R16_SFLOAT);
        return out.view != VK_NULL_HANDLE;
    }

    void destroyBuf(Buf& b)
    {
        if (b.view) { vkDestroyImageView(VulkanHW.m_Device, b.view, nullptr); b.view = VK_NULL_HANDLE; }
        if (b.img)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, b.img, b.alloc); b.img = VK_NULL_HANDLE; }
    }

    void computeVP()
    {
        const Fvector dir{ 0.f, -1.f, 0.f };
        const Fvector up { 0.f,  0.f, 1.f };
        Fvector eye; eye.set(RV().vCameraPosition.x, RV().vCameraPosition.y + kEyeUp, RV().vCameraPosition.z);
        Fmatrix view; view.build_camera_dir(eye, dir, up);
        const float texel = (2.f * kHalf) / float(kSize);   // texel snap -> stable reprojection
        view.c.x = floorf(view.c.x / texel) * texel;
        view.c.y = floorf(view.c.y / texel) * texel;
        Fmatrix proj; proj.build_projection_ortho(2.f * kHalf, 2.f * kHalf, kZNear, kZFar);
        s_vp.mul(proj, view);
    }
}

bool Ready() { return s_inited && !s_failed; }
VkImageView GetView()    { return s_state.view; }
VkSampler   GetSampler() { return s_sampler; }
const Fmatrix& GetVP()   { return s_vp; }
u32         Size()       { return kSize; }
float       Half()       { return kHalf; }

bool Init()
{
    if (s_inited) return true;
    if (s_failed) return false;
    if (ShadowMap::GetRainView() == VK_NULL_HANDLE) return false;   // need the rain map for the ground gate (retry next frame)
    s_vp.identity();

    // One image: STORAGE (compute reads+writes in place) + SAMPLED (consumers sample it).
    const VkImageUsageFlags fieldUse = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!createImage(fieldUse, s_state)) {
        Msg("![VK Deform] image create failed"); s_failed = true; return false;
    }

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;   // toroidal wrap
    si.maxLod = 0.25f;
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_sampler) != VK_SUCCESS) {
        Msg("![VK Deform] sampler create failed"); s_failed = true; return false;
    }

    if (!g_ShaderManager) { Msg("![VK Deform] g_ShaderManager null"); s_failed = true; return false; }

    // Per-slot stamp UBO (host-visible, persistently mapped).
    {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = kStampStride * kFramesInFlight;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (VK::Vram::CreateBuffer(VulkanHW.m_Allocator, &bci, &aci, &s_stampBuf, &s_stampAlloc, &info) != VK_SUCCESS) {
            Msg("![VK Deform] stamp buffer create failed"); s_failed = true; return false;
        }
        s_stampMapped = (u8*)info.pMappedData;
    }

    // binding 0 = field (STORAGE, in place), 2 = stamps (dynamic UBO), 3 = rain (sampler).
    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 2; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[2].binding = 3; b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo slci{};
    slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    slci.bindingCount = 3; slci.pBindings = b;
    vkCreateDescriptorSetLayout(VulkanHW.m_Device, &slci, nullptr, &s_setLayout);

    // The layout above stays hand-rolled: its bindings are 0/2/3, not 0..N.
    s_pool = VK::MakeDescriptorPool({ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,             // field (in place)
                                      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },  // rain
                                    1, "Deform.Stamp");
    if (!s_pool || !VK::AllocSets(s_pool, s_setLayout, 1, &s_set, "Deform.Stamp")) {
        s_failed = true; return false;
    }

    VK::DescriptorWriter(s_set)
        .StorageImage(0, s_state.view)                          // field (storage, in place)
        .Buffer      (2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                         { s_stampBuf, 0, sizeof(StampsUBO) })
        .ImageSampler(3, ShadowMap::GetRainView(), ShadowMap::GetSampler())
        .Flush();

    s_pipeLayout = VK::MakePipelineLayout({ s_setLayout }, sizeof(PushConstants));

    VkShaderModule cs = g_ShaderManager->Load("deform_stamp.comp.spv");
    if (cs == VK_NULL_HANDLE) { Msg("![VK Deform] deform_stamp.comp.spv load failed"); s_failed = true; return false; }
    s_pipe = VK::CreateComputePipeline(cs, s_pipeLayout, "Deform.Stamp");
    if (s_pipe == VK_NULL_HANDLE) { s_failed = true; return false; }

    s_inited = true; s_first = true;
    Msg("[VK Deform] init OK (%ux%u R16F, +-%.0f m, %.1f cm/texel)", kSize, kSize, kHalf, 200.f * kHalf / float(kSize));
    return true;
}

void Dispatch(VkCommandBuffer cmd, const Stamp* stamps, u32 count)
{
    if (!s_inited && !Init()) return;

    computeVP();

    const float life = (ps_r_snow_deform_time > 0.1f) ? ps_r_snow_deform_time : 0.1f;
    const u32   n    = (count < kMaxStamps) ? count : kMaxStamps;

    // ---- CACHE GATE: skip the full-field compute when the field cannot change. ------
    // A fresh stamp keeps the field non-empty for up to `life` seconds; after that every
    // texel has decayed back to flat zero, and reprojecting/decaying ZERO stays ZERO. So
    // with no active prints AND no new stamp this frame, keep the cached image and skip
    // the whole 2048² dispatch + 8 MB copy-back. (Nobody stepped -> nothing to compute:
    // dry areas, interiors, standing still after prints faded, no NPCs/props nearby.)
    // Never gate the first frame (layout init) or a frame that brings a fresh stamp.
    if (n > 0) s_contentUntil = RV().fTimeGlobal + life;
    const bool hasContent = RV().fTimeGlobal < s_contentUntil;
    if (!s_first && n == 0 && !hasContent) {
        s_skipped  = true;
        s_prevCamX = RV().vCameraPosition.x;   // keep berm-direction tracking current
        s_prevCamZ = RV().vCameraPosition.z;
        s_haveCam  = true;
        return;
    }

    // ---- Layout: the field sits in SHADER_READ_ONLY between frames (consumers sample it).
    // Bring it to GENERAL for the compute's in-place read-modify-write; first frame comes
    // from UNDEFINED. The consumer stages (terrain frag/tese/tesc, snow-mesh VS/FS) are the
    // src of the read->write hazard.
    const VkPipelineStageFlags2 kConsumers =
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
        | VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT;
    if (s_first) {
        VK::ImageBarrier(cmd, s_state.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    } else {
        VK::ImageBarrier(cmd, s_state.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         kConsumers, VK_ACCESS_2_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    }

    const float dt = (RV().fTimeDelta < 0.1f) ? RV().fTimeDelta : 0.1f;
    const float camX = RV().vCameraPosition.x, camZ = RV().vCameraPosition.z;

    // Fill this in-flight slot's stamp region (per-slot -> no CPU/GPU overwrite race).
    const u32 slot = CommandManager.GetCurrentFrame() % kFramesInFlight;
    StampsUBO* su = reinterpret_cast<StampsUBO*>(s_stampMapped + slot * kStampStride);
    for (u32 i = 0; i < n; ++i) {
        su->sPos[i][0] = stamps[i].pos.x;
        su->sPos[i][1] = stamps[i].pos.y;     // world Y for the ground gate
        su->sPos[i][2] = stamps[i].pos.z;
        su->sPos[i][3] = stamps[i].radius;
        su->sPar[i][0] = stamps[i].pressDepth;
        su->sPar[i][1] = stamps[i].dirX;      // boot facing (0,0 = round item contact)
        su->sPar[i][2] = stamps[i].dirZ;
        su->sPar[i][3] = 0.f;
    }

    // Movement direction (world XZ) from the camera delta -> the berm is plowed forward.
    float mdx = 0.f, mdz = 0.f;
    if (s_haveCam) {
        const float dx = camX - s_prevCamX, dz = camZ - s_prevCamZ;
        const float len = sqrtf(dx * dx + dz * dz);
        if (len > 0.012f) { mdx = dx / len; mdz = dz / len; }   // ignore tiny jitter / standing still
    }

    PushConstants pc{};
    pc.rainVP = ShadowMap::GetRainVP();
    pc.p0[0] = float(kSize);
    pc.p0[1] = dt / life;                  // decay this frame
    pc.p0[2] = 2.f * kHalf;                // kWorld: the toroidal world period (m)
    pc.p0[3] = float(n);
    pc.p1[0] = mdx; pc.p1[1] = mdz;
    pc.p1[2] = ps_r_snow_berm;   // berm max (× dent depth in the mesh) — front ridge ~1.5×, sides ~0.4×
    pc.p1[3] = 0.45f;            // ground gate (m): contact must be within this of the terrain to stamp
    pc.p2[0] = ShadowMap::RainEyeY();
    pc.p2[1] = kZFar - kZNear;   // rain ortho zRange (349)
    pc.p2[2] = ps_r_snow_rough;  // trail/print imperfection
    pc.p2[3] = 0.f;
    // Camera windows (this frame + last) for the toroidal scroll-clear: texels whose world
    // identity flips between the two windows are cleared. On the first frame or a resume
    // from the cache-gate the field is flat, so using this frame's window (no flips) is
    // correct — nothing to preserve.
    const bool freshWindow = s_first || s_skipped || !s_haveCam;
    pc.p3[0] = camX - kHalf;                              pc.p3[1] = camZ - kHalf;
    pc.p3[2] = freshWindow ? (camX - kHalf) : (s_prevCamX - kHalf);
    pc.p3[3] = freshWindow ? (camZ - kHalf) : (s_prevCamZ - kHalf);
    s_skipped = false;

    const u32 dynOffset = slot * (u32)kStampStride;
    const u32 groups = (kSize + 7u) / 8u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeLayout, 0, 1, &s_set, 1, &dynOffset);
    vkCmdPushConstants(cmd, s_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, groups, groups, 1);

    // Back to SHADER_READ_ONLY for the consumers.
    VK::ImageBarrier(cmd, s_state.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                     kConsumers, VK_ACCESS_2_SHADER_READ_BIT);

    s_prevCamX = camX; s_prevCamZ = camZ; s_haveCam = true;
    s_first  = false;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_pipe)       { vkDestroyPipeline(VulkanHW.m_Device, s_pipe, nullptr); s_pipe = VK_NULL_HANDLE; }
    if (s_pipeLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_pipeLayout, nullptr); s_pipeLayout = VK_NULL_HANDLE; }
    if (s_pool)       { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setLayout)  { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    if (s_sampler)    { vkDestroySampler(VulkanHW.m_Device, s_sampler, nullptr); s_sampler = VK_NULL_HANDLE; }
    if (s_stampBuf)   { VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, s_stampBuf, s_stampAlloc); s_stampBuf = VK_NULL_HANDLE; s_stampMapped = nullptr; }
    destroyBuf(s_state);
    s_set = VK_NULL_HANDLE;
    s_inited = false; s_failed = false; s_first = true;
}

}}  // namespace VK::Deform
