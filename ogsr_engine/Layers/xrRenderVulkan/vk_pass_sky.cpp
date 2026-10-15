// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_env_snapshot.h"
#include "vk_render_view.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_pass_sky.h"
#include "vk_color_space.h"   // ColorSpace::LinearizeRGB — sky/clouds/sun tints are authored sRGB
#include "vk_swapchain.h"
#include "vk_scene_color.h"                // HDR scene target format
#include "vk_shaders.h"
#include "vk_texture.h"
#include "HW_Vulkan.h"
#include "vk_command_buffer.h"             // CommandManager.GetCurrentFrame() — in-flight slot
#include "vk_clouds.h"                     // volumetric cloud noise volumes (bindings 5..7)
#include "vk_buffer.h"                     // CVulkanBuffer — the params UBO
#include "vk_gfx_pipeline.h"               // VK::GfxPipelineBuilder

#include "../../xr_3da/device.h"           // RV().vCameraPosition
#include "../../xr_3da/IGame_Persistent.h" // g_pGamePersistent
#include "../../xr_3da/Environment.h"      // CEnvironment / CEnvDescriptor

#include <unordered_map>
#include <string>

// Cloud cvars (declared in vk_console_min.cpp).
extern int   ps_r_clouds;            // r_clouds — animated cloud layer on/off
extern float ps_r_clouds_intensity;  // r_clouds_intensity — additive brightness multiplier
extern float ps_r_clouds_speed;       // r_clouds_speed — UV scroll speed multiplier
extern int   ps_r_sky_proc;           // r_sky_proc — procedural Rayleigh+Mie sky (0 = legacy cubemap)
extern float ps_r_sky_intensity;      // r_sky_intensity — model units -> game exposure
extern float ps_r_sky_turbidity;      // r_sky_turbidity — aerosol multiplier (Mie)
extern float ps_r_sky_mie_g;          // r_sky_mie_g — Mie anisotropy (sun aureole tightness)
extern float ps_r_fog_dist;           // r_fog_dist -- distance-fog range scale (0 = off)
extern float ps_r_sky_fog_horizon;     // r_sky_fog_horizon -- sky fades to the fog colour below this elevation (0 = off)
// Volumetric clouds (r_clouds_vol) — see the block comment in vk_console_min.cpp.
extern int   ps_r_clouds_vol;
extern float ps_r_clouds_coverage;
extern float ps_r_clouds_density;
extern float ps_r_clouds_detail;
extern float ps_r_clouds_bottom;
extern float ps_r_clouds_top;
extern float ps_r_clouds_shape_scale;
extern float ps_r_clouds_detail_scale;
extern float ps_r_clouds_weather_scale;
extern float ps_r_clouds_wind_dir;
extern float ps_r_clouds_wind_speed;
extern float ps_r_clouds_phase_g;
extern float ps_r_clouds_phase_g_back;
extern float ps_r_clouds_extinction;
extern float ps_r_clouds_powder;
extern float ps_r_clouds_sun;
extern float ps_r_clouds_ambient;
extern int   ps_r_clouds_steps;
extern float ps_r_clouds_max_dist;
extern float ps_r_clouds_cirrus;
extern float ps_r_clouds_cirrus_alt;
extern float ps_r_clouds_cirrus_scale;
extern int   ps_r_clouds_debug;
extern int   ps_r_clouds_weather;

namespace VK {

namespace {
    VkPipeline            s_Pipeline       = VK_NULL_HANDLE;
    VkPipelineLayout      s_PipelineLayout = VK_NULL_HANDLE;
    VkShaderModule        s_VS             = VK_NULL_HANDLE;
    VkShaderModule        s_FS             = VK_NULL_HANDLE;

    constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;

    VkDescriptorSetLayout s_SetLayout      = VK_NULL_HANDLE;
    VkDescriptorPool      s_Pool           = VK_NULL_HANDLE;
    VkDescriptorSet       s_Set[kFramesInFlight] = {};  // one per in-flight slot (no in-place rewrite)
    CVulkanBuffer         s_Ubo;                               // params block (binding 4), one slot per frame
    u8*                   s_UboMapped      = nullptr;
    VkDeviceSize          s_UboStride      = 0;
    // Cloud coverage counters (binding 8, r_clouds_debug). Host-visible on purpose:
    // it is written only while the debug cvar is on, and reading it straight off the
    // mapping avoids a staging copy + fence for what is a once-a-second log line.
    CVulkanBuffer         s_DbgBuf;
    u32*                  s_DbgMapped      = nullptr;
    VkSampler             s_Sampler        = VK_NULL_HANDLE;  // CLAMP — cubemaps (bindings 0,1)
    VkSampler             s_CloudSampler   = VK_NULL_HANDLE;  // REPEAT — tiling clouds (bindings 2,3)

    // Cubemap cache — names are the keys (X-Ray sky_texture_name strings).
    // Lifetime = SkyPass lifetime; freed in Destroy. Lookup is rare so the
    // map cost is negligible.
    std::unordered_map<std::string, CVulkanTexture*> s_CubeCache;

    // 2D cloud-texture cache — keyed by clouds_texture_name. Same lifetime rules.
    std::unordered_map<std::string, CVulkanTexture*> s_Tex2DCache;

    CVulkanTexture* s_FallbackCube  = nullptr;  // 1×1×6 mid-blue
    CVulkanTexture* s_FallbackTex2D = nullptr;  // 1×1 black (no clouds when unbound)

    // What each in-flight slot's descriptor set currently holds. Empty = fallback
    // bound to both bindings. A slot is rewritten only when its own names diverge
    // from CEnv, so the GPU never reads a set mid-rewrite (see EnsureCurrentCubes).
    std::string s_BoundName0[kFramesInFlight];
    std::string s_BoundName1[kFramesInFlight];
    std::string s_BoundCloud0[kFramesInFlight];
    std::string s_BoundCloud1[kFramesInFlight];

    // Push must match shaders/sky.{vert,frag}.glsl exactly. Layout = 4×vec4
    // with manual packing (vec3 + scalar trailing). vec3+float push_constant
    // packing is implementation-defined, so we keep it explicit on both sides.
    // Was a PUSH block. Push constants are guaranteed to only 128 bytes by the Vulkan
    // spec and AMD exposes exactly that; adding the procedural-sky params had already
    // taken this to 144, which would have failed on any AMD GPU and worked here only
    // because the dev machine is NVIDIA (256). The cloud params made that a rout, so
    // it is a UBO now. Layout must match sky_ubo.glsl.
    struct SkyUBO
    {
        float camRightTan[3];  // 12 — vCameraRight * tan(fov/2) * aspect
        float skyRotation;     //  4
        float camUpTan[3];     // 12 — vCameraTop   * tan(fov/2)
        float blendWeight;     //  4
        float camForward[3];   // 12 — vCameraDirection (unit)
        float eyeAltitude;     //  4 — camera height (m), the cloud march origin
        float skyColor[3];     // 12
        float _pad1;           //  4
        float sunDir[3];       // 12 — sun TRAVEL dir (to-sun = -sunDir), for the sun disk
        float _pad2;           //  4
        float sunColor[3];     // 12 — env sun colour (time-of-day)
        float _pad3;           //  4
        float cloudsColor[3];  // 12 — clouds_color tint
        float cloudsWeight;    //  4 — clouds_color.w (intensity / weather cloud weight)
        float cloudTime;       //  4 — fTimeGlobal/10 * r_clouds_speed (UV scroll)
        float cloudEnable;     //  4 — r_clouds (0/1)
        float cloudIntensity;  //  4 — r_clouds_intensity
        float _pad4;           //  4
        // Procedural Rayleigh+Mie sky (r_sky_proc). When enabled the cube/tint/sun
        // fields above are unused by the shader — radiance comes from the model.
        float atmoEnable;      //  4
        float atmoIntensity;   //  4 — r_sky_intensity
        float atmoTurbidity;   //  4 — r_sky_turbidity
        float atmoMieG;        //  4 — r_sky_mie_g
        // ── Volumetric clouds (clouds.glsl reads these as CL.*) ──────────────
        float bandBottom, bandTop, _bandPad0, _bandPad1;
        float coverage, detailStrength, density, cloudDebug;
        float shapeScale, detailScale, weatherScale, _scalePad;
        float windX, windZ, windSpeed, _windPad;
        float phaseG, phaseGBack, extinction, powder;
        float sunStrength, ambStrength, _l2Pad0, _l2Pad1;
        float marchSteps, _marchPad0, marchMaxDist, _marchPad1;
        float cirrusAmount, cirrusAltitude, cirrusScale, _cirrusPad;
        float atmo2Enable, atmo2Intensity, atmo2Turbidity, atmo2MieG;
        float cloudTimeSec, _timePad0, _timePad1, _timePad2;
        // 10-10: the distance fog's colour (linear, as the world gets it in L.fog_color) and the horizon band: the sky
        // fades to it below the band, so the fully fogged far edge of the world meets a sky of its own colour
        float fogColor[3], fogHorizon;
    };
    static_assert(sizeof(SkyUBO) == 144 + 16 * 11, "SkyUBO mismatch with sky_ubo.glsl");

    // Resolve sky_texture_name → "$game_textures$\<name>.dds".
    bool ResolveCubePath(const char* name, string_path& out)
    {
        if (!name || !name[0]) return false;
        string_path leaf;
        xr_sprintf(leaf, "%s.dds", name);
        FS.update_path(out, "$game_textures$", leaf);
        return FS.exist(out);
    }

    CVulkanTexture* CreateFallbackCube()
    {
        auto* tex = xr_new<CVulkanTexture>();
        tex->m_bCubemap    = true;
        tex->m_ArrayLayers = 6;
        tex->Create(1, 1, VK_FORMAT_R8G8B8A8_UNORM, 1);
        if (!tex->IsValid()) { xr_delete(tex); return nullptr; }
        u8 face[24];
        for (u32 i = 0; i < 6; ++i) {
            face[i*4 + 0] = 90; face[i*4 + 1] = 130;
            face[i*4 + 2] = 175; face[i*4 + 3] = 255;
        }
        tex->UploadData(face, sizeof(face));
        return tex;
    }

    // 1×1 black 2D — bound to the cloud slots when a weather has no cloud
    // texture (or clouds are disabled). Sampling returns 0 → no cloud added.
    CVulkanTexture* CreateFallbackTex2D()
    {
        auto* tex = xr_new<CVulkanTexture>();
        tex->Create(1, 1, VK_FORMAT_R8G8B8A8_UNORM, 1);
        if (!tex->IsValid()) { xr_delete(tex); return nullptr; }
        const u8 px[4] = { 0, 0, 0, 0 };
        tex->UploadData(px, sizeof(px));
        return tex;
    }

    // Load (or fetch from cache) a cubemap by `sky_texture_name`. Returns
    // fallback on miss / failure. Reads are cached so we never hit disk twice
    // for the same sky.
    CVulkanTexture* LoadOrGet(const char* name)
    {
        if (!name || !name[0]) return s_FallbackCube;
        auto it = s_CubeCache.find(name);
        if (it != s_CubeCache.end()) return it->second;

        string_path full;
        if (!ResolveCubePath(name, full)) {
            Msg("![VK Sky] sky_texture '%s' not found at $game_textures$ — using fallback", name);
            s_CubeCache.emplace(name, s_FallbackCube);  // negative cache
            return s_FallbackCube;
        }

        auto* tex = xr_new<CVulkanTexture>();
        // Sky cube is Colour: it is sampled both as the visible sky AND, at a blurred
        // high mip, as the hemisphere ambient / specular IBL source — so it feeds the
        // lighting maths directly and must be linear radiance there.
        if (!tex->LoadDDSCubemap(full, /*applyBCSwizzle*/ false, TexColorSpace::Color)) {
            Msg("![VK Sky] LoadDDSCubemap failed: %s", full);
            xr_delete(tex);
            s_CubeCache.emplace(name, s_FallbackCube);
            return s_FallbackCube;
        }

        Msg("[VK Sky] Loaded cubemap: '%s' (%ux%u)", name, tex->GetWidth(), tex->GetHeight());
        s_CubeCache.emplace(name, tex);
        return tex;
    }

    // Load (or fetch from cache) a 2D cloud texture by `clouds_texture_name`.
    // Returns the black fallback on miss / failure (→ no clouds drawn).
    CVulkanTexture* LoadOrGet2D(const char* name)
    {
        if (!name || !name[0]) return s_FallbackTex2D;
        auto it = s_Tex2DCache.find(name);
        if (it != s_Tex2DCache.end()) return it->second;

        string_path full;
        if (!ResolveCubePath(name, full)) {  // same $game_textures$\<name>.dds resolve
            Msg("![VK Sky] clouds_texture '%s' not found — no clouds", name);
            s_Tex2DCache.emplace(name, s_FallbackTex2D);  // negative cache
            return s_FallbackTex2D;
        }

        auto* tex = xr_new<CVulkanTexture>();
        if (!tex->LoadDDS(full, /*applyBCSwizzle*/ false, TexStreamClass::UI,
                          TexColorSpace::Color)) {   // cloud layer albedo
            Msg("![VK Sky] LoadDDS (clouds) failed: %s", full);
            xr_delete(tex);
            s_Tex2DCache.emplace(name, s_FallbackTex2D);
            return s_FallbackTex2D;
        }

        Msg("[VK Sky] Loaded clouds: '%s' (%ux%u)", name, tex->GetWidth(), tex->GetHeight());
        s_Tex2DCache.emplace(name, tex);
        return tex;
    }

    // Bindings 0,1 = sky cubemaps (CLAMP sampler); 2,3 = cloud 2D (REPEAT sampler).
    void WriteSet(VkDescriptorSet set, VkImageView sky0, VkImageView sky1,
                  VkImageView cloud0, VkImageView cloud1)
    {
        const VkImageView views[4]    = { sky0, sky1, cloud0, cloud1 };
        const VkSampler    samplers[4] = { s_Sampler, s_Sampler, s_CloudSampler, s_CloudSampler };

        VK::DescriptorWriter w(set);
        for (u32 i = 0; i < 4; ++i) w.ImageSampler(i, views[i], samplers[i]);
        w.Flush();
    }

    // Bindings 4..7 never change once created: the params UBO slot for this frame
    // index, and the three baked cloud noise fields. Written once at init.
    void WriteStaticSet(u32 slot)
    {
        const VkImageView views[3] = { VK::Clouds::ShapeView(), VK::Clouds::DetailView(), VK::Clouds::WeatherView() };
        const VkSampler   cs       = VK::Clouds::Sampler();

        VK::DescriptorWriter w(s_Set[slot]);
        w.UniformBuffer(4, s_Ubo.GetHandle(), sizeof(SkyUBO), s_UboStride * slot)
         .StorageBuffer(8, s_DbgBuf.GetHandle());
        for (u32 i = 0; i < 3; ++i)
            if (views[i] != VK_NULL_HANDLE && cs != VK_NULL_HANDLE)
                w.ImageSampler(5 + i, views[i], cs);
        w.Flush();
    }

    // Fetch [name0, name1] for the active weather interval. Empty strings
    // when the env subsystem hasn't populated yet.
    void GetCurrentSkyNames(const char** outName0, const char** outName1)
    {
        *outName0 = nullptr;
        *outName1 = nullptr;
        if (!g_pGamePersistent) return;
        auto& env = ES();
        if (env.Current[0] && env.Current[0]->sky_texture_name.size() > 0)
            *outName0 = env.Current[0]->sky_texture_name.c_str();
        if (env.Current[1] && env.Current[1]->sky_texture_name.size() > 0)
            *outName1 = env.Current[1]->sky_texture_name.c_str();
    }

    // Fetch [cloud0, cloud1] for the active weather interval. Empty when env
    // hasn't populated (or a weather with no cloud texture).
    void GetCurrentCloudNames(const char** outName0, const char** outName1)
    {
        *outName0 = nullptr;
        *outName1 = nullptr;
        if (!g_pGamePersistent) return;
        auto& env = ES();
        if (env.Current[0] && env.Current[0]->clouds_texture_name.size() > 0)
            *outName0 = env.Current[0]->clouds_texture_name.c_str();
        if (env.Current[1] && env.Current[1]->clouds_texture_name.size() > 0)
            *outName1 = env.Current[1]->clouds_texture_name.c_str();
    }

    void EnsureCurrentCubes(u32 slot)
    {
        const char* n0 = nullptr;
        const char* n1 = nullptr;
        GetCurrentSkyNames(&n0, &n1);
        const char* cn0 = nullptr;
        const char* cn1 = nullptr;
        GetCurrentCloudNames(&cn0, &cn1);

        // Treat null/empty as "keep current". On the very first call before
        // env is up we leave the fallback bound (s_Bound*[slot] are empty).
        const std::string newN0 = n0 ? std::string(n0) : s_BoundName0[slot];
        const std::string newN1 = n1 ? std::string(n1) : s_BoundName1[slot];
        const std::string newC0 = cn0 ? std::string(cn0) : s_BoundCloud0[slot];
        const std::string newC1 = cn1 ? std::string(cn1) : s_BoundCloud1[slot];
        if (newN0 == s_BoundName0[slot] && newN1 == s_BoundName1[slot] &&
            newC0 == s_BoundCloud0[slot] && newC1 == s_BoundCloud1[slot]) return;

        // No vkDeviceWaitIdle: WaitForFence(slot) in CRender::Begin already proved
        // the GPU finished the previous frame that used s_Set[slot], so rewriting
        // just this slot's set is safe. The other slots keep their old cubemap
        // views (textures are cached, never freed at runtime) until each is
        // refreshed on its own turn — a weather change converges over <=
        // FRAMES_IN_FLIGHT frames, imperceptible against the seconds-long blend.
        CVulkanTexture* t0 = n0 ? LoadOrGet(n0) : s_FallbackCube;
        CVulkanTexture* t1 = n1 ? LoadOrGet(n1) : t0;  // fall back to t0 if no second
        if (!t0) t0 = s_FallbackCube;
        if (!t1) t1 = t0;

        CVulkanTexture* ct0 = cn0 ? LoadOrGet2D(cn0) : s_FallbackTex2D;
        CVulkanTexture* ct1 = cn1 ? LoadOrGet2D(cn1) : ct0;
        if (!ct0) ct0 = s_FallbackTex2D;
        if (!ct1) ct1 = ct0;

        WriteSet(s_Set[slot], t0->GetView(), t1->GetView(), ct0->GetView(), ct1->GetView());
        s_BoundName0[slot]  = newN0;
        s_BoundName1[slot]  = newN1;
        s_BoundCloud0[slot] = newC0;
        s_BoundCloud1[slot] = newC1;
    }
}

namespace SkyPass {

bool AcquireAmbientCubes(VkImageView& v0, VkImageView& v1, VkSampler& sampler, float& weight)
{
    // Sky system not up yet (no sampler / fallback) — caller keeps its fallback.
    if (s_Sampler == VK_NULL_HANDLE || !s_FallbackCube) return false;

    const char* n0 = nullptr;
    const char* n1 = nullptr;
    GetCurrentSkyNames(&n0, &n1);

    // Same cached load the sky draw uses — first sight of a sky triggers the
    // synchronous DDS load + mip-gen; cached forever after, so this is cheap.
    CVulkanTexture* t0 = n0 ? LoadOrGet(n0) : s_FallbackCube;
    CVulkanTexture* t1 = n1 ? LoadOrGet(n1) : t0;
    if (!t0) t0 = s_FallbackCube;
    if (!t1) t1 = t0;

    v0      = t0->GetView();
    v1      = t1->GetView();
    sampler = s_Sampler;
    weight  = 0.0f;
    if (g_pGamePersistent)
        if (auto* mix = ES().CurrentEnv)
            weight = mix->weight;
    return true;
}

bool Init()
{
    if (s_Pipeline) return true;

    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    s_VS = g_ShaderManager->Load("sky.vert.spv");
    s_FS = g_ShaderManager->Load("sky.frag.spv");
    if (s_VS == VK_NULL_HANDLE || s_FS == VK_NULL_HANDLE) {
        Msg("![VK Sky] Failed to load sky shaders");
        return false;
    }

    // Set 0: 0+1 = sky cubemaps, 2+3 = legacy cloud 2D textures, 4 = the params UBO
    // (VS+FS — the VS needs the camera basis), 5+6 = volumetric cloud noise volumes,
    // 7 = cloud weather map.
    {
        VkDescriptorSetLayoutBinding b[9]{};
        for (int i = 0; i < 9; ++i) {
            b[i].binding         = i;
            b[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].descriptorCount = 1;
            b[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        b[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[4].stageFlags     = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;   // cloud debug counters
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 9;
        lci.pBindings    = b;
        if (vkCreateDescriptorSetLayout(VulkanHW.m_Device, &lci, nullptr, &s_SetLayout) != VK_SUCCESS) {
            Msg("![VK Sky] CreateDescriptorSetLayout failed");
            return false;
        }
    }

    {
        VkDescriptorPoolSize ps[3]{};
        ps[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ps[0].descriptorCount = 7 * kFramesInFlight;  // 7 image bindings × FRAMES_IN_FLIGHT
        ps[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ps[1].descriptorCount = 1 * kFramesInFlight;  // the params UBO
        ps[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ps[2].descriptorCount = 1 * kFramesInFlight;  // cloud debug counters

        VkDescriptorPoolCreateInfo pci{};
        pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets       = kFramesInFlight;
        pci.poolSizeCount = 3;
        pci.pPoolSizes    = ps;
        if (vkCreateDescriptorPool(VulkanHW.m_Device, &pci, nullptr, &s_Pool) != VK_SUCCESS) {
            Msg("![VK Sky] CreateDescriptorPool failed");
            return false;
        }
    }

    {
        VkDescriptorSetLayout layouts[kFramesInFlight];
        for (u32 i = 0; i < kFramesInFlight; ++i) layouts[i] = s_SetLayout;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = s_Pool;
        ai.descriptorSetCount = kFramesInFlight;
        ai.pSetLayouts        = layouts;
        if (vkAllocateDescriptorSets(VulkanHW.m_Device, &ai, s_Set) != VK_SUCCESS) {
            Msg("![VK Sky] AllocateDescriptorSets failed");
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
            Msg("![VK Sky] CreateSampler failed");
            return false;
        }

        // Cloud sampler — REPEAT so the scrolling cloud UVs tile seamlessly.
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_CloudSampler) != VK_SUCCESS) {
            Msg("![VK Sky] CreateSampler (clouds) failed");
            return false;
        }
    }

    s_FallbackCube = CreateFallbackCube();
    if (!s_FallbackCube) {
        Msg("![VK Sky] Failed to create fallback cubemap");
        return false;
    }
    s_FallbackTex2D = CreateFallbackTex2D();
    if (!s_FallbackTex2D) {
        Msg("![VK Sky] Failed to create fallback 2D texture");
        return false;
    }
    for (u32 i = 0; i < kFramesInFlight; ++i)
        WriteSet(s_Set[i], s_FallbackCube->GetView(), s_FallbackCube->GetView(),
                 s_FallbackTex2D->GetView(), s_FallbackTex2D->GetView());

    // Params UBO: one aligned slot per in-flight frame, host-visible and persistently
    // mapped (rewritten every frame, so device-local + staging would be pure overhead).
    {
        // Round to 256: the max minUniformBufferOffsetAlignment any desktop GPU asks
        // for. Binding a slot at a misaligned offset is undefined behaviour — the same
        // trap vk_env_light documents (it read garbage on alternate frames).
        s_UboStride = (sizeof(SkyUBO) + 255) & ~VkDeviceSize(255);
        s_Ubo.Create(s_UboStride * kFramesInFlight, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        s_UboMapped = static_cast<u8*>(s_Ubo.Map());
        if (!s_UboMapped) { Msg("![VK Sky] params UBO map failed"); return false; }

        s_DbgBuf.Create(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        s_DbgMapped = static_cast<u32*>(s_DbgBuf.Map());
    }

    // Volumetric cloud noise. The views must exist even if the bake failed — the FS
    // declares sampler3D bindings and nothing else can legally fill them.
    VK::Clouds::Init();

    for (u32 i = 0; i < kFramesInFlight; ++i)
        WriteStaticSet(i);

    // Pipeline layout — set 0 only; the params block is a UBO now (see SkyUBO).
    VkPipelineLayoutCreateInfo plci{};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &s_SetLayout;
    plci.pushConstantRangeCount = 0;
    plci.pPushConstantRanges    = nullptr;
    if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_PipelineLayout) != VK_SUCCESS) {
        Msg("![VK Sky] CreatePipelineLayout failed");
        return false;
    }

    // Procedural dome: no vertex input, depth-tested against the world but never
    // written to.
    s_Pipeline = GfxPipelineBuilder(s_PipelineLayout)
        .Vert(s_VS).Frag(s_FS)
        .Depth(true, false)
        .Color(VK::SceneColor::Format())
        .DepthTarget(Swapchain.m_DepthFormat)
        .Build("Sky");
    if (s_Pipeline == VK_NULL_HANDLE)
        return false;

    Msg("[VK Sky] Init OK (two-cubemap blend, sky_rotation, sky_color tint)");
    return true;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;

    for (auto& kv : s_CubeCache) {
        // Negative-cache entries point at the fallback — skip them.
        if (kv.second && kv.second != s_FallbackCube) {
            kv.second->Destroy();
            xr_delete(kv.second);
        }
    }
    s_CubeCache.clear();

    for (auto& kv : s_Tex2DCache) {
        if (kv.second && kv.second != s_FallbackTex2D) {
            kv.second->Destroy();
            xr_delete(kv.second);
        }
    }
    s_Tex2DCache.clear();

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        s_BoundName0[i].clear();  s_BoundName1[i].clear();
        s_BoundCloud0[i].clear(); s_BoundCloud1[i].clear();
    }

    if (s_FallbackCube)  { s_FallbackCube->Destroy();  xr_delete(s_FallbackCube); }
    if (s_FallbackTex2D) { s_FallbackTex2D->Destroy(); xr_delete(s_FallbackTex2D); }

    if (s_Pipeline)       { vkDestroyPipeline(VulkanHW.m_Device, s_Pipeline, nullptr);             s_Pipeline = VK_NULL_HANDLE; }
    if (s_PipelineLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_PipelineLayout, nullptr); s_PipelineLayout = VK_NULL_HANDLE; }
    if (s_Pool)           { vkDestroyDescriptorPool(VulkanHW.m_Device, s_Pool, nullptr);            s_Pool = VK_NULL_HANDLE; }
    if (s_Sampler)        { vkDestroySampler(VulkanHW.m_Device, s_Sampler, nullptr);                s_Sampler = VK_NULL_HANDLE; }
    VK::Clouds::Destroy();
    if (s_UboMapped)      { s_Ubo.Unmap(); s_UboMapped = nullptr; }
    s_Ubo.Destroy();
    if (s_DbgMapped)      { s_DbgBuf.Unmap(); s_DbgMapped = nullptr; }
    s_DbgBuf.Destroy();
    if (s_CloudSampler)   { vkDestroySampler(VulkanHW.m_Device, s_CloudSampler, nullptr);           s_CloudSampler = VK_NULL_HANDLE; }
    if (s_SetLayout)      { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_SetLayout, nullptr);  s_SetLayout = VK_NULL_HANDLE; }
    s_VS = VK_NULL_HANDLE;
    s_FS = VK_NULL_HANDLE;
}

}  // namespace SkyPass

// Layout is managed centrally (single-layout convention): the swapchain image
// stays in COLOR_ATTACHMENT_OPTIMAL and depth in DEPTH_ATTACHMENT_OPTIMAL for
// the whole frame. This pass only records draws; ExecutePasses inserts the
// inter-pass barrier before it. See CRender::Begin/End.

void Pass_Sky(FrameContext& ctx)
{
    if (!s_Pipeline)               return;
    if (ctx.cmd == VK_NULL_HANDLE) return;

    const u32 slot = CommandManager.GetCurrentFrame();  // fence-guarded in-flight slot
    EnsureCurrentCubes(slot);

    VkCommandBuffer cmd = ctx.cmd;

    // Cloud coverage counters: zero them BEFORE the pass (vkCmdFillBuffer is a
    // transfer op and is illegal inside dynamic rendering), then read last frame's
    // values off the mapping and log them. One frame of lag is irrelevant for a
    // diagnostic and buys us no fence.
    if (ps_r_clouds_debug && s_DbgBuf.GetHandle() != VK_NULL_HANDLE) {
        if (s_DbgMapped) {
            static float s_dbgT = 0.f;
            s_dbgT += (RV().fTimeDelta < 0.1f) ? RV().fTimeDelta : 0.1f;
            if (s_dbgT > 1.5f) {
                s_dbgT = 0.f;
                const u32 sky = s_DbgMapped[0], cld = s_DbgMapped[1];
                const u32 mx  = s_DbgMapped[2], sum = s_DbgMapped[3];
                Msg("[VK Clouds] coverage: %u/%u sky px have cloud (%.1f%%) | maxAlpha=%.3f meanAlpha=%.4f "
                    "| cov=%.2f dens=%.2f band=%.0f..%.0f steps=%d",
                    cld, sky, sky ? (100.0 * double(cld) / double(sky)) : 0.0,
                    mx / 1000.f, sky ? (sum / 1000.f / float(sky)) : 0.f,
                    ps_r_clouds_coverage, ps_r_clouds_density,
                    ps_r_clouds_bottom, ps_r_clouds_top, ps_r_clouds_steps);
            }
        }
        vkCmdFillBuffer(cmd, s_DbgBuf.GetHandle(), 0, 16, 0);
        VkBufferMemoryBarrier bb{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = s_DbgBuf.GetHandle(); bb.offset = 0; bb.size = 16;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 1, &bb, 0, nullptr);
    }

    VK::BeginOverlayRendering(cmd, ctx, VK_ATTACHMENT_STORE_OP_DONT_CARE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_Pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            s_PipelineLayout, 0, 1, &s_Set[slot], 0, nullptr);

    // Camera basis → sky direction (position-invariant by construction).
    // tan(fov_y/2) maps clip-space [-1,1] into world-space ray spread.
    // RV().fASPECT in X-Ray is height/width (CameraManager.cpp:335 ->
    // CCameraManager::Update), so the horizontal half-tan is tan/fASPECT,
    // not tan*fASPECT. Getting this wrong was making horizontal FOV ~3×
    // narrower than the viewport, so any camera yaw shifted the cubemap
    // sample much faster than reality — visible as the sky "accelerating"
    // opposite to the camera.
    const float halfFovRad = deg2rad(RV().fFOV) * 0.5f;
    const float tanHalf    = tanf(halfFovRad);
    Fvector rightScaled, upScaled;
    rightScaled.mul(RV().vCameraRight, tanHalf / RV().fASPECT);
    upScaled   .mul(RV().vCameraTop,   tanHalf);

    SkyUBO push{};
    push.camRightTan[0] = rightScaled.x;
    push.camRightTan[1] = rightScaled.y;
    push.camRightTan[2] = rightScaled.z;
    push.camUpTan[0]    = upScaled.x;
    push.camUpTan[1]    = upScaled.y;
    push.camUpTan[2]    = upScaled.z;
    push.camForward[0]  = RV().vCameraDirection.x;
    push.camForward[1]  = RV().vCameraDirection.y;
    push.camForward[2]  = RV().vCameraDirection.z;

    // Pull the per-frame env knobs (rotation / tint / weight). Default to
    // a neutral state when env isn't ready yet.
    push.skyRotation   = 0.0f;
    push.blendWeight   = 0.0f;
    push.skyColor[0]   = 1.0f;
    push.skyColor[1]   = 1.0f;
    push.skyColor[2]   = 1.0f;
    // Sun disk defaults: pointing down, colour black (no disk) until env is up.
    push.sunDir[0] = 0.0f; push.sunDir[1] = -1.0f; push.sunDir[2] = 0.0f;
    push.sunColor[0] = push.sunColor[1] = push.sunColor[2] = 0.0f;
    push.eyeAltitude = RV().vCameraPosition.y;

    // Cloud defaults: no clouds until env is up. Time = fTimeGlobal/10 (matches
    // R4 timers.z) scaled by r_clouds_speed; the shader multiplies by the
    // per-layer CLOUD_SPEED constants.
    push.cloudsColor[0] = push.cloudsColor[1] = push.cloudsColor[2] = 0.0f;
    push.cloudsWeight   = 0.0f;
    push.cloudTime      = RV().fTimeGlobal * 0.1f * ps_r_clouds_speed;
    push.cloudEnable    = ps_r_clouds ? 1.0f : 0.0f;
    push.cloudIntensity = ps_r_clouds_intensity;
    // Procedural sky (r_sky_proc): radiance from the atmosphere model rather than the
    // authored cube + tint. See the cvar note in vk_console_min.cpp for why.
    push.atmoEnable    = ps_r_sky_proc ? 1.0f : 0.0f;
    push.atmoIntensity = ps_r_sky_intensity;
    push.atmoTurbidity = ps_r_sky_turbidity;
    push.atmoMieG      = ps_r_sky_mie_g;

    // ── Volumetric clouds ────────────────────────────────────────────────────
    // Coverage 0 is the OFF switch the shader tests, so gate it on both the cvar and
    // the bake actually having produced volumes — otherwise a failed bake would march
    // an all-zero field for nothing every frame.
    const bool volClouds = (ps_r_clouds_vol != 0) && VK::Clouds::Ready();
    push.coverage       = volClouds ? ps_r_clouds_coverage : 0.f;
    push.bandBottom     = ps_r_clouds_bottom;
    push.bandTop        = _max(ps_r_clouds_top, ps_r_clouds_bottom + 100.f);
    push.detailStrength = ps_r_clouds_detail;
    push.cloudDebug     = volClouds ? float(ps_r_clouds_debug) : 0.f;
    push.density        = ps_r_clouds_density;
    push.shapeScale     = ps_r_clouds_shape_scale;
    push.detailScale    = ps_r_clouds_detail_scale;
    push.weatherScale   = ps_r_clouds_weather_scale;
    {
        // Wind heading in degrees -> unit XZ. Shares the weather's cloud speed knob so
        // the deck drifts with the same wind the rest of the sky uses.
        const float wr = deg2rad(ps_r_clouds_wind_dir);
        push.windX = cosf(wr); push.windZ = sinf(wr);
    }
    push.windSpeed      = ps_r_clouds_wind_speed;
    push.phaseG         = ps_r_clouds_phase_g;
    push.phaseGBack     = ps_r_clouds_phase_g_back;
    push.extinction     = ps_r_clouds_extinction;
    push.powder         = ps_r_clouds_powder;
    push.sunStrength    = ps_r_clouds_sun;
    push.ambStrength    = ps_r_clouds_ambient;
    push.marchSteps     = float(ps_r_clouds_steps);
    push.marchMaxDist   = ps_r_clouds_max_dist;
    push.cirrusAmount   = ps_r_clouds_cirrus;
    push.cirrusAltitude = ps_r_clouds_cirrus_alt;
    push.cirrusScale    = ps_r_clouds_cirrus_scale;
    // The cloud lighting reads the atmosphere params through its own mirror.
    push.atmo2Enable    = push.atmoEnable;
    push.atmo2Intensity = push.atmoIntensity;
    push.atmo2Turbidity = push.atmoTurbidity;
    push.atmo2MieG      = push.atmoMieG;
    push.cloudTimeSec   = RV().fTimeGlobal;
    // Sun DISK direction/colour are LATCHED against sudden jumps: a thunderbolt
    // momentarily hijacks the env sun_dir/sun_color (the lightning flash is driven
    // as a directional light), which would teleport the disk. The real sun drifts
    // only fractions of a degree per frame, so reject any large frame-to-frame jump
    // (keep the last stable sun) and otherwise follow — the disk stays put through
    // the flash, then tracks time-of-day. (Scene lighting still flashes as before;
    // only the drawn sun disk is stabilised.)
    static Fvector s_sunDir = { 0.f, -1.f, 0.f };
    static Fvector s_sunCol = { 0.f,  0.f, 0.f };
    static bool    s_sunInit = false;
    static int     s_sunReject = 0;   // consecutive frames the live sun disagreed with the latch
    if (g_pGamePersistent) {
        if (auto* mix = ES().CurrentEnv) {
            push.skyRotation = mix->sky_rotation;
            push.skyColor[0] = mix->sky_color.x;
            push.skyColor[1] = mix->sky_color.y;
            push.skyColor[2] = mix->sky_color.z;
            push.blendWeight = mix->weight;

            // Clouds tint + weight (clouds_color.w). R4 skips clouds when w≈0;
            // the shader gates on this too.
            push.cloudsColor[0] = mix->clouds_color.x;
            push.cloudsColor[1] = mix->clouds_color.y;
            push.cloudsColor[2] = mix->clouds_color.z;
            push.cloudsWeight   = mix->clouds_color.w;

            Fvector tgt = mix->sun_dir;
            if (tgt.square_magnitude() > 1e-6f) tgt.normalize(); else tgt.set(0.f, -1.f, 0.f);
            const bool follow = mix->sun_dir.square_magnitude() > 1e-6f &&
                                s_sunDir.dotproduct(tgt) > 0.99f;   // < ~8° change = real sun drift
            if (!s_sunInit || follow) {
                // First valid sample, or normal frame-to-frame drift → track live.
                s_sunInit = true; s_sunReject = 0;
                s_sunDir = tgt;
                s_sunCol.set(mix->sun_color.x, mix->sun_color.y, mix->sun_color.z);
            } else if (++s_sunReject > 6) {
                // The disagreement PERSISTED — this is not a 1-2 frame thunderbolt
                // flash but a genuine discontinuity (time skip / sleep / level load /
                // weather keyframe step, which can jump 18-22°) or a bad initial latch.
                // Snap to the live sun so the disk can never stay stuck (e.g. frozen
                // at the straight-down default → drawn at the zenith while the real
                // sun is at the horizon). A real thunderbolt clears in far fewer frames.
                s_sunReject = 0;
                s_sunDir = tgt;
                s_sunCol.set(mix->sun_color.x, mix->sun_color.y, mix->sun_color.z);
            }
            // else: a brief big jump (thunderbolt) → keep the last stable sun a few frames.
            push.sunDir[0]   = s_sunDir.x; push.sunDir[1] = s_sunDir.y; push.sunDir[2] = s_sunDir.z;
            push.sunColor[0] = s_sunCol.x; push.sunColor[1] = s_sunCol.y; push.sunColor[2] = s_sunCol.z;

            // The sky tints are authored sRGB like every other env colour. Converted
            // here, AFTER the sun latch, so the latch keeps comparing/storing values in
            // the one space it was tuned in — s_sunCol persists across frames, and
            // decoding before it would make the stored latch and the live sample
            // incomparable on the frame the pipeline mode changes.
            ColorSpace::LinearizeRGB(push.skyColor);
            ColorSpace::LinearizeRGB(push.cloudsColor);
            ColorSpace::LinearizeRGB(push.sunColor);

            // 10-10 the horizon in the fog colour (r_sky_fog_horizon, Switch preset): with r_fog_dist 1 the fog closes
            // exactly at the far plane (Environment_misc: fog_distance <= far_plane_render - 10), so what the plane
            // clips is already pure fog_color -- but against a sky of another colour those fogged hills and crowns
            // were grey silhouettes, popping in and out as the planar clip swept them while the camera turned. Same
            // value and the same decode as vk_env_light's L.fog_color. Off while the distance fog itself is off.
            push.fogColor[0] = mix->fog_color.x; push.fogColor[1] = mix->fog_color.y; push.fogColor[2] = mix->fog_color.z;
            ColorSpace::LinearizeRGB(push.fogColor);
            push.fogHorizon = (ps_r_fog_dist > 0.f) ? ps_r_sky_fog_horizon : 0.f;
        }
    }

    // Upload into this frame's UBO slot. WaitForFence(slot) in CRender::Begin already
    // proved the GPU is done with it, so a plain memcpy is safe — same reasoning the
    // cubemap rebind above relies on.
    // ── Couple cloud COVERAGE to the weather (r_clouds_weather) ──────────────
    // Runs here, after the env block above has filled cloudsWeight. Until now the
    // deck's coverage came from a fixed cvar and knew nothing about the weather at
    // all — so an hour the mod authored as solid overcast rendered as the same
    // scattered puffs as a clear one. clouds_color.w IS the weather's cloud weight
    // (R4 skips its cloud pass entirely when it is ~0), which makes it exactly the
    // signal to drive coverage with.
    if (volClouds && ps_r_clouds_weather) {
        const float w = _min(_max(push.cloudsWeight, 0.f), 1.f);
        // Below ~0.01 the weather means "clear" — honour that rather than leaving a
        // residual deck the preset never asked for.
        push.coverage = (w < 0.01f) ? 0.f
                                    : _min(ps_r_clouds_coverage * (0.5f + 1.5f * w), 1.f);
    }

    if (ps_r_clouds_debug) {
        static float s_covT = 0.f;
        s_covT += (RV().fTimeDelta < 0.1f) ? RV().fTimeDelta : 0.1f;
        if (s_covT > 1.5f) {
            s_covT = 0.f;
            Msg("[VK Clouds] weather: clouds_color.w=%.3f -> effective coverage=%.3f (cvar %.2f, couple %d)",
                push.cloudsWeight, push.coverage, ps_r_clouds_coverage, ps_r_clouds_weather);
        }
    }

    if (s_UboMapped) memcpy(s_UboMapped + s_UboStride * slot, &push, sizeof(push));

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
    // No exit transition: image stays COLOR_ATTACHMENT; End brings it to PRESENT.
}

}  // namespace VK
