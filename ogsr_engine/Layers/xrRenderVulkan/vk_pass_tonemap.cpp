// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — tonemap / exposure composite pass. See vk_pass_tonemap.h.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_descriptors.h"       // VK::DescriptorWriter
#include "vk_rendering.h"     // VK::RenderingBuilder
#include "vk_pass_tonemap.h"

// Editor viewport state (CRender_Vulkan.cpp) — 0 objects means an EMPTY viewport, the only
// case where the metered auto-exposure has nothing to average. Declared at file scope on
// purpose: inside `namespace VK` below it would bind to a non-existent VK::VKEditor.
namespace VKEditor { int HostModelCount(); }
#include "vk_scene_color.h"
#include "vk_pass_bloom.h"         // BloomPass — bright-pass + blur before the composite
#include "vk_pass_ssao.h"          // SSAOPass — GTAO+IL: IL is MRT target 1 (binding 5) + prev-colour capture
#include "vk_pass_particles.h"     // ParticlePass::GetDistortView — heat-haze offsets (binding 2)
#include "vk_swapchain.h"          // Swapchain.m_Images / m_ImageViews / m_Format (+ depth for SSR puddles)
#include "vk_shaders.h"            // g_ShaderManager
#include "vk_pipeline_cache.h"     // PipelineCache::GetCacheObject
#include "vk_barriers.h"           // ImageBarrier
#include "vk_framegraph.h"         // g_FrameGraph — keep the swapchain layout entry honest
#include "vk_profiler.h"           // VK::Prof::NameSet — TEMP VUID-hunt instrumentation
#include "vk_env_light.h"          // EnvLight set (set 1) — rain map/VP + camera terms for SSR puddles
#include "vk_vsm.h"                // VSM::MaskReady — dyn-shadow red debug overlay (set 1 binding 14)
#include "vk_volumetrics.h"        // VK::Vol — integrated froxel volume (binding 4) + exp-Z params
#include "vk_dlss.h"               // VK::Dlss — resolved base colour (binding 6) when upscaling
#include "vk_fsr.h"                 // VK::Fsr - the other upscaler that can own binding 6
#include "vk_gfx_pipeline.h"         // VK::PipeStats — the lean pipelines report registers (r_pipe_stats)
#include "vk_motionvec.h"          // VK::MotionVec::Enabled — DLSS gate
#include "vk_exposure.h"           // VK::Exposure — shared auto-exposure constants (also used by bloom)
#include "vk_gi_probe.h"           // VK::GIProbe — the GI pixel debugger rides this pass (r_gi_probe)
#include "vk_fullscreen.h"         // VK::Fullscreen — shared fullscreen pipeline
#include "vk_image.h"              // CreateImageView — the r_tonemap_probe 2 target
#include "vk_buffer.h"             // CVulkanBuffer — 1×1 avg-luminance readback (temporal exposure)
#include "HW_Vulkan.h"
#include "../xrRender/xrRender_console.h"  // ps_r2_img_* — R4 color-grading console knobs
#include "../../xr_3da/device.h"   // RV().fTimeDelta — exposure adaptation rate; vCameraPosition — the enclosure rays
#include "../../xrCDB/xrCDB.h"          // CDB::COLLIDER — r_expo_indoor: rays up from the camera against the level cform
#include "../../xr_3da/IGame_Level.h"   // g_pGameLevel->ObjectSpace.GetStaticModel()
#include <cmath>                    // expf, ldexp
#include <limits>                   // numeric_limits — UFloatToFloat inf/NaN
#include <cstring>                  // memcpy (half→float decode)
#include <algorithm>               // std::min/max

// r_vol composite knobs (global scope — C-linkage console symbols).
extern int ps_r_vol;
extern int ps_r_vol_debug;
extern float ps_r_blm;   // bloom add strength (0 = off) — kBloomIntensity is its default
#include "vk_color_space.h"     // ColorSpace::Active() — latched linear-pipeline selector (OETF gate)
// SSIL composite knobs (global scope — block-scope extern inside the namespace
// would mangle as VK::* → LNK2001).
extern int   ps_r_ssil_enable;
extern int   ps_r_ssil_debug;
extern float ps_r_ssil_strength;
extern float ps_r_dither;   // final 8-bit output dither amplitude (LSBs; 0 = off)
#ifdef __SWITCH__
extern "C" int g_xr_nx_diag;   // nfs28 device.cpp: 110 scissor 1x1, 111 bare resample, 112 no mip chain (113 = crop, CRender)
#endif
extern int ps_r_tonemap_lean;   // r_tonemap_lean: lean composite shader, no bloom pass (Switch)
extern int ps_r_pipe_stats;     // r_pipe_stats: dump the lean pipelines' registers (PipeStats)
#ifdef __SWITCH__
extern "C" int g_xr_nx_wskip_win;   // device.cpp nx_statab: 41 = the pre-merge lean FS (tonemap_lean_v1.frag.spv)
#endif
extern int   ps_r_vsm_debug_dyn;   // red overlay of VSM dyn-atlas (NPC/grass) shadows
extern int   ps_r_vol_upsample;    // froxel-volume reconstruction filter in the composite (0 = old single tap)
extern float ps_r_exp_adapt;       // auto-exposure temporal adaptation time constant (s; 0 = instant)
extern int   ps_r_expo_meter;      // metering estimator: 0 = whole-frame arithmetic mean (the old 1×1 read), 1 = blended robust, 2 = median
extern int   ps_r_expo_interval;   // r_expo_interval: the mip chain + meter only every Nth frame
extern int   ps_r_tonemap_probe;   // r_tonemap_probe: 1 = 1x1-scissor composite, 2 = composite into a private image (timing)
extern float ps_r_expo_meter_mix;  // mode 1 dial: 0 = arithmetic (old look), 1 = pure log-average (see vk_console_min.cpp)
extern float ps_r_expo_indoor;     // r_expo_indoor — how far the meter slides to the median under a roof (0 = off, the A/B)
extern float ps_r_expo_indoor_max; // r_expo_indoor_max — ceiling multiplier on r_expo_max when fully enclosed
extern float ps_r_expo_indoor_dist;// r_expo_indoor_dist — how far up (m) the roof rays look
extern float ps_r_exp_adapt_up;    // adaptation time constant for exposure RISING (image brightening), seconds
extern int   ps_r_expo_debug;      // 1 = print the meter's estimators + the applied exposure ~once a second
extern float ps_r_dlss_sharp;      // CAS sharpen on the DLSS output (0 = off)
extern int   ps_r_dlss_debug;      // DLSS debug view: 1=CAS heatmap, 2=split, 3=gate flag
extern float ps_r_sun_beam_splash;     // god-ray ground-splash strength (0 = off)
extern float ps_r_sun_beam_splash_thr; // in-scatter luminance threshold before the splash kicks in

namespace VK {

namespace {
    constexpr u32 kMaxImages = 8;

    VkPipeline            s_Pipeline       = VK_NULL_HANDLE;
    VkPipelineLayout      s_PipelineLayout = VK_NULL_HANDLE;
    VkShaderModule        s_VS             = VK_NULL_HANDLE;
    VkShaderModule        s_FS             = VK_NULL_HANDLE;
    VkPipeline            s_PipelineLean   = VK_NULL_HANDLE;   // r_tonemap_lean: exposure + grade + Reinhard + gamma only
    VkShaderModule        s_FSLean         = VK_NULL_HANDLE;
    VkPipeline            s_PipelineLeanV1 = VK_NULL_HANDLE;   // Switch A/B only: the lean FS the console ran v138..v145
    VkDescriptorSetLayout s_SetLayout      = VK_NULL_HANDLE;
    VkDescriptorPool      s_Pool           = VK_NULL_HANDLE;
    VkDescriptorSet       s_Set[kMaxImages] = {};
    VkSampler             s_Sampler        = VK_NULL_HANDLE;
    u32                   s_boundGen       = 0;   // SceneColor generation the sets were written for
    u32                   s_boundBloomGen  = 0;   // BloomPass RT generation (binding 1)
    u32                   s_boundDistortGen = 0;  // distort RT generation (binding 2)
    VkImageView           s_boundDepth     = VK_NULL_HANDLE;  // scene depth (binding 3, SSR puddles)
    u32                   s_boundVolGen    = 0;   // Vol volume generation (binding 4, volumetrics)
    u32                   s_boundSsilGen   = 0;   // SSIL result generation (binding 5, indirect light)
    bool                  s_boundResolvedUp = false;          // was binding 6 the DLSS output (vs render-res scene)?
    VkImageView           s_boundResolved  = VK_NULL_HANDLE;  // resolved base-colour view (binding 6)

    // Heat-haze strength: scene UV offset = (distort.rg - 0.5) * kDistortAmount —
    // R2/R4 combine_2.ps: (distort.xy - 127/255) * def_distort, def_distort 0.05.
    constexpr float kDistortAmount = 0.05f;

    // R4 auto-exposure (bloom_luminance_3.ps): exposure = middlegray/(avgLum +
    // low), measured from the whole-frame average luminance (the HDR target's
    // top mip), clamped. R4 defaults are middlegray 1.0 / low 0.0001 with a
    // def_hdr-scaled luminance; our HDR luminance is unscaled, so middlegray is
    // retuned to land midday near exposure ~1 and the clamp bounds how far a
    // dim/bright scene can drift (keeps night from washing to mid-gray).
    // White point 11.2 = R4 tonemap_srgb.h fWhiteIntensity: a soft filmic
    // highlight rolloff (the glow comes from bloom, not from clipping).
    // Screenshot comparison vs R4 (morning, bright sky + tree shade): the
    // sky-heavy frame average pushed our exposure into the 0.6 floor and the
    // whole foreground went darker/flatter than R4. Raise the target + floor.
    // Auto-exposure inputs live in vk_exposure.h (shared with vk_pass_bloom.cpp).
    using Exposure::MiddleGray;    // exposure target (per color pipeline — see vk_exposure.h)
    using Exposure::kLowLum;       // R4 ps_r2_tonemap_low_lum
    using Exposure::ExpMin;        // exposure clamp lo (r_expo_min, live)
    using Exposure::ExpMax;        // exposure clamp hi (r_expo_max, live)
    using Exposure::ExpComp;       // user compensation (r_expo, live)
    constexpr float kWhitePoint  = 11.2f;    // R4 tonemap_sRGB fWhiteIntensity
    // Bloom add strength (blend_soft analog). 0.8 was a constexpr with no console
    // reach — which meant "is this glow the BLOOM or something else?" could not be
    // asked in-game at all (28-08: a white blob in a distant dark window, HDR
    // pixel dark in the probe dump ⇒ the whiteness is post-scene; bloom is the
    // first suspect and needs its own kill switch to separate from the upscaler).
    constexpr float kBloomIntensity = 0.8f;  // the default r_blm ships with

    // Does the presentation surface apply the sRGB OETF itself? If it does, the shader
    // must NOT encode again; if it does not (the usual case here — ChooseSurfaceFormat
    // prefers B8G8R8A8_UNORM), the shader owns the encode. See the display-gamma block
    // in Execute for why this is a surface property and not a user setting.
    inline bool IsSrgbFormat(VkFormat f)
    {
        switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R8G8B8_SRGB:
        case VK_FORMAT_B8G8R8_SRGB:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            return true;
        default:
            return false;
        }
    }

    struct TonemapPush { float p0[4]; float p1[4]; float p2[4]; float p3[4]; float p4[4]; float p5[4]; float p6[4]; float p7[4]; };
    static_assert(sizeof(TonemapPush) == 128, "must match tonemap.frag PC block (128 B = the guaranteed push limit)");

    // ---- Temporal auto-exposure (eye adaptation) ----------------------------------
    // The instantaneous exposure (middlegray / metered avg-luminance) makes the image
    // visibly darken/brighten as the camera tilts between bright sky and dark ground.
    // We read back the HDR target's 1×1 top mip (whole-frame average) with a few-frame
    // lag, compute the target exposure on the CPU, and EASE the applied exposure toward
    // it over ps_r_exp_adapt seconds — the eye-adaptation "breathing" the user reported.
    constexpr u32 kExpFrames = 4;                 // readback ring (≥ frames-in-flight → lag-safe)
    // ROBUST METERING (r_expo_meter). The 1×1 top mip is the ARITHMETIC mean of the
    // whole frame, and an arithmetic mean belongs to its outliers: a bright window
    // filling 2% of a dark room's frame moves it severalfold, so walking room to
    // room (or panning wall to wall) swings the exposure target and the whole
    // image ramps for r_exp_adapt seconds — the "exposure jumps between rooms"
    // complaint. The robust modes read back the mip FOUR LEVELS BELOW the top
    // instead (sides 16..31 texels, a few hundred tile means, ≤ 8 KB) and reduce
    // it on the CPU: mode 1 = log-average (the photographic/Reinhard meter — a
    // ×100 window tile shifts log-space by its share, not by its magnitude),
    // mode 2 = median (ignores outright what fraction the outlier is). Mode 0
    // reproduces the old 1×1 read verbatim.
    // ⭐A side profit: the old path fed max(lum, 1e-4) a whole-frame mean that a
    // single NaN texel had already poisoned (std::max(NaN, x) is NaN — see the
    // froxel-NaN saga). Here a NaN kills only its own tile, and the guard skips it.
    constexpr u32 kMeterMaxTexels = 32 * 32;      // sides < 32 by construction (top is 1×1)
    CVulkanBuffer s_expBuf;                        // kExpFrames × kMeterMaxTexels RGBA16F texels
    u8*           s_expMapped   = nullptr;
    u16           s_expW[kExpFrames] = {};         // copied extent per ring slot (the meter
    u16           s_expH[kExpFrames] = {};         //   mode can change between copy and read)
    u32           s_expFrame    = 0;               // ring cursor / frames processed
    float         s_exposure    = 1.0f;            // the smoothed, applied exposure
    bool          s_expValid    = false;           // false until the first valid readback
    float         s_enclosed    = 0.0f;            // eased "camera is under a roof" (0 open .. 1 closed)

    // ⭐⭐⭐ THE METER KNOWS WHETHER THE CAMERA IS INDOORS (r_expo_indoor, 06-09-2026).
    //
    // Both robust estimators failed the same room from opposite sides, and the
    // reason is that neither knows where it is. Indoors, a window is the ONLY bright
    // patch in an honestly dark room (the traced field there sits 30-40x below the
    // street — physics, not a bug), so any estimator that lets the window vote
    // pins the exposure at r_expo_min and the room reads black ("в помещении
    // супер темно": wall sceneHDR 0.006, exposure 0.80 = the floor). Outdoors the
    // same discounting is the "wall by the window washes out" failure, because a
    // wall in the open IS a dark patch in a bright scene. The user's own verdict
    // settled it in the house: median + a higher ceiling = "намного лучше" — but
    // the median is NoSave and global, so it slid off at the next launch and would
    // have broken the street. The missing input is enclosure, and it is a few CPU
    // rays: up from the camera, tilted, against the level's collision model — the
    // same "is there a roof" test the GI's rtIndoorUp does on the GPU. Under a roof
    // the meter slides to the median and the ceiling rises; in the open nothing
    // changes, bit for bit. Crowns are not in cform, so a forest still meters open.
    // Eased over ~0.4 s so a doorway is a crossfade, not a step.
    // RENDER_THREAD_PLAN step 8: the rays (CDB collider on the live static model) are Tonemap_ExtractEnclosure's job,
    // PREPARE phase; here only the easing runs, on the stored target.
    // Two-slot pipeline: per frame slot (Tonemap_ExtractEnclosure on the main thread, CameraEnclosure on the render thread).
    struct EnclosureSlot { float target = 0.f; u32 tag = u32(-1); };
    RT::PerSlot<EnclosureSlot> s_enclosureSlots;
    float EnclosureTarget(float reach)
    {
        float target = 0.f;
        CDB::MODEL* model = g_pGameLevel ? g_pGameLevel->ObjectSpace.GetStaticModel() : nullptr;
        if (model && reach > 0.f) {
            // A plumb line alone reads "indoors" under a wire and "open" beside a
            // beam; five rays in a ~25 degree cap vote instead (same cap as rtIndoorUp).
            static const float kTilt[5][2] = { {0.f, 0.f}, {0.45f, 0.f}, {-0.45f, 0.f}, {0.f, 0.45f}, {0.f, -0.45f} };
            CDB::COLLIDER xrc;
            u32 hits = 0;
            for (const auto& t : kTilt) {
                Fvector d; d.set(t[0], 1.f, t[1]); d.normalize();
                Fvector o = RV().vCameraPosition; o.y += 0.05f;
                xrc.r_clear();
                xrc.ray_query(CDB::OPT_ONLYFIRST, model, o, d, reach);
                if (xrc.r_count()) ++hits;
            }
            target = float(hits) / 5.f;
        }
        return target;
    }
    float CameraEnclosure(float reach, float dt)
    {
        (void)reach;
        const float target = (s_enclosureSlots.cur().tag == RV().frameIndex) ? s_enclosureSlots.cur().target : s_enclosed;   // no extract this frame: hold
        const float tau = 0.4f;
        s_enclosed += (target - s_enclosed) * (1.0f - expf(-dt / tau));
        return s_enclosed;
    }

    // Unsigned 11/10-bit floats (B10G11R11_UFLOAT_PACK32 texel) → float. Same
    // 5-bit exponent and bias as fp16; the mantissa is 6 (R, G) or 5 (B) bits.
    // ⚠⚠⚠ THE METER DECODED THIS FORMAT AS FP16 FROM 26-08 TO 06-09. SceneColor
    // went packed on 26-08 (r_scene_color_32, default 1) and the readback below
    // kept reading 8-byte RGBA16F texels off a 4-byte-per-texel copy: half the
    // "tiles" were the buffer's stale tail (black), the rest were packed words
    // torn into two halves (NaN, or ~11000). arith ~300 pinned the exposure at
    // r_expo_min in EVERY scene for twelve days, and the street hid it — a bright
    // day clamps to the floor anyway. Interiors did not: "в помещении супер
    // темно". The instrument line's "110 black / 23 blown / brightest 11000 at a
    // random tile" was the tell. Decode by SceneColor::Format(), never assume.
    inline float UFloatToFloat(u32 v, u32 mantBits)
    {
        const u32 e = (v >> mantBits) & 0x1F;
        const u32 m = v & ((1u << mantBits) - 1u);
        const float mant = float(m) / float(1u << mantBits);
        if (e == 0)  return m ? std::ldexp(mant, -14) : 0.0f;
        if (e == 31) return m ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
        return std::ldexp(1.0f + mant, int(e) - 15);
    }

    // IEEE half (RGBA16F texel) → float, for the CPU-side average-luminance decode.
    inline float HalfToFloat(u16 h)
    {
        u32 sign = u32(h & 0x8000) << 16;
        u32 exp  = (h & 0x7C00) >> 10;
        u32 mant = (h & 0x03FF);
        u32 f;
        if (exp == 0) {
            if (mant == 0) f = sign;
            else { exp = 1; while ((mant & 0x0400) == 0) { mant <<= 1; --exp; } mant &= 0x03FF;
                   f = sign | ((exp + 112) << 23) | (mant << 13); }
        } else if (exp == 0x1F) {
            f = sign | 0x7F800000u | (mant << 13);
        } else {
            f = sign | ((exp + 112) << 23) | (mant << 13);
        }
        float out; std::memcpy(&out, &f, 4); return out;
    }
}


namespace TonemapPass {

bool Init()
{
    if (s_Pipeline) return true;

    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    s_VS = g_ShaderManager->Load("tonemap.vert.spv");
    s_FS = g_ShaderManager->Load("tonemap.frag.spv");   // canonical name on every platform (was tonemap_v2 on the Switch SD card)
    if (s_VS == VK_NULL_HANDLE || s_FS == VK_NULL_HANDLE) {
        Msg("![VK Tonemap] shader load failed");
        return false;
    }

    // Set 0: binding 0 = HDR scene (mip chain, avg-lum), 1 = blurred bloom, 2 = distortion,
    // 3 = scene depth (SSR puddles), 4 = integrated volumetrics (sampler3D),
    // 5 = SSIL indirect light (half-res), 6 = RESOLVED base colour (display res; the DLSS
    // upscaled output when upscaling, else the render-res scene). All FS.
    constexpr auto kTex = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    if (!VK::MakeDescriptorSets({ kTex, kTex, kTex, kTex, kTex, kTex, kTex }, kMaxImages,
                                s_SetLayout, s_Pool, s_Set,
                                VK_SHADER_STAGE_FRAGMENT_BIT, "Tonemap"))
        return false;
    for (u32 i = 0; i < kMaxImages; ++i) VK::Prof::NameSet(s_Set[i], "Tonemap.Set");   // TEMP diag: VUID hunt

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0.0f; si.maxLod = VK_LOD_CLAMP_NONE;   // textureLod(maxMip) for the avg-luminance read
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_Sampler) != VK_SUCCESS) {
        Msg("![VK Tonemap] sampler failed"); return false;
    }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pcr.offset = 0; pcr.size = sizeof(TonemapPush);
    // Set 1 = the shared EnvLight set: the SSR puddles read rain_vp/rain_params,
    // the camera frustum terms and the rain occlusion map (binding 9) from it.
    VkDescriptorSetLayout sets[2] = { s_SetLayout, EnvLight::GetSetLayout() };
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = (sets[1] != VK_NULL_HANDLE) ? 2u : 1u; plci.pSetLayouts = sets;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_PipelineLayout) != VK_SUCCESS) {
        Msg("![VK Tonemap] pipeline layout failed"); return false;
    }

    // Writes the actual swapchain (UNORM), RGBA, no blend (final composite).
    s_Pipeline = Fullscreen::CreatePipeline(s_VS, s_FS, Swapchain.m_Format, s_PipelineLayout,
                                            Fullscreen::OpaqueAttachment(), "Tonemap");
    if (s_Pipeline == VK_NULL_HANDLE) {
        Msg("![VK Tonemap] pipeline create failed"); return false;
    }
    // Optional lean variant (same layout). Missing shader -> the switch is simply inert.
    // tonemap_lean.frag.glsl carries the nfs28 bare-resample probe (p5.z, nx_diag 111) and the nfs29 identity-term skip
    // (p4.x > 0.5 forces the full grade) -- the Switch shipped them as tonemap_lean_v2/_v3.
    s_FSLean = g_ShaderManager->Load("tonemap_lean.frag.spv");
    if (s_FSLean != VK_NULL_HANDLE)
        s_PipelineLean = Fullscreen::CreatePipeline(s_VS, s_FSLean, Swapchain.m_Format, s_PipelineLayout,
                                                    Fullscreen::OpaqueAttachment(), "TonemapLean");
#ifdef __SWITCH__
    // nx_statab 41 (v147): the lean composite the console ran v138..v145 (an older tonemap_lean.frag.spv the SD kept).
    if (VkShaderModule v1 = g_ShaderManager->Load("tonemap_lean_v1.frag.spv"))
        s_PipelineLeanV1 = Fullscreen::CreatePipeline(s_VS, v1, Swapchain.m_Format, s_PipelineLayout,
                                                      Fullscreen::OpaqueAttachment(), "TonemapLeanV1");
#endif
    if (ps_r_pipe_stats > 0) {
        VK::PipeStats::Dump(s_PipelineLean, "tonemap lean");
        VK::PipeStats::Dump(s_PipelineLeanV1, "tonemap lean (v1)");
    }

    // Temporal-exposure readback ring: up to kMeterMaxTexels RGBA16F texels per slot
    // (the meter mip — 8 B texels; mode 0 uses just the first one).
    s_expBuf.Create(kExpFrames * kMeterMaxTexels * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    s_expMapped = static_cast<u8*>(s_expBuf.Map());
    s_expFrame  = 0; s_exposure = 1.0f; s_expValid = false; s_enclosed = 0.f;

    Msg("[VK Tonemap] Init OK (auto-exposure mg=%.2f white=%.2f clamp[%.2f,%.2f])",
        MiddleGray(), kWhitePoint, ExpMin(), ExpMax());
    return true;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_Pipeline)       { vkDestroyPipeline(VulkanHW.m_Device, s_Pipeline, nullptr); s_Pipeline = VK_NULL_HANDLE; }
    if (s_PipelineLean)   { vkDestroyPipeline(VulkanHW.m_Device, s_PipelineLean, nullptr); s_PipelineLean = VK_NULL_HANDLE; }
    if (s_PipelineLeanV1) { vkDestroyPipeline(VulkanHW.m_Device, s_PipelineLeanV1, nullptr); s_PipelineLeanV1 = VK_NULL_HANDLE; }
    if (s_PipelineLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_PipelineLayout, nullptr); s_PipelineLayout = VK_NULL_HANDLE; }
    if (s_Sampler)        { vkDestroySampler(VulkanHW.m_Device, s_Sampler, nullptr); s_Sampler = VK_NULL_HANDLE; }
    if (s_Pool)           { vkDestroyDescriptorPool(VulkanHW.m_Device, s_Pool, nullptr); s_Pool = VK_NULL_HANDLE; }
    if (s_SetLayout)      { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_SetLayout, nullptr); s_SetLayout = VK_NULL_HANDLE; }
    s_expBuf.Destroy(); s_expMapped = nullptr; s_expValid = false; s_exposure = 1.0f; s_expFrame = 0; s_enclosed = 0.f;
    s_VS = s_FS = s_FSLean = VK_NULL_HANDLE; s_boundGen = 0; s_boundBloomGen = 0;
}

}  // namespace TonemapPass

// The CPU-smoothed exposure the composite will apply this frame — the ONE number
// bloom must threshold against (vk_exposure.h: the two must agree or bloom halos
// against a brightness the viewer never sees; with the robust meter they now also
// differ in VALUE, not just in smoothing, so the in-shader estimate is no longer
// an acceptable stand-in). 0 until the first readback lands; callers treat 0 as
// "compute your own instantaneous estimate" — the composite shader's own p5.x
// convention.
float Exposure::Smoothed() { return s_expValid ? s_exposure : 0.0f; }

void Tonemap_ExtractEnclosure()
{
    // Same gate the meter used: only when the enclosure blend can matter (r_expo_indoor, not median-only mode 2).
    if (!(ps_r_expo_indoor > 0.f && ps_r_expo_meter != 2)) return;
    s_enclosureSlots.cur().target = EnclosureTarget(ps_r_expo_indoor_dist);
    s_enclosureSlots.cur().tag    = RV().frameIndex;
}

// r_tonemap_probe 2: a private target of the swapchain's format and the composite's extent (timing probe only).
static VkImage       s_probeImg   = VK_NULL_HANDLE;
static VmaAllocation s_probeAlloc = VK_NULL_HANDLE;
static VkImageView   s_probeView  = VK_NULL_HANDLE;
static VkExtent2D    s_probeExt{};
static bool EnsureProbeTarget(VkExtent2D ext)
{
    if (s_probeImg && s_probeExt.width == ext.width && s_probeExt.height == ext.height) return true;
    if (s_probeImg) return false;   // a size change mid-probe: just stand down
    VkImageCreateInfo ic{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ic.imageType = VK_IMAGE_TYPE_2D; ic.format = Swapchain.m_Format;
    ic.extent = { ext.width, ext.height, 1 }; ic.mipLevels = 1; ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ac{}; ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(VulkanHW.m_Allocator, &ic, &ac, &s_probeImg, &s_probeAlloc, nullptr) != VK_SUCCESS) { s_probeImg = VK_NULL_HANDLE; return false; }
    s_probeView = CreateImageView(s_probeImg, Swapchain.m_Format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
    s_probeExt  = ext;
    Msg("[VK Tonemap] probe target %ux%u", ext.width, ext.height);
    return s_probeView != VK_NULL_HANDLE;
}

void Pass_TonemapComposite(FrameContext& ctx)
{
    if (s_Pipeline == VK_NULL_HANDLE || ctx.cmd == VK_NULL_HANDLE) return;
    const u32 idx = ctx.imageIndex;
    if (SceneColor::GetImage(idx) == VK_NULL_HANDLE) return;

    VkCommandBuffer cmd = ctx.cmd;

    // Downsample the HDR scene to its full mip chain (top mip = whole-frame
    // average luminance for auto-exposure). This also leaves ALL mips — incl.
    // mip0 (the scene) — in SHADER_READ for the composite below.
#ifdef __SWITCH__
    // nfs28 nx_diag 112: no mip chain and no meter readback this frame, exposure held at its last value -- prices the
    // avg-luminance chain (9 blits = 9 meta render passes in NVK, ~22 barriers = wait-for-idle each). mip0 still has to
    // leave COLOR_ATTACHMENT for the composite; mips 1.. stay SHADER_READ from the last full frame (the frame-start
    // barrier touches mip0 only).
    const bool nxNoMips = (g_xr_nx_diag == 112) && s_expValid;
#else
    const bool nxNoMips = false;
#endif
    // 10-09 r_expo_interval: on the lean path the chain feeds only the meter (no bloom), and the meter is read a few
    // frames late and eased over ~a second anyway -> build it every Nth frame. The skipped frames bank their dt so the
    // adaptation keeps its speed. (Same frame shape as nx_diag 112: mip0 leaves COLOR_ATTACHMENT, mips 1.. stay put.)
    static u32   s_expTick  = 0;
    static float s_expDtAcc = 0.f;
    const bool intervalSkip = ps_r_expo_interval > 1 && ps_r_tonemap_lean && s_expValid
                           && (++s_expTick % u32(ps_r_expo_interval)) != 0;
    if (intervalSkip)
        s_expDtAcc = _min(s_expDtAcc + _max(RV().fTimeDelta, 0.f), 0.25f);
    const int zMips = VK::Prof::ZoneBegin(cmd, "Tonemap/Mips");
    if (nxNoMips || intervalSkip)
        ImageBarrier(cmd, SceneColor::GetImage(idx), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    else
        SceneColor::GenerateMips(cmd, idx);

    // ---- Temporal auto-exposure: read the previous (lagged) 1×1 average, ease the
    // applied exposure toward its target over ps_r_exp_adapt seconds, then copy THIS
    // frame's average into the ring for a later frame. Eliminates the sky/ground
    // brightness "breathing" (instant exposure). p5.x = 0 on the first frames → the
    // shader falls back to the instantaneous estimate until a readback is valid.
    if (!nxNoMips && !intervalSkip) {
        const u32 slot = s_expFrame % kExpFrames;
        if (s_expMapped && s_expFrame >= kExpFrames) {
            // Reduce whatever extent THIS slot was copied with (the mode is live, so
            // the ring can hold a 1×1 and a 20×11 at once). One walk feeds all three
            // estimators; the median's partial sort runs on ≤ 1024 floats.
            const u32  nw = std::max<u32>(1, s_expW[slot]);
            const u32  nh = std::max<u32>(1, s_expH[slot]);
            const u32  n  = std::min(nw * nh, kMeterMaxTexels);
            const u8*  base   = s_expMapped + size_t(slot) * kMeterMaxTexels * 8;
            const u16* h      = reinterpret_cast<const u16*>(base);
            const u32* packed = reinterpret_cast<const u32*>(base);
            const bool isPacked = SceneColor::Format() == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            float  lums[kMeterMaxTexels];
            u32    cnt = 0;
            double sum = 0.0, logSum = 0.0;
            // Instrument (r_expo_debug): the brightest tile and where it sits, plus
            // how many tiles are black or blown. 📏06-09 the Cordon room metered
            // arith 300-500 against a median of 0.0000 — one tile at the half-float
            // ceiling was pinning the exposure at r_expo_min in every room, and
            // nothing in the line said so.
            float  maxLum = -1.f; u32 maxAt = 0, nBlack = 0, nBlown = 0;
            for (u32 i = 0; i < n; ++i) {
                float r, g, b;
                if (isPacked) {
                    const u32 w = packed[i];
                    r = UFloatToFloat(w & 0x7FFu, 6);
                    g = UFloatToFloat((w >> 11) & 0x7FFu, 6);
                    b = UFloatToFloat((w >> 22) & 0x3FFu, 5);
                } else {
                    r = HalfToFloat(h[i * 4 + 0]);
                    g = HalfToFloat(h[i * 4 + 1]);
                    b = HalfToFloat(h[i * 4 + 2]);
                }
                float lum = r * 0.2126f + g * 0.7152f + b * 0.0722f;
                if (!(lum >= 0.0f)) continue;            // NaN/negative guard: kill the tile, not the meter
                lum = std::min(lum, 65504.0f);
                if (lum > maxLum) { maxLum = lum; maxAt = i; }
                if (lum < 1e-4f) ++nBlack;
                if (lum > 100.f) ++nBlown;
                lums[cnt++] = lum;
                sum    += lum;
                logSum += log(double(lum) + 1e-4);
            }
            const int  mode  = ps_r_expo_meter;
            float dt  = (RV().fTimeDelta > 0.f && RV().fTimeDelta < 0.25f) ? RV().fTimeDelta : 0.016f;
            dt += s_expDtAcc; s_expDtAcc = 0.f;   // r_expo_interval: the frames this meter skipped
            // The median is computed whenever anything can use it: mode 2 outright,
            // or the enclosure blend below (r_expo_indoor). ≤ 1024 floats, one
            // partial sort — cheaper than the readback that fed it.
            const float closed = (ps_r_expo_indoor > 0.f && mode != 2)
                               ? std::min(1.f, CameraEnclosure(ps_r_expo_indoor_dist, dt) * ps_r_expo_indoor)
                               : 0.f;
            float median = 1e-4f;
            if (cnt && (mode == 2 || closed > 0.f)) {
                std::nth_element(lums, lums + cnt / 2, lums + cnt);
                median = lums[cnt / 2];
            }
            float metered;
            if (cnt == 0)          metered = 1e-4f;                       // every tile NaN — hold the floor
            else if (mode == 2)    metered = median;
            else if (mode == 1) {
                // Log-space blend of the two estimators (r_expo_meter_mix). The pure
                // log-average was the opposite failure, seen in-game the day it
                // shipped: it discounts a small bright window so hard that a wall
                // filling the frame beside one is metered as "a dark scene" and
                // walks up to the ExpMax ceiling. The blend keeps the window's
                // authority while capping its grip — the doorway swing scales as
                // (old swing)^(1-mix). See the knob's note in vk_console_min.cpp.
                const double arith = std::max(sum / cnt, 1e-4);
                const double lavg  = std::max(exp(logSum / cnt), 1e-4);
                const double t     = double(std::min(std::max(ps_r_expo_meter_mix, 0.0f), 1.0f));
                metered = float(exp(log(arith) * (1.0 - t) + log(lavg) * t));
            }
            else                   metered = float(sum / cnt);             // arithmetic — the old meter
            // Under a roof: slide the estimate to the median (the window stops
            // voting) and let the ceiling rise (r_expo_indoor_max x r_expo_max) —
            // the eye adapts far further indoors than the outdoor clamp width
            // allows, and that width was measured against pitch pumping in the
            // open, which a roof rules out. Both blends are log-space and 0 in the
            // open, so the street is untouched bit for bit.
            if (closed > 0.f && cnt) {
                const double lm = log(std::max(metered, 1e-4f)), ld = log(std::max(median, 1e-4f));
                metered = float(exp(lm * (1.0 - closed) + ld * closed));
            }
            const float expMax = ExpMax() * (1.f + (std::max(1.f, ps_r_expo_indoor_max) - 1.f) * closed);
            float avgLum = std::max(metered, 1e-4f);
            float target = std::min(std::max(MiddleGray() / (avgLum + kLowLum), ExpMin()), expMax) * ExpComp();
            if (ps_r_expo_debug) {
                static u32 s_dbgLast = 0;
                if (s_expFrame - s_dbgLast >= 60) {                        // ~once a second
                    s_dbgLast = s_expFrame;
                    const float arith = cnt ? float(sum / cnt) : 0.f;
                    const float lavg  = cnt ? float(exp(logSum / cnt)) : 0.f;
                    Msg("[VK Expo] meter %ux%u mode %d mix %.2f | arith %.4f logavg %.4f median %.5f | enclosed %.2f -> metered %.4f max %.2f | target %.3f applied %.3f"
                        " | tiles: %u nan-skipped, %u black(<1e-4), %u blown(>100), brightest %.1f at (%u,%u)",
                        nw, nh, mode, ps_r_expo_meter_mix, arith, lavg, median, closed, metered, expMax, target, s_exposure,
                        n - cnt, nBlack, nBlown, maxLum, maxAt % nw, maxAt / nw);
                }
            }
            // ASYMMETRIC ADAPTATION: rising exposure (the image brightening — walking
            // into darkness, or the meter creeping a dark wall toward the ceiling)
            // uses its own, slower time constant. The eye adapts to darkness far
            // slower than to light, and the slow direction is exactly the one where
            // the wall-by-the-window creep lives — so the creep becomes a lazy
            // drift, while the pan back to the window still snaps down in ~r_exp_adapt.
            float tau = (target > s_exposure) ? ps_r_exp_adapt_up : ps_r_exp_adapt;
            // First MEASURED value snaps: easing toward it from the arbitrary 1.0
            // seed showed as a visible "brightness ramp" on level entry (worse in
            // linear mode, whose steady-state exposure sits higher). Adaptation
            // stays smooth for every later change — that part is eye-adaptation
            // by design.
            if (!s_expValid || tau <= 0.001f) s_exposure  = target;
            else                              s_exposure += (target - s_exposure) * (1.0f - expf(-dt / tau));
            s_expValid = true;
        }
        // The editor used to pin ps_r2_img_gamma = 2.2 and ps_r2_img_cg here. Both are
        // gone: the CDL pin was a no-op (0.5,0.5,0.5 IS the default, and nothing in the
        // engine ever writes that cvar — ssfx_color_grading is a user knob, not level
        // data), and the gamma pin is now handled properly for BOTH game and editor by
        // the display-encode split at the push below.
        // What genuinely IS editor-only stays: an EMPTY viewport gives the meter nothing
        // to average and auto-exposure crushes the frame to black. That premise holds
        // only while the viewport is empty — once the host pushes a real scene there is
        // plenty to meter, and a value tuned for one model on a flat backdrop blows a lit
        // outdoor level out to white. So meter as soon as we can.
        if (Core.Params && strstr(Core.Params, "-vk_editor") && VKEditor::HostModelCount() == 0)
        {
            s_exposure = 2.0f;
            s_expValid = true;
        }
        if (s_expMapped) {
            const u32 topMip = SceneColor::MipLevels() - 1;
            // The meter mip: 4 levels below the top its sides are 16..31 texels (the
            // top is 1×1, so w>>L there is < 2^5) — a few hundred tile means, enough
            // for a robust statistic and still a few-KB copy. Mode 0 keeps the old
            // 1×1 top-mip read verbatim (the A/B).
            const u32 meterMip = (ps_r_expo_meter > 0 && topMip >= 4) ? (topMip - 4) : topMip;
            const VkExtent2D scExt = SceneColor::Extent();
            const u32 mw = std::max<u32>(1, scExt.width  >> meterMip);
            const u32 mh = std::max<u32>(1, scExt.height >> meterMip);
            s_expW[slot] = u16(mw); s_expH[slot] = u16(mh);
            VkImage img = SceneColor::GetImage(idx);
            VkImageMemoryBarrier br{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            br.srcQueueFamilyIndex = br.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            br.image = img; br.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, meterMip, 1, 0, 1 };
            br.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; br.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            br.srcAccessMask = VK_ACCESS_SHADER_READ_BIT; br.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);

            VkBufferImageCopy region{};
            region.bufferOffset      = VkDeviceSize(slot) * kMeterMaxTexels * 8;
            region.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, meterMip, 0, 1 };
            region.imageExtent       = { mw, mh, 1 };
            vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_expBuf.GetHandle(), 1, &region);

            br.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; br.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            br.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; br.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);
        }
        ++s_expFrame;
    }

    VK::Prof::ZoneEnd(cmd, zMips);

    // SSIL history: snapshot this frame's composited HDR scene (half-res) so the
    // NEXT frame's GTAO horizon march can gather one-bounce indirect light from
    // it. The IL itself is produced inside the GTAO pass (MRT target 1, folded
    // into the occlusion march) — here we only feed it. No-op when r_ssil is off.
    SSAOPass::CapturePrevColor(cmd, SceneColor::GetImage(idx), ctx.extent);

    // Bloom chain (bright pass from the scene mips + gaussian blur) — result
    // lands SHADER_READ for binding 1.
    // r_tonemap_lean: the lean composite never samples the bloom, so the bright-pass + blur chain is skipped too.
    const bool lean = ps_r_tonemap_lean && s_PipelineLean != VK_NULL_HANDLE;
    if (!lean)
        BloomPass::Execute(cmd, idx, ctx.extent, SceneColor::Generation());

    // Scene depth → SHADER_READ for the SSR puddle march (binding 3). NOT
    // transitioned back: CRender::Begin re-acquires depth from UNDEFINED each
    // frame (its contents are clear-loaded, never carried over).
    ImageBarrier(cmd, Swapchain.m_DepthImage, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

    // GI pixel debugger (r_gi_probe): everything it samples is settled exactly
    // here — depth just went SHADER_READ, the gather resolved inside World, the
    // scene colour is complete and its mips are built. A 1-thread dispatch;
    // no-op while the cvar is 0.
    if (ctx.viewProj) GIProbe::Execute(cmd, idx, *ctx.viewProj);

    // Rebind the per-image descriptor sets when the HDR target, the bloom RT, the
    // distortion RT, the volume or the SSIL (GTAO MRT) result was (re)created.
    const u32 gen        = SceneColor::Generation();
    const u32 bloomGen   = BloomPass::Generation();
    const u32 distortGen = ParticlePass::DistortGeneration();
    const u32 volGen     = Vol::Generation();
    const u32 ssilGen    = SSAOPass::Generation();   // IL RT rides the GTAO RT generation
    // Binding 6 (resolved base colour): the DLSS-upscaled display-res output when DLSS
    // ran this frame, else the render-res scene (== display res when not upscaling).
    // ResolvedThisFrame: the evaluate really wrote the DLSS output this frame — a
    // failed SL evaluate leaves it undefined, and sampling it painted the whole
    // screen blue (the mid-game DLSS-enable OOM crash, 22:51 log).
    // ⭐EITHER upscaler can own binding 6, never both: Fsr::Enabled() is false
    // whenever DLSS is upscaling. Asking each one separately (rather than through
    // a shared facade) keeps the DLSS path byte-for-byte what it was.
    const bool dlssUp = VK::Dlss::Enabled() && VK::MotionVec::Enabled() && VK::Dlss::FeatureReady()
                     && VK::Dlss::ResolvedThisFrame();
    const bool fsrUp  = !dlssUp && VK::Fsr::Enabled() && VK::MotionVec::Enabled() && VK::Fsr::OutputReady()
                     && VK::Fsr::ResolvedThisFrame();
    const bool        resolvedUp  = dlssUp || fsrUp;
    const VkImageView dlssOutView = dlssUp ? VK::Dlss::OutputView()
                                  : fsrUp  ? VK::Fsr::OutputView()
                                           : VK_NULL_HANDLE;
    if (gen != s_boundGen || bloomGen != s_boundBloomGen || distortGen != s_boundDistortGen
        || Swapchain.m_DepthView != s_boundDepth || volGen != s_boundVolGen || ssilGen != s_boundSsilGen
        || resolvedUp != s_boundResolvedUp || (resolvedUp && dlssOutView != s_boundResolved)) {
        const u32 n = SceneColor::Count();
        for (u32 i = 0; i < n && i < kMaxImages; ++i) {
            VkDescriptorImageInfo ii[7]{};
            ii[0].sampler = s_Sampler; ii[0].imageView = SceneColor::GetSampleView(i);  // full mip chain (avg-lum)
            ii[1].sampler = s_Sampler; ii[1].imageView = BloomPass::GetResultView();
            ii[2].sampler = s_Sampler; ii[2].imageView = ParticlePass::GetDistortView();
            ii[3].sampler = s_Sampler; ii[3].imageView = Swapchain.m_DepthView;          // SSR puddles
            ii[4].sampler = Vol::GetSampler(); ii[4].imageView = Vol::GetIntegratedView(); // integrated volumetrics (3D)
            ii[5].sampler = SSAOPass::GetSampler(); ii[5].imageView = SSAOPass::GetILResultView(); // indirect light (GTAO MRT target 1)
            ii[6].sampler = s_Sampler; ii[6].imageView = (resolvedUp && dlssOutView != VK_NULL_HANDLE)
                                                       ? dlssOutView : SceneColor::GetSampleView(i);  // resolved base
            ii[0].imageLayout = ii[1].imageLayout = ii[2].imageLayout = ii[3].imageLayout = ii[4].imageLayout = ii[5].imageLayout = ii[6].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (ii[1].imageView == VK_NULL_HANDLE) ii[1].imageView = SceneColor::GetSampleView(i); // pre-bloom fallback
            if (ii[2].imageView == VK_NULL_HANDLE) ii[2].imageView = SceneColor::GetSampleView(i); // no-distort fallback (never sampled: scale 0)
            if (ii[3].imageView == VK_NULL_HANDLE) ii[3].imageView = SceneColor::GetSampleView(i); // no-depth fallback (never sampled: wetness 0)
            if (ii[5].imageView == VK_NULL_HANDLE) { ii[5].sampler = s_Sampler; ii[5].imageView = SceneColor::GetSampleView(i); } // SSIL off/not-ready (never sampled: p5.z 0)
            // Write each valid binding by explicit index — binding 4 (3D volume)
            // exists from Vol::Init (eager); the others can transiently be null.
            VK::DescriptorWriter dw(s_Set[i]);
            for (u32 k = 0; k < 7; ++k)
                if (ii[k].imageView != VK_NULL_HANDLE)
                    dw.Image(k, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ii[k]);
            dw.Flush();
        }
        s_boundGen        = gen;
        s_boundBloomGen   = bloomGen;
        s_boundDistortGen = distortGen;
        s_boundDepth      = Swapchain.m_DepthView;
        s_boundVolGen     = volGen;
        s_boundSsilGen    = ssilGen;
        s_boundResolvedUp = resolvedUp;
        s_boundResolved   = resolvedUp ? dlssOutView : VK_NULL_HANDLE;
    }

    // (Depth was already transitioned to SHADER_READ above, before the SSIL gather.)

    // Swapchain image: UNDEFINED (untouched this frame) → COLOR_ATTACHMENT for the composite.
    // The UNDEFINED source is DELIBERATE, not sloppiness: the composite overwrites
    // every pixel (loadOp DONT_CARE below), so discarding the previous contents in
    // the transition is both legal and cheaper than preserving them. Kept as a
    // hand-placed barrier for that reason — Require() would preserve.
    // ⚠srcStage MUST be COLOR_ATTACHMENT_OUTPUT, not the TOP_OF_PIPE the UNDEFINED
    // layout derives to. This image is the one vkAcquireNextImageKHR hands us, and the
    // submit waits its semaphore at COLOR_ATTACHMENT_OUTPUT (vk_command_buffer.cpp).
    // A layout transition is a WRITE; with srcStage=TOP_OF_PIPE nothing orders that
    // write after the acquire, so it may run while the presentation engine still owns
    // the image. Sync validation caught it as SYNC-HAZARD-WRITE-AFTER-READ at
    // vkQueueSubmit ("previously accessed by vkAcquireNextImageKHR", 16-08) — core
    // validation cannot see this class of bug at all, which is why it stood for months.
    ImageBarrier(cmd, Swapchain.m_Images[idx],
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    // ...but the frame graph has to be TOLD, or it still believes PRESENT_SRC and
    // the UI pass after us re-transitions from a layout the image left. A tracker
    // that is only half-informed is worse than none: it states wrong things
    // confidently. Seed records without emitting.
    g_FrameGraph.Track(Swapchain.m_Images[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED)
        .Seed(Swapchain.m_Images[idx], VK_IMAGE_ASPECT_COLOR_BIT,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    // Composite renders at DISPLAY resolution (the swapchain image). When DLSS
    // upscales, ctx.extent is the smaller render res — the base colour comes from the
    // display-res DLSS output (binding 6); the render-res effects (bloom/depth/SSIL/
    // vol) are sampled with normalised UVs, so they upsample bilinearly.
    // nfs28: outExtent = displayExtent, or the render-res rectangle the Switch display crop stretches (then the
    // composite is 1:1 with the scene: p7 below sees extent == outExt).
    const VkExtent2D outExt = (ctx.outExtent.width && ctx.outExtent.height) ? ctx.outExtent : ctx.displayExtent;
    const int zComp = VK::Prof::ZoneBegin(cmd, "Tonemap/Comp");   // the composite's own pixels (Tonemap/Mips = the meter chain)
    // 10-09 r_tonemap_probe (timing only, the picture is wrong while it is on): 1 = the composite with a 1x1 scissor (no
    // pixels -> what the pixels cost), 2 = into a private image of the swapchain's format (is it the scanout target?).
    VkImageView compTarget = Swapchain.m_ImageViews[idx];
    if (ps_r_tonemap_probe == 2 && EnsureProbeTarget(outExt)) {
        ImageBarrier(cmd, s_probeImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        compTarget = s_probeView;
    }
    VK::RenderingBuilder(outExt)
        .Color(compTarget, VK_ATTACHMENT_LOAD_OP_DONT_CARE)   // fullscreen overwrite
        .BeginPlain(cmd);
    if (ps_r_tonemap_probe == 1) {
        const VkRect2D one{ { 0, 0 }, { 1, 1 } };
        vkCmdSetScissor(cmd, 0, 1, &one);
    }
#ifdef __SWITCH__
    if (g_xr_nx_diag == 110) {   // nfs28: every command sent, ~no fragments -> base minus this = the composite's pixels
        const VkRect2D one{ { 0, 0 }, { 1, 1 } };
        vkCmdSetScissor(cmd, 0, 1, &one);
    }
#endif

#ifdef __SWITCH__
    const bool leanV1 = lean && g_xr_nx_wskip_win == 41 && s_PipelineLeanV1 != VK_NULL_HANDLE;   // nx_statab 41
#else
    const bool leanV1 = false;
#endif
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, leanV1 ? s_PipelineLeanV1 : lean ? s_PipelineLean : s_Pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_PipelineLayout, 0, 1, &s_Set[idx], 0, nullptr);
    // Set 1: the shared EnvLight set (rain map + camera terms for SSR puddles).
    if (VkDescriptorSet env = EnvLight::GetCurrentSet())
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_PipelineLayout, 1, 1, &env, 0, nullptr);

    TonemapPush push{};
    push.p0[0] = kWhitePoint;
    push.p0[1] = float(SceneColor::MipLevels() - 1);   // top-mip LOD = whole-frame average
    push.p0[2] = MiddleGray();
    push.p0[3] = kLowLum;
    push.p1[0] = ExpMin(); push.p1[1] = ExpMax(); push.p1[2] = ExpComp();
    // Bloom was forced OFF in the editor so the grid and gizmo lines would stay
    // crisp instead of glowing. That reason no longer holds: both now draw
    // POST-tonemap (EditorOverlay::ExecutePostTonemap), where bloom cannot reach
    // them. The gate only cost the scene its highlight glow and made the viewport
    // read flatter than the game — the opposite of what a WYSIWYG viewport is for.
    push.p1[3] = _max(0.f, _min(2.f, ps_r_blm));   // r_blm; kBloomIntensity is its shipped default
    // R4 CDL grading (ACES_settings.h): Slope = r2_img_exposure, Power =
    // 2*(1 - r2_img_cg), Saturation = r2_img_saturation, gamma from
    // img_corrections — all live console knobs (ssfx_exposure/_gamma/
    // _saturation/ssfx_color_grading), neutral by default.
    push.p2[0] = ps_r2_img_exposure;
    push.p2[1] = ps_r2_img_saturation;
    // DISPLAY ENCODE vs ARTISTIC TRIM. These are genuinely two different things, but
    // whether the encode is OWED depends on the pipeline feeding it, and this engine
    // runs TWO of them.
    //
    // GAME PATH — gamma space, encode already paid. Albedo is never linearized on the
    // way in: DXGIFormatToVk (vk_texture.cpp) maps every _SRGB DDS variant to its UNORM
    // VkFormat, and the legacy BC1/2/3 path does the same, so the sampler hands the
    // shader gamma-encoded values and no hardware sRGB decode happens anywhere. The
    // lighting math then runs on those, and the whole look — sun boost, ambient floor,
    // the Reinhard white point, the exposure constants above — is tuned around it. The
    // frame that reaches this push is therefore ALREADY display-referred. Applying an
    // OETF here is a SECOND encode: mids lift by roughly 2x and the picture blows out.
    // That is also why ChooseSurfaceFormat deliberately prefers B8G8R8A8_UNORM over the
    // _SRGB surface (see its comment) — the same double-gamma, one layer lower.
    //
    // EDITOR PATH — genuinely linear, encode owed. The host scene is lit by real linear
    // sky ambient / IBL / GTAO with no gamma-space lightmaps, so at 1.0 it renders dark
    // and does need the 2.2.
    //
    // So the -vk_editor term is not a band-aid: it selects between two color pipelines.
    // Collapsing it into an unconditional 2.2 is what made the GAME too bright.
    // pow(pow(c, 1/display), 1/user) == pow(c, 1/(display*user)) — one exponent, shader
    // unchanged. hwSrgb stays honored: if we ever land on an _SRGB surface the hardware
    // owns the encode and we must not double it.
    // Under r_linear_color the GAME joins the editor on the linear side: albedo arrives
    // sRGB-decoded, light/env colours are linearised on upload, so the frame reaching
    // here is finally scene-referred linear and DOES owe the display an OETF. That is
    // what makes this one exponent correct in both modes rather than a special case:
    //   linear pipeline  -> encode (2.2)
    //   gamma pipeline   -> pass through (1.0), because the encode was never undone
    // and hwSrgb still wins over both, since then the surface hardware encodes for us.
    //
    // Kept as a single pow exponent (not the piecewise sRGB curve) so it still composes
    // with the artistic ssfx_gamma trim: pow(pow(c,1/d),1/u) == pow(c,1/(d*u)). The two
    // differ only in the deepest shadows; swapping in the piecewise curve is an L-3
    // refinement, and it would have to give up that composition.
    const bool  isEditor     = Core.Params && strstr(Core.Params, "-vk_editor");
    const bool  hwSrgb       = IsSrgbFormat(Swapchain.m_Format);
    const bool  wantEncode   = ColorSpace::Active() || isEditor;
    const float displayGamma = (wantEncode && !hwSrgb) ? 2.2f : 1.0f;
    const float userGamma    = (ps_r2_img_gamma > 0.05f) ? ps_r2_img_gamma : 1.0f;
    push.p2[2] = 1.0f / (displayGamma * userGamma);
    // Heat-haze: scale 0 until the distort RT exists (binding 2 then holds the
    // scene-view fallback and the shader skips the sample entirely).
    push.p2[3] = (ParticlePass::GetDistortView() != VK_NULL_HANDLE) ? kDistortAmount : 0.0f;
    push.p3[0] = 2.0f * (1.0f - ps_r2_img_cg.x);
    push.p3[1] = 2.0f * (1.0f - ps_r2_img_cg.y);
    push.p3[2] = 2.0f * (1.0f - ps_r2_img_cg.z);
    push.p3[3] = ps_r_dither;   // output TPDF dither (LSBs) — de-bands the 8-bit swapchain write
    // Volumetric composite (r_vol): mode + the exp-Z grid params from the LAST
    // Vol::Execute (the EXACT inverse vol_inject used). 0 off / 1 composite / 2 debug.
    const Vol::GridZParams gz = Vol::GetGridZ();
    // This used to carry a `&& !s_editorTM` term: with no level the froxel volume was
    // never integrated (inject/integrate live in Pass_World), so uVolume held
    // transmittance 0 and the composite (scene*T + inscatter) absorbed the whole lit
    // scene to black. That was a band-aid over the missing integrate, and it cost the
    // editor its aerial perspective. Pass_EditorDynamics now drives Vol::Execute, so
    // the composite is fed real data and the term is gone — if fog ever comes back
    // black in the editor, check that Vol::Execute actually ran, don't re-add a gate.
    const bool volOn = (ps_r_vol != 0 || ps_r_vol_debug != 0) && Vol::Ready();
    push.p4[0] = volOn ? (ps_r_vol_debug ? 2.0f : 1.0f) : 0.0f;
    push.p4[1] = gz.nearZ;
    push.p4[2] = gz.farZ;
    push.p4[3] = gz.logFarNear;
    // nfs29: the lean shader reads p4.x as "full grade forced" (it has no volume composite), so it must not inherit the
    // volume flag above -- with r_vol on it would only cost the skipped pow()s, same picture. nx_diag 115 (Switch)
    // prices the identity-term skip against the windows beside it.
#ifdef __SWITCH__
    if (lean) push.p4[0] = (g_xr_nx_diag == 115) ? 1.0f : 0.0f;
#else
    if (lean) push.p4[0] = 0.0f;
#endif
    // SSIL (r_ssil): add the half-res indirect-light buffer (binding 5) as
    // coloured bounce. Enabled only once the gather produced a result (its view
    // is non-null); otherwise binding 5 holds the scene-view fallback and the
    // shader skips it. x = strength, y = debug (show only the bounce), z = enable.
    // SSIL is applied in the forward shaders now; the tonemap only shows the debug
    // view (r_ssil_debug) of the raw IL buffer (binding 5). p5.y = that flag.
    const bool ilReady = ps_r_ssil_enable != 0 && SSAOPass::GetILResultView() != VK_NULL_HANDLE;
    push.p5[0] = s_expValid ? s_exposure : 0.0f;   // CPU-smoothed exposure (0 = shader computes it)
    // Publish the same exposure to DLSS (r_dlss_exp real-exposure path). Next frame's
    // Evaluate (which runs before this pass) reads it — one frame stale is fine, the
    // value eases over seconds.
    VK::Dlss::SetExposureHint(s_expValid ? s_exposure : 1.0f);
    push.p5[1] = (ilReady && ps_r_ssil_debug) ? 1.0f : 0.0f;
#ifdef __SWITCH__
    push.p5[2] = (lean && g_xr_nx_diag == 111) ? 1.0f : 0.0f;   // nfs28: tonemap_lean_v2 bare resample probe
#else
    push.p5[2] = 0.0f;
#endif
    // r_vsm_debug_dyn: red-tint pixels shadowed by the VSM dynamic atlas (mask.B,
    // set 1 binding 14 — the EnvLight set is already bound; dummy-safe when off).
    push.p5[3] = (ps_r_vsm_debug_dyn && VSM::MaskReady()) ? 1.0f : 0.0f;
    // God-ray GROUND SPLASH (r_sun_beam_splash): boost the in-scatter where a bright
    // shaft lands on darker ground so the beam "разбивается о терейн" instead of
    // dissolving (see tonemap.frag). Needs the volume, so off unless the composite runs.
    push.p6[0] = volOn ? ps_r_sun_beam_splash : 0.0f;   // strength (0 = off)
    push.p6[1] = ps_r_sun_beam_splash_thr;              // in-scatter luminance threshold
    // CAS sharpen on the DLSS-resolved colour (only when DLSS actually resolved this
    // frame — sharpening the plain scene would double up with its native mips).
    push.p6[2] = resolvedUp ? ps_r_dlss_sharp : 0.0f;
    // DLSS debug view (r_dlss_debug): magnitude = mode, SIGN = whether the composite
    // actually reads the DLSS output this frame (the gate the shader can't see).
    push.p6[3] = resolvedUp ? float(ps_r_dlss_debug) : -float(ps_r_dlss_debug);
    // ---- Volume RECONSTRUCTION (r_vol_upsample) ----------------------------------
    // The froxel grid is 256×144 — roughly 7-8 screen pixels per froxel — and the
    // composite resolved it with ONE trilinear tap plus a screen-STATIC dither. With
    // the temporal accumulation off that reads as raw 256×144 upscale ("PS1 pixels");
    // with it on, the grid hides but boils. A rotated 4-tap reconstruction filter
    // costs 3 extra 3D taps and removes the blockiness the volume can never carry.
    push.p7[0] = volOn ? float(ps_r_vol_upsample) : 0.0f;
    // Per-frame rotation of the sampling pattern — ONLY when a temporal upscaler
    // resolves this frame. Under DLSS a rotating pattern is free supersampling (it
    // averages the offsets across frames); a STATIC one is the opposite — DLSS reads
    // the fixed grain as real detail and preserves it. Without an upscaler an
    // animated pattern would just crawl, so it stays frozen.
    push.p7[1] = resolvedUp ? float(RV().frameIndex & 63u) : 0.0f;
    // r_render_scale < 1 without an upscaler: the resolved colour is render-res, the composite runs at display-res.
    const bool scaledNoUp = !resolvedUp && (ctx.extent.width != outExt.width || ctx.extent.height != outExt.height);
    push.p7[2] = scaledNoUp ? 1.0f / float(outExt.width)  : 0.0f;
    push.p7[3] = scaledNoUp ? 1.0f / float(outExt.height) : 0.0f;
    // lean: the scene and the target are 1:1 (no upscaler, no render scale) -> the shader reads the texel itself
    // (texelFetch, no bilinear RGBA16F tap). r_tonemap_probe 3 = the filtered read, for the A/B.
    if (lean) push.p4[1] = (!scaledNoUp && !resolvedUp && ps_r_tonemap_probe != 3) ? 1.0f : 0.0f;
    // One-shot gate log on every state change — pairs with r_dlss_debug 3.
    {
        static int s_lastResolved = -1;
        if (int(resolvedUp) != s_lastResolved) {
            s_lastResolved = int(resolvedUp);
            Msg("[VK DLSS] tonemap composite: base = %s (sharp %.2f, dlss view %p)",
                resolvedUp ? "DLSS output" : "scene colour", ps_r_dlss_sharp, (void*)dlssOutView);
        }
    }
    vkCmdPushConstants(cmd, s_PipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    VK::Prof::ZoneEnd(cmd, zComp);
    // Swapchain stays COLOR_ATTACHMENT for the UI pass; End brings it to PRESENT.
}

}  // namespace VK
