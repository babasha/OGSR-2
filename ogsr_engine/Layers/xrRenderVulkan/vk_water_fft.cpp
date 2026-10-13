// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — TESSENDORF FFT OCEAN. See vk_water_fft.h.

#include "stdafx.h"
#include "vk_env_snapshot.h"
#include "vk_render_view.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_water_fft.h"
#include "vk_image.h"
#include "vk_shaders.h"
#include "vk_barriers.h"
#include "vk_compute_util.h"
#include "HW_Vulkan.h"
#include "../../xr_3da/device.h"
#include "../../xr_3da/IGame_Persistent.h"   // Environment() — the wind that drives the spectrum
#include <algorithm>
#include <cmath>

extern int   ps_r_wtr;             // the water master switch: no water, no point transforming one
extern int   ps_r_wtr_fft;         // master enable for this module
extern float ps_r_wtr_fft_size;    // largest cascade's tile, metres
extern float ps_r_wtr_fft_ratio;   // per-cascade shrink
extern float ps_r_wtr_fft_wind;    // BASE wind speed (m/s) — see the note in Dispatch
extern float ps_r_wtr_fft_amp;     // Phillips A
extern float ps_r_wtr_fft_chop;    // horizontal displacement gain (lambda)
extern float ps_r_wtr_fft_depth;   // sea depth for the dispersion relation, metres
extern float ps_r_wtr_fft_repeat;  // the whole field loops exactly this often, seconds
extern float ps_r_wtr_fft_small;   // capillary cutoff, metres
extern float ps_r_wtr_fft_dir;     // how sharply the spectrum favours the wind direction

namespace VK { namespace WaterFFT {

namespace {

constexpr VkFormat kFmtH0    = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat kFmtSpec  = VK_FORMAT_R32G32_SFLOAT;
constexpr VkFormat kFmtOut   = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr u32      kSpecLayers = kCascades * 2;   // (h + i*Dx) and (Dz) per cascade

// ⚠ SCALARS ONLY, and they must match the shader push blocks field for field. A
// vec2 here is 8-byte aligned in std430 and shifts everything behind it relative
// to the C++ struct — this renderer lost a working feature to exactly that once,
// silently, for days. Two floats instead of a vec2 costs nothing and cannot lie.
struct PushH0 {
    float base, ratio, windX, windZ, amp, smallCut, dirPow, seed;
    s32   cascades;
};
struct PushSpec {
    float base, ratio, time, repeat, depth;
    s32   cascades;
};
struct PushFFT {
    s32 axis;
};
struct PushAsm {
    float base, ratio, chop;
    s32   cascades;
};
static_assert(sizeof(PushH0) == 36, "must match water_fft_h0.comp.glsl");
static_assert(sizeof(PushSpec) == 24, "must match water_fft_spectrum.comp.glsl");
static_assert(sizeof(PushAsm) == 16, "must match water_fft_assemble.comp.glsl");

bool s_inited = false, s_failed = false;
bool s_first  = true;      // images still UNDEFINED
bool s_ready  = false;     // a full transform has landed in uDisp

VkImage       s_h0    = VK_NULL_HANDLE, s_spec  = VK_NULL_HANDLE;
VkImage       s_disp  = VK_NULL_HANDLE, s_deriv = VK_NULL_HANDLE;
VmaAllocation s_h0A{}, s_specA{}, s_dispA{}, s_derivA{};
VkImageView   s_h0V   = VK_NULL_HANDLE, s_specV  = VK_NULL_HANDLE;
VkImageView   s_dispV = VK_NULL_HANDLE, s_derivV = VK_NULL_HANDLE;
VkSampler     s_sampler = VK_NULL_HANDLE;

VkDescriptorSetLayout s_setLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_pool      = VK_NULL_HANDLE;
VkDescriptorSet       s_set       = VK_NULL_HANDLE;
VkPipelineLayout      s_layout    = VK_NULL_HANDLE;
VkPipeline            s_pipeH0 = VK_NULL_HANDLE, s_pipeSpec = VK_NULL_HANDLE;
VkPipeline            s_pipeFFT = VK_NULL_HANDLE, s_pipeAsm = VK_NULL_HANDLE;

// The parameters h0 was last built for. h0 does not depend on time, so it is
// rebuilt only when one of these actually moves — which in practice is a few
// times a second as the weather turns the wind, not every frame.
PushH0 s_h0Built{};
bool   s_h0Valid = false;

float s_base = 250.f, s_ratio = 0.175f;

bool Same(const PushH0& a, const PushH0& b)
{
    // NOT `near` — that is a windef.h macro (the old MS-DOS memory-model keyword)
    // and expands to nothing, which turns the declaration into a bare `auto =`.
    auto eq = [](float x, float y, float eps) { return std::fabs(x - y) <= eps; };
    return eq(a.base, b.base, 0.01f) && eq(a.ratio, b.ratio, 1e-4f)
        && eq(a.windX, b.windX, 0.004f) && eq(a.windZ, b.windZ, 0.004f)
        && eq(a.amp, b.amp, 1e-9f) && eq(a.smallCut, b.smallCut, 1e-4f)
        && eq(a.dirPow, b.dirPow, 1e-3f) && a.cascades == b.cascades;
}

bool MakeImage(VkFormat fmt, u32 layers, VkImageUsageFlags usage, const char* name,
               VkImage& img, VmaAllocation& alloc, VkImageView& view)
{
    ImageDesc d{};
    d.format = fmt;
    d.extent = { kTexels, kTexels, 1 };
    d.usage  = usage;
    d.layers = layers;
    d.name   = name;
    if (!CreateImage(d, img, alloc)) return false;
    view = CreateImageView(img, fmt, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                           VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers);
    return view != VK_NULL_HANDLE;
}

bool Init()
{
    if (s_inited) return !s_failed;
    s_inited = true;
    if (!g_ShaderManager) { s_failed = true; return false; }

    VkShaderModule csH0   = g_ShaderManager->Load("water_fft_h0.comp.spv");
    VkShaderModule csSpec = g_ShaderManager->Load("water_fft_spectrum.comp.spv");
    VkShaderModule csFFT  = g_ShaderManager->Load("water_fft_fft.comp.spv");
    VkShaderModule csAsm  = g_ShaderManager->Load("water_fft_assemble.comp.spv");
    if (csH0 == VK_NULL_HANDLE || csSpec == VK_NULL_HANDLE ||
        csFFT == VK_NULL_HANDLE || csAsm == VK_NULL_HANDLE) {
        Msg("![VK WaterFFT] compute shader(s) missing — spectral waves off, analytic swell only");
        s_failed = true; return false;
    }

    const VkImageUsageFlags kWork = VK_IMAGE_USAGE_STORAGE_BIT;
    // The two OUTPUTS are also SAMPLED — by the vertex, tessellation and fragment
    // stages of the water. They stay in GENERAL for their whole life: a storage
    // image the graphics stages sample needs no transition, and the pass binds
    // them with GENERAL to match (the same arrangement the ripple field uses).
    // TRANSFER_DST as well: they are cleared on first use, not just transitioned.
    const VkImageUsageFlags kOut  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                  | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (!MakeImage(kFmtH0,   kCascades,   kWork, "WaterFFT.H0",    s_h0,    s_h0A,    s_h0V)   ||
        !MakeImage(kFmtSpec, kSpecLayers, kWork, "WaterFFT.Spec",  s_spec,  s_specA,  s_specV) ||
        !MakeImage(kFmtOut,  kCascades,   kOut,  "WaterFFT.Disp",  s_disp,  s_dispA,  s_dispV) ||
        !MakeImage(kFmtOut,  kCascades,   kOut,  "WaterFFT.Deriv", s_deriv, s_derivA, s_derivV)) {
        s_failed = true; return false;
    }

    // ⚠ REPEAT, not CLAMP. Every cascade is a PERIODIC tile — that is the whole
    // premise — and clamping would freeze the field into a single stretched tile
    // with three hard seams across the level.
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_sampler) != VK_SUCCESS) {
        s_failed = true; return false;
    }

    // ONE set layout for all four stages: each shader declares the slots it needs
    // at the slot that resource lives in. A pipeline layout is allowed to name
    // bindings its shader never touches, and one set beats four almost-identical
    // ones — there is no per-stage state here to get out of step.
    // ⚠ SIZE THE ARRAY WITH THE BINDINGS — and the pool, and the writes. This
    // project has three separate stack smashes on record from exactly that.
    constexpr auto kImg = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (!VK::MakeDescriptorSets({ kImg, kImg, kImg, kImg }, 1, s_setLayout, s_pool, &s_set,
                                VK_SHADER_STAGE_COMPUTE_BIT, "Water.FFT")) {
        s_failed = true; return false;
    }

    VK::DescriptorWriter(s_set)
        .StorageImage(0, s_h0V)
        .StorageImage(1, s_specV)
        .StorageImage(2, s_dispV)
        .StorageImage(3, s_derivV)
        .Flush();

    // One layout for all four, sized to the LARGEST push block. A shader that
    // declares fewer bytes than the range is legal; the reverse is not.
    s_layout = MakePipelineLayout({ s_setLayout }, sizeof(PushH0));
    if (s_layout == VK_NULL_HANDLE) { s_failed = true; return false; }

    s_pipeH0   = CreateComputePipeline(csH0,   s_layout, "Water.FFT.H0");
    s_pipeSpec = CreateComputePipeline(csSpec, s_layout, "Water.FFT.Spectrum");
    s_pipeFFT  = CreateComputePipeline(csFFT,  s_layout, "Water.FFT.Transform");
    s_pipeAsm  = CreateComputePipeline(csAsm,  s_layout, "Water.FFT.Assemble");
    if (!s_pipeH0 || !s_pipeSpec || !s_pipeFFT || !s_pipeAsm) { s_failed = true; return false; }

    Msg("[VK WaterFFT] init OK — %u tile, %u cascades, %u FFT lines/frame",
        kTexels, kCascades, kTexels * kSpecLayers * 2);
    return true;
}

void ComputeBarrier(VkCommandBuffer cmd)
{
    MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
}

} // namespace

bool EnsureCreated() { return Init(); }

float CascadeLen(u32 c)
{
    return s_base * std::pow(s_ratio, (float)c);
}

void Dispatch(VkCommandBuffer cmd)
{
    s_ready = false;
    if (cmd == VK_NULL_HANDLE) return;
    if (!Init()) return;

    // ⚠ THE IMAGES ARE ALLOCATED AND TRANSITIONED EVEN WHEN THE FEATURE IS OFF,
    // and that is deliberate. The water pass binds these two views into the same
    // descriptor set it uses for everything else, and the shaders declare them as
    // sampler2DArray — there is no valid 2D-ARRAY image lying around to point a
    // disabled slot at, and pointing it at the 2D depth image (the trick the other
    // optional slots use) is a view-type mismatch, not a harmless dummy. Eight
    // megabytes to keep every descriptor legal in every state is the cheap side of
    // that trade. r_wtr_fft 0 still costs nothing per frame, which is the point of
    // the switch.
    if (s_first) {
        s_first = false;
        ImageBarrier(cmd, s_h0,   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_ASPECT_COLOR_BIT, kCascades);
        ImageBarrier(cmd, s_spec, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_ASPECT_COLOR_BIT, kSpecLayers);
        // The two OUTPUTS are CLEARED, not merely transitioned. They are sampled
        // by the water shaders from the very first frame the pass runs, and with
        // the feature off nothing ever writes them — an undefined image is not a
        // theoretical hazard here, it is a screenful of garbage displacement one
        // console command away. (The ripple sim learned this the expensive way:
        // its uninitialised field read as "every puddle in the level is already
        // rippling" and survived for the better part of a minute.)
        VkClearColorValue zero{};
        VkImageSubresourceRange rng{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kCascades };
        for (VkImage img : { s_disp, s_deriv }) {
            ImageBarrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_ASPECT_COLOR_BIT, kCascades);
            vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rng);
            ImageBarrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_ASPECT_COLOR_BIT, kCascades);
        }
        s_h0Valid = false;
    }

    if (!ps_r_wtr || !ps_r_wtr_fft) return;      // images stay valid and stay zero

    s_base  = std::clamp(ps_r_wtr_fft_size,  32.f, 2000.f);
    s_ratio = std::clamp(ps_r_wtr_fft_ratio, 0.05f, 0.6f);

    // ⚠ THE GAME'S WIND IS ZERO MOST OF THE DAY. This is measured, not assumed:
    // the water pass logs `wind vel=0` for the majority of the day/night cycle on
    // this content, and an earlier version of the analytic swell was effectively
    // dead because of it. A spectrum keyed straight off wind_velocity would be a
    // flat mirror the same way. So r_wtr_fft_wind is a BASE the weather modulates
    // around, not a scale the weather can zero.
    float windX = 1.f, windZ = 0.f, gust = 0.f;
    if (g_pGamePersistent) {
        auto& env = ES();
        if (env.CurrentEnv) {
            const float a = env.CurrentEnv->wind_direction;
            windX = std::cos(a); windZ = std::sin(a);
            gust  = std::clamp(env.CurrentEnv->wind_velocity * 0.001f, 0.f, 1.f);
        }
    }
    const float len = std::sqrt(windX * windX + windZ * windZ);
    if (len < 1e-3f) { windX = 1.f; windZ = 0.f; }
    else             { windX /= len; windZ /= len; }
    // ⚠ A NARROW BAND ON PURPOSE (0.85x .. 1.15x), and this is the second half of
    // the same measurement the default wind speed came from. Energy in a Phillips
    // spectrum goes as about V^4: the obvious 0.65..1.55 modulation swung the
    // significant wave height from 7 cm to 2.2 m over one ordinary day — a mirror
    // at dawn and an ocean by noon. Squeezed to this, the same weather range is
    // 0.29 m to 0.75 m, which reads as calm-versus-breezy and nothing worse.
    const float V = std::clamp(ps_r_wtr_fft_wind, 0.5f, 40.f) * (0.85f + 0.30f * gust);

    const u32 grid2D = (kTexels + 15) / 16;

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_layout, 0, 1, &s_set, 0, nullptr);

    // ---- stage 1: h0, only when the wind or the tuning has actually moved ----
    PushH0 h0{};
    h0.base = s_base; h0.ratio = s_ratio;
    h0.windX = windX * V; h0.windZ = windZ * V;
    h0.amp      = std::max(ps_r_wtr_fft_amp, 0.f) * 1e-6f;   // A is a tiny number; keep the cvar human-sized
    h0.smallCut = std::clamp(ps_r_wtr_fft_small, 0.001f, 5.f);
    h0.dirPow   = std::clamp(ps_r_wtr_fft_dir, 0.f, 12.f);
    h0.seed     = 1.0f;                                      // fixed: the sea must not reshuffle on a reload
    h0.cascades = (s32)kCascades;
    if (!s_h0Valid || !Same(h0, s_h0Built)) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeH0);
        vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(h0), &h0);
        vkCmdDispatch(cmd, grid2D, grid2D, kCascades);
        ComputeBarrier(cmd);
        s_h0Built = h0; s_h0Valid = true;
    }

    // ---- stage 2: evolve to now and pack -----------------------------------
    PushSpec sp{};
    sp.base = s_base; sp.ratio = s_ratio;
    sp.time     = RV().fTimeGlobal;
    sp.repeat   = std::clamp(ps_r_wtr_fft_repeat, 10.f, 3600.f);
    sp.depth    = std::clamp(ps_r_wtr_fft_depth, 0.5f, 500.f);
    sp.cascades = (s32)kCascades;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeSpec);
    vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sp), &sp);
    vkCmdDispatch(cmd, grid2D, grid2D, kCascades);
    ComputeBarrier(cmd);

    // ---- stage 3: the transform, rows then columns -------------------------
    // One workgroup per line: x = which line, y = which layer.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeFFT);
    for (s32 axis = 0; axis < 2; ++axis) {
        PushFFT pf{ axis };
        vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pf), &pf);
        vkCmdDispatch(cmd, kTexels, kSpecLayers, 1);
        ComputeBarrier(cmd);
    }

    // ---- stage 4: unpack into what the water shaders sample -----------------
    PushAsm pa{};
    pa.base = s_base; pa.ratio = s_ratio;
    pa.chop = std::clamp(ps_r_wtr_fft_chop, 0.f, 3.f);
    pa.cascades = (s32)kCascades;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeAsm);
    vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pa), &pa);
    vkCmdDispatch(cmd, grid2D, grid2D, kCascades);

    // The graphics stages that sample this must wait for the store.
    MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,   VK_ACCESS_2_SHADER_READ_BIT);
    s_ready = true;
}

bool        Ready()      { return s_ready; }
VkImageView DispView()   { return s_dispV; }
VkImageView DerivView()  { return s_derivV; }
VkSampler   GetSampler() { return s_sampler; }

void Destroy()
{
    if (!s_inited || VulkanHW.m_Device == VK_NULL_HANDLE) return;
    VkDevice dev = VulkanHW.m_Device;
    auto killPipe = [&](VkPipeline& p) { if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; } };
    killPipe(s_pipeH0); killPipe(s_pipeSpec); killPipe(s_pipeFFT); killPipe(s_pipeAsm);
    if (s_layout)    { vkDestroyPipelineLayout(dev, s_layout, nullptr);      s_layout = VK_NULL_HANDLE; }
    if (s_pool)      { vkDestroyDescriptorPool(dev, s_pool, nullptr);        s_pool = VK_NULL_HANDLE; s_set = VK_NULL_HANDLE; }
    if (s_setLayout) { vkDestroyDescriptorSetLayout(dev, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    if (s_sampler)   { vkDestroySampler(dev, s_sampler, nullptr);            s_sampler = VK_NULL_HANDLE; }
    auto killImg = [&](VkImage& img, VmaAllocation& a, VkImageView& v) {
        if (v)   { vkDestroyImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
        if (img) { vmaDestroyImage(VulkanHW.m_Allocator, img, a); img = VK_NULL_HANDLE; a = VK_NULL_HANDLE; }
    };
    killImg(s_h0, s_h0A, s_h0V);
    killImg(s_spec, s_specA, s_specV);
    killImg(s_disp, s_dispA, s_dispV);
    killImg(s_deriv, s_derivA, s_derivV);
    s_inited = false; s_failed = false; s_first = true; s_ready = false; s_h0Valid = false;
}

}}  // namespace VK::WaterFFT
