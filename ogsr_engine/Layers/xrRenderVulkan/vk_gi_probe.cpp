// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — GI pixel debugger (r_gi_probe). See vk_gi_probe.h.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_gi_probe.h"
#include "vk_descriptors.h"
#include "vk_compute_util.h"
#include "vk_buffer.h"
#include "vk_scene_color.h"
#include "vk_pass_gather.h"
#include "vk_pass_ssao.h"      // ProjTerms / DeriveProjTerms / GetNormalView
#include "vk_ddgi.h"
#include "vk_wrc.h"
#include "vk_swapchain.h"
#include "vk_exposure.h"
#include "vk_pass_bloom.h"     // post-chain taps: the bloom RT the composite adds
#include "vk_dlss.h"           // ...the resolved base colour when DLSS upscales
#include "vk_fsr.h"            // ...or FSR
#include "vk_motionvec.h"      // the upscaler gate rides motion vectors (same test as the tonemap)
#include "vk_volumetrics.h"    // ...and the integrated froxel fog
#include "HW_Vulkan.h"
#include "../../xr_3da/device.h"
#include <cstring>

extern int   ps_r_gi_probe;
extern float ps_r_rt_albedo;
extern float ps_r_blm;         // bloom add strength — the printout scales the raw tap by it
extern int   ps_r_vol;
extern int   ps_r_vol_debug;

namespace VK { namespace GIProbe {

namespace {
    constexpr u32 kRing = 4;   // >= frames in flight, same lag rule as every readback here

    // Mirror of the GIProbeOut block in shaders/gi_probe_dump.comp.glsl.
    struct Dump {
        float P[4], N[4], field[4], gather[4], scene[4], misc[4];
        float probeW[8][4], probeI[8][4], probeP[8][4];
        // The WRC cell under the crosshair — appended at the tail, offsets above unchanged.
        float wrc[4];    // rgb = cell radiance, w = cache weight (0 = no shaded cell)
        float wrc2[4];   // x = LOD level (-1 = cache off), y = cell size (m), z = frames since shade (-1 = never), w = r_wrc_field_clamp
        // The post chain (28-08): what stands between sceneHDR and the display.
        float post1[4];  // rgb = resolved base colour (upscaler output when live), w = resolved-live echo
        float post2[4];  // rgb = raw bloom tap (pre-intensity), w = fog transmittance (1 = clear/off)
        float post3[4];  // rgb = fog in-scatter, w = froxel slice sampled
    };
    static_assert(sizeof(Dump) == 35 * 16, "must match the SSBO block in gi_probe_dump.comp");

    struct Push {
        float camDir[4], camRightT[4], camTopT[4], zp[4], camPos[4], res[4];
        float post[4];   // x = resolved live, y = fog nearZ (0 = off), z = farZ, w = log2(far/near)
    };
    static_assert(sizeof(Push) == 112, "must match the PC block in gi_probe_dump.comp");

    VkPipeline            s_pipe      = VK_NULL_HANDLE;
    VkPipelineLayout      s_layout    = VK_NULL_HANDLE;
    VkDescriptorSetLayout s_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool      s_pool      = VK_NULL_HANDLE;
    VkDescriptorSet       s_sets[kRing] = {};
    VkSampler             s_sampler   = VK_NULL_HANDLE;
    CVulkanBuffer         s_buf;
    u8*                   s_mapped    = nullptr;
    u32                   s_frame     = 0;
    bool                  s_failed    = false;

    float lum(const float* rgb) { return rgb[0] * 0.2126f + rgb[1] * 0.7152f + rgb[2] * 0.0722f; }

    // ---- TEMPORAL NOISE METER (28-08). The block below prints once a second,
    // but flicker lives BETWEEN the prints: a cell breathing 0.10<->0.27, a
    // ceiling vanishing for three frames, an upscaler ghost — all invisible in
    // an instantaneous snapshot and all described by an eye as "какой-то шум".
    // The dispatch already runs EVERY frame; only the readback was throttled.
    // Accumulate per-frame luminances per channel and print mean±sd [min..max]
    // — plus the two counters that name the classic causes outright: frames
    // with NO shaded cell under the crosshair (the ceiling's hole) and frames
    // where the scene jumped >25% (bimodal flips, not gaussian grain).
    struct Chan {
        float sum = 0.f, sum2 = 0.f, mn = 1e9f, mx = -1e9f; int n = 0;
        void  add(float v) { sum += v; sum2 += v * v; mn = _min(mn, v); mx = _max(mx, v); ++n; }
        float mean() const { return n ? sum / float(n) : 0.f; }
        float sd()   const { if (n < 2) return 0.f; const float m = mean(); const float v = sum2 / float(n) - m * m; return (v > 0.f) ? sqrtf(v) : 0.f; }
    };
    struct Noise {
        Chan scene, gather, field, cell, bloom, resolved, vol;
        int  cellMiss = 0, sky = 0, frames = 0, flips = 0;
        float lastScene = -1.f;
        void reset() { *this = Noise{}; }
    } s_noise;

    bool EnsureInit()
    {
        if (s_pipe) return true;
        if (s_failed) return false;

        constexpr auto kTex = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        // 8..11 = the WRC (consts + cells + list + ctrl), read through wrcPeek only.
        // 12..14 = the post chain (bloom RT, resolved base colour, fog volume).
        if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kTex, kTex, kTex, kTex, kTex, kTex,
                                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      kTex, kTex, kTex },
                                    kRing, s_setLayout, s_pool, s_sets,
                                    VK_SHADER_STAGE_COMPUTE_BIT, "GIProbe")) { s_failed = true; return false; }

        s_layout = MakePipelineLayout({ s_setLayout }, sizeof(Push));
        if (s_layout == VK_NULL_HANDLE) { s_failed = true; return false; }
        s_pipe = CreateComputePipeline("gi_probe_dump.comp.spv", s_layout, "GIProbe");
        if (s_pipe == VK_NULL_HANDLE) { s_failed = true; return false; }

        // NEAREST everywhere this pass samples with its own sampler: it reads one
        // texel per image and interpolation would only blur the evidence. The two
        // DDGI atlases are bound with DDGI::GetSampler() instead — the octahedral
        // border texels only do their job under LINEAR.
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_sampler) != VK_SUCCESS) { s_failed = true; return false; }

        s_buf.Create(kRing * sizeof(Dump), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VMA_MEMORY_USAGE_AUTO_PREFER_HOST, false);
        if (!s_buf.IsValid()) { s_failed = true; return false; }
        s_mapped = static_cast<u8*>(s_buf.Map());
        if (s_mapped) memset(s_mapped, 0, kRing * sizeof(Dump));

        Msg("[VK GIprobe] init OK — aim at a surface, the [VK GIprobe] block prints ~once a second");
        return true;
    }

    // The post chain in one line: what stands between sceneHDR and the display.
    // A pixel dark in the scene but white on screen is decomposed here — the
    // resolved base (upscaler history included), the bloom add and the fog.
    void PrintPost(const Dump& d)
    {
        char vol[64];
        if (d.post2[3] >= 0.999f && lum(d.post3) <= 0.f)
            xr_sprintf(vol, "vol off/clear");
        else
            xr_sprintf(vol, "vol in +%.3f tr %.2f", lum(d.post3), d.post2[3]);
        char hemi[64];
        if (d.post3[3] >= 0.f) xr_sprintf(hemi, "bakedHemi %.3f", d.post3[3]);
        else                   xr_sprintf(hemi, "bakedHemi: none (raw tag %.3f)", -d.post3[3] - 1.f);
        Msg("[VK GIprobe] post: resolved lum %.3f%s | bloom tap %.3f x r_blm %.2f -> +%.3f | %s | %s",
            lum(d.post1), (d.post1[3] > 0.5f) ? " (UPSCALER)" : " (=scene)",
            lum(d.post2), ps_r_blm, lum(d.post2) * ps_r_blm, vol, hemi);
    }

    void Print(const Dump& d)
    {
        if (d.misc[3] >= 0.5f && d.misc[3] < 1.5f) {
            Msg("[VK GIprobe] sky under the crosshair — the GI part is moot, the post chain is not:");
            PrintPost(d);
            return;
        }

        Msg("[VK GIprobe] P(%.1f %.1f %.1f) d=%.1fm N(%.2f %.2f %.2f) | cascade %d fade %.2f | volumeFade %.2f",
            d.P[0], d.P[1], d.P[2], d.P[3], d.N[0], d.N[1], d.N[2],
            int(d.misc[0]), d.misc[1], d.gather[3]);
        // field[3] = best Chebyshev among the 8; misc[2] = the capture loop's raw
        // sumIrr/sumW luminance BEFORE the confidence gate and the cascade
        // cross-fade — field-vs-raw is the gate's bite at this pixel (plus the
        // cross-fade when fade < 1).
        Msg("[VK GIprobe] field lum %.3f (%.2f %.2f %.2f) | raw-normalised lum %.3f | bestCheb %.3f (conf thr %.2f)",
            lum(d.field), d.field[0], d.field[1], d.field[2], d.misc[2], d.field[3], DDGI::kDDGIConf);
        Msg("[VK GIprobe] gather lum %.3f%s | sceneHDR lum %.3f (%.2f %.2f %.2f)",
            lum(d.gather), (d.scene[3] > 0.5f) ? "" : " (GATHER DEAD)",
            lum(d.scene), d.scene[0], d.scene[1], d.scene[2]);
        // The WRC cell at this very patch — what a gather ray landing here reads,
        // i.e. where r_wrc_field_clamp shows its work. field-vs-cell is the bite.
        if (d.wrc2[0] < -0.5f)
            Msg("[VK GIprobe] WRC: cache off");
        else if (d.wrc[3] <= 0.f)
            Msg("[VK GIprobe] WRC cell: no shaded cell yet (level %d, %.2f m) | field clamp k=%.2f",
                int(d.wrc2[0]), d.wrc2[1], d.wrc2[3]);
        else
            Msg("[VK GIprobe] WRC cell: lum %.3f (%.2f %.2f %.2f) w %.2f | level %d size %.2f m | shaded %d frames ago | field clamp k=%.2f",
                lum(d.wrc), d.wrc[0], d.wrc[1], d.wrc[2], d.wrc[3],
                int(d.wrc2[0]), d.wrc2[1], int(d.wrc2[2]), d.wrc2[3]);
        for (int i = 0; i < 8; ++i) {
            const float fl = d.probeP[i][3];
            if (fl >= 0.5f && fl < 1.5f) { Msg("[VK GIprobe]  p%d OUTSIDE THE WINDOW", i); continue; }
            Msg("[VK GIprobe]  p%d d=%.1fm tri %.2f wrap %.2f cheb %.3f w %.3f | lum %.3f | (%.1f %.1f %.1f)%s%s%s%s",
                i, d.probeI[i][3], d.probeW[i][0], d.probeW[i][1], d.probeW[i][2], d.probeW[i][3],
                lum(d.probeI[i]), d.probeP[i][0], d.probeP[i][1], d.probeP[i][2],
                (int(fl) & 2) ? " BURIED" : "", (int(fl) & 4) ? " moved" : "",
                (int(fl) & 8) ? " SKIPPED(buried)" : "", (int(fl) & 16) ? " inactive" : "");
        }
        Msg("[VK GIprobe] exposure %.2f | indoor boost %.2f | r_rt_albedo %.2f",
            Exposure::Smoothed(), DDGI::IndoorBoost(), ps_r_rt_albedo);
        PrintPost(d);
    }
}

void Execute(VkCommandBuffer cmd, u32 imageIndex, const Fmatrix& viewProj)
{
    if (ps_r_gi_probe == 0 || s_failed || cmd == VK_NULL_HANDLE) return;
    if (!DDGI::Enabled()) {
        static u32 s_said = 0;
        if ((s_said++ % 300) == 0) Msg("[VK GIprobe] DDGI is not live (r_ddgi off or no bubble) — nothing to dump");
        return;
    }
    const VkImageView normalView = SSAOPass::GetNormalView();
    const VkImageView depthView  = Swapchain.m_DepthView;
    if (normalView == VK_NULL_HANDLE || depthView == VK_NULL_HANDLE) return;
    // wrcPeek needs all four WRC buffers bound; DDGI::Enabled() implies the WRC
    // module ran, but a null here must skip a frame, not write a bad descriptor.
    if (WRC::ConstsBuffer() == VK_NULL_HANDLE || WRC::CellsBuffer() == VK_NULL_HANDLE
        || WRC::ListBuffer() == VK_NULL_HANDLE || WRC::CtrlBuffer() == VK_NULL_HANDLE) return;
    if (!EnsureInit()) return;

    const u32 slot = s_frame % kRing;

    // The slot we are about to reuse retired kRing frames ago — read it EVERY
    // frame now: the snapshot prints once a second, the noise meter eats all 60.
    // Same lag-as-fence pattern as the exposure readback and the reloc audit.
    if (s_mapped && s_frame >= kRing) {
        Dump d;
        memcpy(&d, s_mapped + size_t(slot) * sizeof(Dump), sizeof(Dump));
        ++s_noise.frames;
        if (d.misc[3] >= 0.5f && d.misc[3] < 1.5f) {
            ++s_noise.sky;
        } else {
            const float sl = lum(d.scene);
            s_noise.scene.add(sl);
            s_noise.gather.add(lum(d.gather));
            s_noise.field.add(lum(d.field));
            if (d.wrc[3] > 0.f) s_noise.cell.add(lum(d.wrc)); else ++s_noise.cellMiss;
            s_noise.bloom.add(lum(d.post2) * ps_r_blm);
            s_noise.resolved.add(lum(d.post1));
            s_noise.vol.add(lum(d.post3));
            if (s_noise.lastScene >= 0.f && fabsf(sl - s_noise.lastScene) > 0.25f * _max(s_noise.lastScene, 0.02f))
                ++s_noise.flips;
            s_noise.lastScene = sl;
        }
        if ((s_frame % 60) == 0) {
            Print(d);
            const Noise& z = s_noise;
            Msg("[VK GIprobe] noise/%df: scene %.3f\xB1%.3f [%.3f..%.3f] flips %d | gather %.3f\xB1%.3f | field %.3f\xB1%.3f | cell %.3f\xB1%.3f MISS %d | bloom +%.3f\xB1%.3f | resolved %.3f\xB1%.3f | vol +%.3f\xB1%.3f%s",
                z.frames, z.scene.mean(), z.scene.sd(), (z.scene.n ? z.scene.mn : 0.f), (z.scene.n ? z.scene.mx : 0.f), z.flips,
                z.gather.mean(), z.gather.sd(), z.field.mean(), z.field.sd(),
                z.cell.mean(), z.cell.sd(), z.cellMiss,
                z.bloom.mean(), z.bloom.sd(), z.resolved.mean(), z.resolved.sd(),
                z.vol.mean(), z.vol.sd(), z.sky ? " (some sky frames)" : "");
            s_noise.reset();
        }
    }

    const VkImageView gatherView = GatherPass::ResultView();
    const bool gatherLive = (gatherView != VK_NULL_HANDLE);

    // The post chain, gated EXACTLY like the tonemap composite gates it — the
    // instrument must read the same images the pixel is built from, or its
    // decomposition is a story about a different frame.
    const bool dlssUp = VK::Dlss::Enabled() && VK::MotionVec::Enabled() && VK::Dlss::FeatureReady()
                     && VK::Dlss::ResolvedThisFrame();
    const bool fsrUp  = !dlssUp && VK::Fsr::Enabled() && VK::MotionVec::Enabled() && VK::Fsr::OutputReady()
                     && VK::Fsr::ResolvedThisFrame();
    const VkImageView upView   = dlssUp ? VK::Dlss::OutputView() : fsrUp ? VK::Fsr::OutputView() : VK_NULL_HANDLE;
    const bool resolvedUp      = (dlssUp || fsrUp) && upView != VK_NULL_HANDLE;
    const VkImageView bloomView = BloomPass::GetResultView();
    const bool volOn = (ps_r_vol != 0 || ps_r_vol_debug != 0) && Vol::Ready();
    const Vol::GridZParams gz = Vol::GetGridZ();
    // Binding 14 is declared sampler3D — only the (eagerly created) fog volume
    // may stand there, never a 2D stand-in. Without it the whole dispatch skips.
    const VkImageView volView = Vol::GetIntegratedView();
    if (volView == VK_NULL_HANDLE) return;

    // The set is rewritten in place every frame; kRing > frames-in-flight keeps
    // the one being recorded distinct from any the GPU still reads.
    DescriptorWriter(s_sets[slot])
        .UniformBuffer(0, DDGI::ConstsBuffer())
        .ImageSampler(1, DDGI::IrradianceView(), DDGI::GetSampler(), VK_IMAGE_LAYOUT_GENERAL)
        .ImageSampler(2, DDGI::DistanceView(),   DDGI::GetSampler(), VK_IMAGE_LAYOUT_GENERAL)
        .ImageSampler(3, depthView,  s_sampler)
        .ImageSampler(4, normalView, s_sampler)
        // The gather lives in GENERAL; when it is dead the binding still has to be
        // a VALID sampled image in its ACTUAL layout, so the scene stands in and
        // zp.z tells the shader not to read it.
        .ImageSampler(5, gatherLive ? gatherView : SceneColor::GetSampleView(imageIndex),
                      gatherLive ? GatherPass::GetSampler() : s_sampler,
                      gatherLive ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        .ImageSampler(6, SceneColor::GetSampleView(imageIndex), s_sampler)
        .StorageBuffer(7, s_buf.GetHandle(), sizeof(Dump), VkDeviceSize(slot) * sizeof(Dump))
        // The WRC, for wrcPeek: always valid buffers — with the cache off they are
        // the idle stubs and the peek returns on its first line (weight 0).
        .UniformBuffer(8, WRC::ConstsBuffer())
        .StorageBuffer(9, WRC::CellsBuffer())
        .StorageBuffer(10, WRC::ListBuffer())
        .StorageBuffer(11, WRC::CtrlBuffer())
        // The post chain — same fallbacks as the tonemap's own bindings: the
        // scene stands in wherever a stage is off (and is then never trusted:
        // the flags in the push say what is real).
        .ImageSampler(12, bloomView != VK_NULL_HANDLE ? bloomView : SceneColor::GetSampleView(imageIndex), s_sampler)
        .ImageSampler(13, resolvedUp ? upView : SceneColor::GetSampleView(imageIndex), s_sampler)
        .ImageSampler(14, volView, Vol::GetSampler())
        .Flush();

    const ProjTerms pt = DeriveProjTerms(viewProj);
    Push p{};
    p.camDir[0] = pt.dir.x;   p.camDir[1] = pt.dir.y;   p.camDir[2] = pt.dir.z;
    p.camRightT[0] = pt.right.x * pt.tanX; p.camRightT[1] = pt.right.y * pt.tanX; p.camRightT[2] = pt.right.z * pt.tanX;
    p.camTopT[0]   = pt.top.x * pt.tanY;   p.camTopT[1]   = pt.top.y * pt.tanY;   p.camTopT[2]   = pt.top.z * pt.tanY;
    p.zp[0] = pt.p33; p.zp[1] = pt.p43; p.zp[2] = gatherLive ? 1.f : 0.f; p.zp[3] = 0.5f;   // crosshair u
    p.camPos[0] = RV().vCameraPosition.x;
    p.camPos[1] = RV().vCameraPosition.y;
    p.camPos[2] = RV().vCameraPosition.z;
    p.camPos[3] = 0.5f;                                                                     // crosshair v
    const VkExtent2D de = SceneColor::Extent();   // depth is render-res, same as the scene target
    p.res[2] = (de.width  > 0) ? 1.f / float(de.width)  : 0.f;
    p.res[3] = (de.height > 0) ? 1.f / float(de.height) : 0.f;
    p.post[0] = resolvedUp ? 1.f : 0.f;
    p.post[1] = volOn ? gz.nearZ : 0.f;   // 0 = fog off, the shader skips the volume tap
    p.post[2] = gz.farZ;
    p.post[3] = gz.logFarNear;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_layout, 0, 1, &s_sets[slot], 0, nullptr);
    vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, 1, 1, 1);

    ++s_frame;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_pipe)      { vkDestroyPipeline(VulkanHW.m_Device, s_pipe, nullptr); s_pipe = VK_NULL_HANDLE; }
    if (s_layout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, s_layout, nullptr); s_layout = VK_NULL_HANDLE; }
    if (s_sampler)   { vkDestroySampler(VulkanHW.m_Device, s_sampler, nullptr); s_sampler = VK_NULL_HANDLE; }
    if (s_pool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    s_buf.Destroy(); s_mapped = nullptr; s_frame = 0; s_failed = false;
}

}}  // namespace VK::GIProbe
