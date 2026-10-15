// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include <chrono>
#include "CRender_Vulkan.h"
#include "vk_render_view.h"
#include "vk_env_light.h"
#include "../../xr_3da/device.h"
#include "vk_light.h"
#include "vk_env_snapshot.h"
#include "../../xr_3da/IGame_Persistent.h"
#include "../../xr_3da/Environment.h"
#include "HW_Vulkan.h"
#include "vk_swapchain.h"
#include "vk_sync.h"
#include "vk_command_buffer.h"
#include "vk_barriers.h"
#include "vk_texture_stream.h"   // VK::TextureStreamer::Frame — per-frame streaming tick
#include "vk_vram_stats.h"       // VK::Vram::DumpVmaJson — DLSS-rescue post-mortem
#include "vk_cluster_stream.h"   // VK::ClusterStream — cluster-IB page streaming (Stage B)
#include "vk_world_material.h"   // WorldMaterialCache::FrameTick — retired-set recycling
#include "vk_UIPipeline.h"
#include "vk_pass_world.h"
#include "vk_render_queue.h"   // g_RenderQueue (its glass/water lists are reset in Calculate)
#include "vk_pass_sky.h"
#include "vk_pass_snow.h"
#include "vk_scene_color.h"     // HDR scene target (passes render here, then tonemap)
#include "vk_shaders.h"         // g_ShaderManager (SPIRV loader) — editor overlay lines (Spike 2)
#include "vk_gfx_pipeline.h"    // VK::GfxPipelineBuilder
#include "vk_pass_context.h"    // VK::BeginOverlayRendering — editor overlay pass (Spike 2)
#include "vk_motionvec.h"       // screen-space motion vectors + r_mv_debug overlay
#include "vk_dlss.h"            // VK::Dlss — DLSS Super Resolution (DLAA) upscale before tonemap
#include "vk_fsr.h"                     // VK::Fsr - FidelityFX Super Resolution 3.1.4 (the AMD/Intel/older-NVIDIA seat)
#include "vk_rr_guides.h"       // VK::RRGuide — albedo/normal/roughness G-buffer for Ray Reconstruction
#include "vk_sl.h"              // VK::SL — Streamline (Stage B: SR evaluate via sl.dlss, r_dlss_sl)

extern u32 ps_r_dlss;           // r_dlss — master toggle (latched OFF here on a failed evaluate)
extern int ps_r_particles_mt;   // r_particles_mt — CPU particle sim on the TTAPI pool (R4 calculate_particles_async)
extern int ps_r_particles_core; // r_particles_core -- which core's worker runs the particle sim beside the main thread
namespace VK { namespace PartStats { void Take(u64& steps, u64& rays, string512& top); } }   // vk_ParticleEffectDef.cpp
extern int ps_r_dlss_sl;        // r_dlss_sl — route the SR evaluate through Streamline (FG prerequisite)
extern int ps_r_dlss_rr;        // r_dlss_rr — Ray Reconstruction; overrides Super Resolution in the same slot
extern int ps_r_dlss_nr;        // r_dlss_nr — Neural Rendering; overrides BOTH of the above in the same slot
extern u32 ps_r_dlss_nr_featid; // r_dlss_nr_featid — the NGX feature ID being tried (no public constant exists)
extern int ps_r_dlss_fg;        // r_dlss_fg — Stage C: DLSS Frame Generation (MFG)
extern int ps_r_dlss_fg_mult;   // r_dlss_fg_mult — frame multiplier 2..6
extern int ps_r_grass;
extern int ps_r_trees;
extern float ps_r_render_scale; // r_render_scale — internal resolution / display resolution when DLSS is not upscaling
#include "vk_pass_tonemap.h"    // Pass_TonemapComposite — HDR → swapchain
#include "vk_pass_registry.h"   // VK::RegisterPass / ExecutePasses — framegraph seam
#include "vk_pipeline_cache.h"  // PrewarmPending — the precache countdown asks whether the warm-up still needs frames
#include "vk_framegraph.h"      // VK::g_FrameGraph — frame-wide resource-state tracker
#include "vk_async.h"           // VK::Async — async compute (cross-queue) foundation
#include "vk_profiler.h"        // VK::Prof — GPU/CPU zones, debug labels, VRAM
#include "vk_imgui.h"           // VK::ImGuiVK — profiler overlay (r_profiler 2)
#include "vk_DetailManager.h"   // VK::CDetailManager — grass render entry
#include "vk_TreeManager.h"     // VK::CTreeManager — tree render entry
#include "vk_LODManager.h"      // VK::CLODManager — LOD imposter render entry
#include "vk_stub.h"
#include "vk_ModelPool.h"
#include "vk_Visual.h"  // vkRender_Visual base for the particle stub
#include "../../Include/xrRender/ParticleCustom.h"
#include "vk_Particles.h"        // vkCreateParticle (real particle visuals)
#include "vk_pass_particles.h"   // VK::Pass_Particles registration
#include "vk_gpu_particles.h"    // VK::GPUParticles — GPU-driven particles (Phase 0)
#include "vk_wallmarks.h"        // VK::Wallmarks — bullet holes / decals on level geometry
#include "vk_rain.h"             // VK::Pass_Rain — rain drops/splashes + thunderbolt
#include "vk_pass_shadow.h"      // VK::Pass_SunShadow (sun shadow caster, before World)
#include "vk_volumetrics.h"      // VK::Vol::SnapshotShadows (async fog shadow source)
#include "vk_pass_skinned.h"     // VK::Skinned_PreSkin (compute pre-skinning, before every consumer)
#include "vk_pass_lightcones.h"  // VK::Pass_LightCones (per-light volumetric beams)
#include "vk_pass_capsule_shadow.h" // VK::Pass_CapsuleShadows (r_shadow_lite: characters' sun shadows)
#include "vk_pass_water.h"       // VK::Pass_Water (level water bodies)
#include "vk_water_ripple.h"     // VK::WaterRipple::LidExtract — the lid rays, PREPARE phase
#include "vk_pass_rtgi.h"        // VK::RTGIPass::Execute (RT preview + the RT scene step)
#include "vk_ddgi.h"             // VK::DDGI::Update (probe volume — trace + blend)
#include "vk_pass_gather.h"      // VK::GatherPass::DrawDebug (r_gather_debug)
#include "vk_light.h"            // VK::vkLight (dynamic point/spot lights, STEP 3)
#include "vk_instance_gpu.h"     // VK::InstanceGPU (host scene → GPU-driven instanced casters)
#include "../../xrCDB/ISpatial.h"      // g_SpatialSpace, ISpatial, STYPE_RENDERABLE (dynamic collection)
#include "../../xrCDB/Frustum.h"       // CFrustum
#include "../../xr_3da/IRenderable.h"  // IRenderable::renderable_Render
#include "../../xr_3da/IGame_Level.h"  // g_pGameLevel (skip dynamic collection in the menu)
#include "../../xr_3da/CustomHUD.h"    // g_hud->Render_MAIN (collect first-person HUD visuals)
#include "../../xr_3da/PS_instance.h"  // CPS_Instance::PerformFrame (tick particle simulation)
#include "../../xr_3da/IGame_Persistent.h" // g_pGamePersistent (VKEditor readiness gate — same as Pass_Sky)
#include <mutex>                       // per-object hemi cache guard (add_Visual)
#include <cstdlib>                     // std::getenv — XROS_AUTO_SHOT unattended capture
#include "../../xr_3da/XR_IOConsole.h"  // Console->Execute — XROS_AUTO_SHOT_CMDS sweep

CRender RImplementation;

// The render-pass registry (RegisterPass / ExecutePasses / the declared-dependency
// validator) moved to vk_pass_registry.cpp when the declarations gave it a body
// worth reading on its own. The registration LIST still lives here, in Render().

namespace {
// Frame state bridging Begin → Clear → End across IRender_interface methods,
// which were shaped around a D3D11 imm-context. Becomes part of the
// framegraph once we have one.
struct FrameInFlight
{
    VkCommandBuffer cmd          = VK_NULL_HANDLE;
    u32             imageIndex   = 0;
    bool            valid        = false;  // false ⇒ swapchain acquire failed; skip submit
};
FrameInFlight g_FrameInFlight{};
// Per frame slot: has the render side (Begin_Render) of the frame in this slot run yet. Reset at extract, set on the render thread.
bool s_renderSideBegun[RT::kSlots] = { false, false };
}   // anonymous namespace

// Per-frame context handed to Pass_* functions. Begin() fills it; Render()
// forwards to Pass_World. EXTERNAL linkage on purpose: vk_env_light.cpp reads
// the scene render / display extents (ao_params screen scale + DLSS mip bias).

VK::FrameContext g_FrameCtx{};

// See the note on the declaration in vk_pass_context.h. Defined next to
// g_FrameCtx so the fallback (no frame context yet — level load, first frames)
// is honest: before the first Begin the render extent is 0 and the display
// height is all there is.
namespace VK
{
    u32 LodScreenHeight()
    {
        return g_FrameCtx.extent.height ? g_FrameCtx.extent.height : Device.dwHeight;
    }
}

namespace {

// Pre-frame clear color. UI panels render their own backdrops over this.
constexpr VkClearColorValue kBackgroundClear{ {0.0f, 0.0f, 0.0f, 1.0f} };

// Set by CRender::Screenshot, consumed (and cleared) by CRender::End once the
// frame has finished rendering but before vkQueuePresent.
xr_string g_PendingScreenshotPath;
std::mutex g_ShotMx;   // guards g_PendingScreenshotPath: queued from the main thread, consumed by the render thread (two-slot pipeline)
} // namespace

// Allocator-managed staging buffer kept alive across End() submit so the GPU
// can finish the copy before we map. Cleared on the next screenshot.
struct PendingShot {
    VkBuffer       buf{};
    VmaAllocation  alloc{};
    VmaAllocationInfo info{};
    u32            width{}, height{};
    xr_string      path;
};
static PendingShot g_pendingShot{};

static void ScreenshotRecord(VkCommandBuffer cmd, VkImage src, VkExtent2D extent, const xr_string& path)
{
    if (g_pendingShot.buf) {
        // Previous shot wasn't drained yet — skip this one.
        return;
    }
    const u32 width  = extent.width;
    const u32 height = extent.height;
    const VkDeviceSize size = (VkDeviceSize)width * height * 4;  // BGRA8

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    if (VK::Vram::CreateBuffer(VulkanHW.m_Allocator, &bci, &aci,
                        &g_pendingShot.buf, &g_pendingShot.alloc, &g_pendingShot.info) != VK_SUCCESS) {
        Msg("![VK Screenshot] staging buffer alloc failed");
        g_pendingShot = {};
        return;
    }

    auto barrier = [&](VkImageLayout oldL, VkImageLayout newL,
                       VkPipelineStageFlags2 srcStg, VkAccessFlags2 srcAcc,
                       VkPipelineStageFlags2 dstStg, VkAccessFlags2 dstAcc) {
        VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = srcStg; b.srcAccessMask = srcAcc;
        b.dstStageMask = dstStg; b.dstAccessMask = dstAcc;
        b.oldLayout = oldL; b.newLayout = newL;
        b.image = src;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    };

    // Called from End() AFTER the image has been transitioned to PRESENT_SRC
    // (post-EndUIPass / post-tail-transition). Move PRESENT_SRC → TRANSFER_SRC
    // for readback, then back so vkQueuePresent still sees PRESENT_SRC.
    barrier(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT,         VK_ACCESS_2_TRANSFER_READ_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { width, height, 1 };
    vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_pendingShot.buf, 1, &region);

    barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COPY_BIT,         VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT);

    // The round-trip lands back on PRESENT_SRC, so the tracked LAYOUT is still
    // right — but the stage/access it went through are not the ones the registry
    // recorded at End(). Tell it, for the same reason the tonemap does: a tracker
    // is only worth having while it is never quietly behind.
    VK::g_FrameGraph.Track(src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED)
        .Seed(src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT);

    g_pendingShot.width  = width;
    g_pendingShot.height = height;
    g_pendingShot.path   = path;
}

// Drained next frame after the GPU has definitely flushed the CopyImageToBuffer.
static void ScreenshotDrain()
{
    if (!g_pendingShot.buf) return;

    const u8* mapped = (const u8*)g_pendingShot.info.pMappedData;
    if (mapped) {
        FILE* f = nullptr;
        fopen_s(&f, g_pendingShot.path.c_str(), "wb");
        if (f) {
            u8 hdr[18]{};
            hdr[2]  = 2;                  // uncompressed true-color
            hdr[12] = (u8)(g_pendingShot.width  & 0xff); hdr[13] = (u8)((g_pendingShot.width  >> 8) & 0xff);
            hdr[14] = (u8)(g_pendingShot.height & 0xff); hdr[15] = (u8)((g_pendingShot.height >> 8) & 0xff);
            hdr[16] = 24;                 // bits per pixel — TGA bottom-up
            fwrite(hdr, 1, sizeof(hdr), f);

            xr_vector<u8> row(g_pendingShot.width * 3);
            for (int y = (int)g_pendingShot.height - 1; y >= 0; --y) {
                const u8* src_row = mapped + (size_t)y * g_pendingShot.width * 4;
                for (u32 x = 0; x < g_pendingShot.width; ++x) {
                    row[x * 3 + 0] = src_row[x * 4 + 0]; // B
                    row[x * 3 + 1] = src_row[x * 4 + 1]; // G
                    row[x * 3 + 2] = src_row[x * 4 + 2]; // R
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
            Msg("[VK Screenshot] saved %s (%ux%u)", g_pendingShot.path.c_str(), g_pendingShot.width, g_pendingShot.height);
        } else {
            Msg("![VK Screenshot] fopen failed: %s", g_pendingShot.path.c_str());
        }
    }
    VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, g_pendingShot.buf, g_pendingShot.alloc);
    g_pendingShot = {};
}

CRender::CRender()  = default;
CRender::~CRender() = default;

// ----- Loading / Unloading --------------------------------------------------
// level_Load / level_Unload bodies live in rvk_loader.cpp.
//
// NOTE: OGSR engine doesn't call CRender::create()/destroy() through any path
// our build wires up (R4's flow goes through dxRenderDeviceRender). So these
// stay no-ops here and the actual lifecycle (Models, WorldPipeline) is owned
// by vkRenderDeviceRender::Create()/Destroy(), which IS called.
void CRender::create()       { VK_STUB_ONCE("CRender"); }
void CRender::destroy()      { VK_STUB_ONCE("CRender"); }

void CRender::reset_begin()  { VK_STUB_ONCE("CRender"); }
void CRender::reset_end()    { VK_STUB_ONCE("CRender"); }

// ----- Shader compile -------------------------------------------------------
HRESULT CRender::shader_compile(LPCSTR /*name*/, DWORD const* /*pSrcData*/,
                                       UINT /*SrcDataLen*/, LPCSTR /*pFunctionName*/,
                                       LPCSTR /*pTarget*/, DWORD /*Flags*/,
                                       void*& /*result*/)
{
    VK_STUB_ONCE("CRender");
    return E_NOTIMPL;
}

// ----- Information ----------------------------------------------------------
LPCSTR CRender::getShaderPath() { return "vk\\"; }
// Resolves OGF_CHILDREN_L links (child IDs into the level's Visuals[] array)
// during hierarchy/LOD load. Without this, every CHILDREN_L lookup silently
// returns null — composite visuals end up empty (e.g. tree LOD containers
// lose their high-quality mesh children, so trees vanish from the level).
IRenderVisual* CRender::getVisual(int id)
{
    if (id >= 0 && id < (int)Visuals.size())
        return Visuals[id];
    return nullptr;
}
u32 CRender::getVisualCount() { return (u32)Visuals.size(); }

// IRender_Target stub. Engine + xrGame UI code ask for it during init to read
// resolution and silently apply post-process settings before any frame runs.
// Width/height reflect Device dimensions; set_* land when post-FX ports over.
namespace {
struct vkRenderTarget_Stub final : IRender_Target
{
    void set_blur(float)               override {}
    void set_gray(float)               override {}
    void set_duality_h(float)          override {}
    void set_duality_v(float)          override {}
    void set_noise(float)              override {}
    void set_noise_scale(float)        override {}
    void set_noise_fps(float)          override {}
    void set_color_base(u32)           override {}
    void set_color_gray(u32)           override {}
    void set_color_add(const Fvector&) override {}
    u32  get_width(CBackend&)          override { return Device.dwWidth; }
    u32  get_height(CBackend&)         override { return Device.dwHeight; }
    void set_cm_imfluence(float)       override {}
    void set_cm_interpolate(float)     override {}
    void set_cm_textures(const shared_str&, const shared_str&) override {}
};
vkRenderTarget_Stub g_RenderTarget_Stub;
} // namespace

IRender_Target* CRender::getTarget()
{
    VK_STUB_ONCE("CRender");
    return &g_RenderTarget_Stub;
}

// ----- Visuals & wallmarks --------------------------------------------------
namespace {
// Per-object sky-visibility (hemi) — amortized ray tracing, mirrors the engine's
// CROS_impl (LightTrack.cpp). Dynamic objects have no baked lightmap occlusion,
// so without this they get the full hemisphere sky ambient even in a basement
// and glow. We trace a few rays over the upper hemisphere against the STATIC
// level: hemi = fraction that reach open sky. Cached per renderable, re-traced
// every kRetraceFrames or on move, smoothed.
struct HemiEntry { float target = 1.0f; float smooth = 1.0f; u32 frame = 0; Fvector pos{ 1e9f, 1e9f, 1e9f }; };
xr_unordered_map<const void*, HemiEntry> s_hemiCache;
std::mutex s_hemiMtx;

const Fvector kHemiDirs[9] = {
    { 0.f, 1.f, 0.f },
    { 0.707f, 0.707f, 0.f }, { -0.707f, 0.707f, 0.f },
    { 0.f, 0.707f, 0.707f }, { 0.f, 0.707f, -0.707f },
    { 0.5f, 0.707f, 0.5f },  { -0.5f, 0.707f, 0.5f },
    { 0.5f, 0.707f, -0.5f }, { -0.5f, 0.707f, -0.5f },
};
constexpr u32   kHemiRetraceFrames = 20;
constexpr float kHemiRange         = 40.f;   // open sky beyond this
// ⭐What a firefight made of it: 📏07-10 Switch, Cordon village, 8 bandits on the
// village -- 7.8% of the main thread. A man running 5 m/s crossed the old 0.5 m
// step every 6 frames, nine 40 m rays each time, him and the gun in his hands
// apart. The sky over a man does not change in 1.5 m, nor in four frames; and a
// frame traces at most kHemiPerFrame objects -- the rest keep last value a frame.
constexpr float kHemiMoveSqr       = 1.5f * 1.5f;
constexpr u32   kHemiMinFrames     = 4;
constexpr u32   kHemiPerFrame      = 8;
u32 s_hemiBudgetFrame = 0, s_hemiBudget = 0;

float ComputeObjectHemi(const void* key, const Fvector& center)
{
    if (!g_pGameLevel) return 1.0f;
    std::lock_guard<std::mutex> lk(s_hemiMtx);
    if (s_hemiCache.size() > 4096) s_hemiCache.clear();   // bound churn (e.g. particle visuals)
    HemiEntry& e = s_hemiCache[key];
    if (s_hemiBudgetFrame != Device.dwFrame) { s_hemiBudgetFrame = Device.dwFrame; s_hemiBudget = kHemiPerFrame; }
    const u32 age = Device.dwFrame - e.frame;
    const bool moved = center.distance_to_sqr(e.pos) > kHemiMoveSqr && age >= kHemiMinFrames;
    if (e.frame == 0 || ((moved || age > kHemiRetraceFrames) && s_hemiBudget > 0)) {
        if (s_hemiBudget) --s_hemiBudget;
        u32 miss = 0;
        for (const Fvector& d : kHemiDirs)
            if (!g_pGameLevel->ObjectSpace.RayTest(center, d, kHemiRange, collide::rqtStatic, nullptr, nullptr))
                ++miss;
        e.target = float(miss) / 9.0f;
        e.frame  = Device.dwFrame ? Device.dwFrame : 1;
        e.pos    = center;
    }
    const float k = 1.f - expf(-Device.fTimeDelta * 4.0f);   // smooth (eye-adapt-like)
    e.smooth += (e.target - e.smooth) * k;
    return e.smooth;
}
}  // namespace

// Dynamic (spawned) visual registration. Called per-frame from the object
// traversal kicked off in CRender::Calculate(). Collect {visual, world-xform,
// hemi}; Pass_World drains the list. (Was a no-op stub — dynamic objects never drew.)
void CRender::add_Visual(u32, IRenderable* root, IRenderVisual* V, Fmatrix& m)
{
    if (!V) return;
    auto* rv = static_cast<vkRender_Visual*>(V);

    // SMAP phase (g_hud->Render_SMAP): everything collected here is a caster only —
    // the actor's own model, which the colour pass must keep invisible. No hemi (the
    // shadow pass doesn't light) and no wallmark tick (the same skeleton already got
    // one from its normal traversal, and a shadow caster shows no decals anyway).
    if (b_collect_shadow_only) { VK::ShadowOnlyVisuals().push_back({ rv, m, 1.0f }); return; }

    // HUD visuals (player hands + active item) are flagged via renderable_HUD()
    // by g_hud->Render_MAIN; they render in a separate near-depth/HUD-FOV pass.
    const bool isHud = (root && root->renderable_HUD());
    auto& list = isHud ? VK::HudVisuals() : VK::DynamicVisuals();

    // Sky-visibility for the ambient gate (world dynamics only; HUD is hand-lit).
    float hemi = 1.0f;
    if (!isHud && root) {
        Fvector center; m.transform_tiny(center, rv->vis.sphere.P);
        hemi = ComputeObjectHemi(root, center);
    }
    list.push_back({ rv, m, hemi, b_collect_no_shadow });

    // Skeleton wallmarks (blood on NPCs): age + append this skeleton's visible
    // marks for the Wallmarks pass (self-guarded per frame; bridge defined in
    // vk_SkeletonCustom.cpp — needs the full CKinematics type).
    extern void VK_SkelWM_Calculate(IRenderVisual* V, bool hud);
    VK_SkelWM_Calculate(V, isHud);
}

// Wallmark bridges (vk_RenderFactory.cpp) — pull the decal texture name out of
// the renderer-side IWallMarkArray / IUIShader implementations.
extern const char* VK_WallmarkArray_Pick(IWallMarkArray* A);
extern const char* VK_UIShaderTexName(IUIShader* S);

void CRender::add_StaticWallmark(const wm_shader& S, const Fvector& P, float s,
                                        CDB::TRI* T, Fvector* V)
{
    if (const char* tex = VK_UIShaderTexName(S.operator->()))
        VK::Wallmarks::AddStatic(tex, P, s, T, V);
}

void CRender::add_StaticWallmark(IWallMarkArray* pArray, const Fvector& P, float s,
                                        CDB::TRI* T, Fvector* V)
{
    if (const char* tex = VK_WallmarkArray_Pick(pArray))
        VK::Wallmarks::AddStatic(tex, P, s, T, V);
}

// add_SkeletonWallmark (blood on NPCs) is defined in vk_SkeletonCustom.cpp —
// it needs the full CKinematics/CSkeletonWallmark compat context.

void CRender::clear_static_wallmarks() { VK::Wallmarks::Clear(); }

// Minimal IRender_ObjectSpecific so AI memory (CVisualMemoryManager) and rain
// queries can read neutral luminocity without nulldereffing.
namespace {
struct vkROS_Stub final : public IRender_ObjectSpecific
{
    float m_hemi_cube[6]{ 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    float        get_luminocity() override              { return 0.5f; }
    float        get_luminocity_hemi() override         { return 0.5f; }
    const float* get_luminocity_hemi_cube() override    { return m_hemi_cube; }
};
} // namespace
IRender_ObjectSpecific* CRender::ros_create(IRenderable*)            { return xr_new<vkROS_Stub>(); }
void                    CRender::ros_destroy(IRender_ObjectSpecific*& p) { xr_delete(p); }

// ----- Lighting -------------------------------------------------------------
// Real dynamic lights (STEP 3): vkLight registers itself in the global registry;
// EnvLight::Update collects the nearest active ones into the shared Lighting UBO
// each frame and the forward shaders accumulate them. See vk_light.{h,cpp}.
IRender_Light* CRender::light_create() { return xr_new<VK::vkLight>(); }

// ----- Particles ------------------------------------------------------------
// Backed by the Vulkan PS library (vk_PSLibrary.cpp).
extern void  VK_ParticleEffectFillName(xr_vector<shared_str>&);
extern void  VK_ParticleGroupFillName(xr_vector<shared_str>&);
extern float VK_GetParticlesTimeLimit(const char* name);

void CRender::ParticleEffectFillName(xr_vector<shared_str>& s) { VK_ParticleEffectFillName(s); }
void CRender::ParticleGroupFillName(xr_vector<shared_str>& s)  { VK_ParticleGroupFillName(s); }
float CRender::GetParticlesTimeLimit(LPCSTR name)             { return VK_GetParticlesTimeLimit(name); }

// Inert particle visual: satisfies both IRenderVisual (via vkRender_Visual)
// and IParticleCustom so xrGame's `smart_cast<IParticleCustom*>(visual)` lands
// on a non-null object. Reports zero lifetime / not playing / no particles —
// the engine ticks it once, sees no work, and moves on. Real particle classes
// (vk_ParticleEffect / vk_ParticleGroup) replace this when they port over.
namespace {
struct vkParticleVisual_Stub final : public vkParticleVisual
{
    shared_str  m_name;

    vkParticleVisual_Stub() { Type = MT_PARTICLE_EFFECT; }

    // vkParticleVisual — nothing to draw (dcast_ParticleCustom comes from the base).
    void  CollectEffects(xr_vector<vkCParticleEffect*>&) override {}

    // IParticleCustom — all no-op / safe defaults.
    void  OnDeviceCreate()                 override {}
    void  OnDeviceDestroy()                override {}
    void  UpdateParent(const Fmatrix&, const Fvector&, BOOL) override {}
    void  OnFrame(u32)                     override {}
    void  Play()                           override {}
    void  Stop(BOOL)                       override {}
    BOOL  IsPlaying()                      override { return FALSE; }
    BOOL  IsDeferredStopped()              override { return TRUE;  }
    u32   ParticlesCount()                 override { return 0; }
    // Tiny positive value so CParticlesObject::Init takes the time-limited
    // branch (m_iLifeTime = 1 ms) instead of the looped one. Returning 0
    // here previously fataled when auto-remove fire-effects (BM16 etc.)
    // tried to spawn through us — looped + auto-remove is illegal.
    float GetTimeLimit()                   override { return 0.001f; }
    const shared_str Name()                override { return m_name; }
    void  SetHudMode(BOOL)                 override {}
    BOOL  GetHudMode()                     override { return FALSE; }
};
} // namespace

// ----- Models — delegates to vkModelPool (created in CRender::create) -------
IRenderVisual* CRender::model_CreateParticles(LPCSTR name, BOOL /*bNoPool*/)
{
    // Resolve against the loaded PS library → dedicated vk particle visual
    // (effect or group), simulated via PAPI and drawn by Pass_Particles.
    if (vkParticleVisual* p = vkCreateParticle(name))
        return p;

    // Unknown name → inert stub so spawn paths never null-deref.
    auto* s   = xr_new<vkParticleVisual_Stub>();
    s->m_name = name ? name : "";
    return s;
}

IRenderVisual* CRender::model_Create(LPCSTR name, IReader* data)
{
    IRenderVisual* v = Models ? Models->Create(name, data, true) : nullptr;
    if (!v)
        Msg("![CRender::model_Create] '%s' returned nullptr (Models=%p)", name ? name : "<null>", (void*)Models);
    return v;
}

IRenderVisual* CRender::model_CreateChild(LPCSTR name, IReader* data)
{
    return Models ? Models->CreateChild(name, data) : nullptr;
}

IRenderVisual* CRender::model_Duplicate(IRenderVisual* V)
{
    if (!Models || !V) return nullptr;
    return Models->Instance_Duplicate(static_cast<vkRender_Visual*>(V));
}

void CRender::model_Delete(IRenderVisual*& V, BOOL bDiscard)
{
    if (!Models || !V) { V = nullptr; return; }
    auto* p = static_cast<vkRender_Visual*>(V);
    Models->Delete(p, bDiscard);
    V = p;  // pool nulls the pointer; mirror back to caller
}

void CRender::model_Logging(BOOL) {}
void CRender::models_Prefetch() {}
void CRender::models_Clear(BOOL b_complete) { if (Models) Models->ClearPool(b_complete); }
void CRender::models_savePrefetch() {}
void CRender::models_begin_prefetch1(bool) {}

// append_SkeletonWallmark is defined in vk_SkeletonCustom.cpp (needs the
// complete CSkeletonWallmark type) — it queues the mark for VK::Wallmarks.

// ---------------------------------------------------------------------------
// SPIKE 2 (editor-on-Vulkan): minimal world-space line overlay — the primitive
// editors use for grid / gizmos / selection boxes. CDUInterface / IDebugRender
// are no-ops on VK; this proves an "EditorOverlay" framegraph pass (RegisterPass)
// drawing depth-tested 3D lines on top of the scene is feasible. Lazy-inits its
// own line pipeline + host vertex buffer. Additive/experimental; ROADMAP §11.
// ---------------------------------------------------------------------------
namespace EditorOverlay {

struct LineVertex { Fvector pos; u32 color; };   // 16 B; color = R8G8B8A8_UNORM (byte0=R)

static VkPipeline       s_Pipe      = VK_NULL_HANDLE;
static VkPipeline       s_PipeSwap  = VK_NULL_HANDLE;  // swapchain-format variant (post-tonemap editor overlay)
static bool             s_editorBackdrop = false;      // chunk 2: OFF — the HDR EditorClear grey (0.044→85) is the backdrop now,
                                                       // so the lit model (drawn into HDR, depth-tested) survives tonemap. A flat
                                                       // post-tonemap fill would overwrite it. Re-enable only for the no-model view.
static VkPipelineLayout s_Layout    = VK_NULL_HANDLE;
static VkBuffer         s_VB        = VK_NULL_HANDLE;
static VmaAllocation    s_VBAlloc   = VK_NULL_HANDLE;
static void*            s_VBMap     = nullptr;
static bool             s_initDone  = false;
static constexpr u32    MAX_VERTS   = 200000;
static xr_vector<LineVertex> s_Lines;

static inline u32 rgba(u8 r, u8 g, u8 b, u8 a) { return u32(r) | (u32(g) << 8) | (u32(b) << 16) | (u32(a) << 24); }

void AddLine(const Fvector& a, const Fvector& b, u32 col)
{
    if (s_Lines.size() + 2 > MAX_VERTS) return;
    s_Lines.push_back({ a, col });
    s_Lines.push_back({ b, col });
}

void AddBoxAA(const Fvector& c, float h, u32 col)
{
    Fvector p[8];
    for (int i = 0; i < 8; ++i)
        p[i].set(c.x + ((i & 1) ? h : -h), c.y + ((i & 2) ? h : -h), c.z + ((i & 4) ? h : -h));
    static const int e[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
    for (auto& pr : e) AddLine(p[pr[0]], p[pr[1]], col);
}

// Tight wireframe AABB from explicit min/max corners — the original SDK LevelEditor
// obtusely hugs an object's real bounding box (CEditableObject::GetBox), not a
// sphere-derived cube. 12 edges.
void AddBox(const Fvector& mn, const Fvector& mx, u32 col)
{
    Fvector p[8];
    for (int i = 0; i < 8; ++i)
        p[i].set((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z);
    static const int e[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
    for (auto& pr : e) AddLine(p[pr[0]], p[pr[1]], col);
}

// Small RGB world-axis tripod at the origin (X=red, Y=green, Z=blue) — the SDK's
// coordinate marker. Length in metres.
void AddAxis(const Fvector& o, float len)
{
    Fvector x = o, y = o, z = o;
    x.x += len; y.y += len; z.z += len;
    AddLine(o, x, rgba(220, 60, 60, 255));
    AddLine(o, y, rgba(60, 210, 60, 255));
    AddLine(o, z, rgba(70, 110, 230, 255));
}

static void EnsureInit()
{
    if (s_initDone) return;
    if (!g_ShaderManager || VulkanHW.m_Device == VK_NULL_HANDLE) return;
    s_initDone = true;   // one-shot: on failure we log and stay a no-op

    VkShaderModule vs = g_ShaderManager->Load("editor_line.vert.spv");
    VkShaderModule fs = g_ShaderManager->Load("editor_line.frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) { Msg("![VK EditorOverlay] editor_line.{vert,frag}.spv load failed"); return; }

    VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; pcr.size = sizeof(Fmatrix);
    VkPipelineLayoutCreateInfo plci{}; plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_Layout);

    s_Pipe = VK::GfxPipelineBuilder(s_Layout)
        .Vert(vs).Frag(fs)
        .Binding(0, sizeof(LineVertex))
        .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
        .Attr(1, 0, VK_FORMAT_R8G8B8A8_UNORM,  12)
        .Topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
        .Depth(true, false)
        .Color(VK::SceneColor::Format()).BlendAlpha()
        .DepthTarget(Swapchain.m_DepthFormat)
        .Build("EditorOverlay lines");
    if (s_Pipe == VK_NULL_HANDLE)
        return;

    VkBufferCreateInfo bci{}; bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = VkDeviceSize(MAX_VERTS) * sizeof(LineVertex);
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (VK::Vram::CreateBuffer(VulkanHW.m_Allocator, &bci, &aci, &s_VB, &s_VBAlloc, &info) != VK_SUCCESS) {
        Msg("![VK EditorOverlay] vertex buffer alloc failed"); return;
    }
    s_VBMap = info.pMappedData;
    Msg("[VK EditorOverlay] init OK (LINE_LIST pipeline + %u-vertex host buffer)", MAX_VERTS);
}

void Execute(VK::FrameContext& ctx)
{
    EnsureInit();
    if (s_Pipe == VK_NULL_HANDLE || s_VBMap == nullptr || s_Lines.empty()) { s_Lines.clear(); return; }

    u32 n = (u32)s_Lines.size(); if (n > MAX_VERTS) n = MAX_VERTS;
    memcpy(s_VBMap, s_Lines.data(), size_t(n) * sizeof(LineVertex));

    VK::BeginOverlayRendering(ctx.cmd, ctx, VK_ATTACHMENT_STORE_OP_DONT_CARE);   // test depth, don't write
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_Pipe);
    Fmatrix mvp = ctx.viewProj ? *ctx.viewProj : Fidentity;
    vkCmdPushConstants(ctx.cmd, s_Layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Fmatrix), &mvp);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &s_VB, &off);
    vkCmdDraw(ctx.cmd, n, 1, 0, 0);
    vkCmdEndRendering(ctx.cmd);

    s_Lines.clear();
}

// Build the swapchain-format variant of the line pipeline (no depth): used to draw
// the editor grid/gizmos as a POST-TONEMAP overlay on the final LDR image, so the
// bright lines stay crisp (no HDR bloom, no tone-curve crush) — the DCC approach.
static void EnsureInitSwap()
{
    if (s_PipeSwap != VK_NULL_HANDLE) return;
    EnsureInit();  // shares s_Layout + s_VB
    if (s_Layout == VK_NULL_HANDLE || !g_ShaderManager) return;

    VkShaderModule vs = g_ShaderManager->Load("editor_line.vert.spv");
    VkShaderModule fs = g_ShaderManager->Load("editor_line.frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) return;

    // Same as s_Pipe but no depth at all, into the final LDR swapchain image.
    s_PipeSwap = VK::GfxPipelineBuilder(s_Layout)
        .Vert(vs).Frag(fs)
        .Binding(0, sizeof(LineVertex))
        .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
        .Attr(1, 0, VK_FORMAT_R8G8B8A8_UNORM,  12)
        .Topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
        .Color(Swapchain.m_Format).BlendAlpha()   // B8G8R8A8_UNORM
        .Build("EditorOverlay lines (swapchain)");
    if (s_PipeSwap != VK_NULL_HANDLE)
        Msg("[VK EditorOverlay] post-tonemap (swapchain) pipeline OK");
}

// Draw the accumulated lines onto the final tonemapped swapchain image. Crisp +
// bright: direct LDR colors, no bloom, no tone curve. Consumes s_Lines.
void ExecutePostTonemap(VkCommandBuffer cmd, VkImageView colorView, VkExtent2D extent, const Fmatrix& mvp)
{
    EnsureInitSwap();
    if (s_PipeSwap == VK_NULL_HANDLE || s_VBMap == nullptr || s_Lines.empty()) { s_Lines.clear(); return; }

    u32 n = (u32)s_Lines.size(); if (n > MAX_VERTS) n = MAX_VERTS;
    memcpy(s_VBMap, s_Lines.data(), size_t(n) * sizeof(LineVertex));

    VK::RenderingBuilder(extent).Color(colorView).BeginFlipped(cmd);

    // Flat grey backdrop, direct LDR — matches the original SDK editor's
    // scene_clear_color 0x555555. Done here (post-tonemap) because the HDR clear
    // is crushed unpredictably by the tone curve. (Model support: switch to a
    // depth-tested fill so the lit object isn't overwritten — Phase B chunk 2.)
    if (s_editorBackdrop) {
        VkClearAttachment ca{}; ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca.colorAttachment = 0;
        ca.clearValue.color = { { 85.f / 255.f, 85.f / 255.f, 85.f / 255.f, 1.f } };
        VkClearRect cr{}; cr.rect.extent = extent; cr.layerCount = 1;
        vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_PipeSwap);
    Fmatrix m = mvp;
    vkCmdPushConstants(cmd, s_Layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Fmatrix), &m);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &s_VB, &off);
    vkCmdDraw(cmd, n, 1, 0, 0);
    vkCmdEndRendering(cmd);
    s_Lines.clear();
}
} // namespace EditorOverlay

// ---------------------------------------------------------------------------
// SPIKE 1 (editor-on-Vulkan): draw a single .ogf with an orbit camera and NO
// level loaded — the "render a model outside the game" probe. Gated by
// -vk_spike [model_name]. Additive/experimental; see EDITOR_ON_VULKAN_ROADMAP.md
// in the SDK repo. Delete once the real editor host lands.
// ---------------------------------------------------------------------------
namespace Spike {
static bool           s_checked   = false;
static bool           s_enabled   = false;
static string_path    s_model     = { 0 };
static IRenderVisual* s_visual    = nullptr;
static bool           s_triedLoad = false;
static int            s_frame     = 0;
static bool           s_shot      = false;

bool Enabled()
{
    if (!s_checked) {
        s_checked = true;
        const char* p = Core.Params ? strstr(Core.Params, "-vk_spike") : nullptr;
        if (p) {
            s_enabled = true;
            p += xr_strlen("-vk_spike");
            while (*p == ' ') ++p;
            if (*p && *p != '-') {
                int i = 0;
                while (*p && *p != ' ' && i < int(sizeof(s_model)) - 1) s_model[i++] = *p++;
                s_model[i] = 0;
            } else {
                // Placeholder — override with `-vk_spike <path\to\model>` (no .ogf ext).
                xr_strcpy(s_model, "dynamics\\device\\device_torch\\device_torch");
            }
            Msg("[VK][spike] enabled, model='%s'", s_model);
        }
    }
    return s_enabled;
}

void Frame()
{
    if (!s_triedLoad) {
        s_triedLoad = true;
        s_visual = RImplementation.model_Create(s_model, nullptr);
        if (!s_visual)
            Msg("![VK][spike] model_Create failed for '%s'", s_model);
        else
            Msg("[VK][spike] model loaded OK: '%s'", s_model);
    }
    if (!s_visual) return;

    // The main menu draws an opaque fullscreen background over the world pass, so
    // nothing shows there — the box is only visible once a level is loaded.
    if (!g_pGameLevel) return;

    // Drop the box 3 m in front of the player's camera (no camera hijack): it
    // renders on top of the real scene via the normal dynamic path, proving
    // add_Visual + VK rasterization of a model outside the game's own use.
    Fvector pos; pos.mad(Device.vCameraPosition, Device.vCameraDirection, 3.0f);
    Fmatrix world; world.identity(); world.c.set(pos);
    RImplementation.add_Visual(0u, nullptr, s_visual, world);

    // SPIKE 2: editor overlay — yellow selection box around the model + a green
    // ground grid under it, drawn via the EditorOverlay line pass. Proves the
    // grid/gizmo/selection primitives an editor needs are feasible on Vulkan.
    {
        auto* rv = static_cast<vkRender_Visual*>(s_visual);
        const float r  = (rv && rv->vis.sphere.R > 0.1f) ? rv->vis.sphere.R : 0.5f;
        EditorOverlay::AddBoxAA(pos, r, EditorOverlay::rgba(255, 220, 40, 255));
        const float gy = pos.y - r;
        const int   GN = 12;
        const float GS = 0.5f;
        const u32   gc = EditorOverlay::rgba(70, 200, 100, 170);
        for (int i = -GN; i <= GN; ++i)
        {
            Fvector a, b;
            a.set(pos.x + i * GS, gy, pos.z - GN * GS); b.set(pos.x + i * GS, gy, pos.z + GN * GS);
            EditorOverlay::AddLine(a, b, gc);
            a.set(pos.x - GN * GS, gy, pos.z + i * GS); b.set(pos.x + GN * GS, gy, pos.z + i * GS);
            EditorOverlay::AddLine(a, b, gc);
        }
    }

    // One-shot screenshot ~2s after the level is up (swapchain capture in End(),
    // so it's independent of window focus/occlusion). Writes ss_*.tga.
    if (!s_shot && ++s_frame >= 120) {
        s_shot = true;
        RImplementation.Screenshot(IRender_interface::SM_NORMAL, nullptr);
        Msg("[VK][spike] in-level screenshot queued");
    }
}
} // namespace Spike

// ---------------------------------------------------------------------------
// PHASE B — CHUNK 1 (editor-on-Vulkan): a no-menu, no-level editor viewport.
// -vk_editor boots straight into a clean orbit-camera view of ONE model on a
// grid + sky, with NO game menu — the seed of the editor host. A custom
// pureFrame/pureRender sink drives the frame instead of the menu:
//   OnFrame()  (in FrameMove, BEFORE OnCameraUpdated): orbit camera from the
//     mouse → Device.mView/mProject/camera vectors. OnCameraUpdated then builds
//     mFullTransform + SetCacheXform from exactly those.
//   OnRender() (in seqRender, between Begin/End): Calculate (clears the dynamic
//     list; no-ops with no level) → inject the model via add_Visual + build the
//     grid via EditorOverlay → Render (sky + model + overlay).
// The sink self-registers (VKEditor::Tick from CRender::Begin) once
// g_pGamePersistent exists; the menu is suppressed game-side by reading
// -vk_editor. Additive/experimental; see EDITOR_ON_VULKAN_ROADMAP.md §13.
// ---------------------------------------------------------------------------
namespace VKEditor {

static bool        s_checked = false;
static bool        s_enabled = false;
static string_path s_model   = { 0 };

bool Enabled()
{
    if (!s_checked) {
        s_checked = true;
        const char* p = Core.Params ? strstr(Core.Params, "-vk_editor") : nullptr;
        if (p) {
            s_enabled = true;
            p += xr_strlen("-vk_editor");
            while (*p == ' ') ++p;
            if (*p && *p != '-') {
                int i = 0;
                while (*p && *p != ' ' && i < int(sizeof(s_model)) - 1) s_model[i++] = *p++;
                s_model[i] = 0;
            } else {
                // Valid rigid device that exists in Gunslinger's gamedata.db_base_meshes
                // (verified via the .db extractor). The old device_torch path did not.
                xr_strcpy(s_model, "dynamics\\devices\\dev_pda\\dev_pda");
            }
            Msg("[VK][editor] enabled, model='%s'", s_model);
        }
    }
    return s_enabled;
}

// ---- A2.4: host-driven scene ------------------------------------------------
// When the SDK hosts us it owns the camera and the object list; the built-in orbit
// camera + demo model are only the fallback for standalone `-vk_editor`. The host
// calls these from its frame thread, i.e. the same thread as Ed_RenderFrame.
static bool s_hostCam = false;
static Fvector s_camPos, s_camDir, s_camUp;
static float s_camFov = 67.f, s_camZN = 0.1f, s_camZF = 1000.f;

struct HostModel
{
    IRenderVisual* visual;
    Fmatrix xform;
};
static xr_vector<HostModel> s_hostModels;

// Handles the host wants highlighted. Indices into s_hostModels; the host re-sends the
// whole set on every selection change, so we never have to track its scene.
static xr_vector<int> s_hostSelection;

void HostSetCamera(const Fvector& pos, const Fvector& dir, const Fvector& up, float fov_deg, float zn, float zf)
{
    s_hostCam = true;
    s_camPos = pos;
    s_camDir = dir;
    s_camDir.normalize_safe();
    s_camUp = up;
    s_camUp.normalize_safe();
    s_camFov = fov_deg;
    s_camZN = zn;
    s_camZF = zf;
}

int HostAddModel(const char* name, const Fmatrix& xf)
{
    if (!name || !name[0])
        return -1;

    IRenderVisual* v = RImplementation.model_Create(name, nullptr);
    if (!v)
    {
        Msg("![VK][editor] host model_Create failed: '%s'", name);
        return -1;
    }

    s_hostModels.push_back({v, xf});
    Msg("[VK][editor] host model added [%d]: '%s'", int(s_hostModels.size()) - 1, name);
    return int(s_hostModels.size()) - 1;
}

// ---- A2.5: static scene geometry from a HOST-EXPORTED .ogf -------------------
// Ed_AddModel's by-name lookup cannot serve an editor scene: our gamedata holds only
// dynamic visuals and BAKED per-level geometry, never the individual static props a
// level source is built from (0 of Cordon's 434 visuals resolve). So the host exports
// each source object to a real .ogf and hands us the absolute path; we load it through
// the ordinary ModelPool, which keys on `logical_name` — so a level's thousands of
// instances cost only as many file loads as it has distinct visuals.
static xr_vector<shared_str> s_hostLoaded; // logical names whose base model is in the pool

int HostAddModelFile(const char* ogf_path, const char* logical_name, const Fmatrix& xf)
{
    if (!ogf_path || !ogf_path[0] || !logical_name || !logical_name[0])
        return -1;

    bool haveBase = false;
    for (const shared_str& s : s_hostLoaded)
        if (0 == xr_strcmp(s.c_str(), logical_name))
        {
            haveBase = true;
            break;
        }

    IRenderVisual* v = nullptr;
    if (haveBase)
    {
        // Base model already registered — Create duplicates it and never touches the FS.
        v = RImplementation.model_Create(logical_name, nullptr);
    }
    else
    {
        // Feed the reader in explicitly: the path is the host's, outside our gamedata,
        // so it is not in the VFS registry and a by-name lookup would miss it.
        IReader* data = FS.r_open(ogf_path);
        if (!data)
        {
            Msg("![VK][editor] host .ogf not readable: '%s'", ogf_path);
            return -1;
        }
        v = RImplementation.model_Create(logical_name, data);
        FS.r_close(data);
        if (v)
            s_hostLoaded.push_back(shared_str(logical_name));
    }

    if (!v)
    {
        Msg("![VK][editor] host model load failed: '%s' (%s)", logical_name, ogf_path);
        return -1;
    }

    s_hostModels.push_back({v, xf});
    // Adding an object is a STRUCTURAL change to the instanced caster set (new
    // meta entry, possibly a new draw group). Moving one is not — see below.
    VK::InstanceGPU::Invalidate();
    return int(s_hostModels.size()) - 1;
}

int HostModelCount() { return int(s_hostModels.size()); }

void HostSetModelXform(int id, const Fmatrix& xf)
{
    if (id >= 0 && id < int(s_hostModels.size()))
    {
        s_hostModels[id].xform = xf;
        // The whole point of the instanced path: a moved object re-uploads the
        // transform SSBO and nothing else — no re-bake, no realloc, no device
        // wait. Cull bounds are stored in MODEL space precisely so this holds.
        VK::InstanceGPU::TouchTransforms();
    }
}

void HostClearScene()
{
    for (auto& m : s_hostModels)
        RImplementation.model_Delete(m.visual, TRUE);
    s_hostModels.clear();

    // Deleting with discard can drop the registered base models too, so forget what we
    // cached: the next push re-reads the files rather than duplicating a freed base.
    s_hostLoaded.clear();

    // Handles are array indices, so everything the host selected is now meaningless.
    s_hostSelection.clear();

    // The instanced caster set points at meshes we just deleted — it MUST be
    // rebuilt before anything culls or draws it again.
    VK::InstanceGPU::Invalidate();
}

// ---- picking: give the host a ray, let it do the hit test --------------------
// We only unproject. The host's scene knows mesh-level geometry, object classes and
// selection semantics that we never see, so it owns the actual pick; feeding it a ray
// built from OUR matrices is what keeps the pick consistent with the picture.
bool HostScreenRay(int x, int y, Fvector& out_start, Fvector& out_dir)
{
    if (!Device.dwWidth || !Device.dwHeight)
        return false;

    // Client pixels -> NDC. Y flips: window coords run down, clip space runs up.
    const float nx = (2.f * float(x) / float(Device.dwWidth)) - 1.f;
    const float ny = 1.f - (2.f * float(y) / float(Device.dwHeight));

    // Unproject that pixel on the near and far plane. mInvFullTransform is rebuilt every
    // frame in CRenderDevice::OnCameraUpdated from the matrices the frame was drawn with.
    // Clip Z runs 0..1 here, and the renderer draws with a negative-height viewport, so
    // clip space stays Y-up and the flip above is the only one needed.
    Fvector4 src, pNear, pFar;
    src.set(nx, ny, 0.f, 1.f);
    Device.mInvFullTransform.transform(pNear, src);
    src.set(nx, ny, 1.f, 1.f);
    Device.mInvFullTransform.transform(pFar, src);
    if (fis_zero(pNear.w) || fis_zero(pFar.w))
        return false;

    const Fvector a = {pNear.x / pNear.w, pNear.y / pNear.w, pNear.z / pNear.w};
    const Fvector b = {pFar.x / pFar.w, pFar.y / pFar.w, pFar.z / pFar.w};

    out_start = a;
    out_dir.sub(b, a);
    if (fis_zero(out_dir.magnitude()))
        return false;

    out_dir.normalize();
    return true;
}

void HostSetSelection(const int* ids, int count)
{
    s_hostSelection.clear();
    if (!ids || count <= 0)
        return;

    s_hostSelection.reserve(count);
    for (int i = 0; i < count; ++i)
        if (ids[i] >= 0 && ids[i] < int(s_hostModels.size()))
            s_hostSelection.push_back(ids[i]);
}

struct CEditorViewport : public pureFrame, public pureRender
{
    // Orbit camera around `target`: yaw about Y, pitch = elevation (+ = above,
    // looking down), dist = radius. Default is a 3/4 view from ABOVE the grid.
    float   yaw    = PI_DIV_4;
    float   pitch  = 0.55f;
    float   dist   = 6.0f;
    Fvector target = { 0.f, 0.f, 0.f };

    // Polled Win32 input (no WndProc hook — same approach as the Spike 3 ImGui).
    bool  lDrag = false, rDrag = false;
    POINT lastPt = { 0, 0 };

    IRenderVisual* visual  = nullptr;
    int            loadTry = 0;
    bool           framed  = false;
    int            frame   = 0;
    bool           shot    = false;

    bool dragValid = false;

    void PollInput()
    {
        POINT pt;
        if (!GetCursorPos(&pt)) return;
        const bool lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool rb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

        if ((lb || rb) && !(lDrag || rDrag)) {
            lastPt = pt; // drag start — seed the delta
            // Only start orbiting if the drag BEGAN over our viewport window AND not over
            // an editor ImGui panel (the Log window) — otherwise dragging on the host's
            // toolbars/panels or the Log would move the camera. Once a drag starts valid,
            // it keeps control even if the cursor wanders out (natural).
            dragValid = !VK::ImGuiVK::EditorWantsMouse();
            if (dragValid && Device.m_hWnd) {
                POINT c = pt; ScreenToClient(Device.m_hWnd, &c);
                RECT rc; GetClientRect(Device.m_hWnd, &rc);
                dragValid = (c.x >= 0 && c.y >= 0 && c.x < rc.right && c.y < rc.bottom);
            }
        }
        if (!dragValid) { lDrag = lb; rDrag = rb; return; }

        const int dx = pt.x - lastPt.x;
        const int dy = pt.y - lastPt.y;
        lastPt = pt;

        if (lb) { yaw -= dx * 0.006f; pitch -= dy * 0.006f; } // LMB drag → orbit
        if (rb) { dist *= (1.f + dy * 0.005f); }              // RMB drag up/down → dolly
        lDrag = lb; rDrag = rb;

        clamp(pitch, -1.45f, 1.45f);
        clamp(dist, 0.3f, 200.f);
    }

    void UpdateCamera()
    {
        // Host-driven (SDK): adopt its camera verbatim. Aspect still comes from OUR
        // swapchain — the host's viewport panel has its own size, not the host window's.
        if (s_hostCam)
        {
            Fvector right;
            right.crossproduct(s_camUp, s_camDir);
            right.normalize_safe();
            Fvector up;
            up.crossproduct(s_camDir, right);
            up.normalize_safe();

            Device.vCameraPosition = s_camPos;
            Device.vCameraDirection = s_camDir;
            Device.vCameraTop = up;
            Device.vCameraRight = right;

            Device.fFOV = s_camFov;
            Device.fASPECT = Device.dwHeight ? float(Device.dwWidth) / float(Device.dwHeight) : (16.f / 9.f);
            Device.mView.build_camera_dir(Device.vCameraPosition, Device.vCameraDirection, Device.vCameraTop);
            Device.mProject.build_projection(deg2rad(Device.fFOV), Device.fASPECT, s_camZN, s_camZF);
            return;
        }

        // Camera sits on a sphere around `target`; look back at it.
        Fvector off;
        off.set(_cos(pitch) * _sin(yaw), _sin(pitch), _cos(pitch) * _cos(yaw));
        Fvector pos; pos.mad(target, off, dist);            // pos = target + off*dist
        Fvector dir; dir.sub(target, pos); dir.normalize(); // cam → target

        Fvector up; up.set(0.f, 1.f, 0.f);
        Fvector right; right.crossproduct(up, dir); right.normalize();
        up.crossproduct(dir, right); up.normalize();

        Device.vCameraPosition  = pos;
        Device.vCameraDirection = dir;
        Device.vCameraTop       = up;
        Device.vCameraRight     = right;

        Device.fFOV    = 67.f;
        Device.fASPECT = Device.dwHeight ? float(Device.dwWidth) / float(Device.dwHeight) : (16.f / 9.f);
        Device.mView.build_camera_dir(Device.vCameraPosition, Device.vCameraDirection, Device.vCameraTop);
        Device.mProject.build_projection(deg2rad(Device.fFOV), Device.fASPECT, 0.1f, 1000.f);
        // mFullTransform / SetCacheXform are rebuilt by CRenderDevice::OnCameraUpdated,
        // which runs right after FrameMove using the matrices we just set.
    }

    // Keep a normal arrow over the viewport. Embedded, several parties fight over the
    // OS cursor (the host's ImGui, DirectInput acquires on focus, the game's own cursor
    // suppression) and any of them can blank it on a click/focus change. Rather than
    // chase each one, re-assert the cursor every frame while the pointer is over our
    // window — self-correcting: whoever hid it, we bring it back next frame.
    void EnforceCursor()
    {
        extern bool g_ed_embedded;
        if (!g_ed_embedded || !Device.m_hWnd) return;

        POINT p;
        if (!GetCursorPos(&p)) return;
        POINT c = p; ScreenToClient(Device.m_hWnd, &c);
        RECT rc; GetClientRect(Device.m_hWnd, &rc);
        if (c.x < 0 || c.y < 0 || c.x >= rc.right || c.y >= rc.bottom)
            return; // pointer is over the host's UI — leave its cursor alone

        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        CURSORINFO ci{ sizeof(ci) };
        if (GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING))
            while (ShowCursor(TRUE) < 0) // bring the display count back to >= 0
                ;
    }

    // pureFrame — set the editor camera BEFORE OnCameraUpdated consumes it.
    void OnFrame() override
    {
        if (!s_hostCam) // the host owns the camera (and the mouse) when embedded
            PollInput();
        UpdateCamera();
        EnforceCursor();
    }

    // Grid + world-origin axis, drawn in BOTH the host-driven and the demo path.
    void DrawEditorGizmos()
    {
        // World-origin axis tripod (X/Y/Z = R/G/B), 0.5 m — the SDK's coordinate marker.
        EditorOverlay::AddAxis(Fvector{ 0.f, 0.f, 0.f }, 0.5f);

        // Ground grid — only for an EMPTY viewport. It is drawn POST-TONEMAP with no depth
        // test (that is what keeps it crisp), so over a loaded level it paints across the
        // whole picture instead of lying on the ground. Once the host has pushed a scene,
        // the level itself is the ground reference and the grid is just noise.
        if (!s_hostModels.empty())
            return;

        // Matches the original SDK editor (D3DUtils UpdateGrid): 1 m cells, minor lines
        // m_ColorGrid 0x909090, major lines every SUB cells m_ColorGridTh 0xb4b4b4.
        const int GN = 25; const float GS = 1.0f; const int SUB = 5;
        const u32 gc  = EditorOverlay::rgba(144, 144, 144, 255); // minor (0x909090)
        const u32 gcM = EditorOverlay::rgba(180, 180, 180, 255); // major (0xb4b4b4)
        for (int i = -GN; i <= GN; ++i) {
            const u32 col = (i % SUB == 0) ? gcM : gc;
            Fvector a, b;
            a.set(i * GS, 0.f, -GN * GS); b.set(i * GS, 0.f, GN * GS); EditorOverlay::AddLine(a, b, col);
            a.set(-GN * GS, 0.f, i * GS); b.set(GN * GS, 0.f, i * GS); EditorOverlay::AddLine(a, b, col);
        }
    }

    // Wireframe box around every object the host has selected. Drawn post-tonemap with no
    // depth test, so a selection stays visible even behind geometry.
    //
    // The box is the visual's MODEL-space AABB pushed through the object's transform, i.e.
    // an ORIENTED box — the same thing the SDK draws (set_xform_world + DrawSelectionBox).
    // Collapsing it to a world AABB instead would visibly balloon around anything rotated.
    void DrawSelection()
    {
        const u32 col = EditorOverlay::rgba(255, 220, 40, 255); // SDK selection yellow

        for (int id : s_hostSelection)
        {
            if (id < 0 || id >= int(s_hostModels.size()))
                continue;

            const HostModel& m = s_hostModels[id];
            auto* rv = static_cast<vkRender_Visual*>(m.visual);
            if (!rv || !rv->vis.box.is_valid())
                continue;

            // Corner i takes its x/y/z from min or max per bit 0/1/2.
            const Fbox& b = rv->vis.box;
            Fvector c[8];
            for (int i = 0; i < 8; ++i)
            {
                Fvector p;
                p.set((i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z);
                m.xform.transform_tiny(c[i], p);
            }

            static const int edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0},  // z = min face
                                             {4, 5}, {5, 7}, {7, 6}, {6, 4},  // z = max face
                                             {0, 4}, {1, 5}, {2, 6}, {3, 7}}; // verticals
            for (const auto& e : edges)
                EditorOverlay::AddLine(c[e[0]], c[e[1]], col);
        }
    }

    // pureRender — drive the render (replaces the menu as the frame's render sink).
    void OnRender() override
    {
        // ---- Host-driven scene (SDK): draw ITS objects, skip the demo model ----
        if (!s_hostModels.empty()) {
            RImplementation.Calculate();
            for (auto& m : s_hostModels)
                RImplementation.add_Visual(0u, nullptr, m.visual, m.xform);
            DrawEditorGizmos();
            DrawSelection();
            RImplementation.Render();
            return;
        }

        if (!visual && loadTry < 240 && (loadTry++ % 30) == 0) { // lazy load + retry
            visual = RImplementation.model_Create(s_model, nullptr);
            Msg(visual ? "[VK][editor] model loaded: '%s'" : "![VK][editor] model_Create failed: '%s'", s_model);
        }

        // Frame the loaded model once: aim the orbit at the CENTRE of its real
        // bounding box (a flat/offset-pivot model isn't centred on its sphere), and
        // pull back to fit the box diagonal — like the SDK's "focus on selection".
        if (visual && !framed) {
            framed = true;
            auto* rv = static_cast<vkRender_Visual*>(visual);
            if (rv && rv->vis.box.is_valid()) {
                Fvector bc; rv->vis.box.getcenter(bc);
                Fvector bs; rv->vis.box.getsize(bs);
                const float diag = bs.magnitude();          // full box diagonal
                if (diag > 0.01f) { target = bc; dist = diag * 1.6f; }
            } else if (rv && rv->vis.sphere.R > 0.01f) {
                target = rv->vis.sphere.P; dist = rv->vis.sphere.R * 3.f;
            }
        }

        RImplementation.Calculate(); // clears the dynamic list; no-ops without a level

        if (visual) {
            Fmatrix world; world.identity();
            RImplementation.add_Visual(0u, nullptr, visual, world);

            // Selection: a TIGHT wireframe AABB hugging the model's real bounding box
            // (world==identity), like the SDK LevelEditor draws around a selected
            // object — not a loose sphere-cube. SDK selection tint = warm yellow.
            auto* rv = static_cast<vkRender_Visual*>(visual);
            if (rv && rv->vis.box.is_valid())
                EditorOverlay::AddBox(rv->vis.box.min, rv->vis.box.max, EditorOverlay::rgba(255, 220, 40, 255));
        }

        DrawEditorGizmos();

        RImplementation.Render();

        if (!shot && ++frame >= 180) { // one-shot screenshot ~2-3s in (warmed up), for verification
            shot = true;
            RImplementation.Screenshot(IRender_interface::SM_NORMAL, nullptr);
            Msg("[VK][editor] viewport screenshot queued");
        }
    }
};

static CEditorViewport* s_vp     = nullptr;
static bool             s_active = false;

void Activate()
{
    if (s_active) return;
    s_active = true;
    s_vp = xr_new<CEditorViewport>();
    Device.seqFrame.Add(s_vp);     // camera update, inside FrameMove
    Device.seqRender.Add(s_vp, 5); // render sink, same priority slot the menu used
    Msg("[VK][editor] viewport sink registered (frame+render)");
}

bool Active() { return s_active; }

// Clear scene color + depth for the no-level viewport. The normal path relies on
// Pass_World's CLEAR loadOp to establish the targets; with no level Pass_World
// returns early, so Sky/overlay would LOAD an uninitialised target (black frame,
// garbage depth → the grid's depth test fails). Runs FIRST, only in editor mode.
void ClearTargets(VK::FrameContext& ctx)
{
    if (!s_active || ctx.cmd == VK_NULL_HANDLE || ctx.colorView == VK_NULL_HANDLE) return;

    // 0.044 → tonemaps to the SDK grey 0x555555 (85,85,85) once fog is off (see
    // the vk_pass_tonemap editor gate). A null depthView leaves the scope
    // depth-less, exactly as before.
    VK::RenderingBuilder(ctx.extent)
        .ColorClear(ctx.colorView, VkClearColorValue{ { 0.044f, 0.044f, 0.044f, 1.0f } })
        .Depth(ctx.depthView, VK_ATTACHMENT_LOAD_OP_CLEAR)
        .Begin(ctx.cmd);
    vkCmdEndRendering(ctx.cmd);
}

// Called every frame from CRender::Begin. Self-registers the sink once the game
// persistent (environment/sky owner) exists — Pass_Sky null-guards CurrentEnv,
// so activating a hair early only costs a dark sky for a frame or two.
void Tick()
{
    if (!Enabled() || s_active) return;
    if (!g_pGamePersistent)     return;
    Activate();
}

} // namespace VKEditor

// nfs5: OCCLUSION CULLING OF SKELETONS (r_dyn_occ 2; the Switch preset). r_dyn_occ 1 (default) keeps only the R4 small-and-far
// discard (nfs14), r_dyn_occ 0 = the old bare frustum collection. The collection below is a bare frustum query to the far plane: R4
// culled the same list by sector portals and HOM, we never did. In the Cordon camp that drew ~65 skeletal
// renderables (NPCs, their weapons, mutants) while ~5 NPCs were on screen -- the rest 30-60 m away behind houses --
// and skeletons cost 10.4 ms of a 20.4 ms GPU frame (nx_diag mode 4). Stand-in for HOM: rays from the camera to a few
// points of the object's bounding sphere against the static level cform, attenuated by each crossed material's
// fVisTransparencyFactor exactly like the AI's vision (CVisualMemoryManager::feel_vision_mtl_transp), so windows and
// wire fences do not hide anyone. Hysteresis: visible -> hidden only after 3 consecutive fully blocked tests (all 5
// points), hidden -> visible on the first open ray (a hidden object tests 2 rotating points every frame).
// Never culled: HUD, anything within 8 m (incl. the actor's own body), anything beyond 150 m (rays stay short).
// A hidden object is also not animated for rendering (renderable_Render not called) -- what R4's HOM did too.
// ⚠Its shadow goes with it (one collection serves every view); skinned shadows measured ~0 on the Switch.
#include "../../xrCDB/xrCDB.h"
#include "../../xr_3da/GameMtlLib.h"
#include "../../Include/xrRender/Kinematics.h"
#include "../../Include/xrRender/RenderVisual.h"
#include <unordered_map>
#include "vk_hom.h"
#include "vk_hzb_readback.h"   // NxOcc: the depth-pyramid veto (an NPC seen through a fence is not hidden)
#include "../../Include/xrRender/ParticleCustom.h"
#include "../../xrCore/pinned_worker.h"   // r_particles_mt 2: the sim on core 2 while the scene is collected
#include <atomic>
extern float ps_r__ssaDISCARD, ps_r__LOD;   // vk_console_min.cpp (r2_ssa_discard, r__geometry_lod)
extern int   ps_r_dyn_occ;       // r_dyn_occ: 0 off, 1 R4 ssaDISCARD, 2 + skeleton occlusion (vk_console_min.cpp)
extern int   ps_r_dyn_occ_async; // r_dyn_occ_async: its rays on a core-2 worker, a frame late
extern float ps_r_dyn_occ_ppx;   // r_dyn_occ_ppx: particle screen-radius discard in px (0 = the R4 SSA rule)
namespace NxOcc
{
// An NPC seen through a fence with gaps was hidden or flickered: the cform has the planks where the screen has gaps,
// and the 5 sample points drifted in and out of them as it moved (visible -> 3 blocked frames -> hidden -> one open
// ray -> visible). Two fixes, both only ever keep an object drawn:
//  * the VETO: before hiding, the box is tested against the depth pyramid read back from a few frames ago
//    (VK::HzbReadback). The prepass draws alpha-tested geometry with its discard, so the gaps are in it: any texel
//    under the box farther than the box = it was in view, keep it.
//  * the HOLD: openUntil (Device.dwTimeContinual, ms) -- an open ray or a veto proves the object was seen; it is not
//    hidden again for kSeenHoldMs, so the two tests disagreeing frame to frame cannot blink it.
struct State { u32 seen = 0; u32 homNext = 0; u32 openUntil = 0; u8 blockedRun = 0; u8 phase = 0; bool hidden = false; };
constexpr u32 kSeenHoldMs = 2000;
std::unordered_map<const IRenderable*, State> s_state;
u32 s_homTris = 0, s_homRaster = 0, s_homHidden = 0; double s_homMs = 0.0; bool s_useHom = false;
u32 s_vetoed = 0;   // hide decisions the depth pyramid overruled
float s_ssaDiscard = 0.f;   // nfs14: R4 r_ssaDISCARD for this frame
float s_pxPerUnit = 0.f;    // nfs15: screen pixels per (world radius / distance)
u32 s_discard = 0, s_discardP = 0;
u32 s_frame = 0, s_tested = 0, s_hidden = 0, s_rays = 0, s_raysFull = 0, s_frames = 0, s_near = 0, s_far = 0;
double s_us = 0.0;
CDB::COLLIDER s_xrc;   // Calculate runs on the main thread only
CDB::MODEL* s_model = nullptr;

// The collider and model are parameters: the same test runs on the main thread (r_dyn_occ_async 0) and on the
// core-2 ray worker, each with its own COLLIDER (it keeps its results inside); the static model is read-only.
bool RayBlockedWith(CDB::COLLIDER& xrc, CDB::MODEL* model, const Fvector& from, const Fvector& to, u32& rays, u32& raysFull)
{
    Fvector d; d.sub(to, from);
    float len = d.magnitude();
    if (len < 1.f) return false;
    d.div(len);
    len -= 0.5f;   // stop short of the target: the object's own floor / the wall it leans on do not count
    // nfs6: nfs5 asked for EVERY hit along the ray (to multiply the transparencies) and paid ~62 us a ray on the A57
    // (84 rays = 5.2 ms of the main thread). One opaque hit anywhere blocks whatever the order, so ask OPCODE for ANY
    // hit first (it stops at the first it finds); only a see-through first hit needs the full list.
    xrc.r_clear();
    xrc.ray_query(CDB::OPT_ONLYFIRST, model, from, d, len);
    ++rays;
    if (xrc.r_count() == 0)
        return false;
    if (GMLib.GetMaterialByIdx(u16(xrc.r_begin()->material))->fVisTransparencyFactor < 0.2f)
        return true;
    ++raysFull;
    xrc.r_clear();
    xrc.ray_query(0, model, from, d, len);
    float vis = 1.f;
    for (const CDB::RESULT* h = xrc.r_begin(); h != xrc.r_end(); ++h)
    {
        vis *= GMLib.GetMaterialByIdx(u16(h->material))->fVisTransparencyFactor;
        if (vis < 0.2f) return true;
    }
    return false;
}

bool RayBlocked(const Fvector& from, const Fvector& to) { return RayBlockedWith(s_xrc, s_model, from, to, s_rays, s_raysFull); }

// ⭐r_dyn_occ_async: the rays of this frame's Skip calls are only COLLECTED there; after the scene is collected they
// run on a worker pinned to core 2 while the main thread goes on (render, PREPARE), and the answers are applied in
// the next BeginFrame -- one frame late, which the decision already absorbs: "hidden" takes 3 blocked frames, "open"
// any open ray, plus the 2 s hold and the depth-pyramid veto. 📏07-10-2026 Switch fight: ~21 rays a frame, ~0.9 ms
// of the main thread (the [nx occ] cpu line). Jobs carry a copy of the points and the renderable only as a KEY into
// s_state: an object destroyed in between is never dereferenced, its state is simply not found.
struct RayJob
{
    const IRenderable* key;
    Fvector cam;
    Fvector p[5];
    u8 n;           // points to try; open = any of them open
    bool open;
};
xr_vector<RayJob> s_rayJobs;     // filled by Skip on the main thread
xr_vector<RayJob> s_rayRunning;  // owned by the worker between LaunchRays and the next BeginFrame
std::atomic<u32>  s_rayAsyncRays{0}, s_rayAsyncFull{0};
PinnedWorker* RayWorker()
{
    static PinnedWorker* w = new PinnedWorker(2); // leaked: no join at exit
    return w;
}

void LaunchRays()
{
    if (s_rayJobs.empty() || !s_model) return;
    std::swap(s_rayJobs, s_rayRunning);   // the worker is idle: BeginFrame waited for it
    s_rayJobs.clear();
    CDB::MODEL* model = s_model;
    RayWorker()->Run([model] {
        static CDB::COLLIDER xrc;   // the worker's own
        u32 rays = 0, full = 0;
        for (RayJob& j : s_rayRunning)
        {
            j.open = false;
            for (u8 i = 0; i < j.n && !j.open; ++i)
                j.open = !RayBlockedWith(xrc, model, j.cam, j.p[i], rays, full);
        }
        s_rayAsyncRays.fetch_add(rays, std::memory_order_relaxed);
        s_rayAsyncFull.fetch_add(full, std::memory_order_relaxed);
    });
}

void ApplyState(State& st, bool open)
{
    if (open) { st.blockedRun = 0; st.hidden = false; st.openUntil = Device.dwTimeContinual + kSeenHoldMs; }
    else if (!st.hidden && ++st.blockedRun >= 3) st.hidden = true;
}

// the last frame's answers into s_state (main thread; the worker is done after the Wait)
void CollectRays()
{
    RayWorker()->Wait();
    for (const RayJob& j : s_rayRunning)
    {
        auto it = s_state.find(j.key);
        if (it != s_state.end())
            ApplyState(it->second, j.open);
    }
    s_rayRunning.clear();
    s_rays += s_rayAsyncRays.exchange(0, std::memory_order_relaxed);
    s_raysFull += s_rayAsyncFull.exchange(0, std::memory_order_relaxed);
}

void BeginFrame()
{
    CollectRays();   // before s_model can change: the worker may still hold last frame's
    ++s_frame;
    s_model = g_pGameLevel ? g_pGameLevel->ObjectSpace.GetStaticModel() : nullptr;
    {
        // nfs14: R4 r2_R_calculate: g_fSCREEN = W*H*(90/fov)^2*(EPS_S + r__geometry_lod), r_ssaDISCARD = 3.5^2 / g_fSCREEN.
        const float fov = (Device.fFOV > 1.f) ? Device.fFOV : 67.f;
        // nfs15: geometry LOD fixed at the OGSR default 1.0 here -- the Switch preset r__geometry_lod 0.1 is for the
        // geometry LOD, through R4's formula it made NPCs vanish past ~115 m.
        const float scr = float(_max(Device.dwWidth, 1u)) * float(_max(Device.dwHeight, 1u)) * _sqr(90.f / fov) * (EPS_S + 1.f);
        s_ssaDiscard = _sqr(ps_r__ssaDISCARD) / scr;
        s_pxPerUnit = 0.5f * float(_max(Device.dwHeight, 1u)) / tanf(0.5f * deg2rad(fov));
        (void)ps_r__LOD;
    }
    // nfs10: R1/R4's HOM when the level has one, then rays for what HOM lets through (nfs11).
    s_useHom = ps_r_dyn_occ >= 2 && VK::Hom::Ready();
    if (ps_r_dyn_occ >= 2)
        VK::HzbReadback::BeginFrame();
    if (s_useHom)
    {
        VK::Hom::Frame();
        u32 ft, rt; float ms;
        VK::Hom::Stats(ft, rt, ms);
        s_homTris += ft; s_homRaster += rt; s_homMs += ms;
    }
}

// nfs14: R4's small-and-far discard for every dynamic renderable (R4 r_dsgraph_insert_dynamic). Particles on the Switch:
// x4 the threshold (half the distance) -- an anomaly 300 m away is a few pixels of noise on the handheld screen.
bool Discard(const ISpatial* sp, IRenderable* r)
{
    if (ps_r_dyn_occ < 1 || r->renderable_HUD()) return false;
    const Fsphere& s = sp->spatial.sphere;
    if (s.R <= 0.f) return false;
    const float d2 = Device.vCameraPosition.distance_to_sqr(s.P) + EPS;
    const bool particles = r->renderable.visual && r->renderable.visual->dcast_ParticleCustom() != nullptr;
    if (particles && ps_r_dyn_occ_ppx > 0.f)
    {
        // nfs15: a particle effect is dropped when its screen radius is under r_dyn_occ_ppx (Switch 8 px: R = 3 m ~200 m on 720p).
        if (s.R * s_pxPerUnit >= ps_r_dyn_occ_ppx * _sqrt(d2)) return false;
    }
    else if (s.R / d2 > s_ssaDiscard)
        return false;   // R4 r_dsgraph_insert_dynamic
    ++s_discard;
    if (particles) ++s_discardP;
    return true;
}

// true = do not collect this renderable this frame
bool Skip(IRenderable* r)
{
    if (ps_r_dyn_occ < 2) return false;   // occlusion off: not even the dynamic_cast
    IRenderVisual* v = r->renderable.visual;
    if (!v || r->renderable_HUD() || !s_model) return false;
    if (!dynamic_cast<IKinematics*>(v)) return false;
    const Fsphere& sph = v->getVisData().sphere;
    if (sph.R <= 0.f) return false;
    Fvector c; r->renderable.xform.transform_tiny(c, sph.P);
    const Fvector cam = Device.vCameraPosition;
    const float dist = cam.distance_to(c);
    if (dist < 8.f + sph.R) { ++s_near; return false; }
    if (!s_useHom && dist > 150.f) { ++s_far; return false; }

    State& st = s_state[r];
    if (st.seen + 1 != s_frame) st = State{};   // new, or not collected last frame: start visible
    st.seen = s_frame;
    const bool held = Device.dwTimeContinual < st.openUntil;   // seen a moment ago: "hidden" only in the counters
    // The last word on every "hidden": the hold, then the depth-pyramid veto (both explained at State).
    auto hide = [&]() -> bool {
        if (held) return false;
        Fbox box;
        box.xform(v->getVisData().box, r->renderable.xform);   // the skeleton's own bounds: tighter than the sphere
        if (!box.is_valid()) { box.set(c, c); box.grow(sph.R); }
        if (!VK::HzbReadback::BoxSeenOpen(box)) return true;
        st.openUntil = Device.dwTimeContinual + kSeenHoldMs;
        ++s_vetoed;
        return false;
    };
    if (s_useHom)
    {
        // R4 CHOM::visible: a visible object is re-tested 10..25 frames later, a hidden one every frame.
        bool homVis = true;
        if (s_frame >= st.homNext)
        {
            Fbox box;
            box.set(c, c);
            box.grow(sph.R);
            homVis = VK::Hom::VisibleBox(box);
            st.homNext = homVis ? s_frame + ::Random.randI(10, 26) : s_frame;
        }
        if (!homVis)
        {
            st.hidden = true;            // hidden by HOM: no rays for it
            st.blockedRun = 3;
            ++s_hidden; ++s_homHidden;
            return hide();
        }
        // HOM lets it through: the rays (full cform: every wall HOM's coarse mesh does not have) decide, below.
        if (dist > 150.f) { st.hidden = false; return false; }
    }
    // nfs9: nfs7 spent ~3 ms of the main thread on ~83 rays, 2/3 of them re-testing hidden objects. A hidden one now
    // re-tests every other frame (staggered by address): at most one more frame before it reappears.
    if (st.hidden && ((s_frame ^ u32(uintptr_t(r) >> 4)) & 1u))
    {
        ++s_hidden;
        return hide();
    }
    ++s_tested;

    Fvector dir; dir.sub(c, cam).normalize_safe();
    Fvector right; right.crossproduct(Fvector().set(0.f, 1.f, 0.f), dir).normalize_safe();
    Fvector P[5];
    P[0] = c;
    P[1].mad(c, Fvector().set(0.f, 1.f, 0.f), 0.6f * sph.R);    // head
    P[2].mad(c, right, 0.5f * sph.R);
    P[3].mad(c, right, -0.5f * sph.R);
    P[4].mad(c, Fvector().set(0.f, 1.f, 0.f), -0.3f * sph.R);   // not lower: the ground under the feet would block
    if (ps_r_dyn_occ_async)
    {
        // asked now, answered on core 2, applied next frame (CollectRays): decide on what is known
        RayJob& j = s_rayJobs.emplace_back();
        j.key = r;
        j.cam = cam;
        j.open = false;
        if (!st.hidden)
        {
            for (u32 i = 0; i < 5; ++i) j.p[i] = P[i];
            j.n = 5;
        }
        else
        {
            j.p[0] = P[st.phase % 5];
            j.p[1] = P[(st.phase + 2) % 5];
            j.n = 2;
            ++st.phase;
        }
        if (st.hidden) ++s_hidden;
        return st.hidden && hide();
    }
    bool open = false;
    if (!st.hidden)
    {
        for (const Fvector& p : P)
            if (!RayBlocked(cam, p)) { open = true; break; }
    }
    else
    {
        open = !RayBlocked(cam, P[st.phase % 5]) || !RayBlocked(cam, P[(st.phase + 2) % 5]);
        ++st.phase;
    }
    ApplyState(st, open);
    if (st.hidden) ++s_hidden;
    return st.hidden && hide();
}

void EndFrame(double us)
{
    s_us += us;
    if (++s_frames < 127) return;
#ifdef __SWITCH__   // the [nx occ] counters go to the Switch log only
    Msg("[nx occ] discard per frame: %.1f (particles %.1f) | ssaDISCARD %.3g", s_discard / 127.0, s_discardP / 127.0,
        s_ssaDiscard);
    Msg("[nx occ] %s per frame: tested %.1f hidden %.1f (by HOM %.1f) vetoed %.1f near %.1f far %.1f rays %.1f (full %.1f) | hom tris %.0f raster %.0f %.3f ms | cpu %.3f ms",
        s_useHom ? "HOM+rays" : "rays", s_tested / 127.0,
        s_hidden / 127.0, s_homHidden / 127.0, s_vetoed / 127.0, s_near / 127.0, s_far / 127.0, s_rays / 127.0, s_raysFull / 127.0, s_homTris / 127.0, s_homRaster / 127.0, s_homMs / 127.0, s_us / 127.0 / 1000.0);
#endif
    s_discard = s_discardP = 0;
    s_frames = s_tested = s_hidden = s_rays = s_raysFull = s_near = s_far = 0;
    s_homTris = s_homRaster = s_homHidden = 0; s_vetoed = 0; s_homMs = 0.0;
    s_us = 0.0;
    for (auto it = s_state.begin(); it != s_state.end();)   // forget what has not been collected for a while
        it = (s_frame - it->second.seen > 120) ? s_state.erase(it) : std::next(it);
}
} // namespace NxOcc

// rvk_loader.cpp level_Unload: the ray worker reads the level's static model -- it must be done before that goes.
void NxOccWaitRays()
{
    NxOcc::RayWorker()->Wait();
    NxOcc::s_rayRunning.clear();
    NxOcc::s_rayJobs.clear();
}

// ----- Frame ----------------------------------------------------------------
void CRender::Calculate()
{
    VK::Prof::CpuPhaseScope _cpu(VK::Prof::CPU_CALC);   // [VK CPUphase] scene collect
    // Dynamic-visual collection. R4 does this inside Calculate via the dsgraph
    // (portal + HOM culling); here it's a minimal frustum query with no culling.
    // Clear the per-frame list, then drive each visible renderable's
    // renderable_Render(), which calls back into add_Visual({visual, xform}).
    // RENDER_THREAD_PLAN step 5: collect into the extract set; Render() publishes it.
    VK::BeginVisualExtract();

    // (The glass/water clear that lived here is now at the top of CRender::Render(): those
    // lists are record-owned, and Calculate must not touch them once record runs in parallel.)

    // SPIKE 1 (editor-on-Vulkan): with -vk_spike, inject one .ogf. In-level it is
    // placed in front of the camera; then normal collection runs so the world
    // renders too. At the menu the existing !g_pGameLevel guard below returns.
    if (Spike::Enabled()) Spike::Frame();

    // Only collect dynamics while a level is loaded+rendering. CMainMenu::OnRender
    // also calls Calculate(): there g_pGameLevel is null and g_SpatialSpace is an
    // empty tree, so q_frustum would walk a null root and AV.
    if (!g_pGameLevel || !g_SpatialSpace)
        return;

    CFrustum frustum;
    Fmatrix fullT = Device.mFullTransform;   // CreateFromMatrix takes a non-const ref
    frustum.CreateFromMatrix(fullT, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
    // Publish the camera frustum on the interface: CKinematics::CalculateWallmarks
    // (skeleton blood decals) and engine code (IGame_Persistent grass benders)
    // read ::Render->ViewBase — it was never set on the VK path before.
    ViewBase = frustum;
    ViewFrustum() = frustum;   // the record side reads this per-slot copy (see vk_render_view.h)

    static xr_vector<ISpatial*> lstRenderables;
    lstRenderables.clear();
    g_SpatialSpace->q_frustum(lstRenderables, 0, STYPE_RENDERABLE, frustum);

    // The visible particle systems, and (r_particles_mt 2) their simulation STARTED
    // here: it runs on the core-2 worker and a free pool thread while the main thread
    // collects the scene below, and is joined after that. Collection does not read
    // particle data -- add_Visual only stores the visual; Particles_Extract reads the
    // particles in PREPARE, after the join. 📏07-10-2026 Switch: fork-join (mode 1)
    // kept the main thread waiting 1.8 ms a frame in a firefight, whichever cores the
    // pool was on (nx_cores spread changed nothing).
    static xr_vector<ISpatial*>         lstParticles;
    static xr_vector<CPS_Instance*>     s_ps;
    static xr_vector<std::future<void>> s_jobs;
    static std::atomic<u32>             s_psNext{0};
    static PinnedWorker*                s_psWorkers[3] = {}; // per core (r_particles_core), made on first use; leaked: no join
    static PinnedWorker*                s_psWorker = nullptr;
    {
        const int core = ps_r_particles_core < 0 || ps_r_particles_core > 2 ? 2 : ps_r_particles_core;
        if (!s_psWorkers[core])
            s_psWorkers[core] = new PinnedWorker(core);
        s_psWorker = s_psWorkers[core]; // the same one Runs and Waits below, in this frame
    }
    lstParticles.clear();
    s_ps.clear();
    g_SpatialSpace->q_frustum(lstParticles, 0, STYPE_RENDERABLE | STYPE_PARTICLE, frustum);
    for (ISpatial* sp : lstParticles)
        if (CPS_Instance* ps = smart_cast<CPS_Instance*>(sp))
            s_ps.push_back(ps);
    const u32 psCount = (u32)s_ps.size();
    const bool psAsync = ps_r_particles_mt == 2 && TTAPI && psCount > 1;
    // claim systems one at a time: a worker that is preempted does not hold a chunk hostage
    auto psDrain = [] {
        const u32 n = (u32)s_ps.size();
        for (u32 i = s_psNext.fetch_add(1, std::memory_order_relaxed); i < n; i = s_psNext.fetch_add(1, std::memory_order_relaxed))
            s_ps[i]->PerformFrame();
    };
    s_jobs.clear();
    if (psAsync)
    {
        s_psNext.store(0, std::memory_order_relaxed);
        s_psWorker->Run(psDrain);
        // one pool thread: the other one is the frame's second thread, busy right now
        s_jobs.push_back(TTAPI->submit(psDrain));
    }

    const auto occT0 = std::chrono::steady_clock::now();
    NxOcc::BeginFrame();
    for (ISpatial* sp : lstRenderables)
    {
        if (!sp) continue;
        if (IRenderable* r = sp->dcast_Renderable())
        {
            if (NxOcc::Discard(sp, r)) continue;   // nfs14: R4 r_ssaDISCARD (small and far), r_dyn_occ >= 1
            if (NxOcc::Skip(r)) continue;   // nfs5: hidden behind the level geometry, r_dyn_occ 2
            r->renderable_Render(0u, r);
        }
    }
    NxOcc::EndFrame(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - occT0).count());

    // Tick particle simulation for visible particle instances. R4 does this in
    // CRender::calculate_particles_async (PerformFrame → CParticlesObject::DoWork
    // → the visual's OnFrame, which steps PAPI). Without it on-screen particles
    // never advance: CParticlesObject::shedule_Update only updates particles that
    // were NOT rendered last frame. DoWork is idempotent per Device.dwFrame, so
    // this never double-ticks against shedule_Update.
    // r_particles_mt: on the TTAPI pool like R4, fork-join (this frame waits for all of
    // them). It is safe for the same reasons it is in R4: PAPI effects and the spatial DB
    // take their own locks, the GPU-particle registry too, and OnFrame already runs off
    // the main thread from the scheduler. Serial, the sim sat on the main thread -- a gust
    // of wind-blown leaves is hundreds of particles, each a collision ray per step.
    {
        const auto tick0 = std::chrono::steady_clock::now();
        const u32  n     = psCount;
        const bool mt = ps_r_particles_mt == 1 && TTAPI && n > 1;
        if (psAsync) {
            // what is left after the scene collection, then the join: only this is main-thread time
            psDrain();
            s_psWorker->Wait();
            for (auto& j : s_jobs) j.wait();
        } else if (mt) {
            // Chunks, the main thread takes the first: a future per system costs more than most systems.
            const u32 lanes = _max(1u, _min(TTAPI->get_num_threads() + 1u, 8u));
            const u32 chunk = (n + lanes - 1) / lanes;
            s_jobs.clear();
            for (u32 a = chunk; a < n; a += chunk) {
                const u32 b = _min(n, a + chunk);
                s_jobs.push_back(TTAPI->submit([a, b]() {
                    for (u32 i = a; i < b; ++i) s_ps[i]->PerformFrame();
                }));
            }
            for (u32 i = 0; i < _min(chunk, n); ++i) s_ps[i]->PerformFrame();
            for (auto& j : s_jobs) j.wait();
        } else {
            for (CPS_Instance* ps : s_ps) ps->PerformFrame();
        }

        // [VK Parts] every ~5 s: what the sim cost the main thread per frame (wall time of
        // the fork-join), how many systems / particle-steps / collision rays it took.
        // (The counters also catch scheduler ticks of off-screen systems -- shedule_Update -- which is
        // the same CPU cost, just not on this fork-join.)
        static double s_us = 0.0, s_usMax = 0.0;
        static u64    s_sys = 0;
        static u32    s_frames = 0;
        static auto   s_since = tick0;
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tick0).count();
        s_us += us; s_usMax = _max(s_usMax, us); s_sys += n; ++s_frames;
        if (std::chrono::duration<double>(tick0 - s_since).count() >= 5.0) {
            u64 steps = 0, rays = 0; string512 top;
            VK::PartStats::Take(steps, rays, top);
            const double f = 1.0 / double(s_frames);
            Msg("[VK Parts] per frame: systems %.1f, particle-steps %.0f, collision rays %.0f | sim %.2f ms avg, %.2f max (%s) | top steps/rays: %s",
                double(s_sys) * f, double(steps) * f, double(rays) * f, s_us * f * 0.001, s_usMax * 0.001,
                psAsync ? "main waits, core-2 worker" : mt ? "pool" : "main thread", top[0] ? top : "-");
            s_us = s_usMax = 0.0; s_sys = 0; s_frames = 0; s_since = tick0;
        }
    }

    // r_dyn_occ_async: this frame's occlusion rays to the core-2 worker -- after the particle
    // join (a separate worker anyway), so they run through the render and PREPARE.
    NxOcc::LaunchRays();

    // First-person HUD (player hands + active item). R4 does this at the tail of
    // its dsgraph collection (r4_R_render.cpp); Render_MAIN sets renderable_HUD()
    // on the root and calls add_Visual → routed into HudVisuals() (drawn by the
    // near-depth HUD branch of Pass_Skinned).
    if (g_hud)
        g_hud->Render_MAIN(0u);

    // SMAP phase: the actor's own model as a SHADOW CASTER ONLY (g_actor_shadow).
    // In first person CActor::setVisible(!hud_view) hides the body, so without this
    // the ground under you carries no shadow of yourself — the thing Anomaly gets
    // from r__actor_shadow. R4 calls Render_SMAP from its shadow-map dsgraph build;
    // our collection is a single list, so route it with the flag instead.
    // ⚠The first-person legs visual (g_first_person_body) is a SEPARATE model with
    // no arms at all — 11 bones: legs, pelvis, spine, head (checked in the SWM .ogf,
    // and every one of the mod's 310 outfit variants has the same skeleton). Its own
    // cast can therefore never contain arms; the arms in the shadow come from here,
    // exactly as in Anomaly, where r__actor_shadow and the legs mob both cast.
    if (g_hud)
    {
        b_collect_shadow_only = true;
        g_hud->Render_SMAP(0u);
        b_collect_shadow_only = false;
    }

    // RENDER_THREAD_PLAN step 4: every light has been moved by now -> freeze this frame's set.
    VK::Lights::Extract();
}
#ifdef __SWITCH__
extern "C" void xr_nx_rt_add(int slot, unsigned long long us);     // xr_3da/device.cpp (v118)
#endif

// The scene passes and PREPARE phases, registered once, in draw order. Called from the EXTRACT side (CRender::Render,
// main thread) before the first RunPrepares: registered from the record side instead, the very first frame ran its
// PREPARE phases on the render thread -- they read live game state and assert the main thread.
static void RegisterScenePassesOnce()
{
    // Register the scene passes once, in draw order. Adding a pass later (lights,
    // SSAO, bloom) is a RegisterPass call, not an edit here. ExecutePasses owns
    // the inter-pass barriers.
    //   World → Grass (depth-tests world geometry, writes own depth) → Sky (fills
    //   uncleared z==1 pixels behind the world). Grass skips if Details didn't
    //   load (no level.details) or compute is disabled.
    //
    // Each pass also DECLARES the named resources it reads and writes (see
    // vk_pass_deps.h). ValidatePassDeps then makes "reads what a later pass
    // produces" a crash on the first frame instead of a silently stale picture.
    // Conventions this list follows:
    //   * declare the reads of what OTHER passes produce — a pass reading back its
    //     own output (the whole SSAO/gather/VSM chain inside World) owns that
    //     ordering itself and the top-level check cannot see it anyway;
    //   * declare every write another pass may depend on;
    //   * the forward-lit passes bind EnvLight's set, which is exactly
    //     {SunCascades, VSM, DDGIProbes, SSAO, GatherGI} — hence that recurring
    //     read list;
    //   * a depth-tested overlay READS SceneDepth even when it never samples it;
    //   * History(res) = "last frame's contents, on purpose".
    // Under-declaring is safe (it can only hide a violation), so a pass whose
    // inputs are not fully traced yet declares the subset that IS certain.
    static bool s_registered = false;
    if (!s_registered)
    {
        using R = VK::Res;
        // The forward-lit read set: everything reachable through EnvLight's
        // descriptor set (sun shadow / VSM mask / probe volume / AO / gather).
        // Named once because five passes share it verbatim.
        #define VK_ENVLIGHT_READS R::SunCascades, R::VSM, R::DDGIProbes, R::SSAO, R::GatherGI

        // PHASE B (editor-on-Vulkan): with -vk_editor there is no level, so Pass_World
        // (the normal owner of the color+depth CLEAR) returns early. Clear here FIRST so
        // Sky + the editor overlay draw onto a defined target with depth=1.0. No-ops in
        // game mode (guarded by VKEditor::Active()).
        VK::RegisterPass("EditorClear", {}, { R::SceneColor, R::SceneDepth },
            [](VK::FrameContext& c) { VKEditor::ClearTargets(c); });
        // COMPUTE PRE-SKINNING (r_preskin) before any consumer: skins every
        // visible skeleton leaf ONCE into a shared world-space pool, so the
        // shadow/prepass/colour/VSM passes below draw pre-transformed geometry
        // instead of re-running the bone blend in each of their vertex shaders.
        // Must sit outside a render pass (it dispatches compute) — hence its own
        // registered pass rather than a call inside Pass_SunShadow.
        // ⚠ Its position is now ENFORCED: every consumer declares the read, so
        // moving PreSkin after any of them fails at startup.
        // ⭐И ЕГО CPU-ПОЛОВИНА — В ФАЗУ PREPARE, ДО ПЕРВОЙ КОМАНДЫ КАДРА.
        // Тело пасса строило кости и карту пула ПО ХОДУ записи своих диспатчей,
        // а карту читает каждый рисующий скиннед-пасс (ResolveDrawSrc) — то есть
        // SunShadow и World гонялись с PreSkin по построению, а не по GPU. Это и
        // был краш этапа Б (`Pass_SunShadow -> Skinned_UploadBones -> std::_Hash`).
        // Теперь к моменту старта первого воркера кости, s_psMap и список
        // диспатчей уже готовы и дальше только читаются. См. vk_pass_registry.h.
        // ДВЕ записи, а не одна, чтобы прибор различал две разные работы внутри
        // одной фазы: кости (CalculateBones по всем скелетам) и обход листьев
        // (ResolveSkinnedLeaf + раскладка пула). Обе идемпотентны, порядок между
        // ними тот же, что был внутри тела пасса.
        VK::RegisterPrepare("Skin/bones", [] { VK::Skinned_UploadBones(); });
        VK::RegisterPrepare("PreSkin/leaves", [] { VK::Skinned_PreSkinPrepare(); });
        // Two-slot pipeline: blood decals on NPCs are CPU-skinned against the live pose, so they are built here, once the bones
        // are final, instead of by the record side (which would read bones the next extract is rewriting).
        VK::RegisterPrepare("Skeleton wallmarks", [] { extern void VK_SkelWM_Prepare(); VK_SkelWM_Prepare(); });
        // RENDER_THREAD_PLAN step 7: particle vertices / smoke media / GPU emitters read the live PAPI + effects.
        VK::RegisterPrepare("Particles", [] { VK::Particles_Extract(); VK::GPUParticles::ExtractWorldEmitters(); });
        // RENDER_THREAD_PLAN step 8: everything below reads live game state (rain sim, object list, collision model).
        VK::RegisterPrepare("Rain", [] { VK::Rain_Extract(); });
        VK::RegisterPrepare("Grass interactors", [] { if (RImplementation.Details) RImplementation.Details->ExtractInteractors(); });
        VK::RegisterPrepare("Capsule shadows", [] { VK::CapsuleShadows_Extract(); });
        VK::RegisterPrepare("Sun probe", [] { VK::EnvLight::ExtractSunProbe(); });
        VK::RegisterPrepare("Exposure enclosure", [] { VK::Tonemap_ExtractEnclosure(); });
        VK::RegisterPrepare("Water lid", [] { VK::WaterRipple::LidExtract(); });
        // r_ptcl_mt: the particle billboards a core-1 worker filled while the prepares above ran -- LAST, whatever is added
        VK::RegisterPrepare("Particles finish", [] { VK::Particles_ExtractFinish(); });
        VK::RegisterPass("PreSkin", {}, { R::SkinnedPool },
            [](VK::FrameContext& c) { VK::Skinned_PreSkin(c.cmd); });
        // DDGI probe volume. EARLY, and for the same structural reason as PreSkin:
        // it records the acceleration-structure build plus three compute dispatches,
        // none of which may sit inside a rendering scope. Being first also means the
        // RT scene is stepped here, so SSAO's own call later in the frame becomes the
        // no-op — the build lands once, before anything traces it.
        // The sky probe it lights its rays from is produced INSIDE the World pass
        // (EnvLight::Update → IBL::Update), i.e. after this — so the read is one
        // frame old, and says so. Harmless (the sky changes over minutes), but it is
        // exactly the dependency that used to be nowhere written down.
        VK::RegisterPass("DDGI", { VK::History(R::SkyIBL) }, { R::DDGIProbes, R::RTScene },
            [](VK::FrameContext& c) { VK::DDGI::Update(c.cmd); });
        // Sun shadow caster FIRST: renders static world depth from the sun POV into
        // the shadow map (own depth target), leaves it SHADER_READ for the receivers.
        VK::RegisterPass("SunShadow", { R::SkinnedPool }, { R::SunCascades, R::VolShadowSnapshot },
            [](VK::FrameContext& c) {
            VK::Pass_SunShadow(c);
            // Async compute (r_async): the fog inject records onto the COMPUTE queue,
            // where sampling these cascades would race the graphics queue rewriting
            // them next frame. Take the downscaled snapshot the inject reads instead,
            // here — the one point where the cascades are complete and still in
            // SHADER_READ. No-op with r_async off.
            VK::Vol::SnapshotShadows(c.cmd);
        });
        // The frame's centre of gravity, and the reason the declarations stop at
        // pass granularity: Pass_World internally runs depth prepass → MV resolve →
        // SSAO → final gather → VRS → VSM mark/raster/resolve → colour, producing
        // and consuming half this table inside one body. What it owes the REST of
        // the frame is the write list; what it needs FROM the rest is the read list.
        VK::RegisterPass("World",
            { R::SkinnedPool, R::SunCascades, R::DDGIProbes, R::RTScene, R::VolShadowSnapshot,
              VK::History(R::SceneColorPrev) },
            { R::SceneColor, R::SceneDepth, R::MotionVec, R::SSAO, R::GatherGI, R::VSM, R::SkyIBL },
            [](VK::FrameContext& c) { VK::Pass_World(c); });
        // PHASE B (editor-on-Vulkan, chunk 2): with -vk_editor Pass_World returns early
        // (no level) so add_Visual'd models aren't drawn. Draw them here — into the same
        // HDR SceneColor + depth EditorClear seeded — so the lit model composites on the
        // grey backdrop through tonemap. No-op in game mode (Pass_World already drew them).
        VK::RegisterPass("EditorDynamics", { R::SkinnedPool, VK_ENVLIGHT_READS },
            { R::SceneColor, R::SceneDepth },
            [](VK::FrameContext& c) { if (VKEditor::Active()) VK::Pass_EditorDynamics(c); });
        // Dense snow surface (VHM-style): drawn on top of the world, owns the near snow
        // geometry + footprint dents when r_snow_mesh is on. Writes depth (trees/grass
        // test against it). No-op unless r_snow_mesh + deform texture are active.
        VK::RegisterPass("SnowMesh", { VK_ENVLIGHT_READS }, { R::SceneColor, R::SceneDepth },
            [](VK::FrameContext& c) { VK::Pass_SnowMesh(c); });
        VK::RegisterPass("Trees", { VK_ENVLIGHT_READS }, { R::SceneColor, R::SceneDepth },
            [](VK::FrameContext& c) {
            if (ps_r_trees && RImplementation.Trees && RImplementation.Trees->IsReady())
                RImplementation.Trees->Render(c);
        });
        VK::RegisterPass("Grass", { VK_ENVLIGHT_READS }, { R::SceneColor, R::SceneDepth },
            [](VK::FrameContext& c) {
            if (ps_r_grass && RImplementation.Details && RImplementation.Details->IsLoaded())
                RImplementation.Details->Render(c);
        });
        // Imposters carry their own baked lighting — no EnvLight set, so no read set.
        VK::RegisterPass("LODs", {}, { R::SceneColor, R::SceneDepth },
            [](VK::FrameContext& c) {
            if (RImplementation.LODs && RImplementation.LODs->IsReady())
                RImplementation.LODs->Render(c);
        });
        // Dynamic motion vectors: now that the full opaque depth exists, overlay
        // self-moving geometry (skinned NPCs) onto the camera/static MV field.
        // Second writer of MotionVec on purpose — a refinement of the field World
        // already produced, which is why the check keys on the FIRST producer.
        // Declares the depth BOTH ways, because it does both: the reconstruction
        // samples it, then the NPC overlay attaches it for the depth test. A pass
        // that does both is handed the attachment layout and opens its own sample
        // window from there — the reverse would leave its rendering scope wrong.
        VK::RegisterPass("MotionVecDyn",
            { R::SceneDepth, VK::Sampled(R::SceneDepth), R::SkinnedPool }, { R::MotionVec },
            [](VK::FrameContext& c) { VK::MotionVec::ExecuteDynamic(c.cmd, c); });
        // An EMPTY editor viewport uses a flat grey clear, not the game sky, so the SDK's
        // backdrop shows. Once the host has pushed a scene the sky belongs there: the
        // editor's whole point is to look like the game, and the sky pass also prepares
        // the weather cubes that the hemisphere ambient samples — without it every
        // surface gets the same flat grey constant regardless of its normal.
        // Same "empty viewport vs real scene" split as the grid and auto-exposure.
        VK::RegisterPass("Sky", { R::SceneDepth }, { R::SceneColor },
            [](VK::FrameContext& c) { if (!VKEditor::Active() || VKEditor::HostModelCount() > 0) VK::Pass_Sky(c); });
        // SPIKE 2 (editor-on-Vulkan): editor overlay — depth-tested world-space
        // lines (grid / gizmo / selection box) drawn on top of the opaque scene.
        // In -vk_editor the grid is drawn POST-TONEMAP (crisp, no bloom) — see the
        // ExecutePostTonemap call after Tonemap; skip the HDR pass so the queued lines
        // survive. The Spike (-vk_spike, in-level) still uses this HDR overlay path.
        VK::RegisterPass("EditorOverlay", { R::SceneDepth }, { R::SceneColor },
            [](VK::FrameContext& c) { if (!VKEditor::Active()) EditorOverlay::Execute(c); });
        // (Sun shafts used to be registered here — a fullscreen additive raymarch of
        // the FAR sun map. Removed 2026-07-24: under VSM that map is deliberately left
        // cleared = "fully lit", so the pass painted warm rays THROUGH walls and had to
        // be gated off entirely; and god rays are now the froxel volumetrics' job, where
        // they come out of a real medium with a phase function instead of a screen
        // effect layered on top. See [[vulkan-vsm-light-leak-fix]] / the fog V-1 layers.)
        // Per-light volumetric cones: real raymarched beams for the volumetric
        // spots (headlights/searchlights/pole lamps) — replaces the R4
        // lightplanes texture-sheet fakes. Additive over the lit scene.
        // WATER bodies — after the whole opaque world AND the sky (both are the
        // "bottom" the surface blends over: a pond reflects the sky and shows the
        // riverbed through itself), before glass/particles/rain, which composite
        // on top of it. See vk_pass_water.h.
        // Sampled, not attached: the surface draw binds colour only and reads the
        // depth through a descriptor (water.frag's uSceneDepth). Saying so is what
        // lets the derived barrier hand it SHADER_READ directly and leave the window
        // open for the light cones, instead of flipping to attachment and back.
        VK::RegisterPass("Water", { VK::Sampled(R::SceneDepth), VK_ENVLIGHT_READS }, { R::SceneColor },
            [](VK::FrameContext& c) { VK::Pass_Water(c); });
        // Same shape as Water, and adjacent to it on purpose — the two share one
        // depth flip now (the cone march samples the depth in its fragment shader).
        VK::RegisterPass("LightCones", { VK::Sampled(R::SceneDepth), VK_ENVLIGHT_READS }, { R::SceneColor },
            [](VK::FrameContext& c) { VK::Pass_LightCones(c); });
        // R1-hybrid shadow option (r_shadow_lite 1): characters leave the sun cascades and are drawn here as analytic
        // capsule shadows -- one depth-sampled multiply over the HDR scene. No-op with the option off.
        VK::RegisterPass("CapsuleShadows", { VK::Sampled(R::SceneDepth) }, { R::SceneColor },
            [](VK::FrameContext& c) { VK::Pass_CapsuleShadows(c); });
        // Translucent GLASS panes — late flush AFTER the whole opaque world + sky
        // (they blend without z-write; anything drawn after them behind the pane
        // would overwrite the blended pixels). Before wallmarks/particles.
        VK::RegisterPass("Glass", { R::SceneDepth, VK_ENVLIGHT_READS }, { R::SceneColor },
            [](VK::FrameContext& c) { VK::Pass_WorldGlass(c); });
        // Particles last: camera-facing billboards composited over the scene
        // (depth-tested vs world, additive/alpha blend, no depth write).
        // Wallmarks (bullet holes / scorch decals) over the lit world, before
        // particles so impact smoke composites on top of the fresh hole.
        VK::RegisterPass("Wallmarks", { R::SceneDepth }, { R::SceneColor },
            [](VK::FrameContext& c) { VK::Wallmarks::Render(c); });
        VK::RegisterPass("Particles", { R::SceneDepth }, { R::SceneColor },
            [](VK::FrameContext& c) { VK::Pass_Particles(c); });
        // GPU-driven particles (Phase 1): one hardcoded effect simulated +
        // drawn entirely on the GPU when r_gpu_particles=1 (off by default).
        VK::RegisterPass("GPUParticles", { R::SceneDepth }, { R::SceneColor },
            [](VK::FrameContext& c) { VK::GPUParticles::DispatchComputeAndDraw(c); });
        // Weather: rain drop streaks + ground splashes + thunderbolt, drawn
        // over everything (alpha/additive, depth-tested). Also ticks the rain
        // simulation (CEffect_Rain::Calculate) — the engine's RenderLast path
        // is never called on the VK build, so the pass owns it.
        VK::RegisterPass("Rain", { R::SceneDepth }, { R::SceneColor },
            [](VK::FrameContext& c) { VK::Pass_Rain(c); });

        // LAST on purpose. Two reasons, both structural rather than stylistic:
        // the preview OVERWRITES the frame (it is a view of what the tracer sees,
        // not a layer over the raster), and it steps the RT scene, which records
        // acceleration-structure builds — those must not land inside a rendering
        // scope, and between passes is the only place that is guaranteed of.
        VK::RegisterPass("RTGIdbg", { R::SceneDepth, R::RTScene, R::DDGIProbes }, { R::SceneColor },
            [](VK::FrameContext& c) { VK::RTGIPass::Execute(c); });
        // The final gather's own view (r_gather_debug). Last, and after RTGIdbg, so
        // whichever preview is asked for wins over the raster rather than fighting
        // it: both OVERWRITE the frame by design.
        VK::RegisterPass("GatherDbg", { R::GatherGI }, { R::SceneColor },
            [](VK::FrameContext& c) { VK::GatherPass::DrawDebug(c); });
        // The probes and their rays, drawn INTO the world rather than over it: this
        // one composits (it does not overwrite the frame like the two previews
        // above), because the whole question it answers is WHERE the probes are in
        // relation to the geometry you are looking at.
        VK::RegisterPass("DDGIdbg", { R::SceneDepth, R::DDGIProbes }, { R::SceneColor },
            [](VK::FrameContext& c) { VK::DDGI::DrawDebug(c); });

        #undef VK_ENVLIGHT_READS

        // ⭐КТО МОЖЕТ ПИСАТЬСЯ НА ВОРКЕРЕ (`r_mt_record 2`, дефолт выключен).
        //
        // Список -- результат аудита 19-09, а не догадка. Критерий в
        // MarkPassParallel: тело не двигает трекер фреймграфа и не читает то,
        // что наполняет тело ДРУГОГО пасса.
        //   SunShadow/Trees/Grass -- рисуют из состояния, подготовленного фазой
        //   calc до начала записи; трекер не трогают (проверено grep'ом).
        // ⛔ПОЧЕМУ НЕ ОСТАЛЬНЫЕ, хотя они дороже:
        //   World (0.52-0.78 core-ms, 58% записи) -- единственный пасс, который
        //     двигает трекер (SceneDepthToSampled/ToAttachment) и наполняет
        //     общую RenderQueue; его вынос -- отдельная работа, и делить надо
        //     не пасс, а его цикл по 639 группам материалов;
        //   Particles/Glass/Water -- читают GlassItems/WaterItems, которые
        //     наполняет тело World ⇒ обязаны идти ПОСЛЕ него, в порядке;
        //   PreSkin (0.15-0.20) -- следующий кандидат, но сначала надо доказать,
        //     что тело World не зависит от его CPU-состояния.
        // ⛔⛔СЕЙЧАС НЕ ПОМЕЧЕН НИ ОДИН ПАСС, и это результат ДВУХ ЗАМЕРОВ
        // 19-09, а не осторожности. Машинерия готова и проверена (порядок
        // проигрывания, дорожки, откат на главный поток), но оба кандидата
        // упали на ОБЩЕМ ИЗМЕНЯЕМОМ СОСТОЯНИИ, которого не видно в теле пасса:
        //
        // 1. `-vk_validation`: 4364 ошибки VUID-vkUpdateDescriptorSets-None-03047
        //    за кадр, появляются через 9 мс после `r_mt_record 2` и исчезают с
        //    его выключением. Набор -- `EnvLight.WorldSet1`: его обновляет
        //    EnvLight::Update ВНУТРИ тела World, пока Trees/Grass уже привязали
        //    его в своих записывающихся буферах. ⇒ первым делом Update надо
        //    вынести из тела пасса в начало кадра: данные там КАДРОВЫЕ (погода,
        //    виды AO/IBL/VSM), в теле пасса они оказались исторически.
        // 2. Без валидации -- КРАШ на воркере через секунду:
        //      Pass_SunShadow -> VK::Skinned_UploadBones() -> std::_Hash<u64,u32>
        //    то есть тело тени правит общую хеш-карту скиннинга, которой в тот
        //    же момент пользуется главный поток.
        //
        // ⭐Урок для аудита: критерий -- что тело вызывает ТРАНЗИТИВНО, а не что
        // видно в его файле. Grep по vk_pass_shadow.cpp «трекер не трогает,
        // дескрипторы 2» выглядел чисто, а сломалось двумя уровнями ниже.
        // ⚠Картинка при этом НЕ ломалась: mean|diff| между кадрами mt=2 и mt=0
        // = 0.86 при пороге шума сцены 5.2-5.9 ⇒ «выглядит нормально» тут не
        // доказывает ничего, гонка просто не успела проявиться в кадре.
        //
        // ▶Порядок работ: (а) EnvLight::Update в начало кадра; (б) карта
        // скиннинга под блокировку ИЛИ вынос UploadBones в фазу до записи;
        // (в) затем помечать по одному пассу и мерить [VK RecCPU] + прогон с
        // валидацией на КАЖДЫЙ шаг.

        s_registered = true;
    }
}

void CRender::Render()
{
    VK_STUB_ONCE("CRender");
    // (no CpuPhaseScope here: CPU_RECORD belongs to Render_Record on the render thread; both writing one timer made the numbers garbage)

    // ---- EXTRACT SIDE of Render(): everything up to the marker below runs on the main thread and is the last thing before
    // the frame is handed to the record side. PREPARE (bones, pre-skin leaves, particles, rain, ...) is extract work: it
    // drives game bone callbacks and reads live game state, and it fills THIS frame's slot and ring slot (RV().ringSlot, whose
    // fence Begin_Extract waited).
    RegisterScenePassesOnce();
    // (A frame whose ring slot is not proven free skips PREPARE: the rings must not be written; its record then finds no
    // built data and draws nothing of it, exactly like a dropped frame.)
    if (RV().ringReady) VK::RunPrepares(RV().frameIndex);
    // ---- END OF THE EXTRACT SIDE. The rest of the frame (record, then End_Render queued by Frame_Submit / End) runs on
    // the render thread -- every frame, menu and load screen included -- while the main thread goes on to emit the UI.
    RT_Enqueue([this] {
        const u64 t0 = CPU::QPC_us();
        RT_Profiled("record", [this] { Render_Record(); }); // [rt prof]
#ifdef __SWITCH__
        xr_nx_rt_add(0, CPU::QPC_us() - t0);
#else
        (void)t0;
#endif
    });
}

void CRender::Render_Record()
{
    RT_ASSERT_RENDER();
    { LoadProf::Scope _lp("rt.begin"); EnsureRenderBegun(); }   // fence, acquire, command buffer, targets, jitter: the render side of Begin
    VK::Prof::CpuPhaseScope _cpuRec(VK::Prof::CPU_RECORD);
    // ---- RECORD SIDE from here.
    // The late-glass and water lists live ACROSS the frame that builds them (the particles pass
    // re-draws glass into the distortion RT), so somebody has to drop them at the START of the next
    // one. It used to be Pass_World -- which returns early when no level is loaded, while
    // Pass_WorldGlass has no such guard: after a level unload the main menu flushed a list of RAW
    // pointers to deleted visuals every frame (fault swallowed by CMainMenu's catch(...); xrGame is
    // built without /EH). Calculate+Render run on every frame, level or not, so the lifetime is
    // owned here -- the record side, which is the only side that fills and reads them.
    VK::g_RenderQueue.ClearGlass();
    VK::g_RenderQueue.ClearWater();


    // A frame that failed Begin (fence timeout / device lost / no swapchain image)
    // returned early WITHOUT populating g_FrameCtx — it still points at the PREVIOUS
    // frame's already-submitted command buffer, so recording the scene into it faults
    // in the driver. Skip this frame's render entirely (End() guards the same way at
    // its top). This turns any transient stall — e.g. the DLSS-G present pacer briefly
    // skipping a present — into a dropped frame instead of a hard crash.
    if (!g_FrameInFlight.valid) return;

    VK::ExecutePasses(g_FrameCtx);
    LoadProf::Scope _lpPost("rt.post"); // [rt prof]: async split, DLSS, tonemap, UI -- everything after the passes

    // Async frame-split (r_async): the scene is now fully drawn into the HDR target
    // (no swapchain touched yet). Submit it as its OWN graphics batch and continue
    // tonemap+UI in a second segment, so a compute pass can overlap the scene batch
    // (wired in increment 2b). The scene batch carries NO imageAvailable/renderFinished/
    // fence (the post segment does) and does NOT wait the async timeline (compute is
    // meant to run CONCURRENTLY with it). Uploads are flushed first so the scene draws
    // are GPU-ordered after their data. r_async 0 = one batch (the else path below).
    if (VK::Async::Available()) {
        CommandManager.FlushUploads();
        CommandManager.End(g_FrameCtx.cmd);
        CommandManager.Submit(g_FrameCtx.cmd, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                              /*waitAsyncCompute=*/false);
        VkCommandBuffer post = CommandManager.BeginSecondSegment();
        if (post != VK_NULL_HANDLE) {
            g_FrameInFlight.cmd = post;   // End() submits this segment (imageAvailable/renderFinished/fence)
            g_FrameCtx.cmd      = post;   // tonemap + MV-debug record here
            g_VkUI_FrameCmd     = post;   // deferred UI (End) records here too
        }
    }

    // Resolve the HDR scene target to the swapchain: exposure + Reinhard-white
    // tonemap (R4 combine_tonemap). After this the swapchain holds the final LDR
    // image in COLOR_ATTACHMENT; the UI pass (End) draws on top, un-tonemapped.
    // Profiled as its own zone (hosts the SSR puddle march — a rain cost), then
    // FrameEnd closes the profiler frame.
    // DLSS Super Resolution (increment 2c): temporal-upscale the render-res HDR scene
    // to display resolution BEFORE tonemap, using color + depth + motion vectors (all
    // at render res). The upscaled DlssOutput (display res) is left in SHADER_READ and
    // the tonemap samples it as its base colour (resolved-colour binding); SceneColor
    // stays at render res and still feeds the avg-luminance mip chain + bloom. DLAA
    // (r_dlss_quality 0) takes the same path with render==display. Gated r_dlss
    // (default 0) → the normal path is untouched.
    if (VK::Dlss::Enabled() && VK::MotionVec::Enabled())
    {
        const u32   idx  = g_FrameCtx.imageIndex;
        const VkExtent2D rext = g_FrameCtx.extent;        // scene render res
        const VkExtent2D dext = g_FrameCtx.displayExtent; // upscale target
        VkCommandBuffer  cmd = g_FrameCtx.cmd;
        VkImage     scImg  = VK::SceneColor::GetImage(idx);
        VkImageView scView = VK::SceneColor::GetView(idx);
        VkImageView mvView = VK::MotionVec::GetResultView();
        VkImage     mvImg  = VK::MotionVec::GetResultImage();
        // RAY RECONSTRUCTION (r_dlss_rr). NVIDIA's guide: "when DLSS-RR is enabled it
        // effectively overrides DLSS-SR execution" — so this is a ROUTE CHOICE in the
        // same slot, not an extra stage. RR reads the guide G-buffer on top of the
        // three inputs SR already gets. No Streamline route for it here: SR-under-SL
        // exists as the Frame-Generation prerequisite, and FG + RR is a later step.
        // NEURAL RENDERING (r_dlss_nr) is the same kind of route choice one step
        // further: a third tenant of this one slot, and the one with priority when
        // the player asks for it. It reads colour/depth/MV like SR and wants no
        // guides — so unlike RR it has nothing extra to be ready first.
        const bool useNR = VK::Dlss::NeuralRenderEnabled();
        const bool useRR = !useNR && VK::Dlss::RayReconEnabled();
        // SL route: sl.dlss owns the NGX feature — the raw EnsureFeature would build
        // a SECOND tensor set (the mid-game-enable OOM crash, 22:51 log).
        const bool useSL = !useNR && !useRR && ps_r_dlss_sl && VK::SL::SuperResAvailable();
        // Same courtesy for NR, and it needs it more than RR does: its feature ID is
        // a guess, so "nothing happened" is a REACHABLE state with a fixable cause.
        // ⚠Not latched behind a static bool alone — re-arm it when the player picks a
        // different r_dlss_nr_featid, or the one line they need never prints twice.
        if (ps_r_dlss_nr && !useNR) {
            static int s_saidNoNRFor = -1;
            if (s_saidNoNRFor != (int)ps_r_dlss_nr_featid) {
                s_saidNoNRFor = (int)ps_r_dlss_nr_featid;
                Msg("![VK NR] r_dlss_nr 1 but Neural Rendering is not running: available=%d, "
                    "feature id %u. If a CreateFeature line above says it was refused, try the "
                    "next id (`r_dlss_nr_featid 15`). Whatever else was enabled stays active.",
                    (int)VK::Dlss::NeuralRenderAvailable(), ps_r_dlss_nr_featid);
            }
        }
        // Say it once when the user asked for RR and cannot have it, instead of
        // silently running SR and letting them conclude RR "does nothing".
        if (ps_r_dlss_rr && !useRR && !useNR) {
            static bool s_saidNoRR = false;
            if (!s_saidNoRR) {
                s_saidNoRR = true;
                Msg("![VK RR] r_dlss_rr 1 but Ray Reconstruction is not running: available=%d, guides=%d. "
                    "Super Resolution stays active.",
                    (int)VK::Dlss::RayReconAvailable(), (int)VK::RRGuide::Ready());
            }
        }
        // Streamline can only be brought up BEFORE the Vulkan device exists, so it is
        // decided at startup from user.ltx (HW_Vulkan.cpp Step 0). Flipping r_dlss_sl
        // mid-run therefore silently does nothing — say so instead of pretending.
        if (ps_r_dlss_sl && !VK::SL::Inited()) {
            static bool s_saidNoSL = false;
            if (!s_saidNoSL) {
                s_saidNoSL = true;
                Msg("![VK DLSS] r_dlss_sl 1 but Streamline was NOT initialised this run — it must init "
                    "before the Vulkan device is created. Put `r_dlss_sl 1` in user.ltx (or pass "
                    "-force_sl) and RESTART; raw NGX stays active meanwhile.");
            }
        }
        if (scImg != VK_NULL_HANDLE && mvImg != VK_NULL_HANDLE &&
            (useNR ? VK::Dlss::EnsureFeatureNR(cmd, rext.width, rext.height, dext.width, dext.height,
                                               VK::SceneColor::Generation())
                   : useRR ? VK::Dlss::EnsureFeatureRR(cmd, rext.width, rext.height, dext.width, dext.height,
                                                       VK::SceneColor::Generation())
                   : useSL ? VK::Dlss::EnsureFeatureSL(rext.width, rext.height, dext.width, dext.height)
                           : VK::Dlss::EnsureFeature(cmd, rext.width, rext.height, dext.width, dext.height,
                                                     VK::SceneColor::Generation())))
        {
            VK::Dlss::EnsureOutput(dext.width, dext.height);
            const int zd = VK::Prof::ZoneBegin(cmd, "DLSS");

            // Discard the temporal history on a discontinuity the motion vectors
            // cannot describe (eval-chain gap / level load / teleport). Called ONCE
            // per evaluated frame — it rolls the detector's own camera+frame state.
            const bool histReset = VK::Dlss::TakeHistoryReset();

            // INPUT LAYOUTS. The two routes want different ones, so pick here and
            // restore symmetrically after the eval:
            //   raw NGX  — GENERAL (what NGX requires; go straight there from the
            //              attachment layout, 2 transitions instead of 4);
            //   sl.dlss  — SHADER_READ, because EvaluateSR TAGS its resources with
            //              that layout (vk_sl.cpp MakeRes) and SL trusts the tag.
            // MV already sits in SHADER_READ from its producing pass.
            const VkImageLayout inLayout = useSL ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_GENERAL;
            VK::ImageBarrier(cmd, scImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, inLayout);
            VK::ImageBarrier(cmd, Swapchain.m_DepthImage, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         inLayout, VK_IMAGE_ASPECT_DEPTH_BIT);
            if (!useSL)
                VK::ImageBarrier(cmd, mvImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            // The three guide targets follow the same contract as colour/depth/MV:
            // the caller owns their transitions, NGX gets them in GENERAL, and they
            // go back to COLOR_ATTACHMENT (their frame-wide layout) after.
            if (useRR) VK::RRGuide::ToGeneral(cmd);

            // Stage B (r_dlss_sl): route the SR evaluate through Streamline's sl.dlss
            // instead of raw NGX — same inputs/output/barriers, prerequisite for FG.
            bool evalOk = true;
            static u32 s_slRescueFrame = 0;   // frame the VRAM rescue started (0 = none)
            // While a rescue is in progress, probe sl.dlss only every 20th frame —
            // hammering its failed NGX creation every frame is what made Streamline
            // throw out of its vkQueuePresentKHR hook before (the 22:51 crash).
            const bool slProbe = (s_slRescueFrame == 0) ||
                                 ((Device.dwFrame - s_slRescueFrame) % 20u == 0u);
            if (useSL) {
                // Stage C: when Frame Generation is on, (re)configure DLSS-G for this
                // frame BEFORE the SR eval — the eval then tags depth+mvec so they
                // survive to present, and the interposer's pacer generates frames.
                if (VK::SL::FrameGenWanted()) {
                    const u32 mult = (ps_r_dlss_fg_mult < 2) ? 2u :
                                     (ps_r_dlss_fg_mult > 6) ? 6u : (u32)ps_r_dlss_fg_mult;
                    VK::SL::ConfigureFG(mult - 1, rext.width, rext.height, dext.width, dext.height);
                    VK::SL::DebugTick();   // r_dlss_fg_debug: verify FG generating + Reflex live
                }
                // Output → GENERAL (fully overwritten; raw path does this inside Evaluate).
                // Explicit stage/access for the same reason as the raw path: sl.dlss ends
                // up in the same NGX snippet, which CLEARS this image as well as computing
                // into it, and the layout-derived compute-only pair does not cover that.
                VK::ImageBarrier(cmd, VK::Dlss::OutputImage(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
                float jpx = 0.f, jpy = 0.f;
                VK::Dlss::GetJitterPix(jpx, jpy);
                VK::SL::SrImg c{ scImg, scView, VK::SceneColor::Format(), rext.width, rext.height };
                VK::SL::SrImg d{ Swapchain.m_DepthImage, Swapchain.m_DepthView, Swapchain.m_DepthFormat, rext.width, rext.height };
                VK::SL::SrImg m{ mvImg, mvView, VK::MotionVec::Format(), rext.width, rext.height };
                VK::SL::SrImg o{ VK::Dlss::OutputImage(), VK::Dlss::OutputView(), VK::Dlss::OutputFormat(), dext.width, dext.height };
                evalOk = slProbe && VK::SL::EvaluateSR(cmd, c, d, m, o, VK::MotionVec::CurVP(), VK::MotionVec::PrevVP(),
                                                      jpx, jpy, histReset);
            } else {
                VK::Dlss::Img c{ scView, scImg, VK::SceneColor::Format() };
                VK::Dlss::Img d{ Swapchain.m_DepthView, Swapchain.m_DepthImage, Swapchain.m_DepthFormat };
                VK::Dlss::Img m{ mvView, mvImg, VK::MotionVec::Format() };
                if (useNR) {
                    evalOk = VK::Dlss::EvaluateNR(cmd, c, d, m, rext.width, rext.height, histReset);
                } else if (useRR) {
                    VK::Dlss::Img gn{ VK::RRGuide::NormalView(),  VK::RRGuide::NormalImage(),  VK::RRGuide::NormalFormat()  };
                    VK::Dlss::Img gd{ VK::RRGuide::DiffAlbView(), VK::RRGuide::DiffAlbImage(), VK::RRGuide::DiffAlbFormat() };
                    VK::Dlss::Img gs{ VK::RRGuide::SpecAlbView(), VK::RRGuide::SpecAlbImage(), VK::RRGuide::SpecAlbFormat() };
                    evalOk = VK::Dlss::EvaluateRR(cmd, c, d, m, gn, gd, gs, rext.width, rext.height, histReset);
                } else {
                    evalOk = VK::Dlss::Evaluate(cmd, c, d, m, rext.width, rext.height, histReset);
                }
            }
            // Both routes leave the inputs in `inLayout` and the output in GENERAL.

            // Gate the tonemap on the ACTUAL resolve — a failed evaluate leaves the
            // output undefined (was: solid blue screen), so until it succeeds the
            // composite keeps sampling native scene colour.
            // ⚠This used to hold for the SL route ONLY: the raw NGX calls returned
            // void and swallowed their failure, so `evalOk` stayed at its initial
            // `true` and the tonemap composited an unwritten image — black, or last
            // frame frozen in place. Both routes report honestly now.
            VK::Dlss::SetResolvedThisFrame(evalOk);
            if (!evalOk && useSL) {
                if (s_slRescueFrame == 0) {
                    // sl.dlss could not create its NGX feature — 0xbad0000d = out of
                    // GPU memory (the card is saturated by the streamers, and NGX
                    // allocates OUTSIDE VMA so cached-block holes don't help). Free
                    // REAL memory: hard-demote streamed textures (their big images
                    // are dedicated allocations now → the retire ring returns them
                    // to the OS in a few frames) and keep probing before giving up.
                    Msg("![VK DLSS] evaluate failed — freeing VRAM for NGX (texture demote) and retrying...");
                    VK::TextureStreamer::Instance().FreeDeviceVram(768);
                    s_slRescueFrame = Device.dwFrame ? Device.dwFrame : 1;
                } else if (slProbe && Device.dwFrame - s_slRescueFrame > 240) {
                    Msg("![VK DLSS] still failing after the VRAM rescue — DLSS OFF "
                        "(enable it before loading a level, or lower r_txstream_budget)");
                    // Post-mortem: full allocator JSON so the GB-scale vma-slack that
                    // starved NGX can be attributed pool-by-pool (also: r_vram_dump).
                    VK::Vram::DumpVmaJson();
                    ps_r_dlss = 0;
                    s_slRescueFrame = 0;
                }
            } else if (evalOk) {
                s_slRescueFrame = 0;
            }

            // DlssOutput (display res) → SHADER_READ so the tonemap samples it as the
            // base colour. SceneColor → COLOR_ATTACHMENT again so GenerateMips (avg-lum
            // + bloom source) runs on the render-res scene. Depth → DEPTH_ATTACHMENT
            // for the tonemap's own DEPTH→SHADER_READ barrier. MV back to SHADER_READ
            // for the r_mv_debug overlay. Each undoes exactly the `inLayout` hop above.
            VkImage outImg = VK::Dlss::OutputImage();
            // ⚠Explicit src, mirroring the pre-evaluate barrier: the LAST write to this
            // image is not the compute the GENERAL layout implies — sync validation names
            // it outright, `vkCmdClearColorImage[DLSS::nv.ngx.dlss.Evaluate]`. NGX clears
            // the output inside its own evaluate, so a src of COMPUTE/STORAGE_WRITE alone
            // leaves this transition racing that clear (151 hits in one run, 16-08).
            VK::ImageBarrier(cmd, outImg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            VK::ImageBarrier(cmd, scImg,  inLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VK::ImageBarrier(cmd, Swapchain.m_DepthImage, inLayout,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            if (!useSL)
                VK::ImageBarrier(cmd, mvImg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (useRR) VK::RRGuide::ToAttachment(cmd);
            VK::Prof::ZoneEnd(cmd, zd);
        }
    }


    // FSR 3.1.4 (r_fsr). The SAME slot the DLSS block above occupies, and never at
    // the same time — Fsr::Enabled() stands down whenever DLSS is upscaling, so the
    // two can be reasoned about one at a time. Everything after this point (tonemap
    // base colour, UI, present) is identical either way.
    //
    // ⭐This block is short because the machinery was already here: motion vectors,
    // the render<display extent, the jitter seam, and the tonemap's resolved-colour
    // binding were all built for DLSS and none of them are NVIDIA-specific. What was
    // missing was an upscaler that would run on the other 60% of GPUs.
    else if (VK::Fsr::Enabled() && VK::MotionVec::Enabled())
    {
        const u32        idx  = g_FrameCtx.imageIndex;
        const VkExtent2D rext = g_FrameCtx.extent;
        const VkExtent2D dext = g_FrameCtx.displayExtent;
        VkCommandBuffer  cmd  = g_FrameCtx.cmd;
        VkImage     scImg  = VK::SceneColor::GetImage(idx);
        VkImageView scView = VK::SceneColor::GetView(idx);
        VkImage     mvImg  = VK::MotionVec::GetResultImage();
        VkImageView mvView = VK::MotionVec::GetResultView();

        if (scImg != VK_NULL_HANDLE && mvImg != VK_NULL_HANDLE)
        {
            VK::Fsr::EnsureOutput(dext.width, dext.height);
            const int zf = VK::Prof::ZoneBegin(cmd, "FSR");

            // ⚠⚠GENERAL IN, GENERAL OUT — and this is not a convention we chose, it
            // is what the FidelityFX VK backend guarantees. It records each app
            // resource's initialState when the dispatch registers it and
            // UnregisterResourcesVK walks every one of them back to that state before
            // returning (ffx_fsr3upscaler.cpp:1174). We declare UNORDERED_ACCESS,
            // which that backend maps to GENERAL, so the undo barriers below are
            // symmetric with these — exactly as the DLSS route is with `inLayout`.
            VK::ImageBarrier(cmd, scImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            VK::ImageBarrier(cmd, Swapchain.m_DepthImage, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            VK::ImageBarrier(cmd, mvImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

            // Discard the temporal history on a discontinuity the motion vectors
            // cannot describe. Called ONCE per evaluated frame — it rolls the
            // detector's own camera+frame state.
            const bool histReset = VK::Fsr::TakeHistoryReset();

            VK::Fsr::Img c{ scView, scImg, VK::SceneColor::Format() };
            VK::Fsr::Img d{ Swapchain.m_DepthView, Swapchain.m_DepthImage, Swapchain.m_DepthFormat };
            VK::Fsr::Img m{ mvView, mvImg, VK::MotionVec::Format() };
            const bool evalOk = VK::Fsr::Evaluate(cmd, c, d, m, rext.width, rext.height,
                                                  dext.width, dext.height, histReset);

            // ⚠⚠THE GATE. A failed dispatch leaves FSR.Output undefined, and the
            // tonemap composites its base colour from exactly that image. The DLSS
            // path shipped for weeks with this gate reachable only on one of its two
            // routes; the symptom was a black or frozen frame that looked like a
            // driver bug. One line, and it is not optional.
            VK::Fsr::SetResolvedThisFrame(evalOk);

            // Undo each hop above. The output goes to SHADER_READ for the tonemap;
            // ⚠its src stage/access is spelled out rather than derived from GENERAL
            // for the same reason the DLSS route does it: an upscaler that clears its
            // own output inside the dispatch leaves this transition racing that clear.
            if (evalOk)
                VK::ImageBarrier(cmd, VK::Fsr::OutputImage(), VK_IMAGE_LAYOUT_GENERAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            VK::ImageBarrier(cmd, scImg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VK::ImageBarrier(cmd, Swapchain.m_DepthImage, VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            VK::ImageBarrier(cmd, mvImg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            VK::Prof::ZoneEnd(cmd, zf);
        }
    }
    {
        const int z = VK::Prof::ZoneBegin(g_FrameCtx.cmd, "Tonemap");
        VK::Pass_TonemapComposite(g_FrameCtx);
        VK::Prof::ZoneEnd(g_FrameCtx.cmd, z);
    }

    // PHASE B (editor-on-Vulkan): draw the editor grid/gizmos POST-TONEMAP on the
    // final LDR swapchain — crisp + bright direct colors, immune to bloom and the
    // tone curve (the scene itself still went fully through HDR). Consumes the lines
    // the CEditorViewport sink queued this frame via EditorOverlay::AddLine.
    if (VKEditor::Active())
        EditorOverlay::ExecutePostTonemap(g_FrameCtx.cmd, Swapchain.m_ImageViews[g_FrameCtx.imageIndex],
                                          g_FrameCtx.displayExtent, RV().mFullTransform);

    // DLSS-G hudless: the swapchain now holds the final tonemapped scene with NO UI
    // (the UI pass runs later in End()). Snapshot + tag it so Frame Generation keeps
    // the HUD crisp on generated frames instead of warping it. FG-only (no-op else).
    // Captured here, before the r_mv_debug overlay, so hudless stays a clean frame.
    VK::SL::TagHudless(g_FrameCtx.cmd, Swapchain.m_Images[g_FrameCtx.imageIndex],
                       g_FrameCtx.displayExtent.width, g_FrameCtx.displayExtent.height, Swapchain.m_Format);

    // r_mv_debug: paint the motion-vector field over the final image (the
    // swapchain is COLOR_ATTACHMENT here; the UI pass in End draws on top). No-op
    // unless r_mv_debug is set. Verifies the MV reconstruction before any DLSS.
    VK::MotionVec::DrawDebugOverlay(g_FrameCtx.cmd, Swapchain.m_ImageViews[g_FrameCtx.imageIndex], g_FrameCtx.displayExtent);
    // r_dlss_rr_debug — same post-tonemap slot, same reason: the guide viewer must
    // show the stored number, not the number after exposure.
    VK::RRGuide::DrawDebugOverlay(g_FrameCtx.cmd, Swapchain.m_ImageViews[g_FrameCtx.imageIndex], g_FrameCtx.displayExtent);
    // nfs13: Prof::FrameEnd moved to the end of CRender::End -- the UI tail is part of the GPU frame and was in no zone.

    // The UI never records into this frame directly: every UI command is queued and End replays it on top of
    // the finished image. (The removed immediate path once let the cursor -- seqRender pri 1 -- open the UI pass
    // before the menu's scene, pri 5, which then recorded query resets and barriers INSIDE it: the 18-08 crash on
    // ESC from a loaded level.)
}
void CRender::AfterWorldRender() { VK_STUB_ONCE("CRender"); }
void CRender::AfterUIRender()    { VK_STUB_ONCE("CRender"); }
void CRender::Screenshot(ScreenshotMode mode, LPCSTR name) { ScreenshotImpl(mode, name); }
void CRender::ScreenshotImpl(ScreenshotMode /*mode*/, LPCSTR name)
{
    string_path fname;
    if (name && name[0]) {
        xr_strcpy(fname, name);
    } else {
        // Timestamped under $screenshots$/ss_<user>_<timestamp>.tga
        SYSTEMTIME t; GetLocalTime(&t);
        string_path leaf;
        xr_sprintf(leaf, "ss_%s_%02d-%02d-%02d_%02d-%02d-%02d.tga",
                   Core.UserName, t.wDay, t.wMonth, t.wYear % 100, t.wHour, t.wMinute, t.wSecond);
        FS.update_path(fname, "$screenshots$", leaf);
    }
    { std::lock_guard<std::mutex> g(g_ShotMx); g_PendingScreenshotPath = fname; }
    Msg("[VK Screenshot] queued: %s", fname);
}

// ----- Render mode ----------------------------------------------------------
void CRender::rmNear(CBackend&)   {}
void CRender::rmFar(CBackend&)    {}
void CRender::rmNormal(CBackend&) {}

// ----- Stats ----------------------------------------------------------------
u32 CRender::memory_usage()     { return 0; }
u32 CRender::GetCacheStatPolys() { return 0; }

// ----- Backbuffer -----------------------------------------------------------
// Begin/Clear/End drive a single per-frame command buffer so we can confirm
// Vulkan device + swapchain + sync are alive end-to-end. The framegraph will
// own the frame once it lands.

namespace
{
void FillEnvDesc(EnvDescSnap& d, const CEnvDescriptor& s)
{
    auto str = [](const shared_str& x) { return x.size() ? std::string(x.c_str()) : std::string(); };
    d.exec_time = s.exec_time;
    d.sky_texture_name = str(s.sky_texture_name); d.sky_texture_env_name = str(s.sky_texture_env_name);
    d.clouds_texture_name = str(s.clouds_texture_name); d.m_identifier = str(s.m_identifier);
    d.lens_flare_id = str(s.lens_flare_id); d.tb_id = str(s.tb_id);
    d.clouds_color = s.clouds_color; d.sky_color = s.sky_color; d.sky_rotation = s.sky_rotation;
    d.far_plane = s.far_plane; d.far_plane_render = s.far_plane_render;
    d.fog_color = s.fog_color; d.fog_density = s.fog_density; d.fog_distance = s.fog_distance;
    d.rain_density = s.rain_density; d.rain_color = s.rain_color;
    d.bolt_period = s.bolt_period; d.bolt_duration = s.bolt_duration;
    d.wind_velocity = s.wind_velocity; d.wind_velocity_old = s.wind_velocity_old;
    d.wind_direction = s.wind_direction; d.wind_direction_old = s.wind_direction_old;
    d.ambient = s.ambient; d.hemi_color = s.hemi_color; d.hemi_color_unmodded = s.hemi_color_unmodded;
    d.sun_color = s.sun_color; d.sun_dir = s.sun_dir;
    d.m_fSunShaftsIntensity = s.m_fSunShaftsIntensity; d.m_fWaterIntensity = s.m_fWaterIntensity;
    d.m_fTreeAmplitudeIntensity = s.m_fTreeAmplitudeIntensity; d.m_fTreeAmplitudeIntensity_old = s.m_fTreeAmplitudeIntensity_old;
    d.bloom_threshold = s.bloom_threshold; d.bloom_exposure = s.bloom_exposure; d.bloom_sky_intensity = s.bloom_sky_intensity;
}
} // namespace

void EnvSnapshot::Capture()
{
    static Fvector s_lastRealSun{0.f, -1.f, 0.f};   // main thread only (Capture runs in Begin_Extract)
    CurrentEnv = nullptr; Current[0] = Current[1] = nullptr;
    thunderActive = false; thunderFlash.set(0.f, 0.f, 0.f);
    sunDirVisual = s_lastRealSun;
    if (!g_pGamePersistent) return;
    const CEnvironment& env = g_pGamePersistent->Environment();
    wetness_factor = env.wetness_factor;
    wind_anim = env.wind_anim;
    thunderActive = env.IsThunderboltActive();
    thunderFlash = env.ThunderboltFlash();
    if (env.CurrentEnv)
    {
        FillEnvDesc(m_cur, *env.CurrentEnv);
        m_cur.weight = env.CurrentEnv->weight; m_cur.fog_near = env.CurrentEnv->fog_near; m_cur.fog_far = env.CurrentEnv->fog_far;
        CurrentEnv = &m_cur;
        if (!thunderActive) s_lastRealSun = m_cur.sun_dir;
        sunDirVisual = s_lastRealSun;
    }
    if (env.Current[0]) { FillEnvDesc(m_c0, *env.Current[0]); Current[0] = &m_c0; }
    if (env.Current[1]) { FillEnvDesc(m_c1, *env.Current[1]); Current[1] = &m_c1; }
}

void RenderView::Capture()
{
    frameIndex = Device.dwFrame;
    frameSeq = RT::FrameSeq();
    ringSlot = u32(RT::FrameSeq64() % CVulkanCommandManager::FRAMES_IN_FLIGHT);
    dwWidth = Device.dwWidth;   dwHeight = Device.dwHeight;
    fTimeDelta = Device.fTimeDelta; fTimeDeltaReal = Device.fTimeDeltaReal; fTimeDeltaRealMS = Device.fTimeDeltaRealMS;
    fTimeGlobal = Device.fTimeGlobal;
    dwTimeGlobal = Device.dwTimeGlobal; dwTimeContinual = Device.dwTimeContinual;
    vCameraPosition = Device.vCameraPosition; vCameraDirection = Device.vCameraDirection;
    vCameraTop = Device.vCameraTop;           vCameraRight = Device.vCameraRight;
    mView = Device.mView; mProject = Device.mProject;
    mFullTransform = mFullTransformUnjit = Device.mFullTransform;
    mInvFullTransform = Device.mInvFullTransform;
    mFullTransform_hud = mFullTransform_hudUnjit = Device.mFullTransform_hud;
    mFullTransform_hud2 = Device.mFullTransform_hud2;
    fFOV = Device.fFOV; fASPECT = Device.fASPECT;
    jitterX = jitterY = 0.0f;
}

void RenderView::ApplyJitter(float jx, float jy)
{
    jitterX = jx; jitterY = jy;
    if (jx != 0.0f || jy != 0.0f)
    {
        Fmatrix Tj; Tj.identity(); Tj._41 = jx; Tj._42 = jy;
        mFullTransform.mulA_44(Tj);        // M := M * T  -> clip.x += jx*clip.w
        mFullTransform_hud.mulA_44(Tj);
    }
}

#ifdef __SWITCH__
extern "C" void xr_nx_stall_mark(const char* name);                 // xr_3da/device.cpp (v118)
extern "C" void xr_nx_rt_add(int slot, unsigned long long us);     // xr_3da/device.cpp (v118)
#endif

#ifdef __SWITCH__
// v121: per-frame GPU timestamps (see Begin_Render / End). Slot = frame-in-flight index.
namespace NxGpu2
{
VkQueryPool s_pool = VK_NULL_HANDLE;
float       s_periodNs = 1.f;
bool        s_written[CVulkanSync::FRAMES_IN_FLIGHT] = {};
u64         s_prevEnd = 0;
double      s_durSum = 0, s_gapSum = 0, s_durMax = 0, s_gapMax = 0;
u32         s_n = 0, s_gapN = 0;
double      s_winSum = 0;   // v123: per-[nx frame]-window average (xr_nx_gpu2_window)
u32         s_winN = 0;
double      s_perfSum = 0;  // nfs13: same, for the [VK Frame] window (xr_nx_gpu2_perf_window)
u32         s_perfN = 0;

// 08-10 [nx gpu slow]: named timestamps inside the frame (after every pass, and inside the sun shadow pass), read back
// with the frame's own start/end -- a frame whose GPU time is >= kSlowMs prints where it went. ([VKhitch] zones could
// not: the hitch it reports is the CPU frame, the zones are of the frame three back.) Per slot: start, end, marks.
constexpr u32 kMarks = 128, kPer = 2 + kMarks;   // 10-09: 64 -> 128 (the pass registry + world + shadow marks were ~62)
constexpr double kSlowMs = 14.0;
std::atomic<u32> s_markN[CVulkanSync::FRAMES_IN_FLIGHT];
const char*      s_markName[CVulkanSync::FRAMES_IN_FLIGHT][kMarks];
std::atomic<u32> s_curSlot{0};
u32              s_slowPrinted = 0;

void Mark(VkCommandBuffer cmd, const char* name)
{
    if (s_pool == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE) return;
    const u32 slot = s_curSlot.load(std::memory_order_relaxed);
    const u32 k = s_markN[slot].fetch_add(1, std::memory_order_relaxed);
    if (k >= kMarks) return;
    s_markName[slot][k] = name;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_pool, slot * kPer + 2 + k);
}

// The slot's frame is done (fence): if it was slow, print its marks in GPU order, each as the time since the previous.
void ReportSlow(u32 slot, u64 t0, u64 t1, double dur)
{
    const u32 n = _min(s_markN[slot].load(std::memory_order_relaxed), kMarks);
    if (dur < kSlowMs || !n || s_slowPrinted >= 120) return;
    u64 ts[kMarks] = {};
    if (vkGetQueryPoolResults(VulkanHW.m_Device, s_pool, slot * kPer + 2, n, sizeof(ts), ts, sizeof(u64),
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    u32 order[kMarks];
    for (u32 i = 0; i < n; ++i) order[i] = i;
    std::sort(order, order + n, [&](u32 a, u32 b) { return ts[a] < ts[b]; });
    string2048 line; line[0] = 0;
    u64 prev = t0;
    for (u32 j = 0; j < n; ++j) {
        const u32 i = order[j];
        const double ms = ts[i] >= prev ? double(ts[i] - prev) * s_periodNs * 1e-6 : 0.0;
        prev = _max(prev, ts[i]);
        if (ms < 0.3) continue;
        string128 part;
        xr_sprintf(part, "%s%s %.1f", line[0] ? " | " : "", s_markName[slot][i], ms);
        xr_strcat(line, part);
    }
    ++s_slowPrinted;
    Msg("~ [nx gpu slow] GPU frame %.1f ms: %s | (after the last mark) %.1f", dur, line,
        t1 > prev ? double(t1 - prev) * s_periodNs * 1e-6 : 0.0);
}

// 10-09 [nx gpu avg]: the WHOLE GPU frame by mark, averaged over 300 frames -- every frame, not only the slow ones. The
// [VK Perf] zones covered ~9.3 of the ~10.3 timestamp units of a heavy frame; this names where the rest goes (barriers,
// bubbles, passes without a zone). A mark's share = GPU time since the previous mark (or the frame start).
struct AvgEntry { const char* name; double sum; };
AvgEntry s_avg[kMarks + 1];
u32      s_avgN = 0, s_avgFrames = 0;
double   s_avgTail = 0, s_avgTotal = 0;
void Accumulate(u32 slot, u64 t0, u64 t1, double dur)
{
    const u32 n = _min(s_markN[slot].load(std::memory_order_relaxed), kMarks);
    u64 ts[kMarks] = {};
    if (n && vkGetQueryPoolResults(VulkanHW.m_Device, s_pool, slot * kPer + 2, n, sizeof(ts), ts, sizeof(u64),
                                   VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    u32 order[kMarks];
    for (u32 i = 0; i < n; ++i) order[i] = i;
    std::sort(order, order + n, [&](u32 a, u32 b) { return ts[a] < ts[b]; });
    u64 prev = t0;
    for (u32 j = 0; j < n; ++j) {
        const u32 i = order[j];
        const double ms = ts[i] >= prev ? double(ts[i] - prev) * s_periodNs * 1e-6 : 0.0;
        prev = _max(prev, ts[i]);
        const char* nm = s_markName[slot][i];
        u32 e = 0;
        while (e < s_avgN && s_avg[e].name != nm) ++e;
        if (e == s_avgN) { if (s_avgN >= kMarks) continue; s_avg[s_avgN++] = { nm, 0.0 }; }
        s_avg[e].sum += ms;
    }
    s_avgTail  += t1 > prev ? double(t1 - prev) * s_periodNs * 1e-6 : 0.0;
    s_avgTotal += dur;
    if (++s_avgFrames < 300) return;
    u32 idx[kMarks + 1];
    for (u32 e = 0; e < s_avgN; ++e) idx[e] = e;
    std::sort(idx, idx + s_avgN, [](u32 a, u32 b) { return s_avg[a].sum > s_avg[b].sum; });
    string2048 line; line[0] = 0;
    for (u32 k = 0; k < s_avgN; ++k) {
        const double ms = s_avg[idx[k]].sum / s_avgFrames;
        if (ms < 0.05) break;
        string128 part;
        xr_sprintf(part, "%s%s %.2f", line[0] ? " | " : "", s_avg[idx[k]].name, ms);
        xr_strcat(line, part);
    }
    Msg("~ [nx gpu avg] %u frames, GPU frame %.2f: %s | (after the last mark) %.2f", s_avgFrames, s_avgTotal / s_avgFrames,
        line, s_avgTail / s_avgFrames);
    s_avgN = 0; s_avgFrames = 0; s_avgTail = s_avgTotal = 0;
}

void FrameStart(VkCommandBuffer cmd, u32 slot)
{
    if (s_pool == VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kPer * CVulkanSync::FRAMES_IN_FLIGHT;
        if (vkCreateQueryPool(VulkanHW.m_Device, &qi, nullptr, &s_pool) != VK_SUCCESS) { s_pool = VK_NULL_HANDLE; return; }
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(VulkanHW.m_PhysicalDevice, &props);
        s_periodNs = props.limits.timestampPeriod;
    }
    // This slot's previous frame is fence-proven complete (Begin_Render waited on it): read without blocking.
    if (s_written[slot]) {
        u64 ts[2] = {};
        if (vkGetQueryPoolResults(VulkanHW.m_Device, s_pool, slot * kPer, 2, sizeof(ts), ts, sizeof(u64),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0]) {
            const double dur = double(ts[1] - ts[0]) * s_periodNs * 1e-6;
            ReportSlow(slot, ts[0], ts[1], dur);
            Accumulate(slot, ts[0], ts[1], dur);
            s_durSum += dur; if (dur > s_durMax) s_durMax = dur;
            s_winSum += dur; ++s_winN;
            s_perfSum += dur; ++s_perfN;
            if (s_prevEnd && ts[0] >= s_prevEnd) {
                const double gap = double(ts[0] - s_prevEnd) * s_periodNs * 1e-6;
                s_gapSum += gap; ++s_gapN; if (gap > s_gapMax) s_gapMax = gap;
            }
            s_prevEnd = ts[1];
            if (++s_n >= 300) {
                Msg("[nx gpu2] %u frames: GPU frame avg %.2f max %.2f ms | GPU idle between frames avg %.2f max %.2f ms",
                    s_n, s_durSum / s_n, s_durMax, s_gapN ? s_gapSum / s_gapN : 0.0, s_gapMax);
                s_n = s_gapN = 0; s_durSum = s_gapSum = s_durMax = s_gapMax = 0;
            }
        }
    }
    vkCmdResetQueryPool(cmd, s_pool, slot * kPer, kPer);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_pool, slot * kPer);
    s_written[slot] = false;
    s_markN[slot].store(0, std::memory_order_relaxed);
    s_curSlot.store(slot, std::memory_order_relaxed);
}
void FrameEnd(VkCommandBuffer cmd, u32 slot)
{
    if (s_pool == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_pool, slot * kPer + 1);
    s_written[slot] = true;
}
} // namespace NxGpu2
// [nx gpu slow] marks from the passes (vk_pass_registry.cpp, vk_pass_shadow.cpp). Render thread / its pass workers.
void NxGpuMark(VkCommandBuffer cmd, const char* name)
{
    NxGpu2::Mark(cmd, name);
}
// v123: average GPU frame duration (timestamp ticks as ms) of the frames read back since the last call ([nx frame] window).
extern "C" double xr_nx_gpu2_perf_window(void);
extern "C" double xr_nx_gpu2_perf_window(void)
{
    const double r = NxGpu2::s_perfN ? NxGpu2::s_perfSum / NxGpu2::s_perfN : 0.0;
    NxGpu2::s_perfSum = 0; NxGpu2::s_perfN = 0;
    return r;
}
extern "C" double xr_nx_gpu2_window(void);
extern "C" double xr_nx_gpu2_window(void)
{
    const double r = NxGpu2::s_winN ? NxGpu2::s_winSum / NxGpu2::s_winN : 0.0;
    NxGpu2::s_winSum = 0; NxGpu2::s_winN = 0;
    return r;
}
extern "C" int g_xr_nx_crop;     // nfs28 (device.cpp): nx_crop.txt = display crop for the whole run
extern "C" int g_xr_nx_ui_native;   // 06-10 (device.cpp): a game screen is up -> no crop, the UI at display res
extern "C" void* nwindowGetDefault(void);                                 // libnx: this title's compositor layer
extern "C" u32 nwindowSetCrop(void* nw, s32 left, s32 top, s32 right, s32 bottom);   // libnx (Result, NWindow*)
#endif

namespace VK
{
// nfs28: where the composite and the UI draw this frame -- the whole swapchain image, or (display crop) its top-left
// render-res rectangle. UI geometry is in Device pixels and maps through the viewport, so only scissors need scaling.
VkExtent2D OutputExtent()
{
    const VkExtent2D o = g_FrameCtx.outExtent;
    // Never larger than the swapchain: a UI pass between a resize and the next Begin must not see last frame's size.
    return (o.width && o.height && o.width <= Swapchain.m_Extent.width && o.height <= Swapchain.m_Extent.height)
        ? o : Swapchain.m_Extent;
}
VkRect2D ScaleRectToOutput(VkRect2D r)
{
    const VkExtent2D o = OutputExtent(), s = Swapchain.m_Extent;
    if ((o.width == s.width && o.height == s.height) || !s.width || !s.height) return r;
    const float sx = float(o.width) / float(s.width), sy = float(o.height) / float(s.height);
    const s32 x0 = _max(0, s32(floorf(float(r.offset.x) * sx)));
    const s32 y0 = _max(0, s32(floorf(float(r.offset.y) * sy)));
    const s32 x1 = _min(s32(o.width),  s32(ceilf(float(r.offset.x + s32(r.extent.width))  * sx)));
    const s32 y1 = _min(s32(o.height), s32(ceilf(float(r.offset.y + s32(r.extent.height)) * sy)));
    VkRect2D q{};
    q.offset = { x0, y0 };
    q.extent = { u32(_max(0, x1 - x0)), u32(_max(0, y1 - y0)) };
    return q;
}
}   // namespace VK

void CRender::Begin()
{
    VK_STUB_ONCE("CRender");
    // TWO-SLOT PIPELINE: only the EXTRACT half runs here (main thread). Begin_Render -- fence, acquire, command buffer, targets,
    // jitter -- runs on the render thread at the head of this frame's first job (EnsureRenderBegun).
    Begin_Extract();
}

void CRender::Begin_Extract()
{
    // PHASE B — editor viewport: lazily register the no-menu sink once ready.
    VKEditor::Tick();
    // RENDER_THREAD_PLAN step 2/3: the frame's view + environment snapshots, taken before anything below
    // reads the camera. Everything the recording passes need from Device / the environment comes from here.
    // (Into the slot Device.Begin just made current: RV()/ES() resolve by role.)
    RV().Capture();
    ES().Capture();
    // A frame without a Calculate (menu, load screen) must not inherit this slot's lists from two frames ago.
    VK::BeginVisualExtract();
    {
        extern void VK_SkelWM_BeginExtract();   // vk_SkeletonCustom.cpp
        VK_SkelWM_BeginExtract();
    }

    // The frame's ring slot is fixed here, and the extract side writes rings indexed by it (bones, particles, rain, UI
    // vertices) while the GPU may still be reading the slot's previous user (frame N-3). Wait for it HERE -- it used to be
    // Begin_Render's fence wait, which now runs later and on the other thread. The fence is only WAITED here; it is reset
    // right before the submit that signals it, so a frame that never submits leaves it signalled instead of hanging the
    // next user of the slot.
    const u32 ring = RV().ringSlot;
#ifdef __SWITCH__
    // 08-10 v248: render_begin 25-26 ms on frames with no slow render-thread record and no long GPU frame -- split
    // it in [hitch] / [nx frame]: rbx_fence = the slot's fence (normally the 60 fps pacing), rbx_ui, rbx_tex = the
    // texture streamer + material cache tick. (render_begin itself is now the view/environment capture.)
    const bool nxMarks = Device.OnMainThread();
    if (nxMarks) xr_nx_stall_mark("rbx_fence");
#endif
    RV().ringReady = g_bDeviceLost || Sync.WaitForFence(ring);   // a failed wait (2 s timeout): the GPU may still read this slot's rings
#ifdef __SWITCH__
    if (nxMarks) xr_nx_stall_mark("rbx_ui");
#endif
    VulkanUI::BeginExtract(ring, RV().ringReady);
#ifdef __SWITCH__
    if (nxMarks) xr_nx_stall_mark("rbx_tex");
#endif
    // Texture streamer tick -- here, on the main thread, with the ring slot's fence just proven: (a) the feedback readback slot
    // is safe to decode, (b) descriptor sets/images retired 4+ frames ago recycle, (c) a mip promote/demote flips material sets.
    // It used to be Begin_Render's job; on the render thread it raced the material cache against model loads on main
    // (unordered_map rehash vs iteration, VkDescriptorPool allocation). The retired sets stay valid for 4 frames, so the
    // record of the previous frame is not hurt by a flip happening beside it.
    if (RV().ringReady)
    {
        VK::TextureStreamer::Instance().Frame();
        VK::WorldMaterialCache::FrameTick();
    }
    s_renderSideBegun[RT::Slot()] = false;
}

// Runs on the render thread, at the head of the first job of every frame (Render_Record, or End for a frame that has no
// scene -- load screen, precache bookkeeping): the renderer's per-frame state is opened exactly once per frame.
void CRender::EnsureRenderBegun()
{
    RT_ASSERT_RENDER();
    bool& begun = s_renderSideBegun[RT::Slot()];
    if (begun) return;
    begun = true;
    Begin_Render();
}

void CRender::Begin_Render()
{
    VK::Prof::CpuPhaseScope _cpu(VK::Prof::CPU_BEGIN);   // [VK CPUphase] fence wait + acquire
    // ⚠⚠A frame that fails here must ALSO disown the UI command buffer, and that
    // was the hole: g_VkUI_FrameCmd is only cleared at the bottom of End(), which
    // an invalid frame never reaches -- so it kept pointing at the PREVIOUS
    // frame's already-submitted buffer. The UI does not skip drawing when the
    // scene does (CMainMenu::OnRender and CHUDManager::RenderUI run either way),
    // so it took the IMMEDIATE path (cmd non-null) and recorded vkCmdBeginRendering
    // into a buffer the GPU already owns. s_bUIPassActive then stuck true, so the
    // NEXT real frame refused to open a pass and recorded every UI draw outside
    // one -- vkCmdEndRendering with no pass active, and the driver faults.
    //
    // 📏Found 18-08 as a 100% crash on ESC from a loaded level with the new RmlUi
    // menu (CRender::End -> VulkanUI::EndUIPass, ACCESS_VIOLATION inside the
    // driver). The validation layer names both halves in one frame:
    // "vkCmdBeginRendering(): ... inside an active render pass instance" at the
    // top, then eleven "vkCmdDraw(): must be issued inside an active render pass".
    // ⭐With the cmd cleared, UI draws take the deferred path -- they queue into
    // the mapped ring and get dropped by ReplayDeferredUI, which is the right
    // answer for a frame nobody is going to present.
    // ⚠The same comment three screens down (in Render()) describes this exact
    // hazard for the SCENE and was written before the UI shared the buffer.
    if (!Swapchain.ShouldRender() || g_bDeviceLost) {
        g_FrameInFlight.valid = false;
        g_VkUI_FrameCmd = VK_NULL_HANDLE;
        return;
    }

    const u32 frame = RV().ringSlot;   // assigned at extract; the record side reads it back through GetCurrentFrame()
    CommandManager.SetCurrentFrame(frame);
#ifdef __SWITCH__
    const bool nxMarks = Device.OnMainThread();   // v118: render_begin split in [nx frame] (stall marks are main-only)
    if (nxMarks) xr_nx_stall_mark("rb_fence");
    {   // v120 fence census: before waiting on slot `frame` (last used by frame N-3), how many of the 3 frame fences
        // are already signaled? With 3 frames really in flight the waited slot is nearly always ready.
        static u32 s_n = 0, s_notReady = 0, s_sigSum = 0;
        u32 sig = 0;
        for (u32 i = 0; i < CVulkanSync::FRAMES_IN_FLIGHT; ++i)
            if (vkGetFenceStatus(VulkanHW.m_Device, Sync.GetCurrentFrame(i).inFlightFence) == VK_SUCCESS) ++sig;
        if (vkGetFenceStatus(VulkanHW.m_Device, Sync.GetCurrentFrame(frame).inFlightFence) != VK_SUCCESS) ++s_notReady;
        s_sigSum += sig;
        if (++s_n >= 300) {
            Msg("[nx fence] %u frames: waited slot NOT ready %u (%.0f%%), signaled frame fences avg %.2f of %u",
                s_n, s_notReady, 100.0 * s_notReady / s_n, double(s_sigSum) / s_n, (u32)CVulkanSync::FRAMES_IN_FLIGHT);
            s_n = s_notReady = s_sigSum = 0;
        }
    }
#endif
    // (Already signalled: the extract side waited this fence. Kept as the cheap proof the next lines rely on.) The reset
    // is NOT here any more: it sits right before the submit that signals the fence again (CRender::End).
    if (!Sync.WaitForFence(frame)) { g_FrameInFlight.valid = false; g_VkUI_FrameCmd = VK_NULL_HANDLE; return; }

    // Texture streamer tick — BEFORE this frame's command buffer opens: the frame
    // fence just proved slot N-3 done, so (a) the feedback readback slot is safe to
    // decode, (b) descriptor sets/images retired 4+ frames ago recycle, and (c) any
    // mip promote/demote can flip material sets with nothing recording. Swaps wait
    // the TRANSFER timeline only — the graphics queue never stalls (no device idle).
    // (TextureStreamer::Frame and WorldMaterialCache::FrameTick moved to Begin_Extract, main thread: they mutate the material
    // cache and the descriptor pool that runtime model loads (spawns, also main) read and allocate from.)
#ifdef __SWITCH__
    if (nxMarks) xr_nx_stall_mark("rb_mat");
#endif
    // Cluster-page streaming tick: decode the fence-proven feedback slot, drain
    // IO completions into staged installs, schedule new page reads (Stage B).
#ifdef __SWITCH__
    if (nxMarks) xr_nx_stall_mark("rb_cl");
#endif
    VK::ClusterStream::Frame();

    FrameSync& sync = Sync.GetCurrentFrame(frame);
#ifdef __SWITCH__
    if (nxMarks) xr_nx_stall_mark("rb_acquire");
#endif
    g_FrameInFlight.imageIndex = Swapchain.AcquireNextImage(sync.imageAvailable);
#ifdef __SWITCH__
    if (nxMarks) xr_nx_stall_mark("rb_rest");
#endif
    if (g_FrameInFlight.imageIndex == UINT32_MAX) {
        g_FrameInFlight.valid = false;
        g_VkUI_FrameCmd = VK_NULL_HANDLE; // see the note above the fence waits
        return;
    }

    g_FrameInFlight.cmd   = CommandManager.Begin();
    g_FrameInFlight.valid = (g_FrameInFlight.cmd != VK_NULL_HANDLE);
    if (!g_FrameInFlight.valid) { g_VkUI_FrameCmd = VK_NULL_HANDLE; return; }
#ifdef __SWITCH__
    NxGpu2::FrameStart(g_FrameInFlight.cmd, frame);
#endif

    // Texture-streaming GPU feedback resolve: copy LAST frame's per-texture
    // desired-LOD buffer into this slot's host region, then refill it with
    // 0xFFFFFFFF for this frame's world-pass atomics. First thing in the frame
    // cmd — the barrier orders against prior submits on the same queue.
    VK::TextureStreamer::Instance().RecordFeedbackResolve(g_FrameInFlight.cmd, frame);

    // Cluster-page streaming frame ops: request/touched readback + reset, staged
    // page-data copies into the pools, dirty residency-bits/slot-base uploads.
    // MUST precede every world/VSM cull dispatch this frame (they consume the
    // bits) — first thing in the frame cmd, right after the texture resolve.
    VK::ClusterStream::RecordFrameOps(g_FrameInFlight.cmd, frame);

    // Streamline frame start: acquire this frame's SL token (shared by the DLSS SR
    // eval + FG) and, when Frame Generation is on, run Reflex sleep + the sim-start
    // latency marker. No-op when SL is off. Placed on the presenting path so the SL
    // token count tracks the interposer's present count.
    VK::SL::FrameBegin();

    // Expose this frame's command buffer to vkUIRender / VulkanUI so they
    // hit the immediate path instead of buffering into deferred queue.
    g_VkUI_FrameCmd = g_FrameInFlight.cmd;
    // Rotate the UI vertex-buffer ring to this slot's region (fence above
    // proved the GPU finished reading it) and reset the write offset.
    VulkanUI::OnFrameBegin();

    // DLSS render<display (increment 2c): all scene targets render at renderExtent
    // (< display when upscaling); End() upscales the HDR result to the swapchain via
    // DLSS. When not upscaling, renderExtent == the swapchain extent → identical to
    // before. NGX gives the recommended render dims for the r_dlss_quality preset.
    VkExtent2D renderExtent = Swapchain.m_Extent;
    const char* extentOwner = "native";
    if (VK::Dlss::Upscaling() && VK::MotionVec::Enabled()) {
        VK::Dlss::GetRenderExtent(Swapchain.m_Extent.width, Swapchain.m_Extent.height,
                                  renderExtent.width, renderExtent.height);
        extentOwner = "DLSS quality";
    }
    else if (VK::Fsr::Upscaling() && VK::MotionVec::Enabled()) {
        // FSR sits between DLSS and the plain scale knob on purpose. Above
        // r_render_scale because a temporal upscaler that picked its own render
        // size and then had it overridden by a slider would produce a ratio its
        // jitter sequence was not built for; below DLSS because when both are
        // somehow available NGX is the better reconstruction and Fsr::Enabled()
        // already stands down for it.
        VK::Fsr::GetRenderExtent(Swapchain.m_Extent.width, Swapchain.m_Extent.height,
                                 renderExtent.width, renderExtent.height);
        extentOwner = "FSR quality";
    }
    else if (ps_r_render_scale < 0.999f) {
        // ⭐r_render_scale — the same render<display path DLSS drives, offered as
        // a plain knob for the machines DLSS cannot serve (and for anyone who
        // wants it off). No new machinery: every scene pass already sizes itself
        // from renderExtent, and the tonemap composites at displayExtent while
        // sampling the smaller buffers with normalised UVs.
        // ⚠Even dimensions — half-res passes (bloom, SSAO) derive their extent by
        // halving this one, and an odd width there costs a column of pixels.
        const float s = _max(0.5f, ps_r_render_scale);
        renderExtent.width  = _max(64u, u32(float(Swapchain.m_Extent.width)  * s + 0.5f)) & ~1u;
        renderExtent.height = _max(64u, u32(float(Swapchain.m_Extent.height) * s + 0.5f)) & ~1u;
        extentOwner = "r_render_scale";
    }

    // A render-resolution change resizes SHARED scene targets — the scene depth is a
    // single image across frames-in-flight — so wait the GPU idle before recreating.
    // Only fires on a quality / window change (rare); steady-state play is a no-op.
    static VkExtent2D s_lastRenderExtent = { 0, 0 };
    if (renderExtent.width != s_lastRenderExtent.width || renderExtent.height != s_lastRenderExtent.height) {
        VK_DeviceWaitIdleLocked();
        s_lastRenderExtent = renderExtent;
        // ⭐One line, and it is the ONLY evidence a settings change had an effect
        // on the resolution the scene is actually drawn at. DLSS logs its own
        // extent inside GetRenderExtent; r_render_scale had nothing, so a run
        // could not tell "the slider moved" from "the slider did something".
        Msg("[VK] render extent %ux%u -> display %ux%u, set by %s",
            renderExtent.width, renderExtent.height,
            Swapchain.m_Extent.width, Swapchain.m_Extent.height, extentOwner);
    }
    Swapchain.ResizeDepth(renderExtent);   // scene depth follows the render resolution

    // HDR scene target: all scene passes render into this floating-point image
    // (so highlights exceed 1.0); the Tonemap pass maps it to the swapchain.
    // One per swapchain image index, sized to the render resolution.
    VK::SceneColor::EnsureSize(renderExtent, (u32)Swapchain.m_Images.size());
    // FSR: build/resize its context and output HERE, not from the evaluate at the end
    // of the frame. Keyed on the DISPLAY size, so this is a no-op after the first
    // frame and a quality change never rebuilds anything.
    VK::Fsr::OnFrameBegin(Swapchain.m_Extent.width, Swapchain.m_Extent.height);
    VK::MotionVec::EnsureSize(renderExtent);   // RG16F motion target tracks the render resolution
    // Ray Reconstruction guide G-buffer — render-res like every other scene target.
    // Built only while r_dlss_rr is on; EnsureSize releases it when it goes off.
    VK::RRGuide::EnsureSize(renderExtent);
    VkImage     hdrImage = VK::SceneColor::GetImage(g_FrameInFlight.imageIndex);
    VkImageView hdrView  = VK::SceneColor::GetView(g_FrameInFlight.imageIndex);

    // Populate the per-frame context handed to Pass_*. colorView/Image point at
    // the HDR target (scene passes are oblivious — Pass_TonemapComposite reads
    // it and writes the actual swapchain image in Render()).
    g_FrameCtx.cmd          = g_FrameInFlight.cmd;
    g_FrameCtx.imageIndex   = g_FrameInFlight.imageIndex;
    g_FrameCtx.extent       = renderExtent;        // scene passes render at render res
    g_FrameCtx.displayExtent= Swapchain.m_Extent;  // tonemap composites / present at display res
    // nfs28: the Switch display scaler. With the crop on, the composite and the UI draw only the top-left render-res
    // rectangle of the swapchain image and nwindowSetCrop (CRender::End, before present) has the compositor stretch it
    // to the screen: the tone curve runs per scene pixel instead of per display pixel, the stretch costs nothing. The UI
    // is then render-res too. displayExtent stays the real one (the texture LOD bias in vk_env_light keys on it).
    g_FrameCtx.outExtent    = Swapchain.m_Extent;
#ifdef __SWITCH__
    // 06-10: not under a game screen or the main menu -- the stretched UI was unreadable there (g_xr_nx_ui_native).
    const bool uiScreen = g_xr_nx_ui_native
        || (g_pGamePersistent && g_pGamePersistent->m_pMainMenu && g_pGamePersistent->m_pMainMenu->IsActive());
    if (!uiScreen && g_xr_nx_crop   // nfs29: on by default
        && renderExtent.width < Swapchain.m_Extent.width && renderExtent.height < Swapchain.m_Extent.height)
        g_FrameCtx.outExtent = renderExtent;
#endif
    g_FrameCtx.colorImage   = hdrImage;
    g_FrameCtx.colorView    = hdrView;
    g_FrameCtx.depthView    = Swapchain.m_DepthView;
    g_FrameCtx.viewProj     = &RV().mFullTransform;
    g_FrameCtx.viewProjPrev = nullptr;  // history matrix lands with motion vectors / TAA
    g_FrameCtx.frameIdx     = RV().frameIndex;
    g_FrameCtx.dt           = RV().fTimeDelta;

    // OGSR calls Begin → Calculate → Render → End — no Clear in between, so we
    // clear at frame start. Clear the HDR SCENE target (not the swapchain — that
    // gets fully overwritten by the Tonemap pass). Same single-layout idea: the
    // HDR image stays COLOR_ATTACHMENT for every scene pass; Pass_TonemapComposite
    // later flips it to SHADER_READ. The swapchain image is left UNDEFINED here
    // and transitioned by the tonemap pass.
    VK::ImageBarrier(g_FrameInFlight.cmd, hdrImage,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(g_FrameInFlight.cmd, hdrImage,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &kBackgroundClear, 1, &range);

    // TRANSFER_WRITE (clear) → COLOR_ATTACHMENT, ordering the clear before the
    // first pass's color load so ExecutePasses skips the barrier before pass 0.
    VK::ImageBarrier(g_FrameInFlight.cmd, hdrImage,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Guide G-buffer: same story one target over — cleared to the RR defaults here,
    // then COLOR_ATTACHMENT for the whole pass sequence. Done HERE and not in
    // Pass_World because Pass_World returns early when there is no level while the
    // overlay scopes (sky/trees/grass) still run and would load an undefined image.
    VK::RRGuide::BeginFrame(g_FrameInFlight.cmd);

    // Depth: UNDEFINED → DEPTH_ATTACHMENT_OPTIMAL, ONCE per frame, here in Begin()
    // — not inside Pass_World. Pass_World can early-out (no level / pipelines not
    // ready) while Pass_Sky / Details still run and use the depth attachment with
    // loadOp=LOAD; if the transition lived in Pass_World those passes would begin
    // rendering against an UNDEFINED depth layout (VUID-vkCmdBeginRendering-09588).
    // UNDEFINED as oldLayout is fine — Pass_World clear-loads depth each frame and
    // nothing samples the previous contents. The renderer is depth-ONLY: every
    // depth attachment uses DEPTH_ATTACHMENT_OPTIMAL and no stencil aspect, so the
    // barrier must match (adding a stencil aspect to a depth-only layout would trip
    // VUID-VkImageMemoryBarrier2-aspectMask-08703). FindDepthFormat prefers the
    // stencil-less D32_SFLOAT to keep this consistent.
    {
        VkImageMemoryBarrier2 b{};
        b.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
        b.srcAccessMask = 0;
        b.dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout     = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        b.image         = Swapchain.m_DepthImage;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        VkDependencyInfo di{};
        di.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers    = &b;
        vkCmdPipelineBarrier2(g_FrameInFlight.cmd, &di);
    }

    // Render graph: seed the frame-wide resource-state tracker at the layouts this
    // frame's color/depth are now in (COLOR_ATTACHMENT / DEPTH_ATTACHMENT). Passes
    // route their transitions through g_FrameGraph so redundant barriers coalesce
    // (the depth-read window in Pass_World is the first consumer). See vk_framegraph.
    VK::g_FrameGraph.BeginFrame(hdrImage, Swapchain.m_DepthImage);

    // DLSS sub-pixel JITTER (increment 2b.2). DLAA/DLSS is temporal super-sampling:
    // each frame nudges the projection by a Halton(2,3) sub-pixel offset so the scene
    // is sampled at N sub-pixel sites over N frames — that IS the AA mechanism (without
    // it DLSS is a ~2.4ms no-op). We jitter the COMBINED view-proj here, AFTER
    // OnCameraUpdated made it final and BEFORE any scene pass reads it (passes read
    // g_FrameCtx.viewProj = &Device.mFullTransform live during Render). GOTCHA: the
    // naive proj._31/_32 trick only works on a separate proj matrix (w=vz); for the
    // combined matrix we post-multiply by a clip translation T (T._41=jx, T._42=jy) so
    // clip.x += jx*clip.w. mulA_44(T) computes M := M*T. The HUD weapon uses its own
    // FOV projection (mFullTransform_hud) — jitter it too so the closest/highest-detail
    // geometry AAs consistently. MV history rolls AFTER this (in MotionVecDyn), so every
    // MV carries the jitter → paired with the MVJittered feature flag (Option B).
    // Gated to exactly when the DLSS eval will run (see CRender::End). Option A
    // (jitter-free MV): capture the UNJITTERED view-proj for the MV passes FIRST, then
    // jitter the raster matrix. The MV passes reproject with the unjittered matrix and
    // re-apply `jx/jy` only to gl_Position — so color/depth are super-sampled by the
    // jitter while motion vectors stay clean (MVJittered flag dropped in vk_dlss.cpp).
    float jx = 0.0f, jy = 0.0f;
    // ⚠!FeatureBlocked(): while the VRAM room gate holds NGX off (vk_dlss.cpp) nothing
    // resolves the jitter, and a jittered frame with no reconstruction just shimmers.
    if (VK::Dlss::Enabled() && VK::MotionVec::Enabled() && !VK::Dlss::FeatureBlocked())
    {
        VK::Dlss::NewFrameJitter(renderExtent.width, renderExtent.height);   // jitter in RENDER pixels
        VK::Dlss::GetProjJitterNDC(jx, jy);
    }
    else if (VK::Fsr::Enabled() && VK::MotionVec::Enabled())
    {
        // Same contract, different sequence. FSR derives its phase count from the
        // render/display RATIO, so it needs the display width too - handing it a
        // DLSS-length Halton run is a slow crawl in the image, not a visible break.
        VK::Fsr::NewFrameJitter(renderExtent.width, renderExtent.height, Swapchain.m_Extent.width);
        VK::Fsr::GetProjJitterNDC(jx, jy);
    }
    VK::MotionVec::SetFrameVP(RV().mFullTransformUnjit, RV().mFullTransform_hudUnjit, jx, jy);
    // RENDER_THREAD_PLAN step 2: the jitter (M := M * T, clip.x += jx*clip.w; same shift for the HUD
    // FOV) goes into the SNAPSHOT only. Device.mFullTransform stays clean for game code and culling.
    RV().ApplyJitter(jx, jy);

    // Async compute (r_async): latch this frame's in-flight slot and the graphics
    // timeline point the compute batch orders against. The work itself is recorded
    // later, by whichever pass owns it (Pass_World records the fog inject), and the
    // graphics Submit() in End() adds the wait on its timeline.
    VK::Async::FrameBegin(frame);
}

void CRender::Clear()
{
    VK_STUB_ONCE("CRender");
    // Begin already cleared the swapchain image; engine occasionally calls
    // Clear separately for sub-target reset, no-op there.
}

void CRender::End()
{
    VK_STUB_ONCE("CRender");
    EnsureRenderBegun();   // a frame with no scene (load screen) has no Render_Record job: it is opened here
    VK::Prof::CpuPhaseScope _cpu(VK::Prof::CPU_END);   // [VK CPUphase] submit + present
    if (!g_FrameInFlight.valid) return;

    // Replay any UI draws queued via the deferred path before we close out the
    // frame; finish the dynamic-rendering UI pass so the image lands in
    // PRESENT_SRC for vkQueuePresent.
    {
        const int zUI = VK::Prof::ZoneBegin(g_FrameInFlight.cmd, "UI");   // nfs13: the HUD / menus / fonts replay
        VulkanUI::ReplayDeferredUI();
        VulkanUI::EndUIPass();
        VK::Prof::ZoneEnd(g_FrameInFlight.cmd, zUI);
    }

    // The embedded game editor (Shift+F10): its draw data was handed over by End_Render just before this. Same
    // rectangle as the UI pass (laid out in Device px, scaled to the output like the UI).
    VK::ImGuiVK::DrawGame(g_FrameInFlight.cmd, Swapchain.m_ImageViews[g_FrameInFlight.imageIndex], VK::OutputExtent());

    // Profiler overlay (r_profiler 2) — drawn on top of the finished frame while
    // the swapchain image is still COLOR_ATTACHMENT, before the PRESENT barrier.
    {
        const int zOv = VK::Prof::ZoneBegin(g_FrameInFlight.cmd, "ImGuiOverlay");
        VK::ImGuiVK::DrawOverlay(g_FrameInFlight.cmd,
                                 Swapchain.m_ImageViews[g_FrameInFlight.imageIndex],
                                 Swapchain.m_Extent, RV().fTimeDeltaReal);
        VK::Prof::ZoneEnd(g_FrameInFlight.cmd, zOv);
    }

    VkImage image = Swapchain.m_Images[g_FrameInFlight.imageIndex];

    // Bring the image home to PRESENT exactly once. This used to assert a fixed
    // COLOR_ATTACHMENT source ("in COLOR_ATTACHMENT since Begin") — which is only
    // true on frames where the tonemap or the UI actually touched it; a frame that
    // drew neither transitioned FROM a layout the image was never in. Asking the
    // registry instead makes the source whatever it really is, and correctly emits
    // NOTHING when the image is already in PRESENT_SRC (untouched frame).
    // ScreenshotRecord below round-trips PRESENT↔TRANSFER_SRC on its own.
    VK::g_FrameGraph.Track(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED)
        .Require(g_FrameInFlight.cmd, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    Swapchain.m_bRenderedThisFrame = false;

    // UNATTENDED CAPTURE (XROS_AUTO_SHOT). A scripted run has no keyboard, so
    // the only way it could ever look at what it built was the console channel —
    // and when that channel is unavailable the run is blind. This is the same
    // idea as XROS_CLIENT_CONSOLE, one level lower and with no dependencies:
    //   XROS_AUTO_SHOT=<dir\prefix>    write <prefix>NNN.tga
    //   XROS_AUTO_SHOT_AT=120,240,…    at these frame numbers (default 120)
    //   XROS_AUTO_SHOT_CMDS=a|b|c      console command run 1 frame BEFORE each
    //                                  capture, so one run can sweep debug views
    // Frames are counted from the first frame WITH A LEVEL (b_loaded), so the
    // capture lands at the same point in the scene regardless of how long the
    // load screen or the intro took on this machine.
    {
        static bool  s_autoInit = false;
        static char  s_autoPrefix[512] = {};
        static char  s_autoCmds[1024]  = {};
        // 32, not 8: an A/B sweep needs many alternations so scene drift averages
        // out between the paired samples — 8 points is barely three pairs.
        static u32   s_autoAt[32] = {};
        static u32   s_autoN      = 0;
        static u32   s_autoDone   = 0;
        static u32   s_autoFrame  = 0;
        static bool  s_autoArmed  = false;   // command for shot N already issued
        if (!s_autoInit) {
            s_autoInit = true;
            if (const char* p = std::getenv("XROS_AUTO_SHOT")) {
                xr_strcpy(s_autoPrefix, p);
                if (const char* c = std::getenv("XROS_AUTO_SHOT_CMDS")) xr_strcpy(s_autoCmds, c);
                const char* at = std::getenv("XROS_AUTO_SHOT_AT");
                if (at && at[0]) {
                    u32 v = 0; bool any = false;
                    for (const char* c = at; ; ++c) {
                        if (*c >= '0' && *c <= '9') { v = v * 10 + u32(*c - '0'); any = true; }
                        else { if (any && s_autoN < 32) s_autoAt[s_autoN++] = v; v = 0; any = false; if (!*c) break; }
                    }
                }
                if (!s_autoN) { s_autoAt[0] = 120; s_autoN = 1; }
                Msg("[VK AutoShot] armed: prefix='%s', %u capture point(s), cmds='%s'",
                    s_autoPrefix, s_autoN, s_autoCmds);
            }
        }
        // A loaded save parks on the level's "press any key" screen, which does
        // Device.Pause(bTimer=TRUE). That freezes the GLOBAL CLOCK — which is
        // also why the XROS_CLIENT_CONSOLE channel goes deaf there (it polls on
        // dwTimeGlobal) — while frames keep rendering the backdrop. Injected key
        // events do not reach the game's DirectInput, so an unattended run can
        // never get past it. Do exactly what CGamePersistent::OnKeyboardPress
        // does, ~300 frames in so the scripts have settled (starting the level
        // INSTANTLY, via keypress_on_start off, makes the mod's scripts throw).
        // ⚠⚠СЧЁТЧИК СБРАСЫВАЕТСЯ НА КАЖДОЙ НОВОЙ ЗАГРУЗКЕ, а не один раз за запуск.
        // 18-09 это убило прогон с переходом между уровнями: после jump_to_level
        // счётчик уже стоял на тысячах, и автопауза снималась НА ПЕРВОМ ЖЕ кадре
        // нового уровня — то есть ПОСРЕДИ очереди g_loading_events. Скрипты мода
        // падали, и следом падал клиент (AV в CRenderDevice::on_idle на шаге загрузки).
        static u32 s_autoWait = 0;
        static bool s_autoWasRegistered = false;
        if (load_screen_renderer.b_registered && !s_autoWasRegistered)
            s_autoWait = 0;
        s_autoWasRegistered = load_screen_renderer.b_registered;
        if (s_autoPrefix[0] && b_loaded && load_screen_renderer.b_registered) {
            if (++s_autoWait > 300) {
                Msg("[VK AutoShot] dismissing the start autopause (unattended run)");
                Device.Pause(FALSE, TRUE, TRUE, "AUTOSHOT_START");
                load_screen_renderer.stop();
            }
        }
        // Count only frames the game actually runs: while it is paused nothing
        // in the scene advances, so counted frames would photograph a freeze.
        if (s_autoPrefix[0] && b_loaded && !Device.Paused()) {
            ++s_autoFrame;
            if (s_autoDone < s_autoN) {
                // One frame early: run this capture's console command so the
                // cvar is already in effect for the frame that gets captured.
                if (!s_autoArmed && s_autoFrame + 1 >= s_autoAt[s_autoDone]) {
                    s_autoArmed = true;
                    if (s_autoCmds[0] && Console) {
                        // pick the s_autoDone-th '|'-separated command
                        const char* c = s_autoCmds; u32 idx = 0;
                        while (idx < s_autoDone && *c) { if (*c == '|') ++idx; ++c; }
                        if (*c) {
                            string256 one; u32 k = 0;
                            while (*c && *c != '|' && k < sizeof(one) - 1) one[k++] = *c++;
                            one[k] = 0;
                            if (one[0]) { Msg("[VK AutoShot] cmd: %s", one); Console->Execute(one); }
                        }
                    }
                }
                bool shotPending; { std::lock_guard<std::mutex> g(g_ShotMx); shotPending = !g_PendingScreenshotPath.empty(); }
                if (s_autoFrame >= s_autoAt[s_autoDone] && !shotPending) {
                    string_path fn;
                    xr_sprintf(fn, "%s%03u.tga", s_autoPrefix, s_autoDone);
                    { std::lock_guard<std::mutex> g(g_ShotMx); g_PendingScreenshotPath = fn; }
                    Msg("[VK AutoShot] level-frame %u -> %s", s_autoFrame, fn);
                    ++s_autoDone;
                    s_autoArmed = false;
                }
            }
        }
    }

    // F12 / `screenshot` console cmd → record CopyImageToBuffer into this
    // frame's cmd buffer.
    xr_string shotPath;
    { std::lock_guard<std::mutex> g(g_ShotMx); shotPath.swap(g_PendingScreenshotPath); }
    const bool recordScreenshot = !shotPath.empty();
    if (recordScreenshot) ScreenshotRecord(g_FrameInFlight.cmd, image, Swapchain.m_Extent, shotPath);

    VK::Prof::FrameEnd(g_FrameInFlight.cmd);   // nfs13: here, so the UI zones above are this frame's
#ifdef __SWITCH__
    NxGpu2::FrameEnd(g_FrameInFlight.cmd, CommandManager.GetCurrentFrame());   // v121: last command before the buffer ends
#endif
    if (!CommandManager.End(g_FrameInFlight.cmd)) return;

    // Submit any async uploads staged this frame on the transfer queue (signals the
    // upload timeline). The graphics Submit below adds a wait on that timeline, so
    // draws consuming just-uploaded data are GPU-ordered after the copy completes.
    CommandManager.FlushUploads();

    const u32  frame = CommandManager.GetCurrentFrame();
    FrameSync& sync  = Sync.GetCurrentFrame(frame);
    // renderFinished is per swapchain IMAGE (indexed by imageIndex), not per
    // frame-in-flight — see CVulkanSwapchain::m_RenderFinished. imageAvailable +
    // inFlightFence stay per-frame-in-flight (the fence gates their reuse).
    VkSemaphore renderFinished = Swapchain.m_RenderFinished[g_FrameInFlight.imageIndex];
    // Reflex/DLSS-G PCL markers: bracket the sim end + the render-submit + the
    // present so the pacer knows the frame boundaries (no-op unless FG is active).
    VK::SL::MarkSimEnd();
    VK::SL::MarkRenderSubmit(true);
    Sync.ResetFence(frame);   // right before the submit that signals it again (see Begin_Extract)
    if (!CommandManager.Submit(g_FrameInFlight.cmd,
                               sync.imageAvailable,
                               renderFinished,
                               sync.inFlightFence))
    {
        // The fence was reset for this submit and nothing will signal it: every later user of the ring slot (the main
        // thread in Begin_Extract included) would wait it forever. Signal it with an empty submit.
        std::lock_guard<std::mutex> queueLock(g_VkQueueMx);
        vkQueueSubmit(VulkanHW.m_GraphicsQueue, 0, nullptr, sync.inFlightFence);
    }
    VK::SL::MarkRenderSubmit(false);
    VK::SL::MarkPresent(true);
#ifdef __SWITCH__
    const u64 nxPr0 = CPU::QPC_us();
    // nfs28: the crop goes out with every nwindowQueueBuffer (libnx copies nw->crop into the queue input), so setting it
    // right before this frame's present makes it this frame's. All zero = no crop (the libnx default).
    {
        static s32 s_cropW = -1, s_cropH = -1;
        const VkExtent2D o = VK::OutputExtent();
        const bool crop = o.width != Swapchain.m_Extent.width || o.height != Swapchain.m_Extent.height;
        const s32 cw = crop ? s32(o.width) : 0, ch = crop ? s32(o.height) : 0;
        if (cw != s_cropW || ch != s_cropH)
        {
            const u32 rc = nwindowSetCrop(nwindowGetDefault(), 0, 0, cw, ch);
            Msg("[nx crop] %s: %dx%d of %ux%u (rc 0x%x)", crop ? "ON" : "off", cw, ch,
                Swapchain.m_Extent.width, Swapchain.m_Extent.height, rc);
            s_cropW = cw; s_cropH = ch;
        }
    }
#endif
    Swapchain.Present(renderFinished, g_FrameInFlight.imageIndex);
#ifdef __SWITCH__
    xr_nx_rt_add(2, CPU::QPC_us() - nxPr0);
#endif
    VK::SL::MarkPresent(false);

    // Block briefly so the staging buffer has the copy finished before we map.
    // Screenshots are rare (manual F12), one-frame stall is acceptable.
    if (recordScreenshot) {
        { std::lock_guard<std::mutex> queueLock(g_VkQueueMx); vkQueueWaitIdle(VulkanHW.m_GraphicsQueue); }
        ScreenshotDrain();
    }

    // (the ring slot is assigned per frame at extract -- no NextFrame)
    g_FrameInFlight.valid = false;
    g_VkUI_FrameCmd       = VK_NULL_HANDLE;
}

void CRender::ClearTarget() {}

// The only per-frame warm-up a level load owes the renderer: the weather-variant
// pipeline prewarm, deliberately spread one compile per frame because a cold uber-FS
// takes ~300 ms. While its queue holds keys the precache frames are earning their
// cost; once it is dry they are 40 ms of GPU each for a screen nobody sees.
bool CRender::PrecacheWarmupPending() { return VK::PipelineCache::PrewarmPending(); }

// ----- Cached transforms ----------------------------------------------------
void CRender::SetCacheXform(Fmatrix&, Fmatrix&)    {}
void CRender::SetCacheXformOld(Fmatrix&, Fmatrix&) {}

// ----- Backend --------------------------------------------------------------
// Engine code (xrGame UI, GameFont, CameraManager) calls get_imm_command_list
// purely to forward the returned ref into IRender_Target::get_width/get_height.
// Our Target stub ignores the parameter, so a typed reinterpret over a static
// buffer is safe — nobody dereferences the returned ref. UB the moment somebody
// does, in which case the AV stack will name the offending caller. Real
// CBackend lands when the framegraph drives the frame.
namespace {
alignas(16) char g_DummyBackendStorage[2048];  // sized to outsize current CBackend layout
} // namespace

CBackend& CRender::get_imm_command_list()
{
    VK_STUB_ONCE("CRender");
    return *reinterpret_cast<CBackend*>(g_DummyBackendStorage);
}

void CRender::OnCameraUpdated(bool) {}

// ----- pureFrame ------------------------------------------------------------
void CRender::OnFrame() {}

#ifdef __SWITCH__
// Used by CLoadScreenRenderer::stop() on Horizon: the load screen's textures/buffers are freed right after, and memory there
// is unified -- freeing a BO the GPU still reads from a frame in flight faults the GPU (the process and the console froze
// the moment "loadscreen down" was logged, 30-09).
extern "C" void xr_vk_wait_idle()
{
    if (VulkanHW.m_Device != VK_NULL_HANDLE)
        VK_DeviceWaitIdleLocked();
}
#endif
