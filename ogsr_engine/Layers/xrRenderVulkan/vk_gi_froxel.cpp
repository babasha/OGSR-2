// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// The GI field in froxel form — see vk_gi_froxel.h for what it is and why. This
// file is the plumbing: two volumes, one buffer of per-probe SH, two compute
// dispatches, and a self-audit that answers the only question that matters —
// whether the field says anything the flat constant it replaces did not.

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_gi_froxel.h"
#include "vk_ddgi.h"            // the lattice this resamples (atlases + constants)
#include "vk_buffer.h"
#include "vk_command_buffer.h"  // FRAMES_IN_FLIGHT
#include "vk_shaders.h"
#include "vk_image.h"
#include "vk_compute_util.h"
#include "vk_descriptors.h"
#include "vk_barriers.h"
#include "vk_profiler.h"
#include "../../xr_3da/device.h"

extern float ps_r_gi_froxel;       // master weight: 0 = off, 1 = the field owns the ambient
extern float ps_r_gi_froxel_dir;   // how much of the band-1 (directional) term a medium sees
extern float ps_r_gi_froxel_gain;  // its scale against the analytic fill it displaces
extern int   ps_r_gi_froxel_stats; // keep the self-audit line running past its window

namespace VK { namespace GiFroxel {

namespace {

constexpr u32      kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;
constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// The field's own grid — HALF the fog grid's XY and Z, i.e. the resolution the
// smoke media grid already settled on (vk_volumetrics kSmoke*). Both are sampled
// at the same normalised frustum coordinates, so the mismatch never shows; see the
// header comment in gi_froxel.comp.glsl for why coarser is not a compromise here.
constexpr u32 kGiX = 128, kGiY = 72, kGiZ = 64;

// Audit counters: [0] sum of field luminance x64 (fixed point), [1] froxels the
// field reaches, [2] froxels visited. [3] is spare and kept so the buffer, the
// clear and the readback all speak in whole vec4s.
constexpr u32 kStatWords = 4;

bool s_inited = false, s_failed = false, s_ready = false;
// The frame the field was last BUILT. Not a bool: the pass is skipped under async
// compute (see the header), and a stale volume built for a previous camera would
// otherwise keep answering as if it were current — light in the air lagging the
// player by however many frames r_async stayed on.
u32  s_resultFrame = 0xFFFFFFFFu;
bool s_everRan     = false;
u32  s_generation = 0;

VkImage       s_l0Img = VK_NULL_HANDLE, s_l1Img = VK_NULL_HANDLE;
VmaAllocation s_l0Alloc = VK_NULL_HANDLE, s_l1Alloc = VK_NULL_HANDLE;
VkImageView   s_l0View = VK_NULL_HANDLE, s_l1View = VK_NULL_HANDLE;
VkSampler     s_sampler = VK_NULL_HANDLE;

CVulkanBuffer s_sh;                       // per-probe SH (device-local)
u32           s_shProbes = 0;             // what it was sized for
CVulkanBuffer s_stats;                    // device-local counters
CVulkanBuffer s_readback[kFramesInFlight];// host-visible copies, one frame per slot

VkDescriptorSetLayout s_setLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_pool      = VK_NULL_HANDLE;
VkDescriptorSet       s_sets[kFramesInFlight] = {};
VkPipelineLayout      s_layout    = VK_NULL_HANDLE;
VkPipeline            s_probePipe = VK_NULL_HANDLE;
VkPipeline            s_froxPipe  = VK_NULL_HANDLE;

// What each set points at. A DDGI atlas regeneration swaps both views and the
// constants buffer at once, and a set an in-flight command buffer still references
// must not be rewritten under it — so rewrite only on a real change. Same rule as
// the gather / RTAO / DDGI passes.
struct Bound { VkBuffer consts = VK_NULL_HANDLE, sh = VK_NULL_HANDLE;
               VkImageView irr = VK_NULL_HANDLE, dep = VK_NULL_HANDLE; };
Bound s_bound[kFramesInFlight] = {};

// ⚠ Mirrors the PC block in gi_froxel.comp.glsl EXACTLY.
struct FroxelPush {
    float camPosNear[4];
    float camDirLogFN[4];
    float camRightT[4];
    float camTopT[4];
    float dims[4];
};
static_assert(sizeof(FroxelPush) == 80, "must match gi_froxel.comp's PC block");

u32   s_auditUntil = 0;   // frame the self-audit window closes
float s_flatFill   = 0.f; // the consumer's analytic fill, for the verdict line
float s_effGain    = 1.f; // ...and the gain it actually applied to the field (see NoteFlatFill)

// The audit runs itself for the first ~900 frames after the field comes up, then
// the atomics go away. r_gi_froxel_stats 1 keeps it on. The window is opened by
// Execute the first frame the field is actually built, NOT by Init — a number that
// prints before the feature runs measures nothing.
bool StatsWanted()
{
    return ps_r_gi_froxel_stats != 0 || (s_auditUntil != 0 && RV().frameIndex < s_auditUntil);
}

void ReportStats(u32 slot)
{
    if (!StatsWanted()) return;
    void* src = s_readback[slot].Map();
    if (!src) return;
    s_readback[slot].Invalidate();
    u32 v[kStatWords];
    memcpy(v, src, sizeof(v));
    s_readback[slot].Unmap();

    static u32 s_last = 0;
    if (RV().frameIndex - s_last < 120) return;
    s_last = RV().frameIndex;
    if (v[2] == 0) return;

    const double covered = double(v[1]);
    const double mean    = (v[1] > 0) ? (double(v[0]) / 64.0) / covered : 0.0;
    // ⭐ THE VERDICT LINE, and it carries BOTH halves of the question.
    //
    // The RATIO says whether the gain is calibrated: near 1 the field lands where the
    // constant it displaced did, which is what makes the A/B a comparison of SHAPE
    // (bright by the window, dark at the back of the room) rather than of exposure.
    // Far from 1 and every screenshot taken with it is measuring brightness.
    // ⚠ The flat fill printed is the OUTDOOR one, before the per-froxel sky-visibility
    // scalar (ambK) dims it under a roof — so indoors the field is legitimately
    // expected to sit BELOW it, and that gap is the feature, not a miscalibration.
    //
    // The REACH is the other half: a field covering 12% of the frustum cannot change
    // a frame however right its values are (raise r_ddgi_cascades / r_ddgi_spacing),
    // and from the outside that failure is indistinguishable from "GI is off".
    // ⚠ s_effGain, NOT Gain(): the consumer applies its own taming on top of the
    // console knob (the fog folds in r_vol_ambient, without which this read 7.1x),
    // and a ratio computed from the knob alone would keep insisting the field is
    // seven times too bright long after it had been calibrated.
    const double gained = mean * double(s_effGain);
    Msg("[VK GiFroxel] field reaches %.0f%% of the frustum (%u froxels) | mean radiance %.4f "
        "x gain %.3f = %.4f  vs  the flat fill it replaces %.4f  (ratio %.2fx)",
        100.0 * covered / double(v[2]), v[2], mean, double(s_effGain), gained, double(s_flatFill),
        (s_flatFill > 1e-6f) ? gained / double(s_flatFill) : 0.0);
}

bool CreateVolume(VkImage& img, VmaAllocation& alloc, VkImageView& view, const char* name)
{
    VK::ImageDesc d;
    d.type   = VK_IMAGE_TYPE_3D;
    d.format = kFormat;
    d.extent = { kGiX, kGiY, kGiZ };
    d.usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    d.name   = name;
    // The fog inject samples this, and under r_async that inject records onto the
    // compute queue. The field itself is not BUILT there (see the header), but the
    // descriptor is still bound from a compute command buffer, so the image must
    // survive the family switch. No-op when compute aliases graphics.
    d.computeShared = true;
    if (!VK::CreateImage(d, img, alloc)) return false;
    view = VK::CreateImageView(img, kFormat, VK_IMAGE_VIEW_TYPE_3D);
    return view != VK_NULL_HANDLE;
}

// The per-probe SH buffer follows the lattice's shape, which the console can
// change (r_ddgi_dim_xz / _dim_y / _cascades all reallocate the atlases). Resize
// on a real change only, behind a device idle — the same deal vk_ddgi takes for
// the same reason.
bool EnsureProbeBuffer(u32 probes)
{
    if (probes == 0) return false;
    if (s_sh.IsValid() && probes == s_shProbes) return true;
    if (s_sh.IsValid()) VK_DeviceWaitIdleLocked();
    s_sh.Destroy();
    s_sh.Create(VkDeviceSize(probes) * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, /*gpuOnly=*/true, /*computeShared=*/true);
    if (!s_sh.IsValid()) { s_shProbes = 0; return false; }
    s_shProbes = probes;
    for (auto& b : s_bound) b = Bound{};   // the buffer moved; every set must be rewritten
    return true;
}

}   // anon namespace

bool Wanted() { return ps_r_gi_froxel > 0.f; }
bool Ready()  { return s_ready && !s_failed; }
bool Enabled() { return Wanted() && Ready() && DDGI::Enabled(); }

VkImageView L0View()   { return s_l0View; }
VkImageView L1View()   { return s_l1View; }
VkSampler   GetSampler() { return s_sampler; }
u32         Generation() { return s_generation; }

float Weight()
{
    if (s_resultFrame != RV().frameIndex || !Enabled()) return 0.f;
    return _max(0.f, _min(1.f, ps_r_gi_froxel));
}
float DirWeight() { return _max(0.f, _min(1.f, ps_r_gi_froxel_dir)); }
float Gain()      { return _max(0.f, _min(8.f, ps_r_gi_froxel_gain)); }
void  NoteFlatFill(float luminance, float effectiveGain) { s_flatFill = luminance; s_effGain = effectiveGain; }

void OnLevelUnload()
{
    // The probes describe a world that is going away, and so does everything this
    // resampled out of them. Consumers fall back to their analytic fill until a
    // frame in the NEW world has filled it again.
    s_resultFrame = 0xFFFFFFFFu;
    // The audit reopens on the next level: its two numbers are properties of the
    // MAP (how much of the frustum the lattice reaches, how bright its air is), so
    // one level's answer says nothing about the next one's.
    s_auditUntil  = 0;
    s_everRan     = false;
}

bool Init()
{
    if (s_inited) return s_ready;
    s_inited = true;

    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();

    // Eager, like the fog volumes: a consumer descriptor written at ITS init must
    // point at something valid whatever r_gi_froxel says at the time.
    if (!CreateVolume(s_l0Img, s_l0Alloc, s_l0View, "GiFroxel.L0") ||
        !CreateVolume(s_l1Img, s_l1Alloc, s_l1View, "GiFroxel.L1")) {
        Msg("![VK GiFroxel] volume create failed — froxel GI off");
        s_failed = true; return false;
    }

    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.f;
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_sampler) != VK_SUCCESS) {
        Msg("![VK GiFroxel] sampler create failed"); s_failed = true; return false;
    }

    s_stats.Create(kStatWords * sizeof(u32),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, /*gpuOnly=*/true, /*computeShared=*/true);
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        // hostRead: this is a readback target, and a write-combined mapping would
        // make the CPU read of it crawl (see the flag's note in vk_buffer.h).
        s_readback[i].Create(kStatWords * sizeof(u32), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST, /*gpuOnly=*/false,
                             /*computeShared=*/false, /*hostRead=*/true);
        // A readback slot is READ BEFORE IT IS FIRST WRITTEN (the first frames come
        // back before any copy has landed). Zero it or the first audit line reports
        // whatever this mapping held before.
        if (void* z = s_readback[i].Map()) { memset(z, 0, kStatWords * sizeof(u32)); s_readback[i].Flush(); s_readback[i].Unmap(); }
    }
    if (!s_stats.IsValid()) { Msg("![VK GiFroxel] stats buffer failed"); s_failed = true; return false; }

    // 0 DDGI constants, 1 irradiance atlas, 2 distance atlas, 3 per-probe SH,
    // 4/5 the two output volumes, 6 the audit counters. ONE layout for BOTH
    // dispatches — gi_probe_sh declares only 0/1/3 out of it (see the note there).
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
                                kFramesInFlight, s_setLayout, s_pool, s_sets,
                                VK_SHADER_STAGE_COMPUTE_BIT, "GiFroxel")) {
        s_failed = true; return false;
    }

    s_layout = VK::MakePipelineLayout({ s_setLayout }, sizeof(FroxelPush));
    if (!s_layout) { s_failed = true; return false; }
    s_probePipe = VK::CreateComputePipeline("gi_probe_sh.comp.spv", s_layout, "GiProbeSH");
    s_froxPipe  = VK::CreateComputePipeline("gi_froxel.comp.spv",   s_layout, "GiFroxel");
    if (!s_probePipe || !s_froxPipe) {
        Msg("![VK GiFroxel] compute pipeline create failed — froxel GI off");
        s_failed = true; return false;
    }

    // One-time UNDEFINED -> SHADER_READ so a consumer's descriptor is valid before
    // the first Execute (and stays valid with the feature off — never sampled, the
    // weight is zero).
    if (VkCommandBuffer cmd = VulkanHW.BeginSingleTimeCommands()) {
        VK::ImageBarrier(cmd, s_l0Img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        VK::ImageBarrier(cmd, s_l1Img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        VulkanHW.EndSingleTimeCommands(cmd);
    }

    s_ready = true;
    ++s_generation;
    Msg("[VK GiFroxel] init OK — %ux%ux%u froxels, 2-band SH in 2x RGBA16F = %.1f MB",
        kGiX, kGiY, kGiZ, double(kGiX * kGiY * kGiZ * 8 * 2) / (1024.0 * 1024.0));
    return true;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_l0View) { vkDestroyImageView(VulkanHW.m_Device, s_l0View, nullptr); s_l0View = VK_NULL_HANDLE; }
    if (s_l1View) { vkDestroyImageView(VulkanHW.m_Device, s_l1View, nullptr); s_l1View = VK_NULL_HANDLE; }
    if (s_l0Img)  { vmaDestroyImage(VulkanHW.m_Allocator, s_l0Img, s_l0Alloc); s_l0Img = VK_NULL_HANDLE; s_l0Alloc = VK_NULL_HANDLE; }
    if (s_l1Img)  { vmaDestroyImage(VulkanHW.m_Allocator, s_l1Img, s_l1Alloc); s_l1Img = VK_NULL_HANDLE; s_l1Alloc = VK_NULL_HANDLE; }
    if (s_sampler) { vkDestroySampler(VulkanHW.m_Device, s_sampler, nullptr); s_sampler = VK_NULL_HANDLE; }
    if (s_probePipe) { vkDestroyPipeline(VulkanHW.m_Device, s_probePipe, nullptr); s_probePipe = VK_NULL_HANDLE; }
    if (s_froxPipe)  { vkDestroyPipeline(VulkanHW.m_Device, s_froxPipe, nullptr);  s_froxPipe = VK_NULL_HANDLE; }
    if (s_layout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, s_layout, nullptr); s_layout = VK_NULL_HANDLE; }
    if (s_pool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    for (auto& s : s_sets) s = VK_NULL_HANDLE;
    for (auto& b : s_bound) b = Bound{};
    s_sh.Destroy(); s_shProbes = 0;
    s_stats.Destroy();
    for (auto& r : s_readback) r.Destroy();
    s_inited = false; s_failed = false; s_ready = false; s_resultFrame = 0xFFFFFFFFu; s_everRan = false;
}

void Execute(VkCommandBuffer cmd, const ProjTerms& pt, float nearZ, float logFN, u32 slot)
{
    if (!Enabled()) return;
    if (slot >= kFramesInFlight) slot = 0;

    const VkBuffer    consts = DDGI::ConstsBuffer();
    const VkImageView irrV   = DDGI::IrradianceView();
    const VkImageView depV   = DDGI::DistanceView();
    const VkSampler   samp   = DDGI::GetSampler();
    const u32         probes = DDGI::ProbeCount();
    if (!consts || !irrV || !depV || !samp || !probes) {
        // Naming the missing piece once beats a pass that quietly does nothing —
        // "GI in the fog does not work" has exactly this shape from the outside.
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            Msg("![VK GiFroxel] idle: ddgi consts=%s atlases=%s probes=%u",
                consts ? "ok" : "MISSING", (irrV && depV) ? "ok" : "MISSING", probes);
        }
        return;
    }
    if (!EnsureProbeBuffer(probes)) return;

    if (s_auditUntil == 0 && !s_everRan) {
        s_auditUntil = RV().frameIndex + 900;
        Msg("[VK GiFroxel] self-audit for ~900 frames: how much of the frustum the field reaches "
            "and how bright it says the air is (r_gi_froxel_stats 1 to keep it on)");
    }

    const int zone = Prof::ZoneBegin(cmd, "GiFroxel");

    // Last frame's counters on their way out, read kFramesInFlight frames after they
    // were copied — which the frame fence has already made safe.
    const bool stats = StatsWanted();
    if (stats) ReportStats(slot);

    {
        Bound want{ consts, s_sh.GetHandle(), irrV, depV };
        if (memcmp(&want, &s_bound[slot], sizeof(Bound)) != 0) {
            VK::DescriptorWriter(s_sets[slot])
                .UniformBuffer(0, consts)
                .ImageSampler(1, irrV, samp, VK_IMAGE_LAYOUT_GENERAL)   // the atlases live in GENERAL
                .ImageSampler(2, depV, samp, VK_IMAGE_LAYOUT_GENERAL)
                .StorageBuffer(3, s_sh.GetHandle())
                .StorageImage(4, s_l0View)
                .StorageImage(5, s_l1View)
                .StorageBuffer(6, s_stats.GetHandle())
                .Flush();
            s_bound[slot] = want;
        }
    }

    if (stats) {
        // The counters are ONE buffer, not one per slot, so this frame's zeroing races
        // the PREVIOUS frame's readback copy unless they are ordered — a write-after-
        // read across frames that submission order alone does not express.
        VK::BufferBarrier(cmd, s_stats.GetHandle(),
                          VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(cmd, s_stats.GetHandle(), 0, kStatWords * sizeof(u32), 0);
        VK::BufferBarrier(cmd, s_stats.GetHandle(),
                          VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_layout, 0, 1, &s_sets[slot], 0, nullptr);

    // ---- 1. every probe's octahedral map -> two-band SH.
    // The atlases were written by DDGI::Update earlier in THIS command buffer, as
    // storage images; that write has to be visible to these samples.
    VK::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_probePipe);
    vkCmdDispatch(cmd, (probes + 63) / 64, 1, 1);

    // ---- 2. the froxel gather. Reads what the dispatch above just wrote.
    VK::BufferBarrier(cmd, s_sh.GetHandle(),
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    // The volumes: last frame's sampled reads (fog inject on compute, particles on
    // fragment) have to finish before this frame overwrites them — a WAR the layout
    // change alone would not express.
    VK::ImageBarrier(cmd, s_l0Img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    VK::ImageBarrier(cmd, s_l1Img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    FroxelPush push{};
    const Fvector& eye = RV().vCameraPosition;
    push.camPosNear[0] = eye.x; push.camPosNear[1] = eye.y; push.camPosNear[2] = eye.z;
    push.camPosNear[3] = nearZ;
    push.camDirLogFN[0] = pt.dir.x; push.camDirLogFN[1] = pt.dir.y; push.camDirLogFN[2] = pt.dir.z;
    push.camDirLogFN[3] = logFN;
    push.camRightT[0] = pt.right.x * pt.tanX; push.camRightT[1] = pt.right.y * pt.tanX; push.camRightT[2] = pt.right.z * pt.tanX;
    push.camTopT[0]   = pt.top.x   * pt.tanY; push.camTopT[1]   = pt.top.y   * pt.tanY; push.camTopT[2]   = pt.top.z   * pt.tanY;
    push.dims[0] = float(kGiX); push.dims[1] = float(kGiY); push.dims[2] = float(kGiZ);
    push.dims[3] = stats ? 1.f : 0.f;
    vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_froxPipe);
    vkCmdDispatch(cmd, (kGiX + 3) / 4, (kGiY + 3) / 4, (kGiZ + 3) / 4);

    // Hand both volumes to their consumers: the fog inject (compute, later in this
    // same buffer) and the particle pass (fragment, in the scene segment).
    VK::ImageBarrier(cmd, s_l0Img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VK::ImageBarrier(cmd, s_l1Img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    if (stats) {
        // ⚠ THE SOURCE SCOPE HAS TO NAME THE FILL, NOT JUST THE DISPATCH. Sync
        // validation caught this on the first run: the copy below reads bytes written
        // by BOTH the atomics AND the vkCmdFillBuffer that zeroed them, and the
        // earlier fill->compute barrier made that fill visible to COMPUTE only. A
        // barrier whose src is compute/storage-write leaves the transfer write
        // unordered against this transfer read — READ_AFTER_WRITE, every frame the
        // audit is open.
        //
        // ⚠⚠ AND `COPY_BIT` IS NOT THE FILL'S STAGE. Naming COPY here was the second
        // wrong answer, and it looked right: the fix built, deployed, and the hazard
        // came back word for word. Under synchronization2 vkCmdFillBuffer executes in
        // CLEAR, not COPY — COPY covers vkCmdCopy* alone — so a barrier sourced on
        // COPY simply does not see the fill. ALL_TRANSFER is the honest scope for
        // "whatever transfer command wrote this", and it is what every one of these
        // three barriers uses.
        VK::BufferBarrier(cmd, s_stats.GetHandle(),
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy region{ 0, 0, kStatWords * sizeof(u32) };
        vkCmdCopyBuffer(cmd, s_stats.GetHandle(), s_readback[slot].GetHandle(), 1, &region);
    }

    Prof::ZoneEnd(cmd, zone);
    s_resultFrame = RV().frameIndex;
    s_everRan     = true;
}

}}  // namespace VK::GiFroxel
