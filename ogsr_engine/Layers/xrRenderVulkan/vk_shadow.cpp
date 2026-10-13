// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — sun directional shadow map. See vk_shadow.h.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_shadow.h"
#include "vk_sun_cascades_cfg.h"         // nfs47: cascade sizes/boxes/z ranges
#include "vk_image.h"                   // VK::CreateImage / CreateImageView
#include "vk_light.h"                   // Lights::kMaxShadowPoints (point cube array size)
#include "vk_swapchain.h"               // (depth format reference, allocator via VulkanHW)
#include "vk_profiler.h"                // VK::Prof::NameImage (debug-utils names)
#include "../../xr_3da/device.h"        // RV().vCameraPosition / vCameraDirection

extern int ps_r_sun_casc_lite;   // vk_console_min.cpp: r_sun_casc_lite (0 = stock cascades, 1 = handheld profile)
extern int ps_r_sun_far_size;    // vk_console_min.cpp: r_sun_far_size (0 = the profile's far map size)

namespace VK { namespace SunCfg {
namespace { const Profile* s_profile = &kStock; u32 s_farSize = 0; }
const Profile& Get() { return *s_profile; }
u32 FarSize() { return s_farSize; }
void Latch()
{
    s_profile = ps_r_sun_casc_lite ? &kLite : &kStock;
    s_farSize = ps_r_sun_far_size ? u32(ps_r_sun_far_size) : s_profile->farSize;
    Msg("[VK Shadow] sun cascade profile: %s (cascades %u/%u, box %.0f/%.0f m, z cut %.0f/%.0f m)",
        ps_r_sun_casc_lite ? "lite" : "stock", s_profile->cascadeSize[0], s_profile->cascadeSize[1],
        s_profile->cascadeBox[0], s_profile->cascadeBox[1], s_profile->cascadeZCull[0], s_profile->cascadeZCull[1]);
    Msg("[VK Shadow] far map %ux%u (%.0f cm a texel)", s_farSize, s_farSize, 32000.f / float(s_farSize));
}
}}   // namespace VK::SunCfg

namespace VK { namespace ShadowMap {

namespace {
    // The far map's texels: r_sun_far_size or the SunCfg profile (2048 stock, 1024 kLite), latched in Init before the
    // images are made.
    u32 FarSize() { return VK::SunCfg::FarSize(); }

    // Ortho box around the camera. halfSize ≈ near scene radius; D lifts the
    // light eye up the sun ray so the whole box is in front of it.
    constexpr float kHalfSize = 160.f;
    constexpr float kEyeDist  = 350.f;
    constexpr float kZNear    = 1.f;
    constexpr float kZFar     = kEyeDist + 2.f * kHalfSize + 400.f;

    constexpr u32 kSpotSize   = 1024;   // one pool tile
    constexpr u32 kSpotAtlasX = 4;      // tiles per atlas row
    constexpr u32 kSpotAtlasY = 2;      // rows — X*Y tiles = Lights::kMaxShadowSpots
    constexpr u32 kPointSize  = 512;

    // Near sun cascades — R4 sizes (ps_ssfx_shadow_cascades 25/60, cascade 2 is
    // the cached far map above) at 4096² → ~0.61 / 1.46 cm texels. View origin
    // at the player camera with a NEGATIVE ortho near covering casters up-sun:
    // hanging the eye up the sun ray (far-map style) puts the raster lattice on
    // a 350 m lever — a 0.0007°/frame sun creep then slides the whole lattice
    // ~1/3 texel every frame and the scene shimmers like heat haze.
    // Cascade 0 carries the crispness (0.61 cm texel) — full res, every frame.
    // Cascade 1 covers 12–30 m where 2.9 cm reads fine — quarter the raster.
    // nfs47: the numbers live in vk_sun_cascades_cfg.h (profile latched at Init, r_sun_casc_lite).
#define kCascadeSizes (VK::SunCfg::Get().cascadeSize)
#define kCascadeBox   (VK::SunCfg::Get().cascadeBox)
    // Caster column up-sun: 160 m is plenty — a caster that far up the ray
    // needs to be ~40 m tall (at 15° sun) to shadow the box at all; rastering
    // the full 350 m column every frame was a measurable chunk of the cost.
    constexpr float kCascadeZBehind = VK::SunCfg::kCascadeZBehind;
    constexpr float kCascadeZAhead  = VK::SunCfg::kCascadeZAhead;
    // R4 anchor: camera snapped to this world grid; its projection is pinned to
    // the texel lattice (see ComputeCascadeVP).
    constexpr float kAnchorGrid = 4.f;

    bool          s_inited = false;
    bool          s_failed = false;
    VkImage       s_imageStatic = VK_NULL_HANDLE;   // cached statics-only depth
    VmaAllocation s_allocStatic = VK_NULL_HANDLE;
    VkImageView   s_viewStatic  = VK_NULL_HANDLE;
    VkImage       s_image  = VK_NULL_HANDLE;        // combined (static copy + dynamics) — sampled
    VmaAllocation s_alloc  = VK_NULL_HANDLE;
    VkImageView   s_view   = VK_NULL_HANDLE;
    VkSampler     s_sampler = VK_NULL_HANDLE;
    Fmatrix       s_lightVP;
    Fmatrix       s_lightView;   // world→light-view, kept for SphereVisible

    // Dynamic light shadows (STEP 3b)
    // Spot shadow POOL: ATLASES of kSpotAtlasX×kSpotAtlasY tiles (1024² each),
    // one tile per pooled spot light (headlights, searchlights, flashlight all
    // shadow at once). Tiles are cached VSM-style — vk_pass_shadow re-renders
    // layers independently:
    //   STATIC atlas (statics + trees)  → only when the light moves/casters change
    //   CLEAN  atlas (= static + NPCs)  → copy + NPC overlay, distance/visibility LOD
    //   BEAM   atlas (= clean + grass)  → copy + grass, near lights (wind animation)
    // STATIC layer — never sampled, copy source only (rests in TRANSFER_SRC).
    VkImage       s_spotStaticImage = VK_NULL_HANDLE;
    VmaAllocation s_spotStaticAlloc = VK_NULL_HANDLE;
    VkImageView   s_spotStaticView  = VK_NULL_HANDLE;
    VkImage       s_spotImage = VK_NULL_HANDLE;     // CLEAN atlas — surfaces sample this
    VmaAllocation s_spotAlloc = VK_NULL_HANDLE;
    VkImageView   s_spotView  = VK_NULL_HANDLE;
    // Spot BEAM atlas = per-tile copy of the clean tile + grass casters on top.
    // The visible volumetric cone / fog sample THIS (blades cut the beam);
    // surfaces blend both (r_spot_grass_shadow) — a single shared map let dense
    // grass blanket the ground and eat the headlight's light pool.
    VkImage       s_spotBeamImage = VK_NULL_HANDLE;
    VmaAllocation s_spotBeamAlloc = VK_NULL_HANDLE;
    VkImageView   s_spotBeamView  = VK_NULL_HANDLE;
    Fmatrix       s_spotTileVP[kSpotAtlasX * kSpotAtlasY];
    Fmatrix       s_spotVP;
    // Point shadow POOL: an ARRAY of kMaxShadowPoints cubes (512²×6 each). One
    // cube per pooled point light — several campfires with NPCs around each all
    // shadow at once. Sampled as samplerCubeArray; rendered per (cube,face).
    VkImage       s_pointImage = VK_NULL_HANDLE;    // COMBINED cube array (sampled) = static copy + dynamics
    VmaAllocation s_pointAlloc = VK_NULL_HANDLE;
    VkImageView   s_pointCubeView = VK_NULL_HANDLE;                        // CUBE_ARRAY (sampling)
    VkImageView   s_pointFaceView[6 * Lights::kMaxShadowPoints] = {};      // 2D per (cube,face) for rendering
    // STATIC cube array (statics/walls only, cached until the fire moves / casters
    // change) — copy SOURCE for the combined map, never sampled. Same static/dyn
    // split the spot pool and sun cascades use: an NPC or grass refresh copies this
    // + draws only the dynamic overlay, instead of re-rastering all 6 static faces.
    VkImage       s_pointStaticImage = VK_NULL_HANDLE;
    VmaAllocation s_pointStaticAlloc = VK_NULL_HANDLE;
    VkImageView   s_pointStaticFaceView[6 * Lights::kMaxShadowPoints] = {};

    // Near sun cascades. Each has a STATIC map (cached statics+trees+opaque,
    // re-rastered only on camera/sun move — see vk_pass_shadow) and a COMBINED
    // map (the sampled one): every frame a copy of the static + skinned dynamics
    // on top. Same split as the far map → a standing camera re-rasters nothing.
    VkImage       s_cascImage[kNumSunCascades] = {};   // combined (sampled)
    VmaAllocation s_cascAlloc[kNumSunCascades] = {};
    VkImageView   s_cascView[kNumSunCascades]  = {};
    VkImage       s_cascStaticImage[kNumSunCascades] = {};   // cached statics-only depth (copy src)
    VmaAllocation s_cascStaticAlloc[kNumSunCascades] = {};
    VkImageView   s_cascStaticView[kNumSunCascades]  = {};
    VkImage       s_cascMidImage[kNumSunCascades] = {};      // nfs48: static + live trees (cascades in SunCfg::kNpcLayerMask)
    VmaAllocation s_cascMidAlloc[kNumSunCascades] = {};
    VkImageView   s_cascMidView[kNumSunCascades]  = {};
    Fmatrix       s_cascVP[kNumSunCascades];
    Fmatrix       s_cascCullVP[kNumSunCascades];   // nfs47: same xy, far plane at SunCfg::kCascadeZCull
    Fmatrix       s_cascViewM[kNumSunCascades];   // world→light-view, kept for CascadeSphereVisible

    // Rain occlusion map: straight-down ortho box around the camera. The fixed
    // (vertical) direction means a plain view-translation texel snap fully
    // stabilizes re-renders — no rotating lattice like the sun cascades.
    constexpr u32   kRainSize    = 1024;    // shared by rain + ground/water maps (2048 cost SunShadow w/o helping puddles)
    constexpr float kRainHalf    = 75.f;    // ±75 m around the camera (~14.6 cm texels). Shared by wetness +
                                            // the fog's lamp occlusion (r_light_occ). Kept at 75 for perf
                                            // (a wider box made the rain-time redraw heavier); lamps past ±75 m
                                            // may leak a little, but they're distant/fogged.
    constexpr float kRainEyeUp   = 100.f;   // eye this far above the camera
    constexpr float kRainZNear   = 1.f;
    constexpr float kRainZFar    = 350.f;   // covers 100 m above to 250 m below
    VkImage       s_rainImage = VK_NULL_HANDLE;
    VmaAllocation s_rainAlloc = VK_NULL_HANDLE;
    VkImageView   s_rainView  = VK_NULL_HANDLE;
    Fmatrix       s_rainVP;
    Fmatrix       s_rainViewM;   // world→light-view, kept for RainSphereVisible
    float         s_rainEyeY = 0.f;   // ortho eye world-Y at the last redraw (exact height reconstruction)

    // Ground-height map: same ortho box/VP as the rain map but rendered from
    // STATICS+TERRAIN ONLY (no trees) — a CLEAN, stable top-down height field
    // for the water flow sim. Trees in the rain map flicker (LOD/canopy) and
    // their tops dominate the height, which made puddles jump and pool beside
    // (not in) the ground's real dips. Same size/VP → the sim grid still aligns.
    VkImage       s_groundImage = VK_NULL_HANDLE;
    VmaAllocation s_groundAlloc = VK_NULL_HANDLE;
    VkImageView   s_groundView  = VK_NULL_HANDLE;

    bool CreateDepthImageWH(u32 w, u32 h, u32 layers, VkImageCreateFlags flags, VkImageUsageFlags usage,
                            VkImage& image, VmaAllocation& alloc)
    {
        VK::ImageDesc d;
        d.format   = VK_FORMAT_D32_SFLOAT;
        d.extent   = { w, h, 1 };
        d.layers   = layers;
        d.flags    = flags;
        d.usage    = usage;
        d.memUsage = VMA_MEMORY_USAGE_AUTO;
        return VK::CreateImage(d, image, alloc);
    }

    bool CreateDepthImageEx(u32 size, u32 layers, VkImageCreateFlags flags, VkImageUsageFlags usage,
                            VkImage& image, VmaAllocation& alloc)
    {
        return CreateDepthImageWH(size, size, layers, flags, usage, image, alloc);
    }

    bool CreateDepthView(VkImage image, VkImageViewType type, u32 baseLayer, u32 layers, VkImageView& view)
    {
        view = VK::CreateImageView(image, VK_FORMAT_D32_SFLOAT, type,
                                   VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, baseLayer, layers);
        return view != VK_NULL_HANDLE;
    }

    bool CreateDepthImage(VkImageUsageFlags usage, VkImage& image, VmaAllocation& alloc, VkImageView& view)
    {
        return CreateDepthImageEx(FarSize(), 1, 0, usage, image, alloc)
            && CreateDepthView(image, VK_IMAGE_VIEW_TYPE_2D, 0, 1, view);
    }
}

bool        Ready()       { return s_inited && !s_failed; }
const Fmatrix& GetLightVP(){ return s_lightVP; }
VkImage     GetStaticImage() { return s_imageStatic; }
VkImageView GetStaticView()  { return s_viewStatic; }
VkImage     GetImage()    { return s_image; }
VkImageView GetView()     { return s_view; }
VkSampler   GetSampler()  { return s_sampler; }
u32         Size()        { return FarSize(); }
VkImage     GetSpotImage()    { return s_spotImage; }
VkImageView GetSpotView()     { return s_spotView; }
VkImage     GetSpotStaticImage() { return s_spotStaticImage; }
VkImageView GetSpotStaticView()  { return s_spotStaticView; }
VkImage     GetSpotBeamImage(){ return s_spotBeamImage; }
VkImageView GetSpotBeamView() { return s_spotBeamView; }
u32         SpotSize()        { return kSpotSize; }
u32         SpotAtlasX()      { return kSpotAtlasX; }
u32         SpotAtlasY()      { return kSpotAtlasY; }
const Fmatrix& GetSpotVP()    { return s_spotVP; }
VkImage     GetPointImage()   { return s_pointImage; }
VkImageView GetPointCubeView(){ return s_pointCubeView; }   // CUBE_ARRAY (sampling)
VkImageView GetPointFaceView(u32 cube, u32 face) {
    const u32 idx = cube * 6 + (face % 6);
    return (idx < 6 * Lights::kMaxShadowPoints) ? s_pointFaceView[idx] : VK_NULL_HANDLE;
}
VkImage     GetPointStaticImage() { return s_pointStaticImage; }
VkImageView GetPointStaticFaceView(u32 cube, u32 face) {
    const u32 idx = cube * 6 + face;
    return (idx < 6 * Lights::kMaxShadowPoints) ? s_pointStaticFaceView[idx] : VK_NULL_HANDLE;
}
u32         PointSize()       { return kPointSize; }
VkImage     GetRainImage()    { return s_rainImage; }
VkImageView GetRainView()     { return s_rainView; }
VkImage     GetGroundImage()  { return s_groundImage; }
VkImageView GetGroundView()   { return s_groundView; }
u32         RainSize()        { return kRainSize; }
float       RainEyeY()        { return s_rainEyeY; }
float       RainZNear()       { return kRainZNear; }
float       RainZFar()        { return kRainZFar; }
const Fmatrix& GetRainVP()    { return s_rainVP; }
const Fmatrix& GetCascadeVP(u32 i) { return s_cascVP[i < kNumSunCascades ? i : 0]; }
const Fmatrix& GetCascadeCullVP(u32 i) { return s_cascCullVP[i < kNumSunCascades ? i : 0]; }
VkImage     GetCascadeImage(u32 i) { return (i < kNumSunCascades) ? s_cascImage[i] : VK_NULL_HANDLE; }
VkImageView GetCascadeView(u32 i)  { return (i < kNumSunCascades) ? s_cascView[i]  : VK_NULL_HANDLE; }
u32         CascadeSize(u32 i)     { return kCascadeSizes[i < kNumSunCascades ? i : 0]; }
VkImage     GetCascadeStaticImage(u32 i) { return (i < kNumSunCascades) ? s_cascStaticImage[i] : VK_NULL_HANDLE; }
VkImageView GetCascadeStaticView(u32 i)  { return (i < kNumSunCascades) ? s_cascStaticView[i]  : VK_NULL_HANDLE; }
VkImage     GetCascadeMidImage(u32 i)   { return (i < kNumSunCascades) ? s_cascMidImage[i] : VK_NULL_HANDLE; }
VkImageView GetCascadeMidView(u32 i)    { return (i < kNumSunCascades) ? s_cascMidView[i]  : VK_NULL_HANDLE; }

bool Init()
{
    if (s_inited) return !s_failed;
    s_inited = true;
    VK::SunCfg::Latch();   // nfs47: the cascade sizes / MID images below depend on the profile
    s_lightVP.identity();
    s_lightView.identity();
    s_rainVP.identity();
    s_rainViewM.identity();
    for (u32 i = 0; i < kNumSunCascades; ++i) {
        s_cascVP[i].identity();
        s_cascCullVP[i].identity();
        s_cascViewM[i].identity();
    }

    // Static map is render target + copy source; combined map is copy dest +
    // render target (dynamics on top) + the texture receivers sample.
    if (!CreateDepthImage(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          s_imageStatic, s_allocStatic, s_viewStatic) ||
        !CreateDepthImage(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,   // SRC: async fog snapshot
                          s_image, s_alloc, s_view)) {
        s_failed = true; return false;
    }
    s_spotVP.identity();
    for (auto& m : s_spotTileVP) m.identity();

    // Dynamic light shadow targets: spot map + point cube (render per face,
    // sample as cube). Both attachment + sampled.
    // TRANSFER_SRC on the sampled maps: under async compute the volumetrics pass
    // blits a downscaled snapshot of the cascades + rain map on the graphics queue,
    // because its inject samples them from the COMPUTE queue a frame later and the
    // live maps are being rewritten by then (see vk_volumetrics SnapshotShadows).
    // A usage flag only widens what is legal — nothing else changes.
    const VkImageUsageFlags dynUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                     | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // Combined cascade = sampled + copy DEST (static copied into it each frame);
    // static cascade = render target + copy SOURCE (same roles as the far map pair).
    for (u32 i = 0; i < kNumSunCascades; ++i)
        if (!CreateDepthImageEx(kCascadeSizes[i], 1, 0, dynUsage | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                s_cascImage[i], s_cascAlloc[i]) ||
            !CreateDepthView(s_cascImage[i], VK_IMAGE_VIEW_TYPE_2D, 0, 1, s_cascView[i]) ||
            !CreateDepthImageEx(kCascadeSizes[i], 1, 0,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                s_cascStaticImage[i], s_cascStaticAlloc[i]) ||
            !CreateDepthView(s_cascStaticImage[i], VK_IMAGE_VIEW_TYPE_2D, 0, 1, s_cascStaticView[i])) {
            s_failed = true; return false;
        }
    // nfs48: the MID layer of the split cascades -- copy dst of the static map, render target, copy src of the combined.
    for (u32 i = 0; i < kNumSunCascades; ++i)
        if ((VK::SunCfg::Get().npcLayerMask >> i) & 1u)
            if (!CreateDepthImageEx(kCascadeSizes[i], 1, 0,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    s_cascMidImage[i], s_cascMidAlloc[i]) ||
                !CreateDepthView(s_cascMidImage[i], VK_IMAGE_VIEW_TYPE_2D, 0, 1, s_cascMidView[i])) {
                s_failed = true; return false;
            }
    if (!CreateDepthImageEx(kRainSize, 1, 0, dynUsage, s_rainImage, s_rainAlloc) ||
        !CreateDepthView(s_rainImage, VK_IMAGE_VIEW_TYPE_2D, 0, 1, s_rainView)) {
        s_failed = true; return false;
    }
    if (!CreateDepthImageEx(kRainSize, 1, 0, dynUsage, s_groundImage, s_groundAlloc) ||
        !CreateDepthView(s_groundImage, VK_IMAGE_VIEW_TYPE_2D, 0, 1, s_groundView)) {
        s_failed = true; return false;
    }
    if (!CreateDepthImageWH(kSpotSize * kSpotAtlasX, kSpotSize * kSpotAtlasY, 1, 0,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            s_spotStaticImage, s_spotStaticAlloc) ||
        !CreateDepthView(s_spotStaticImage, VK_IMAGE_VIEW_TYPE_2D, 0, 1, s_spotStaticView) ||
        !CreateDepthImageWH(kSpotSize * kSpotAtlasX, kSpotSize * kSpotAtlasY, 1, 0,
                            dynUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            s_spotImage, s_spotAlloc) ||
        !CreateDepthView(s_spotImage, VK_IMAGE_VIEW_TYPE_2D, 0, 1, s_spotView) ||
        !CreateDepthImageWH(kSpotSize * kSpotAtlasX, kSpotSize * kSpotAtlasY, 1, 0,
                            dynUsage | VK_IMAGE_USAGE_TRANSFER_DST_BIT, s_spotBeamImage, s_spotBeamAlloc) ||
        !CreateDepthView(s_spotBeamImage, VK_IMAGE_VIEW_TYPE_2D, 0, 1, s_spotBeamView) ||
        !CreateDepthImageEx(kPointSize, 6 * Lights::kMaxShadowPoints, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                            dynUsage | VK_IMAGE_USAGE_TRANSFER_DST_BIT, s_pointImage, s_pointAlloc) ||   // combined: + copy DEST
        !CreateDepthView(s_pointImage, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 0, 6 * Lights::kMaxShadowPoints, s_pointCubeView) ||
        !CreateDepthImageEx(kPointSize, 6 * Lights::kMaxShadowPoints, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,   // static: attachment + copy SRC
                            s_pointStaticImage, s_pointStaticAlloc)) {
        s_failed = true; return false;
    }
    for (u32 f = 0; f < 6 * Lights::kMaxShadowPoints; ++f)
        if (!CreateDepthView(s_pointImage,       VK_IMAGE_VIEW_TYPE_2D, f, 1, s_pointFaceView[f]) ||
            !CreateDepthView(s_pointStaticImage, VK_IMAGE_VIEW_TYPE_2D, f, 1, s_pointStaticFaceView[f])) {
            s_failed = true; return false;
        }

    // Manual-compare sampler (no hardware PCF): linear, clamp-to-border, white
    // border so anything outside the map reads depth 1.0 → never shadowed.
    VkSamplerCreateInfo sci{};
    sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.maxAnisotropy = 1.0f;
    sci.compareEnable = VK_FALSE;
    sci.minLod = 0.0f; sci.maxLod = 1.0f;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (vkCreateSampler(VulkanHW.m_Device, &sci, nullptr, &s_sampler) != VK_SUCCESS) {
        Msg("![VK Shadow] sampler create failed"); s_failed = true; return false;
    }

    // Tag the depth targets so RenderDoc/Nsight captures read them by name.
    Prof::NameImage(s_imageStatic, "Shadow.SunStatic");
    Prof::NameImage(s_image,       "Shadow.SunCombined");
    Prof::NameImage(s_rainImage,   "Shadow.RainOcclusion");
    Prof::NameImage(s_groundImage, "Shadow.GroundHeight");
    Prof::NameImage(s_spotImage,   "Shadow.Spot");
    Prof::NameImage(s_spotStaticImage, "Shadow.SpotStatic");
    Prof::NameImage(s_spotBeamImage, "Shadow.SpotBeam");
    Prof::NameImage(s_pointImage,  "Shadow.PointCube");

    Msg("[VK Shadow] init OK (%ux%u D32)", FarSize(), FarSize());
    return true;
}

void ComputeLightVP(const Fvector& sunDir)
{
    BuildLightVP(sunDir, RV().vCameraPosition, s_lightView, s_lightVP);
}

void SetLightVP(const Fmatrix& view, const Fmatrix& vp)
{
    s_lightView = view;
    s_lightVP   = vp;
}

void BuildLightVP(const Fvector& sunDirIn, const Fvector& center, Fmatrix& outView, Fmatrix& outVP)
{
    Fvector sunDir = sunDirIn;
    if (sunDir.magnitude() < 1e-4f) sunDir.set(0.f, -1.f, 0.f);
    sunDir.normalize();

    Fvector eye;   eye.mad(center, sunDir, -kEyeDist);   // center - sunDir*D (sunDir points down → eye up)
    Fvector up;    up.set(0.f, 1.f, 0.f);
    if (_abs(sunDir.y) > 0.99f) up.set(0.f, 0.f, 1.f);   // sun near vertical → avoid degenerate up

    Fmatrix view; view.build_camera_dir(eye, sunDir, up);

    // Texel snap: shift the view translation so the box origin lands on a
    // texel boundary — re-renders (cached map, see vk_pass_shadow) then move
    // the projection by whole texels and shadow edges don't crawl/pop.
    const float texel = (2.f * kHalfSize) / float(FarSize());
    view.c.x = floorf(view.c.x / texel) * texel;
    view.c.y = floorf(view.c.y / texel) * texel;

    outView = view;
    Fmatrix proj; proj.build_projection_ortho(2.f * kHalfSize, 2.f * kHalfSize, kZNear, kZFar);
    outVP.mul(proj, view);                                // = view·proj (X-Ray order)
}

bool SphereVisible(const Fvector& center, float radius)
{
    return SphereVisibleIn(s_lightView, center, radius);
}

bool SphereVisibleIn(const Fmatrix& lightView, const Fvector& center, float radius)
{
    Fvector vs; lightView.transform_tiny(vs, center);
    if (_abs(vs.x) > kHalfSize + radius) return false;
    if (_abs(vs.y) > kHalfSize + radius) return false;
    if (vs.z < kZNear - radius || vs.z > kZFar + radius) return false;
    return true;
}

void ComputeCascadeVP(u32 i, const Fvector& sunDirIn)
{
    if (i >= kNumSunCascades) return;

    Fvector sunDir = sunDirIn;
    if (sunDir.magnitude() < 1e-4f) sunDir.set(0.f, -1.f, 0.f);
    sunDir.normalize();

    // View origin AT the camera (not up the sun ray) — see kCascadeBox comment.
    Fvector up; up.set(0.f, 1.f, 0.f);
    if (_abs(sunDir.y) > 0.99f) up.set(0.f, 0.f, 1.f);
    Fmatrix view; view.build_camera_dir(RV().vCameraPosition, sunDir, up);
    s_cascViewM[i] = view;

    Fmatrix proj; proj.build_projection_ortho(kCascadeBox[i], kCascadeBox[i], kCascadeZBehind, kCascadeZAhead);
    Fmatrix vp;   vp.mul(proj, view);

    // R4 pixel alignment (render_phase_sun.cpp): project a WORLD-anchored point
    // (camera snapped to the kAnchorGrid world grid) through the raw VP, then
    // shift the matrix so that point lands exactly on a texel-lattice multiple.
    // This pins the raster lattice phase to a fixed world location: camera
    // translation can't slide it (the anchor doesn't move), and sun rotation
    // only turns the lattice AROUND the anchor — which sits within a couple of
    // metres of the player, so the per-frame phase drift near the receivers
    // that matter is microscopic. This is what keeps R4's per-frame-rendered
    // cascades from shimmering; a plain view.c floor-snap is NOT enough (its
    // quantization basis rotates with the sun and the phase slides through).
    Fvector anchor;
    anchor.set((floorf(RV().vCameraPosition.x / kAnchorGrid) + 0.5f) * kAnchorGrid,
               (floorf(RV().vCameraPosition.y / kAnchorGrid) + 0.5f) * kAnchorGrid,
               (floorf(RV().vCameraPosition.z / kAnchorGrid) + 0.5f) * kAnchorGrid);
    Fvector a; vp.transform_tiny(a, anchor);             // ortho → already NDC
    const float texNdc = 2.f / float(kCascadeSizes[i]);  // one texel in NDC units
    const float fx = a.x - floorf(a.x / texNdc) * texNdc;
    const float fy = a.y - floorf(a.y / texNdc) * texNdc;
    Fmatrix snap; snap.identity();
    snap.c.x = -fx;
    snap.c.y = -fy;
    s_cascVP[i].mul(snap, vp);                           // NDC translate AFTER vp
    // nfs47: the caster-cull twin -- identical x/y (and snap), far plane pulled in to kCascadeZCull.
    Fmatrix projC; projC.build_projection_ortho(kCascadeBox[i], kCascadeBox[i], kCascadeZBehind, VK::SunCfg::Get().cascadeZCull[i]);
    Fmatrix vpC;   vpC.mul(projC, view);
    s_cascCullVP[i].mul(snap, vpC);
}

bool CascadeSphereVisible(u32 i, const Fvector& center, float radius, float inflate)
{
    if (i >= kNumSunCascades) return false;
    Fvector vs; s_cascViewM[i].transform_tiny(vs, center);
    const float half = kCascadeBox[i] * 0.5f;
    const float r = radius + inflate;
    if (_abs(vs.x) > half + r) return false;
    if (_abs(vs.y) > half + r) return false;
    if (vs.z < kCascadeZBehind - r || vs.z > VK::SunCfg::Get().cascadeZCull[i] + r) return false;   // nfs47: cull column
    return true;
}

void ComputeRainVP()
{
    // Straight down. Up = +Z (any horizontal works — direction never rotates).
    const Fvector dir{ 0.f, -1.f, 0.f };
    const Fvector up { 0.f,  0.f, 1.f };
    Fvector eye; eye.set(RV().vCameraPosition.x, RV().vCameraPosition.y + kRainEyeUp, RV().vCameraPosition.z);
    s_rainEyeY = eye.y;   // remember for exact height reconstruction (snow mesh)

    Fmatrix view; view.build_camera_dir(eye, dir, up);

    // Texel snap (same idea as ComputeLightVP): vertical direction → the
    // lattice basis is fixed, snapping the view translation is sufficient.
    const float texel = (2.f * kRainHalf) / float(kRainSize);
    view.c.x = floorf(view.c.x / texel) * texel;
    view.c.y = floorf(view.c.y / texel) * texel;

    s_rainViewM = view;
    Fmatrix proj; proj.build_projection_ortho(2.f * kRainHalf, 2.f * kRainHalf, kRainZNear, kRainZFar);
    s_rainVP.mul(proj, view);
}

bool RainSphereVisible(const Fvector& center, float radius)
{
    Fvector vs; s_rainViewM.transform_tiny(vs, center);
    if (_abs(vs.x) > kRainHalf + radius) return false;
    if (_abs(vs.y) > kRainHalf + radius) return false;
    if (vs.z < kRainZNear - radius || vs.z > kRainZFar + radius) return false;
    return true;
}

Fmatrix ComputeSpotVPFor(const Fvector& pos, const Fvector& dirIn, float range, float cone)
{
    Fvector dir = dirIn;
    if (dir.magnitude() < 1e-5f) dir.set(0.f, 0.f, 1.f);
    dir.normalize();
    Fvector up; up.set(0.f, 1.f, 0.f);
    if (_abs(dir.y) > 0.99f) up.set(0.f, 0.f, 1.f);

    // A bit wider than the cone so the PCF/edge falloff has data to sample.
    const float fov = _min(cone * 1.1f + 0.1f, deg2rad(170.f));
    Fvector eye = pos;
    Fmatrix view; view.build_camera_dir(eye, dir, up);
    // Near plane 0.5 m, NOT kPointNear (0.1): when a scene lamp wins the spot
    // budget, its own FIXTURE (headlight housing/glass, lamp shade — all within
    // ~0.3 m of the emitter) rasterizes into the map and shadows the entire
    // beam to black. Nothing legit casts within 0.5 m of the flashlight either
    // (HUD never casts), so this only clips fixture shells.
    Fmatrix proj; proj.build_projection(fov, 1.f, 0.5f, _max(range, 1.f));
    Fmatrix vp; vp.mul(proj, view);
    return vp;
}

void SetSpotTileVP(u32 tile, const Fmatrix& vp)
{
    if (tile < kSpotAtlasX * kSpotAtlasY) s_spotTileVP[tile] = vp;
}

const Fmatrix& GetSpotTileVP(u32 tile)
{
    return s_spotTileVP[tile < kSpotAtlasX * kSpotAtlasY ? tile : 0];
}

// Legacy single-VP accessor — now the COOKIE spot's VP (vk_pass_shadow stores
// the flashlight tile's matrix here; EnvLight projects the cookie through it).
void SetSpotVP(const Fmatrix& vp) { s_spotVP = vp; }
static float s_spotTileScale = 1.f;
float SpotTileScale()           { return s_spotTileScale; }
void  SetSpotTileScale(float s) { s_spotTileScale = s; }

Fmatrix ComputePointFaceVP(const Fvector& pos, float range, u32 face)
{
    // Vulkan/D3D cube face order and orientations (+X,-X,+Y,-Y,+Z,-Z).
    static const Fvector kDirs[6] = {
        { 1.f, 0.f, 0.f }, { -1.f, 0.f, 0.f },
        { 0.f, 1.f, 0.f }, { 0.f, -1.f, 0.f },
        { 0.f, 0.f, 1.f }, { 0.f, 0.f, -1.f },
    };
    static const Fvector kUps[6] = {
        { 0.f, 1.f, 0.f }, { 0.f, 1.f, 0.f },
        { 0.f, 0.f, -1.f }, { 0.f, 0.f, 1.f },
        { 0.f, 1.f, 0.f }, { 0.f, 1.f, 0.f },
    };
    Fvector dir = kDirs[face % 6], up = kUps[face % 6];
    Fvector eye = pos;
    Fmatrix view; view.build_camera_dir(eye, dir, up);
    Fmatrix proj; proj.build_projection(deg2rad(90.f), 1.f, kPointNear, _max(range, 1.f));
    Fmatrix vp; vp.mul(proj, view);
    return vp;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_sampler)    { vkDestroySampler(VulkanHW.m_Device, s_sampler, nullptr); s_sampler = VK_NULL_HANDLE; }
    if (s_view)       { vkDestroyImageView(VulkanHW.m_Device, s_view, nullptr); s_view = VK_NULL_HANDLE; }
    if (s_image)      { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_image, s_alloc); s_image = VK_NULL_HANDLE; s_alloc = VK_NULL_HANDLE; }
    if (s_viewStatic) { vkDestroyImageView(VulkanHW.m_Device, s_viewStatic, nullptr); s_viewStatic = VK_NULL_HANDLE; }
    for (u32 i = 0; i < kNumSunCascades; ++i) {
        if (s_cascView[i])  { vkDestroyImageView(VulkanHW.m_Device, s_cascView[i], nullptr); s_cascView[i] = VK_NULL_HANDLE; }
        if (s_cascImage[i]) { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_cascImage[i], s_cascAlloc[i]); s_cascImage[i] = VK_NULL_HANDLE; s_cascAlloc[i] = VK_NULL_HANDLE; }
        if (s_cascStaticView[i])  { vkDestroyImageView(VulkanHW.m_Device, s_cascStaticView[i], nullptr); s_cascStaticView[i] = VK_NULL_HANDLE; }
        if (s_cascStaticImage[i]) { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_cascStaticImage[i], s_cascStaticAlloc[i]); s_cascStaticImage[i] = VK_NULL_HANDLE; s_cascStaticAlloc[i] = VK_NULL_HANDLE; }
        if (s_cascMidView[i])  { vkDestroyImageView(VulkanHW.m_Device, s_cascMidView[i], nullptr); s_cascMidView[i] = VK_NULL_HANDLE; }   // nfs48
        if (s_cascMidImage[i]) { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_cascMidImage[i], s_cascMidAlloc[i]); s_cascMidImage[i] = VK_NULL_HANDLE; s_cascMidAlloc[i] = VK_NULL_HANDLE; }
    }
    if (s_imageStatic){ VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_imageStatic, s_allocStatic); s_imageStatic = VK_NULL_HANDLE; s_allocStatic = VK_NULL_HANDLE; }
    if (s_rainView)   { vkDestroyImageView(VulkanHW.m_Device, s_rainView, nullptr); s_rainView = VK_NULL_HANDLE; }
    if (s_rainImage)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_rainImage, s_rainAlloc); s_rainImage = VK_NULL_HANDLE; s_rainAlloc = VK_NULL_HANDLE; }
    if (s_groundView) { vkDestroyImageView(VulkanHW.m_Device, s_groundView, nullptr); s_groundView = VK_NULL_HANDLE; }
    if (s_groundImage){ VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_groundImage, s_groundAlloc); s_groundImage = VK_NULL_HANDLE; s_groundAlloc = VK_NULL_HANDLE; }
    if (s_spotView)   { vkDestroyImageView(VulkanHW.m_Device, s_spotView, nullptr); s_spotView = VK_NULL_HANDLE; }
    if (s_spotImage)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_spotImage, s_spotAlloc); s_spotImage = VK_NULL_HANDLE; s_spotAlloc = VK_NULL_HANDLE; }
    if (s_spotStaticView)  { vkDestroyImageView(VulkanHW.m_Device, s_spotStaticView, nullptr); s_spotStaticView = VK_NULL_HANDLE; }
    if (s_spotStaticImage) { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_spotStaticImage, s_spotStaticAlloc); s_spotStaticImage = VK_NULL_HANDLE; s_spotStaticAlloc = VK_NULL_HANDLE; }
    if (s_spotBeamView)  { vkDestroyImageView(VulkanHW.m_Device, s_spotBeamView, nullptr); s_spotBeamView = VK_NULL_HANDLE; }
    if (s_spotBeamImage) { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_spotBeamImage, s_spotBeamAlloc); s_spotBeamImage = VK_NULL_HANDLE; s_spotBeamAlloc = VK_NULL_HANDLE; }
    for (auto& v : s_pointFaceView)
        if (v) { vkDestroyImageView(VulkanHW.m_Device, v, nullptr); v = VK_NULL_HANDLE; }
    for (auto& v : s_pointStaticFaceView)
        if (v) { vkDestroyImageView(VulkanHW.m_Device, v, nullptr); v = VK_NULL_HANDLE; }
    if (s_pointCubeView) { vkDestroyImageView(VulkanHW.m_Device, s_pointCubeView, nullptr); s_pointCubeView = VK_NULL_HANDLE; }
    if (s_pointImage)    { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_pointImage, s_pointAlloc); s_pointImage = VK_NULL_HANDLE; s_pointAlloc = VK_NULL_HANDLE; }
    if (s_pointStaticImage) { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_pointStaticImage, s_pointStaticAlloc); s_pointStaticImage = VK_NULL_HANDLE; s_pointStaticAlloc = VK_NULL_HANDLE; }
    s_inited = false; s_failed = false;
}

}}  // namespace VK::ShadowMap
