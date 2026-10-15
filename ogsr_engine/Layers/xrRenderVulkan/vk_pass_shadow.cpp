// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — sun shadow caster pass. See vk_pass_shadow.h.
#include "stdafx.h"
#include "vk_env_snapshot.h"
#include "vk_render_view.h"
#include "vk_rendering.h"          // VK::RenderingBuilder
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_pass_shadow.h"
#include "vk_shadow.h"
#include "vk_sun_cascades_cfg.h"            // nfs47: every sun-cascade tunable
#include "vk_render_queue.h"               // RenderQueue (local caster queue)
#include "vk_water_sim.h"                  // WaterSim::Dispatch (shallow-water flow sim)
#include "vk_deform.h"                     // Deform::Dispatch (snow deform press field)
#include "vk_pipeline_cache.h"             // depth pipelines/layout
#include "vk_compute_util.h"               // VK::MakePipelineLayout / CreateComputePipeline
#include "vk_gfx_pipeline.h"               // VK::GfxPipelineBuilder
#include "vk_barriers.h"                   // ImageBarrier
#include "vk_pass_skinned.h"               // Skinned_UploadBones / Skinned_RenderShadow
#include "vk_pass_world.h"                 // DynamicVisuals() — rigid dynamic sun casters
#include "vk_light.h"                      // Lights::CollectFrame (shadowed spot/point picks)
#include "vk_TreeManager.h"                // Trees->RenderDepth (leafy crown casters)
#include "vk_DetailManager.h"              // CDetailManager Vsm_* getters — grass casters into the spot map
#include "vk_buffer.h"                     // CVulkanBuffer (GPU grass-cull arena/count/indirect ring)
#include "vk_command_buffer.h"             // CommandManager.GetCurrentFrame() (grass-cull in-flight slot)
#include "vk_shaders.h"                    // g_ShaderManager (grass_spot_depth.{vert,frag}.spv)
#include "vk_shadow_gpu.h"                 // ShadowGPU::Cull/Draw (GPU-driven opaque casters)
#include "vk_world_gpu.h"                  // WorldGPU::CullShadow/DrawShadow (Phase 3 cluster-LOD casters)
#include "vk_instance_gpu.h"               // InstanceGPU::CullShadow/DrawShadow (host-pushed rigid instances)
#include "vk_vsm.h"                        // VSM::MaskReady (skip the redundant sun cascade/far raster under VSM)
#include "vk_cull.h"                       // VK::ExtractFrustumPlanes (light frustum → cull planes)
#include "vk_profiler.h"                   // VK::Prof::ZoneBegin/ZoneEnd (per-target shadow GPU timing)
#include "CRender_Vulkan.h"                // RImplementation.Visuals / b_loaded
#include "vk_Visual.h"                     // vkRender_Visual::Submit
#include "vk_world_material.h"             // WorldMaterial::alphaRef (AT caster sublist probe)
#include "../../xrCore/pinned_worker.h"    // r_shadow_far_async: the far map's caster collect on core 2
#include <atomic>
#ifdef __SWITCH__
void NxGpuMark(VkCommandBuffer cmd, const char* name);   // CRender_Vulkan.cpp: [nx gpu slow], GPU time up to here
#define NX_GPU_MARK(cmd, name) NxGpuMark(cmd, name)
#else
#define NX_GPU_MARK(cmd, name) ((void)0)
#endif
#include "HW_Vulkan.h"
#include "../../xr_3da/IGame_Persistent.h" // ES()
#include "../../xr_3da/Environment.h"      // CEnvDescriptorMixer (sun_dir)
#include "../../xr_3da/device.h"           // RV().vCameraPosition (cache check)

// VKEditor has no header — every consumer forward-declares just what it needs
// (see vk_imgui.cpp:20, vk_pass_tonemap.cpp:15). >0 means a host pushed a scene,
// which is exactly the condition the instanced caster path is for.
namespace VKEditor { int HostModelCount(); }

// GLOBAL scope (NOT inside namespace VK — a block-scope extern there would
// mangle as VK::ps_r_rain_enable → LNK2001). r_rain master off skips the map.
extern int ps_r_rain_enable;
extern int ps_r_sun_mask;          // r_sun_mask: nfs46 half-res sun mask for the R1 receivers
extern int ps_r_world_flat;        // r_world_flat: R1-class statics/terrain (the only sun-mask readers)
extern int ps_r_shadow_lite;      // r_shadow_lite: characters are drawn by the capsule pass, not rasterised into the sun maps
extern int   ps_r_light_debug;        // r_light_debug — point-pool per-cube diagnostic log
extern int   ps_r_snow_deform;        // snow footprint deformation enable
extern int   ps_r_snow_deform_tex;    // use the dense deform texture (vk_deform)
extern float ps_r_mud_deform;         // mud footprints (same deform texture, active without snow)
extern int   ps_r_snow_mesh;          // dense snow surface mesh (needs the ground-height map)
extern float ps_r_snow_deform_radius; // print radius (m)
extern float ps_r_snow;               // TARGET snow coverage (gate the deform dispatch)
extern int ps_r_water_sim;   // gate the ground-height map render (sim)
extern int   ps_r_wtr;           // water pass master switch
extern float ps_r_wtr_shelter;   // water asks the rain map whether it has sky overhead
extern int   ps_r_wtr_lean;      // lean water FS: no shelter query
extern int   ps_r_wtr_sim;       // interactive ripple sim
extern int   ps_r_wtr_sim_pools; // ...bounded by the pool mask, which needs the GROUND map
extern int ps_r_puddle_sss;  // gate the ground-height map render (SSS puddle real-dip placement)
extern int ps_r_light_occ;   // dynamic-light occlusion: render the ground-height map always
extern int ps_r_gpu_shadows; // GPU-driven opaque sun shadow casters (A/B with 0)
// ⚠Объявлять ЗДЕСЬ, на файловом уровне, а не внутри Pass_SunShadow: та функция
// лежит в namespace VK, и локальный extern объявил бы VK::ps_r_sun_shadows —
// определение в vk_console_min.cpp глобальное, компоновка падает LNK2001.
extern int ps_r_sun_shadows; // r_sun_shadows 0 — солнечные тени совсем (см. ниже)
extern int   ps_r_shadow_lod;      // caster-LOD: distant casters draw coarse geometry (A/B with 0)
extern float ps_r_shadow_lod_dist; // metres from camera beyond which casters go coarse
extern int   ps_r_shadow_cluster;  // Phase 3: sun targets draw the cluster-LOD cut instead of per-mesh casters (live A/B)
extern int   ps_r_shadow_casc_cache; // cascade static-map cache (0 = re-raster every frame)
extern int   ps_r_shadow_far_async;  // far map: caster collect on a worker, old map held meanwhile (0 = in place)
extern float ps_r_shadow_casc_sun;   // sun-rotation degrees that forces a cascade static redraw
extern float ps_r_wind_shadow_dist;  // tree-shadow wind radius: near (< dist) per-frame, far cached; 0 = all static
extern int   ps_r_sun_cad_npc, ps_r_sun_cad_c0, ps_r_sun_cad_c1;   // nfs48: live layer rates
extern float ps_r_tree_shadow_lod;    // nfs48: LOD of the cached tree casters
extern int   ps_r_tree_shadow_gpu;   // GPU-cull + indirect the tree casters per shadow target (0 = the CPU walk)
extern int   ps_r_sun_far_trees;     // tree crowns in the far map (0 = none; Switch preset)
extern float ps_r_sun_far_tree_dist; // ... only within this distance of the far map centre (0 = all)
extern int   ps_r_spot_grass;        // grass casters into the spot shadow map (beam cutouts through a grass field)
extern float ps_r_spot_scale;        // the corner of each spot tile the maps are rendered into (resolution)
extern int   ps_r_light_cones;       // volumetric beams: sample the spot BEAM atlas at full tile scale
extern float ps_r_spot_npc_dist;     // NPC casters in a flashlight tile only this near the light (0 = its range)
extern int   ps_r_sun_grass;         // grass casters into the sun cascades (swaying blade shadows near the camera)
extern float ps_r_sun_grass_dist;    // sun grass-shadow instance-cull radius around the camera (m)
extern int   ps_r_point_grass;       // grass casters into the campfire point cubes (dapples around near fires)
extern int   ps_r_grass_cull_debug;  // [VK GrassCull] per-frame culled-instance totals (readback probe)
extern int   ps_r_vsm;               // VSM on → its mask drives ALL sun receivers, so the cascade/far sun maps are redundant
extern int   ps_r_vol;               // froxel volumetrics: samples the cascade for froxel SUN occlusion → keep it rendered even under VSM
extern int   ps_r_vol_debug;

namespace VK {

namespace {
    RenderQueue s_ShadowQueue;            // static caster queue (rebuilt on redraw)
    bool        s_firstUse = true;        // static map starts UNDEFINED, TRANSFER_SRC thereafter
    // Принудительная перерисовка far-карты на ОДИН кадр, независимо от кэша.
    // Нужна при переключении r_sun_shadows: ветка `(!sunLit && !s_firstUse)`
    // объявляет кэш валидным, и без этого флага выключение теней заморозило бы
    // последний дневной кадр в карте вместо того, чтобы очистить её.
    // ⚠Через s_firstUse этого делать НЕЛЬЗЯ: он выбирает oldLayout барьера.
    bool        s_forceStaticRedraw = false;
    // 10-10: the far map was re-rastered this frame (a cache miss, not a forced/first one): the cascades' static caches
    // wait a frame (staggerStaticRedraw), so a sun step is two smaller GPU frames, not one 15-19 ms frame.
    bool        s_farDrewThisFrame = false;
    bool        s_combinedFirst = true;   // combined map starts UNDEFINED, SHADER_READ thereafter
    bool        s_pointFirst    = true;   // point cube: UNDEFINED on first use

    // Spot shadow POOL: persistent per-tile state. A tile keeps its owner light
    // across frames — a static lamp's tile is rendered ONCE and reused until the
    // light moves, its casters change, or an NPC enters the cone (VSM-style
    // static caching at spot scale). Owner pointers are COMPARED only, never
    // dereferenced outside the owning frame.
    struct SpotTile {
        const void* owner = nullptr;       // vkLight* identity (compare only)
        Fvector pos{}, dir{};              // light params at the last STATIC render
        float   range = 0.f, cone = 0.f;
        bool    staticValid  = false;      // STATIC layer holds this owner's statics+trees
        bool    contentValid = false;      // CLEAN layer composited (static copy + NPC)
        bool    hadSkinned   = false;      // NPCs were in the cone at the last clean render
        u32     lastSeen     = 0;          // frame the owner was last in the pool (LRU)
        u32     lastDyn      = 0;          // frame the NPC overlay was last recomposited (LOD cadence)
        RenderQueue queue;                 // cached static-caster queue
        bool    qValid = false; Fvector qPos{}; float qRange = 0.f; size_t qVis = 0;
    };
    SpotTile s_spotTiles[Lights::kMaxShadowSpots];
    int      s_giTile[Lights::kMaxLights];          // this frame: gpu[] index → tile (-1)
    bool     s_spotAtlasFirst = true;               // all spot atlases start UNDEFINED
    u32      s_spotRendPx = 0;                      // tile corner the maps were last rendered into (r_spot_scale)

    // Point shadow POOL: persistent per-cube state, same cache/LOD as spots.
    // Owner pointers COMPARED only, never dereferenced outside the owning frame.
    struct PointCube {
        const void* owner = nullptr;       // vkLight* identity (compare only)
        Fvector pos{};  float range = 0.f; // light params at the last render
        bool    staticValid  = false;      // STATIC cube holds this owner's statics (6 faces cached)
        bool    contentValid = false;      // COMBINED cube composited (static copy + NPC/grass overlay)
        bool    hadSkinned   = false;      // NPCs were in radius at the last render
        u32     lastSeen     = 0;          // frame the owner was last in the pool (LRU)
        u32     lastDyn      = 0;          // frame the cube was last re-rendered (LOD cadence)
        RenderQueue queue;                 // cached static-caster queue
        bool    qValid = false; Fvector qPos{}; float qRange = 0.f; size_t qVis = 0;
    };
    PointCube s_pointCubes[Lights::kMaxShadowPoints];
    bool      s_pointStaticFirst = true;            // STATIC cube array starts UNDEFINED
    int       s_giCube[Lights::kMaxLights];         // this frame: gpu[] index → cube (-1)

    // Statics with huge bounding spheres (terrain chunks, giant merges) are
    // receivers, not meaningful blockers, for a hand-held light — and they cost
    // the most raster. Keep fences/props/buildings, drop the monsters.
    constexpr float kSpotCasterMaxR = 30.f;

    // Depth bias to fight self-shadow acne (tunable). Constant + slope-scaled.
    constexpr float kBiasConst = 1.5f;
    constexpr float kBiasSlope = 2.5f;

    // ── Grass casters into the SPOT map (r_spot_grass) ─────────────────────
    // A beam crossing a grass field shone straight through it: the spot map
    // held statics + NPCs + trees but no grass. Re-draw the detail manager's
    // GPU-driven instance buffer (1 frame stale — same trick as the VSM grass
    // casters) depth-only with the spot VP; blades then cut BOTH the surface
    // light (spotShadowF) and the visible volumetric cone (per-step tap).
    VkPipeline            s_grassSpotPipe   = VK_NULL_HANDLE;
    VkPipelineLayout      s_grassSpotLayout = VK_NULL_HANDLE;
    VkShaderModule        s_grassSpotVS     = VK_NULL_HANDLE;
    VkShaderModule        s_grassSpotFS     = VK_NULL_HANDLE;
    bool                  s_grassSpotTried  = false;   // load the SPV once; missing = feature off

    struct GrassSpotPush {
        Fmatrix  vp;              // spot light view-proj
        Fvector4 lightPosRange;   // xyz light pos, w range (whole-instance cull)
        Fvector4 wind_params;     // SSFX wind (w = PER-TYPE wind scale, re-pushed per draw)
        Fvector4 wsetup_grass;
        Fvector4 wind_anim;
    };   // 128 B — the guaranteed push-constant minimum, VS-only range
    static_assert(sizeof(GrassSpotPush) == 128, "grass spot push must fit 128 B");

    // Lazy: needs the detail manager's mesh stride + diffuse set layout (level-load).
    VkPipeline EnsureGrassSpotPipeline()
    {
        if (s_grassSpotPipe != VK_NULL_HANDLE) return s_grassSpotPipe;
        CDetailManager* dm = RImplementation.Details;
        if (!dm) return VK_NULL_HANDLE;
        const u32 vstride = dm->Vsm_VertexStride();
        VkDescriptorSetLayout diffuseL = dm->Vsm_GfxSetLayout();
        if (vstride == 0 || diffuseL == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        if (!s_grassSpotTried) {
            s_grassSpotTried = true;
            s_grassSpotVS = g_ShaderManager->Load("grass_spot_depth.vert.spv");
            s_grassSpotFS = g_ShaderManager->Load("grass_spot_depth.frag.spv");
            if (!s_grassSpotVS || !s_grassSpotFS)
                Msg("![VK Shadow] grass_spot_depth.{vert,frag}.spv missing - grass spot shadows disabled");
        }
        if (s_grassSpotVS == VK_NULL_HANDLE || s_grassSpotFS == VK_NULL_HANDLE) return VK_NULL_HANDLE;

        if (s_grassSpotLayout == VK_NULL_HANDLE) {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GrassSpotPush) };
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.setLayoutCount = 1; plci.pSetLayouts = &diffuseL;   // set 0 = per-type diffuse (FS alpha test)
            plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_grassSpotLayout) != VK_SUCCESS)
                return VK_NULL_HANDLE;
        }
        VkVertexInputBindingDescription vibd[2] = {
            { 0, vstride, VK_VERTEX_INPUT_RATE_VERTEX },
            { 1, (u32)sizeof(DetailInstance), VK_VERTEX_INPUT_RATE_INSTANCE },
        };
        VkVertexInputAttributeDescription via[6] = {
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0  },   // aPos
            { 1, 0, VK_FORMAT_R32G32_SFLOAT,       12 },   // aUV (alpha test)
            { 2, 0, VK_FORMAT_R32_SFLOAT,          20 },   // aHeight (wind stiffness)
            { 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0  },   // aInstRow0
            { 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },   // aInstRow1
            { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32 },   // aInstRow2
        };
        // Depth-only: no colour attachment, bias set per draw by the caller.
        s_grassSpotPipe = VK::GfxPipelineBuilder(s_grassSpotLayout)
            .Vert(s_grassSpotVS).Frag(s_grassSpotFS)
            .Bindings(vibd, 2).Attrs(via, 6)
            .DynamicDepthBias()
            .Depth(true, true)
            .DepthTarget(VK_FORMAT_D32_SFLOAT)
            .Build("Shadow grass spot stride=%u", vstride);
        return s_grassSpotPipe;
    }

    // Draw all grass types into the current depth target (viewport/scissor/bias
    // already set by the caller). Reads last frame's instance buffer — the same
    // 1-frame-stale convention the VSM grass casters use. Serves the spot BEAM
    // atlas, the SUN cascades (lightPos = camera, range = r_sun_grass_dist) and
    // the point cubes — callers gate on their own cvar (r_spot/sun/point_grass).
    void DrawGrassSpotCasters(VkCommandBuffer cmd, const Fmatrix& vp,
                              const Fvector& lightPos, float lightRange)
    {
        CDetailManager* dm = RImplementation.Details;
        if (!dm) return;
        VkBuffer vis = dm->Vsm_VisibleSSBO();
        VkBuffer ind = dm->Vsm_IndirectBuf();
        const u32 types   = dm->Vsm_TypeCount();
        const u32 section = dm->Vsm_SectionSize();
        if (vis == VK_NULL_HANDLE || ind == VK_NULL_HANDLE || types == 0 || section == 0) return;
        VkPipeline pipe = EnsureGrassSpotPipeline();
        if (pipe == VK_NULL_HANDLE) return;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        GrassSpotPush push{};
        push.vp = vp;
        push.lightPosRange.set(lightPos.x, lightPos.y, lightPos.z, lightRange);
        // Same SSFX wind the colour pass used (1 frame stale, like the instances)
        // so a swaying blade carries its beam cutout with it.
        dm->Vsm_WindPush(push.wind_params, push.wsetup_grass, push.wind_anim);
        vkCmdPushConstants(cmd, s_grassSpotLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        for (u32 i = 0; i < types; ++i) {
            VkBuffer mvb, mib; u32 ic;
            if (!dm->Vsm_TypeMesh(i, mvb, mib, ic)) continue;
            VkDescriptorSet diffuse = dm->Vsm_TypeDiffuseSet(i);
            if (diffuse == VK_NULL_HANDLE) continue;
            // Per-type wind scale (DO_NO_WAVING micro-plants stay static) —
            // 4-byte push update at wind_params.w, same convention as the draw.
            const float windScale = dm->Vsm_TypeWindScale(i);
            vkCmdPushConstants(cmd, s_grassSpotLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               offsetof(GrassSpotPush, wind_params) + 3u * sizeof(float),
                               sizeof(float), &windScale);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_grassSpotLayout, 0, 1, &diffuse, 0, nullptr);
            VkBuffer vbs[2] = { mvb, vis };
            VkDeviceSize off[2] = { 0, (VkDeviceSize)i * section * sizeof(DetailInstance) };
            vkCmdBindVertexBuffers(cmd, 0, 2, vbs, off);
            vkCmdBindIndexBuffer(cmd, mib, 0, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexedIndirect(cmd, ind, (VkDeviceSize)i * sizeof(VkDrawIndexedIndirectCommand),
                                     1, sizeof(VkDrawIndexedIndirectCommand));
        }
    }

    // ── GrassCull: GPU per-light grass caster cull + COMPACTION (Phase 1) ────
    // DrawGrassSpotCasters above is brute force: it runs the grass VS over EVERY
    // visible caster blade for each shadow light — and ×6 for a point cube —
    // degen-culling the out-of-range 99% inside the VS. Near a campfire that was
    // the single fattest Shadow/Dyn cost (~3.6 ms, 2026-07-04 profile). Here a
    // compute counting-sort compacts blades into per-(light,type) arena regions
    // once per frame; each depth pass then draws only its few-hundred-instance
    // region via indirect (firstInstance = region base). One CullLights() call
    // serves a whole client (the active spot BEAM tiles, or the point CUBES —
    // all 6 faces of a cube share ONE sphere cull). See grass_cull(.setup).comp.
    namespace GrassCull {
        constexpr u32 kMaxLights = 16;                      // spot pool 8 + point pool 4 (+ headroom)
        constexpr u32 kMaxTypes  = 64;                      // dm_max_objects
        constexpr u32 kMaxCells  = kMaxLights * kMaxTypes;  // 1024 draw commands
        constexpr u32 kArenaCap  = 131072;                  // instances / frame (8 MB) — total across all lights
        // The light list is host-written per CullLights CALL, but a frame makes
        // several (spot beams, campfire cubes) and the GPU reads them 1+ frames
        // later — so a single UBO region would be clobbered before the first cull
        // read it. Sub-region the UBO per call and bind it with a DYNAMIC offset
        // (256 B/call == kMaxLights*vec4, already offset-aligned on any GPU).
        constexpr u32 kLightBytes = kMaxLights * (u32)sizeof(Fvector4);   // 256 B / call
        constexpr u32 kMaxCalls   = 4;                      // spots + cubes (+ sun/headroom)

        VkPipeline            s_cullPipe    = VK_NULL_HANDLE;   // count + scatter (push .pass selects)
        VkPipelineLayout      s_cullLayout  = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_cullSetL    = VK_NULL_HANDLE;
        VkPipeline            s_setupPipe   = VK_NULL_HANDLE;   // prefix-sum + indirect emit
        VkPipelineLayout      s_setupLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_setupSetL   = VK_NULL_HANDLE;
        VkDescriptorPool      s_pool        = VK_NULL_HANDLE;
        VkDescriptorSet       s_cullSet [VK_FRAMES_IN_FLIGHT] = {};
        VkDescriptorSet       s_setupSet[VK_FRAMES_IN_FLIGHT] = {};

        CVulkanBuffer* s_arena   [VK_FRAMES_IN_FLIGHT] = {};
        CVulkanBuffer* s_count   [VK_FRAMES_IN_FLIGHT] = {};
        CVulkanBuffer* s_cursor  [VK_FRAMES_IN_FLIGHT] = {};
        CVulkanBuffer* s_indirect[VK_FRAMES_IN_FLIGHT] = {};
        CVulkanBuffer* s_lightUBO[VK_FRAMES_IN_FLIGHT] = {};   void* s_lightPtr[VK_FRAMES_IN_FLIGHT] = {};
        CVulkanBuffer* s_typeInfo = nullptr;                    // per-type indexCount (shared, level-stable)

        VkBuffer s_boundVis[VK_FRAMES_IN_FLIGHT] = {};         // descriptor cache: the dm buffers a set points at
        VkBuffer s_boundInd[VK_FRAMES_IN_FLIGHT] = {};
        u32      s_updFrame[VK_FRAMES_IN_FLIGHT] = { ~0u, ~0u, ~0u };
        u32      s_callFrame[VK_FRAMES_IN_FLIGHT] = { ~0u, ~0u, ~0u };   // which frame s_callIdx tracks
        u32      s_callIdx[VK_FRAMES_IN_FLIGHT]   = {};                  // per-call light sub-region cursor
        u32      s_typeInfoTypes = 0;
        bool     s_tried = false, s_ready = false;

        struct CullPush  { u32 sectionSize, typeCount, numLights, pass, maxCull, p0, p1, p2; };
        struct SetupPush { u32 numLights, typeCount, maxCull, pad; };

        inline void MemBarrier(VkCommandBuffer cmd, VkAccessFlags src, VkAccessFlags dst,
                               VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
            VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            b.srcAccessMask = src; b.dstAccessMask = dst;
            vkCmdPipelineBarrier(cmd, ss, ds, 0, 1, &b, 0, nullptr, 0, nullptr);
        }

        bool Ensure() {
            if (s_ready) return true;
            if (s_tried) return false;
            s_tried = true;
            if (!g_ShaderManager) return false;

            VkShaderModule csCull  = g_ShaderManager->Load("grass_cull.comp.spv");
            VkShaderModule csSetup = g_ShaderManager->Load("grass_cull_setup.comp.spv");
            if (!csCull || !csSetup) { Msg("![VK Shadow] grass_cull(.setup).comp.spv missing - GPU grass cull disabled"); return false; }

            // Set layouts: cull(0 vis,1 detail-indirect,2 lights[UBO],3 count,4 cursor,5 arena); setup(0 count,1 cursor,2 indirect,3 typeinfo).
            constexpr auto kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            s_cullSetL  = VK::MakeSetLayout({ kSSBO, kSSBO, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                              kSSBO, kSSBO, kSSBO },
                                            VK_SHADER_STAGE_COMPUTE_BIT, "SpotShadow.Cull");
            s_setupSetL = VK::MakeSetLayout({ kSSBO, kSSBO, kSSBO, kSSBO },
                                            VK_SHADER_STAGE_COMPUTE_BIT, "SpotShadow.Setup");
            if (!s_cullSetL || !s_setupSetL) return false;

            // One pool for both set groups (cull ×F + setup ×F).
            VkDescriptorPoolSize ps[2] = {
                { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         (5 + 4) * VK_FRAMES_IN_FLIGHT },
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 * VK_FRAMES_IN_FLIGHT } };
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = 2 * VK_FRAMES_IN_FLIGHT; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
            if (vkCreateDescriptorPool(VulkanHW.m_Device, &pci, nullptr, &s_pool) != VK_SUCCESS) return false;
            if (!VK::AllocSets(s_pool, s_cullSetL,  VK_FRAMES_IN_FLIGHT, s_cullSet,  "SpotShadow.Cull"))  return false;
            if (!VK::AllocSets(s_pool, s_setupSetL, VK_FRAMES_IN_FLIGHT, s_setupSet, "SpotShadow.Setup")) return false;

            auto mkPipe = [&](VkShaderModule cs, VkDescriptorSetLayout setL, u32 pushSz,
                              VkPipelineLayout& pl, VkPipeline& pipe, const char* tag) -> bool {
                pl = VK::MakePipelineLayout({ setL }, pushSz);
                if (pl == VK_NULL_HANDLE) return false;
                pipe = VK::CreateComputePipeline(cs, pl, tag);
                return pipe != VK_NULL_HANDLE;
            };
            if (!mkPipe(csCull,  s_cullSetL,  sizeof(CullPush),  s_cullLayout,  s_cullPipe,  "SpotShadow.Cull"))  return false;
            if (!mkPipe(csSetup, s_setupSetL, sizeof(SetupPush), s_setupLayout, s_setupPipe, "SpotShadow.Setup")) return false;

            for (u32 i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) {
                s_arena[i] = xr_new<CVulkanBuffer>();
                s_arena[i]->Create((VkDeviceSize)kArenaCap * sizeof(DetailInstance),
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
                s_count[i] = xr_new<CVulkanBuffer>();
                s_count[i]->Create(kMaxCells * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
                s_cursor[i] = xr_new<CVulkanBuffer>();
                s_cursor[i]->Create(kMaxCells * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
                s_indirect[i] = xr_new<CVulkanBuffer>();
                s_indirect[i]->Create(kMaxCells * sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
                s_lightUBO[i] = xr_new<CVulkanBuffer>();
                s_lightUBO[i]->Create((VkDeviceSize)kMaxCalls * kLightBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
                s_lightPtr[i] = s_lightUBO[i]->Map();
            }
            s_typeInfo = xr_new<CVulkanBuffer>();
            s_typeInfo->Create(kMaxTypes * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);

            s_ready = true;
            Msg("[VK Shadow] GPU grass cull ready (arena %u inst/frame, %u cells, %.1f MB)",
                kArenaCap, kMaxCells, VK_FRAMES_IN_FLIGHT * (float)kArenaCap * sizeof(DetailInstance) / (1024.0f * 1024.0f));
            return true;
        }

        void updateSets(u32 cur, VkBuffer vis, VkBuffer ind) {
            VK::DescriptorWriter(s_cullSet[cur])
                .StorageBuffer(0, vis)
                .StorageBuffer(1, ind)
                // range = ONE call's window (the dynamic offset picks it)
                .Buffer       (2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                  { s_lightUBO[cur]->GetHandle(), 0, kLightBytes })
                .StorageBuffer(3, s_count[cur]->GetHandle())
                .StorageBuffer(4, s_cursor[cur]->GetHandle())
                .StorageBuffer(5, s_arena[cur]->GetHandle())
                .Flush();

            VK::DescriptorWriter(s_setupSet[cur])
                .StorageBuffer(0, s_count[cur]->GetHandle())
                .StorageBuffer(1, s_cursor[cur]->GetHandle())
                .StorageBuffer(2, s_indirect[cur]->GetHandle())
                .StorageBuffer(3, s_typeInfo->GetHandle())
                .Flush();
        }

        // Cull the given light spheres into slots [0..nLights) of THIS frame's
        // arena. Idempotent to call more than once per frame (spots, then cubes):
        // the entry barrier serializes each call against the prior client's draws.
        void CullLights(VkCommandBuffer cmd, const Fvector4* lights, u32 nLights) {
            if (!s_ready || nLights == 0) return;
            CDetailManager* dm = RImplementation.Details;
            if (!dm) return;
            VkBuffer vis = dm->Vsm_VisibleSSBO();
            VkBuffer ind = dm->Vsm_IndirectBuf();
            const u32 types = dm->Vsm_TypeCount();
            const u32 section = dm->Vsm_SectionSize();
            if (vis == VK_NULL_HANDLE || ind == VK_NULL_HANDLE || types == 0 || section == 0) return;
            if (nLights > kMaxLights) nLights = kMaxLights;
            const u32 tc  = (types < kMaxTypes) ? types : kMaxTypes;
            const u32 cur = CommandManager.GetCurrentFrame();

            if (s_typeInfoTypes != tc) {                          // per-type indexCount (level-stable)
                if (u32* p = (u32*)s_typeInfo->Map()) {
                    for (u32 i = 0; i < kMaxTypes; ++i) { VkBuffer vb, ib; u32 ic = 0; p[i] = (i < tc && dm->Vsm_TypeMesh(i, vb, ib, ic)) ? ic : 0u; }
                    s_typeInfo->Flush();
                }
                s_typeInfoTypes = tc;
            }
            // Descriptor sets point at level-stable buffers → update once per
            // frame-slot (rewriting a set an earlier same-frame call may still be
            // consuming would corrupt it; the slot's own use N frames ago is fenced).
            if (s_updFrame[cur] != RV().frameIndex || s_boundVis[cur] != vis || s_boundInd[cur] != ind) {
                updateSets(cur, vis, ind);
                s_boundVis[cur] = vis; s_boundInd[cur] = ind; s_updFrame[cur] = RV().frameIndex;
            }
            // Per-call light sub-region (dynamic UBO offset): a fresh window each
            // CullLights so the GPU still reads the spot lights after the host has
            // written the cube lights for the same in-flight frame.
            if (s_callFrame[cur] != RV().frameIndex) { s_callFrame[cur] = RV().frameIndex; s_callIdx[cur] = 0u; }
            const u32 call   = (s_callIdx[cur] < kMaxCalls) ? s_callIdx[cur] : (kMaxCalls - 1u);
            s_callIdx[cur]   = call + 1u;
            const u32 dynOff = call * kLightBytes;
            { Fvector4* lp = (Fvector4*)((u8*)s_lightPtr[cur] + dynOff);
              for (u32 i = 0; i < nLights; ++i) lp[i] = lights[i];
              s_lightUBO[cur]->Flush(); }

            const u32 nCells = nLights * tc;
            // Entry barrier: the prior consumer (this frame's earlier grass draws
            // AND/OR the prior client's cull compute) must finish before we clear
            // and overwrite the shared ring buffers.
            MemBarrier(cmd,
                VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

            vkCmdFillBuffer(cmd, s_count[cur]->GetHandle(), 0, nCells * sizeof(u32), 0u);
            MemBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

            CullPush cp{}; cp.sectionSize = section; cp.typeCount = tc; cp.numLights = nLights; cp.maxCull = kArenaCap;
            const u32 groups = (section * tc + 63u) / 64u;

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullPipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullLayout, 0, 1, &s_cullSet[cur], 1, &dynOff);
            cp.pass = 0u; vkCmdPushConstants(cmd, s_cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cp), &cp);
            vkCmdDispatch(cmd, groups, 1, 1);                     // COUNT
            MemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

            SetupPush spc{}; spc.numLights = nLights; spc.typeCount = tc; spc.maxCull = kArenaCap;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_setupPipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_setupLayout, 0, 1, &s_setupSet[cur], 0, nullptr);
            vkCmdPushConstants(cmd, s_setupLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(spc), &spc);
            vkCmdDispatch(cmd, 1, 1, 1);                          // PREFIX-SUM + indirect emit
            MemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullPipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullLayout, 0, 1, &s_cullSet[cur], 1, &dynOff);
            cp.pass = 1u; vkCmdPushConstants(cmd, s_cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cp), &cp);
            vkCmdDispatch(cmd, groups, 1, 1);                     // SCATTER
            // Publish arena (instances) + indirect (commands) to the depth draws.
            MemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);

            if (ps_r_grass_cull_debug) {
                static u32 s_dbg = 0;
                if (RV().dwTimeGlobal - s_dbg > 1500) { s_dbg = RV().dwTimeGlobal;
                    Msg("[VK GrassCull] lights=%u types=%u cells=%u (section=%u)", nLights, tc, nCells, section); }
            }
        }

        // Draw one culled light's compact region into the current depth target.
        // Reuses the grass-spot pipeline (identical vertex layout); the arena is
        // bound whole and the per-cell indirect command's firstInstance offsets it.
        void Draw(VkCommandBuffer cmd, const Fmatrix& vp, const Fvector& lightPos, float lightRange, u32 slot) {
            CDetailManager* dm = RImplementation.Details;
            if (!dm) return;
            VkPipeline pipe = EnsureGrassSpotPipeline();
            if (pipe == VK_NULL_HANDLE) return;
            const u32 types = dm->Vsm_TypeCount();
            const u32 tc = (types < kMaxTypes) ? types : kMaxTypes;
            if (slot >= kMaxLights || tc == 0) return;
            const u32 cur = CommandManager.GetCurrentFrame();
            if (!s_arena[cur] || !s_indirect[cur]) return;

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            GrassSpotPush push{};
            push.vp = vp;
            push.lightPosRange.set(lightPos.x, lightPos.y, lightPos.z, lightRange);
            dm->Vsm_WindPush(push.wind_params, push.wsetup_grass, push.wind_anim);
            vkCmdPushConstants(cmd, s_grassSpotLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

            VkBuffer arena = s_arena[cur]->GetHandle();
            VkBuffer indir = s_indirect[cur]->GetHandle();
            for (u32 i = 0; i < tc; ++i) {
                VkBuffer mvb, mib; u32 ic;
                if (!dm->Vsm_TypeMesh(i, mvb, mib, ic)) continue;
                VkDescriptorSet diffuse = dm->Vsm_TypeDiffuseSet(i);
                if (diffuse == VK_NULL_HANDLE) continue;
                const float windScale = dm->Vsm_TypeWindScale(i);
                vkCmdPushConstants(cmd, s_grassSpotLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   offsetof(GrassSpotPush, wind_params) + 3u * sizeof(float), sizeof(float), &windScale);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_grassSpotLayout, 0, 1, &diffuse, 0, nullptr);
                VkBuffer vbs[2] = { mvb, arena };
                VkDeviceSize off[2] = { 0, 0 };   // firstInstance in the indirect command offsets the arena
                vkCmdBindVertexBuffers(cmd, 0, 2, vbs, off);
                vkCmdBindIndexBuffer(cmd, mib, 0, VK_INDEX_TYPE_UINT16);
                const u32 cell = slot * tc + i;
                vkCmdDrawIndexedIndirect(cmd, indir, (VkDeviceSize)cell * sizeof(VkDrawIndexedIndirectCommand),
                                         1, sizeof(VkDrawIndexedIndirectCommand));
            }
        }

        void Destroy() {
            for (u32 i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) {
                if (s_arena[i])    { xr_delete(s_arena[i]); }
                if (s_count[i])    { xr_delete(s_count[i]); }
                if (s_cursor[i])   { xr_delete(s_cursor[i]); }
                if (s_indirect[i]) { xr_delete(s_indirect[i]); }
                if (s_lightUBO[i]) { xr_delete(s_lightUBO[i]); s_lightPtr[i] = nullptr; }
                s_boundVis[i] = s_boundInd[i] = VK_NULL_HANDLE; s_updFrame[i] = ~0u;
            }
            if (s_typeInfo) xr_delete(s_typeInfo);
            if (s_cullPipe    != VK_NULL_HANDLE) { vkDestroyPipeline(VulkanHW.m_Device, s_cullPipe, nullptr);    s_cullPipe = VK_NULL_HANDLE; }
            if (s_setupPipe   != VK_NULL_HANDLE) { vkDestroyPipeline(VulkanHW.m_Device, s_setupPipe, nullptr);   s_setupPipe = VK_NULL_HANDLE; }
            if (s_cullLayout  != VK_NULL_HANDLE) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_cullLayout, nullptr);  s_cullLayout = VK_NULL_HANDLE; }
            if (s_setupLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_setupLayout, nullptr); s_setupLayout = VK_NULL_HANDLE; }
            if (s_pool        != VK_NULL_HANDLE) { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
            if (s_cullSetL    != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_cullSetL, nullptr);  s_cullSetL = VK_NULL_HANDLE; }
            if (s_setupSetL   != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setupSetL, nullptr); s_setupSetL = VK_NULL_HANDLE; }
            s_typeInfoTypes = 0; s_tried = false; s_ready = false;
        }
    }  // namespace GrassCull

    // Static-map cache: static casters + the box tracking the camera only go
    // stale when the camera or the sun actually moves. Redraw when the camera
    // drifted > kRedrawDist from the last render, the sun rotated more than
    // ~0.1° (cos threshold), or the visual set changed (level load/unload).
    // Dynamic (skinned) casters are NOT cached — they render every frame into
    // the combined map on top of a copy of this one.
    // nfs47: vk_sun_cascades_cfg.h (profile latched at ShadowMap::Init)
#define kRedrawDist   (VK::SunCfg::Get().farRedrawDist)
#define kSunDotRedraw (VK::SunCfg::Get().farSunDotRedraw)
    bool    s_cacheValid   = false;
    Fvector s_lastCamPos;
    Fvector s_lastSunDir;
    size_t  s_lastVisCount = 0;

    // Near sun cascades (R4 scheme): the MAPS are re-rendered every frame with
    // the continuous sun (stability comes from the R4 world-anchored texel
    // alignment in ComputeCascadeVP, exactly like xrRenderPC_R4), but each
    // static-caster QUEUE is cached — rebuilding one walks all level visuals.
    // Casters are collected with kCascQueueInflate of slack so a queue stays
    // correct while the camera drifts < kCascQueueDist and the sun creeps.
    RenderQueue s_CascQueue[ShadowMap::kNumSunCascades];
    bool        s_cascQValid[ShadowMap::kNumSunCascades] = {};
    bool        s_cascFirst = true;           // cascade maps start UNDEFINED
    Fvector     s_cascQCamPos[ShadowMap::kNumSunCascades] = {};
    Fvector     s_cascQSunDir[ShadowMap::kNumSunCascades] = {};
    size_t      s_cascQVis[ShadowMap::kNumSunCascades] = {};
    constexpr float kCascQueueDist    = 4.f;
    constexpr float kCascQueueInflate = 8.f;
    constexpr float kCascQueueSunDot  = 0.9999863f;   // cos(0.3°)

    // Cascade STATIC-map cache (mirrors the far map's static/combined split): the
    // statics-only cascade depth is re-rastered ONLY when the camera moved past
    // kCascRedrawDist, the sun rotated past kCascQueueSunDot, or the level changed.
    // While valid the cascade VP is FROZEN (ComputeCascadeVP not re-run) so the
    // sampled matrix matches the cached contents; every frame the combined map is
    // copy(static)+skinned overlay. Standing/aiming/turning → ZERO static raster
    // (camera rotation doesn't move vCameraPosition). This is what kills the
    // ~3 ms/frame Shadow/Casc0 raster the profiler sub-zones pinned down.
    // nfs47: one record per cascade (were four parallel arrays). camPos is ALSO the centre of both tree bands of the
    // cascade (static [wind_dist, inf) and live [0, wind_dist)) -- see TreeShadowJob::eye.
    struct CascStaticCache
    {
        bool    valid  = false;
        Fvector camPos{};
        Fvector sunDir{};
        size_t  vis    = 0;
    };
    CascStaticCache s_cascStatic[ShadowMap::kNumSunCascades];
    u32  s_cadNpc = 1, s_cadC0 = 1, s_cadC1 = 1;               // nfs48: this frame's live-layer rates
    bool s_cascMidFirst = true;                                // nfs48: mid images start UNDEFINED
    u32  s_nxDeferred = 0;                                     // nfs48: cascade-1 cache redraws pushed a frame
    u32 s_nxRedrawWhy[ShadowMap::kNumSunCascades][4] = {};     // nfs47: first, dist, sun, vis
    bool        s_cascStaticFirst = true;   // static cascade images start UNDEFINED
    // Per-cascade redraw threshold (m). Cascade 0 (25 m box, 0.6 cm texels) tracks
    // the player tighter; cascade 1 (60 m, lower res) tolerates more drift. Small
    // enough that the near-field stays well inside the frozen box between redraws;
    // beyond it cascade 1 / the far map cover, so a stale edge never drops shadows.
#define kCascRedrawDist (VK::SunCfg::Get().cascRedrawDist)   // nfs47: vk_sun_cascades_cfg.h

    // Sun-direction low-pass: R4 feeds the raw env sun in, but our env mixer
    // showed frame-to-frame wobble earlier in this port — a 1 s exponential
    // filter eats it for free and lags the real motion imperceptibly.
    constexpr float kCascSunTau = 1.0f;
    // A one-frame change bigger than this (with no bolt corrupting sunDir — see
    // below) is a discontinuity (level/save load, scripted time-skip): the
    // cascade low-pass SNAPS to it instead of slewing, so shadows don't visibly
    // rotate into place over ~1 s after a load.
    constexpr float kCascSnapDot = 0.99939f;     // cos(2°)
    Fvector s_cascSunDir{};
    bool    s_cascSunInit = false;

    // Lightning rejection. A flash (thunderbolt.cpp OnFrame) OVERWRITES
    // CurrentEnv->sun_dir with the bolt direction for the WHOLE strike, so the
    // shadow sun would swing to the bolt and back — the "cascades shift then roll
    // back" bug. We hold the last real direction while a bolt is active (queried
    // directly via CEnvironment::IsThunderboltActive — robust at any framerate /
    // strike length, unlike the old frame-counter hold which released mid-strike
    // on high-refresh displays).
    Fvector s_stableSunDir{};
    bool    s_stableSunInit = false;

    // Rain occlusion map cache: statics + trees from straight above, redrawn
    // when the camera moved far enough or the level changed. Rendered only
    // while it's raining (or surfaces are still drying) — but at least once,
    // so the sampled image is never in UNDEFINED layout.
    RenderQueue s_RainQueue;
    bool        s_rainFirst  = true;
    bool        s_rainValid  = false;
    Fvector     s_rainCamPos{};
    size_t      s_rainVis    = 0;
    // Rain occlusion map redraws on camera move (nVis = total loaded visuals,
    // only changes on stream/spawn — NOT per frame). Profiling showed the redraw
    // (19795 casters + all trees into 1024²) is the rain GPU/CPU SPIKE while
    // walking. 16 m halves that frequency vs the old 8 m; the top-down ±75 m map
    // is tolerant of a stale border.
    constexpr float kRainRedrawDist = 16.f;
    // Skip tiny casters (barrels/crates/debris): the dry patch a sub-1.5 m prop
    // shelters is negligible, but they're most of the 19795-item count → cutting
    // them shrinks the redraw spike. Roofs/cars/buildings/trees (large R) stay.
    constexpr float kRainMinCasterR = 1.5f;

    // Precomputed depth-caster list. Every cached caster queue in this pass
    // (far sun map, rain/occlusion map, 2 near cascades, fog map, spot tiles /
    // point cubes) used to rebuild by walking ALL of RImplementation.Visuals —
    // on heavy levels ~450k virtual Submit() calls per target per rebuild,
    // each a cache miss on rv->vis.sphere. With r_vsm 0 the cascades re-collect
    // every few metres of drift / 0.3° of sun creep, so the CPU sat at 48-87 ms
    // while the GPU idled at 14-16 ms (under VSM the sun rebuilds sleep, which
    // is why nobody saw it). The vast majority of those visuals can NEVER push
    // a caster: trees draw through CTreeManager (Submit no-op), skinned leaves
    // are no-ops, pool-compacted statics have their CPU slices freed. Probe
    // each visual ONCE per level with a scratch queue, keep only real casters,
    // and copy the cull sphere inline so a rebuild scan is a contiguous read.
    struct CasterEntry { Fsphere bs; vkRender_Visual* rv; };
    xr_vector<CasterEntry> s_Casters;
    // Alpha-tested ITEM cache for the rain/occlusion map. When the GPU cluster
    // path draws the opaque casters, FlushDepth(alphaTestedOnly) draws ONLY
    // items whose material has alphaRef >= 0 — but the old rebuild re-Submitted
    // whole MU hierarchies (13k AT casters → 188k pushed items) and re-sorted
    // them, ~48 ms per rain-window move, firing even in DRY weather via the
    // lamp-occlusion/ground maps. Cache the exact AT DrawItems once (captured
    // from the probe queue, PRE-SORTED by sortKey); a rebuild is then a sphere-
    // filtered ordered copy — no Submit recursion, no sort.
    struct RainATItem { Fsphere bs; float casterR; DrawItem item; };
    xr_vector<RainATItem> s_RainATItems;
    bool   s_castersBuilt     = false;
    size_t s_castersVis       = 0;      // Visuals.size() at build
    bool   s_castersCompacted = false;  // pool compaction no-ops more Submits → rebuild once to harvest

    // Far map collect on a worker (r_shadow_far_async). 📏08-10-2026 Switch v247 fight: a far-map cache miss (camera
    // 10 m / sun step) cost the render thread ShadowQ/far 9-11 + sq.sort 1.5-1.7 ms in ONE frame (the walk over every
    // static caster), the frame came out 26-29 ms and the main thread then waited 16-22 ms in render_begin for its
    // fence. The map is cached anyway, so a miss can wait a few frames: the worker builds the queue for the box at the
    // camera of the kick, the old map stays sampled meanwhile, and the frame that finds it done swaps the queue in,
    // installs that box (SetLightVP) and draws. Everything the job reads -- s_Casters and the visuals in it -- is held
    // still by FarJobWait() before any change: EnsureCasterList rebuild, level unload, a forced/first redraw.
    RenderQueue       s_FarJobQueue;
    bool              s_farJobBusy = false;          // render thread: a job is queued or running
    std::atomic<bool> s_farJobDone{false};           // the worker: queue sorted, matrices final
    Fmatrix           s_farJobView, s_farJobVP;
    Fvector           s_farJobCam, s_farJobSun;
    u64               s_farJobKickUs = 0;
    u32               s_farJobFrames = 0;            // frames the old map was held for this job
    // ⚠08-10 v251: the redraw frame still costs the GPU ~20 ms (avg 7.4) once per sun step. Rasterising the queue over
    // 4 frames (tried, r_shadow_far_chunks) did NOT move it -- the 4.2k static casters are not that GPU cost. Unfound.

    PinnedWorker* FarWorker()
    {
        static PinnedWorker* w = new PinnedWorker(2, 0x3B); // leaked: no join at exit; background priority on core 2
        return w;
    }

    void FarJobWait()
    {
        if (!s_farJobBusy) return;
        FarWorker()->Wait();
        s_farJobBusy = false;
        s_farJobDone.store(false, std::memory_order_relaxed);
        s_FarJobQueue.Clear();
    }

    // The worker is done: its sorted queue becomes the far map's.
    void TakeFarJob(RenderQueue& into)
    {
        std::swap(into, s_FarJobQueue);
        into.SetSideEffects(true);   // the flag travelled with the swap
        s_FarJobQueue.Clear();
        s_farJobBusy = false;
        s_farJobDone.store(false, std::memory_order_relaxed);
        static u32 s_jobs = 0, s_heldMax = 0, s_total = 0;
        static u64 s_waitUsMax = 0;
        s_heldMax   = _max(s_heldMax, s_farJobFrames);
        s_waitUsMax = _max(s_waitUsMax, CPU::QPC_us() - s_farJobKickUs);
        ++s_total;
        if (++s_jobs >= 8 || s_total <= 16) {   // the first 16 one by one, then a line per 8
            Msg("[VK FarShadow] async: %u collects, last %u items; old map held up to %u frames (%.1f ms) before the draw",
                s_jobs, u32(into.Size()), s_heldMax, double(s_waitUsMax) / 1000.0);
            s_jobs = s_heldMax = 0; s_waitUsMax = 0;
        }
    }

    // ⚠ TRAP, PAID FOR ONCE — read before sampling the FAR sun map anywhere new.
    // Under VSM (and at night) its raster + combined copy are SKIPPED and the map
    // stays CLEARED, which reads as "fully lit" at every world point. That is fine
    // for the receivers (they read the VSM mask instead), but anything that samples
    // this map DIRECTLY gets a unanimous "in the sun" answer. The sun-shafts pass
    // did exactly that and painted warm additive rays through walls for months,
    // immune to every fix applied on the shadow side — because it never sampled a
    // shadow. That pass is gone (2026-07-24; god rays come from the froxel fog now),
    // which is why the `s_sunFarValid` flag it consulted is gone with it. A future
    // direct sampler must republish it: the condition is (!vsmActive && sunLit).

    // ---- RIGID dynamic sun casters -------------------------------------------------
    // Skinned_RenderShadow only ever draws SKELETONS: it walks s_uploads, which is built
    // by filtering DynamicVisuals() through dynamic_cast<CKinematics*> with non-empty
    // children. So a rigid visual has never cast a sun shadow in this renderer at all.
    //
    // In game that goes unnoticed, because rigid props are level geometry and reach the
    // sun maps through the static caster list instead. It becomes glaring the moment a
    // host drives the renderer with rigid models only — the editor, where every object of
    // a whole level arrives through DynamicVisuals() and nothing casts anything.
    //
    // Queue is static: reused every call, cleared per invocation. It is drawn from the
    // same three sites as the skinned overlay (far/combined map and each cascade).
    RenderQueue s_RigidDynQueue;
    RenderQueue s_RigidFullQueue;   // visuals the instanced path does not own

    void Rigid_RenderShadow(VkCommandBuffer cmd, const Fmatrix& lightVP, s32 cascade = -1)
    {
        if (DynamicVisuals().empty())
            return;

        s_RigidDynQueue.Clear();
        // Visuals the instanced path does NOT own still need their OPAQUE casters
        // drawn here, so they go to a second queue flushed without the AT-only
        // filter. Without this split their opaque leaves would fall between the two
        // paths and lose their shadows entirely.
        s_RigidFullQueue.Clear();

        for (const DynVisual& d : DynamicVisuals())
        {
            if (!d.vis)
                continue;

            // Skeletons belong to Skinned_RenderShadow — drawing them here too would
            // double-draw them in their BIND pose, as this path uploads no bones.
            if (Skinned_HandlesVisual(d.vis))
                continue;

            // Cull against the target this call is filling: a cascade box, or the
            // combined far map when the caller has no cascade.
            const Fsphere& bs = d.vis->vis.sphere;
            if (bs.R > 0.f)
            {
                Fvector c;
                d.xform.transform_tiny(c, bs.P);
                const bool visible = (cascade >= 0) ? ShadowMap::CascadeSphereVisible((u32)cascade, c, bs.R)
                                                    : ShadowMap::SphereVisible(c, bs.R);
                if (!visible)
                    continue;
            }

            d.vis->Submit(InstanceGPU::OwnsVisual(d.vis) ? s_RigidDynQueue : s_RigidFullQueue,
                          d.xform, 0.0f);
        }

        if (s_RigidDynQueue.Size() == 0 && s_RigidFullQueue.Size() == 0)
            return;

        s_RigidDynQueue.SortByKey();
        s_RigidFullQueue.SortByKey();
        // When the instanced GPU path owns the opaque leaves this queue draws only
        // the alpha-tested cutouts — the same split the static GPU-driven path
        // already uses ("opaque handled by GPU-driven shadow path",
        // vk_render_queue.cpp). Ready() is false until the set builds, so a failed
        // build falls back to drawing everything here rather than losing shadows.
        const bool atOnly = InstanceGPU::Ready();
        s_RigidDynQueue.FlushDepth(cmd, lightVP, false, atOnly);
        // Not owned by the instanced path → this queue draws them in full.
        s_RigidFullQueue.FlushDepth(cmd, lightVP);

        static bool s_diag = false;
        if (!s_diag)
        {
            s_diag = true;
            Msg("[VK Shadow] rigid dynamic casters: %zu draw item(s) from %zu dynamic visual(s); mode=%s",
                s_RigidDynQueue.Size(), DynamicVisuals().size(),
                atOnly ? "GPU-instanced opaque + CPU alpha-tested" : "full CPU");
            if (atOnly)
                Msg("[VK InstanceGPU] %u instance(s) in %u group(s) now GPU-driven per shadow target",
                    InstanceGPU::InstanceCount(), InstanceGPU::GroupCount());
        }
    }

    // A caster's Submit, but a hierarchy's children are tested one by one. 📏08-10-2026 Switch v240:
    // ShadowQ/casc 11-13 ms + sq.sort 3-4 ms per cascade, ShadowQ/far 14.5 + 4.7, on every sun step --
    // the same for the 25 m cascade and the big one: vkFHierrarhyVisual::Submit pushes ALL its children,
    // so a level hierarchy whose sphere touched the box put thousands of items in the queue (and the
    // sort). Only the shadow walks cull per child; the world pass keeps the stock Submit.
    template <class Visible>
    void SubmitCulled(vkRender_Visual* v, RenderQueue& q, const Fmatrix& xform, const Visible& visible)
    {
        if (v->Type == MT_HIERRARHY || v->Type == MT_LOD)   // vkFLOD submits its full-detail children, like a hierarchy
        {
            for (vkRender_Visual* c : static_cast<vkFHierrarhyVisual*>(v)->children)
                if (c && (c->vis.sphere.R <= 0.f || visible(c->vis.sphere)))
                    SubmitCulled(c, q, xform, visible);
            return;
        }
        v->Submit(q, xform, 0.0f);
    }

    void EnsureCasterList()
    {
        const size_t nVis      = RImplementation.Visuals.size();
        const bool   compacted = WorldGPU::PoolsCompacted();
        if (s_castersBuilt && s_castersVis == nVis && s_castersCompacted == compacted)
            return;
        FarJobWait();   // the far-map worker walks s_Casters
        s_castersBuilt     = true;
        s_castersVis       = nVis;
        s_castersCompacted = compacted;
        s_Casters.clear();
        s_RainATItems.clear();

        Fmatrix identity; identity.identity();
        RenderQueue probe;                 // scratch: "did Submit push anything?"
        size_t nProbe = 0;
        for (IRenderVisual* iv : RImplementation.Visuals) {
            if (!iv) continue;
            auto* rv = static_cast<vkRender_Visual*>(iv);
            rv->Submit(probe, identity, 0.0f);
            // Glass diverts to the queue's late list at Push — count it as a
            // caster too so the rebuild-time Push side effects stay identical.
            if (probe.Size() != nProbe || probe.HasGlass()) {
                s_Casters.push_back({ rv->vis.sphere, rv });
                // Capture the AT items THIS Submit pushed — same material
                // predicate FlushDepth uses to route to the AT depth pipeline.
                // The item keeps its OWN visual's (smaller) sphere for the
                // rain-window test; the parent caster radius feeds the dry-mode
                // clutter trim so filtering matches the old Submit-based walk.
                const auto& items = probe.Items();
                for (size_t k = nProbe; k < items.size(); ++k) {
                    auto* fv = static_cast<vkFVisual*>(items[k].vis);
                    const WorldMaterial* mat = (fv && fv->m_pWorldMaterial) ? fv->m_pWorldMaterial : nullptr;
                    if (mat && !mat->isWmark && mat->alphaRef >= 0.f)
                        s_RainATItems.push_back({ fv->vis.sphere, rv->vis.sphere.R, items[k] });
                }
                if (probe.HasGlass())      probe.ClearGlass();
                if (probe.HasWater())      probe.ClearWater();
                if (probe.Size() > 16384)  probe.Clear();
                nProbe = probe.Size();
            }
        }
        // Pre-sort once with SortByKey's exact comparator: a rebuild's filtered
        // copy preserves this order, so it can skip its own sort entirely.
        std::sort(s_RainATItems.begin(), s_RainATItems.end(),
                  [](const RainATItem& a, const RainATItem& b) { return a.item.sortKey < b.item.sortKey; });
        Msg("[VK Shadow] caster list: %zu casters (%zu AT items cached) of %zu visuals%s",
            s_Casters.size(), s_RainATItems.size(), nVis, compacted ? " (post-compaction)" : "");
    }
}

// rvk_loader.cpp level_Unload: the far-map worker reads the level's visuals -- it must be done before they go.
void SunShadow_WaitWorker()
{
    FarJobWait();
}

void Pass_SunShadow(FrameContext& ctx)
{
    if (ctx.cmd == VK_NULL_HANDLE) return;
    if (!ShadowMap::Init())        return;        // image/sampler not available
    if (PipelineCache::GetDepthLayout() == VK_NULL_HANDLE) return;  // caster shader missing

    VkCommandBuffer cmd = ctx.cmd;

    // Env sun direction (normalized for the cache comparison below).
    Fvector sunDir; sunDir.set(0.f, -1.f, 0.f);
    if (g_pGamePersistent)
        if (auto* E = ES().CurrentEnv) sunDir = E->sun_dir;
    if (sunDir.magnitude() < 1e-4f) sunDir.set(0.f, -1.f, 0.f);
    sunDir.normalize();

    // Fresh level: the sun-smoothing statics persist across loads, so a new
    // level's different sun would be HELD as a "transient" and swing into place
    // ~1.5 s later (and the cascade low-pass would lerp from the old level's
    // direction). Detect the load transition and snap the smoothing state so the
    // first frame adopts this level's sun directly.
    {
        const bool nowLoaded = RImplementation.b_loaded && !RImplementation.Visuals.empty();
        static bool s_wasLoaded = false;
        if (nowLoaded && !s_wasLoaded) {
            s_stableSunInit = false;
            s_cascSunInit   = false;
            s_castersBuilt  = false;   // new level → the caster list holds the old one's visuals
        }
        s_wasLoaded = nowLoaded;
    }

    // Hold the last real sun direction while a thunderbolt is flashing (it
    // transiently overwrites CurrentEnv->sun_dir — see s_stableSunDir notes).
    // Used by BOTH the far map cache check and the cascades below, so neither
    // swings during a strike.
    const bool boltActive = g_pGamePersistent && ES().IsThunderboltActive();
    if (!s_stableSunInit) {
        s_stableSunDir = sunDir; s_stableSunInit = true;
    } else if (!boltActive) {
        s_stableSunDir = sunDir;                          // no bolt → track the real sun exactly
    }
    sunDir = s_stableSunDir;                              // bolt → hold last real direction

    // NIGHT gate: with a black env sun every receiver multiplies the sun-shadow
    // term by sun_color = 0 and the fog's sun in-scatter is black too — nothing
    // can see the sun maps' CONTENT. Skip the sun raster/copy work exactly like
    // the vsmActive path below (transitions/clears/zones still run, so layouts
    // and the profiler's zone order stay valid). Measured: the per-frame cascade
    // combined rebuild alone was ~1.5-3.5 ms of pure waste at night. The dyn
    // spot/point (headlamps, campfires) and rain/deform maps are NOT sun work
    // and are unaffected.
    // r_sun_shadows 0 — тот же путь, что и ночь. Тир `r_shadows Off` гасит
    // только VSM и отдаёт солнце каскадам, у которых гейта не было: 18-09-2026
    // тени остались на экране при полностью «выключенных» тенях, а SunShadow
    // 1.85 мс оставалась самой дорогой зоной кадра. Переиспользуем ночной
    // гейт: клиры и переходы слоёв остаются, растр и копии пропускаются,
    // очищенная глубина (1.0) читается приёмником как «всё освещено».
    bool sunLit = (ps_r_sun_shadows != 0);
    if (sunLit && g_pGamePersistent)
        if (auto* E = ES().CurrentEnv)
            sunLit = (E->sun_color.x + E->sun_color.y + E->sun_color.z) > 1e-3f;

    // ⚠Переключение квара ОБЯЗАНО инвалидировать кэши. Иначе при выключении
    // `staticValid` остаётся истинным (ветка `!sunLit && !s_firstUse`), блок
    // перерисовки не входится, и в карте застывает ПОСЛЕДНИЙ дневной кадр —
    // тени не исчезнут, а замрут. Та же идиома, что у s_prevCascForVol ниже.
    {
        static int s_prevSunShadows = -1;
        if (s_prevSunShadows != ps_r_sun_shadows) {
            s_prevSunShadows    = ps_r_sun_shadows;
            s_cacheValid        = false;
            s_forceStaticRedraw = true;
            s_cascStaticFirst   = true;
            s_cascFirst         = true;
            for (u32 ci = 0; ci < ShadowMap::kNumSunCascades; ++ci) {
                s_cascStatic[ci].valid = false;
                s_cascQValid[ci]      = false;
            }
        }
    }

    const bool   loaded = RImplementation.b_loaded && !RImplementation.Visuals.empty();
    const size_t nVis   = loaded ? RImplementation.Visuals.size() : 0;

    // GPU-driven opaque sun casters: compute-cull + indirect draw replaces the
    // per-object CPU FlushDepth of OPAQUE statics in the three sun targets (far
    // map + 2 near cascades). The CPU queues then draw only the alpha-tested
    // cutout casters (atOnly). Falls back to the full CPU path when disabled or
    // when the GPU system has no casters / failed to build (so opaque shadows
    // never silently vanish).
    // Pool compaction: the GPU-set opaque casters' CPU slices are freed (their
    // per-mesh ShadowGPU snapshot excludes them too — it built after the swap),
    // so both the full-CPU fallback (r_gpu_shadows 0) and the per-mesh path
    // (r_shadow_cluster 0) would silently lose the freed statics. Force the
    // cluster path; r_pool_compact 0 + reload restores the A/Bs.
    const bool compacted  = WorldGPU::PoolsCompacted();
    const bool gpuShadows = (ps_r_gpu_shadows || compacted) && ShadowGPU::Built();
    // caster-LOD distance (0 disables → full detail everywhere). Distant casters
    // (esp. progressive terrain/big meshes) emit their coarse slice in the cull.
    const float shadowLodDist = ps_r_shadow_lod ? ps_r_shadow_lod_dist : 0.0f;
    // Phase 3: opaque statics in the sun targets via the cluster-LOD cut (the
    // WorldGPU DAG entries with a constant per-target world-error budget)
    // instead of the per-mesh ShadowGPU set. Cull AND draw switch on the same
    // flag each frame; the CPU AT cutout path is identical in both modes.
    const bool clShadows = gpuShadows && (ps_r_shadow_cluster || compacted) && WorldGPU::ShadowReady();
    // GPU AT casters (r_gpu_shadows_at): the cluster cull/draw covers the
    // alpha-tested cutouts too → the CPU caster queues (far/cascade/fog + the
    // rain AT replay + spot/point statics) aren't built OR flushed at all.
    // Any fallback (cvar off, no AT layout, per-mesh path) keeps the legacy
    // split: GPU opaques + CPU FlushDepth(alphaTestedOnly).
    const bool gpuAT = clShadows && WorldGPU::ShadowATActive();
    // HOST-PUSHED RIGID INSTANCES (the editor's entire scene). None of the paths
    // above can take them: they bake geometry into WORLD SPACE at level load and
    // carry no transform, while these are one shared mesh instanced many times
    // with a matrix each. vk_instance_gpu is that other shape — local geometry +
    // a per-instance transform SSBO + compute cull + indirect, so moving an
    // object costs 64 bytes instead of a re-bake.
    // Editor-only: in game the rigid dynamic set is small and the CPU path costs
    // nothing, so this stays out of the shipping frame. r_gpu_shadows doubles as
    // the kill switch. InstanceGPU::Ready() only turns true after CullShadow has
    // built the set, so the CPU path keeps the first frame and any failed build —
    // opaque rigid shadows can never silently vanish.
    const bool instShadows = ps_r_gpu_shadows && VKEditor::HostModelCount() > 0;
    { static bool s_atDiag = false;
      if (!s_atDiag && gpuAT) { s_atDiag = true;
          Msg("[VK Shadow] AT casters: GPU indirect (r_gpu_shadows_at 1) — CPU caster queues idle"); } }
    if (compacted && (!ps_r_gpu_shadows || (!ps_r_shadow_cluster && clShadows))) {
        static bool s_warned = false;   // nfs47: once (a preset that sets them would print it every 5 s forever)
        if (!s_warned) { s_warned = true;
            Msg("![VK Shadow] r_gpu_shadows/r_shadow_cluster 0 ignored: pools compacted (r_pool_compact 0 + reload for A/B)"); }
    }
    // Target texel size in metres from its ortho VP: ndc.x spans 2 over the
    // box width, X-Ray row-vector matrices put the world→ndc.x scale in column
    // 0 → width = 2/|col0|, texel = width/resolution.
    auto orthoTexel = [](const Fmatrix& vp, u32 size) -> float {
        const float sc = _sqrt(vp._11 * vp._11 + vp._21 * vp._21 + vp._31 * vp._31);
        return (sc > 1e-8f && size) ? 2.0f / (sc * (float)size) : 0.05f;
    };

    // VSM owns the SUN shadow once its screen-space mask is ready: every sun receiver
    // samples the mask instead of these cascade/far maps (vk_env_light shadow_params.w).
    // So skip the redundant sun RASTER (caster cull + opaque/tree draws + skinned overlay)
    // while VSM is live — the maps' transitions/clears/copies still run, leaving a valid
    // fully-lit map for the one remaining reader (the HUD weapon). Reclaims the measured
    // double-pay (~Combined+Casc0+Casc1; more during sun motion). NOTE: the rain/ground/
    // water-sim maps and the spot/point dynamic-light shadows (Shadow/Dyn) below are NOT
    // sun shadows → they are unaffected.
    const bool vsmActive = ps_r_vsm && VK::VSM::MaskReady();
    // Froxel volumetrics samples the sun per-froxel for in-air occlusion (god rays
    // through windows). It can read EITHER the cascade OR the VSM STATIC ATLAS directly
    // (vol_inject sampleVSMStatic, selected by gridParams.w==1 = atlas ready). When the
    // atlas IS the fog occluder, the near cascades are pure redundant fallback under
    // VSM → drop them and reclaim the ~2.2ms double-pay (the per-frame Casc0+Casc1
    // combined rebuild). The shader gracefully falls back to the cleared, fully-lit
    // cascade for the rare froxel with no resident VSM page. We keep the cascades alive
    // for the fog ONLY while the atlas is not ready yet.
    const bool fogUsesVSM = VK::VSM::AtlasReady();
    const bool cascForVol = (ps_r_vol || ps_r_vol_debug) && !fogUsesVSM;
    // ===== LEGACY / DEAD-END PATH (decided 12-08-2026): the classic cascades are no
    // longer the primary sun-shadow system (r_vsm defaults to 1) and are NOT to be
    // developed further — do not port caster LODs or other improvements here; that
    // work belongs on the VSM side.
    // ⚠ A stationary-camera A/B will show the cascades WINNING (they did, by 3.4 ms).
    // That comparison is biased and should not be used to argue for reverting: with a
    // still camera the cached static layer never refreshes, so its cost enters the
    // measurement as zero (`Sh/TreeCache0` = 0.00 avg / 0.53 max). The cascades pay in
    // SPIKES when the camera or sun moves (SunShadow avg 9.73 / max 15.19), which is
    // exactly what VSM's per-page refresh is designed to avoid. Measure while MOVING,
    // and compare tails, not averages.
    // Kept, and must keep working, for two reasons:
    //   1. VOLUMETRIC FOG FALLBACK. Until the VSM atlas is ready (level load, page
    //      thrash) the froxel pass has no occluder; `cascForVol` below keeps the
    //      cascades rasterizing for exactly that window, and the froxel shader falls
    //      back to the cleared/fully-lit cascade for any page that is not resident.
    //      (r_vol_shadow and its dedicated map were removed 12-08-2026.)
    //   2. r_vsm 0 remains a valid user/debug choice (and the only path on hardware
    //      where VSM misbehaves).
    // ⚠ Do NOT delete this path without first giving the fog its own warm-up
    // occluder. It is legacy, not dead. Its known weakness — no caster LOD for
    // trees, which measured 65% of the whole SunShadow budget — is precisely why
    // the default moved to VSM; see [[vulkan-tree-forward-no-lod-double-draw]].
    const bool cascRaster = (!vsmActive || cascForVol) && sunLit;   // render the cascade casters this frame
    // r_vol toggled WHILE under VSM: the near cascades were sitting idle (cleared /
    // never transitioned), so their cached-static state + image layouts are stale.
    // Force a fresh first-frame re-init so the froxel occlusion samples real depth.
    {
        static bool s_prevCascForVol = false;
        if (vsmActive && cascForVol != s_prevCascForVol) {
            s_cascStaticFirst = true; s_cascFirst = true;
            for (u32 ci = 0; ci < ShadowMap::kNumSunCascades; ++ci) {
                s_cascStatic[ci].valid = false; s_cascQValid[ci] = false;
            }
        }
        s_prevCascForVol = cascForVol;
    }
    // Live r_shadow_cluster flip: cull and draw read whichever path's buffers
    // are selected THIS frame, so the cached far/cascade maps must re-cull +
    // re-raster through the new path immediately (the other path's indirect
    // data is stale; its counts are zeroed at Build so a stray draw is empty,
    // not garbage — but the shadow would silently vanish until a cache miss).
    {
        static bool s_prevClShadows = false;
        if (clShadows != s_prevClShadows) {
            s_cacheValid = false;
            for (u32 ci = 0; ci < ShadowMap::kNumSunCascades; ++ci)
                s_cascStatic[ci].valid = false;
        }
        s_prevClShadows = clShadows;
    }
    // VSM hand-over: while VSM owns the sun, the far/cascade caster queues are
    // neither drawn nor collected (see the gates below) — so when VSM releases
    // (mask lost, r_vsm flipped) every cache must rebuild from scratch, and vice
    // versa a fresh grab must drop stale VSM-era validity stamps.
    {
        static bool s_prevVsmActive = false;
        if (vsmActive != s_prevVsmActive) {
            s_cacheValid = false;
            for (u32 ci = 0; ci < ShadowMap::kNumSunCascades; ++ci) {
                s_cascStatic[ci].valid = false;
                s_cascQValid[ci]      = false;
            }
        }
        s_prevVsmActive = vsmActive;
    }

    // (Bones for the skinned casters below were built by the PREPARE phase "Skin/bones" -- extract side.)

    const u32 sz = ShadowMap::Size();
    const VkExtent2D extShadow{ sz, sz };

    // ---- STATIC map (cached): redraw only when the camera/sun/level moved. ----
    // While valid we keep the cached lightVP (ComputeLightVP is NOT re-run), so
    // the sampling matrix always matches the cached contents.
    // At night the map is unread (sun_color = 0) → hold it as-is once the image
    // has a defined layout (first use still runs the clear/transition path).
    // Under live VSM the far map is as unread as at night (receivers sample the
    // VSM mask; only the HUD weapon reads the combined map, which stays fully
    // lit) — so skip the whole redraw path, NOT just the draws. Before this the
    // caster COLLECT below still ran on every 10 m cache miss: a fast demo
    // flight re-walked ~90k casters + queue-pushed the visible tens of
    // thousands every few frames for a map nobody sampled (54-68 ms/frame of
    // the flight CPU wall). The vsmActive-transition invalidation above forces
    // a real redraw the moment VSM hands the sun back.
    bool staticValid = !s_forceStaticRedraw
        && (((!sunLit || vsmActive) && !s_firstUse)
        || (s_cacheValid && !s_firstUse && nVis == s_lastVisCount
        && RV().vCameraPosition.distance_to_sqr(s_lastCamPos) < kRedrawDist * kRedrawDist
        && s_lastSunDir.dotproduct(sunDir) > kSunDotRedraw));

    // r_shadow_far_async (see s_FarJobQueue): a camera/sun miss kicks the collect on the worker and HOLDS the old map
    // until it is done; a forced / first / new-level redraw, or one the job cannot serve, goes the old way at once.
    bool farFromJob = false;   // this frame redraws from the job's queue
    {
        const bool canAsync = ps_r_shadow_far_async && loaded && sunLit && !vsmActive && !gpuAT
            && !s_forceStaticRedraw && !s_firstUse && s_cacheValid && nVis == s_lastVisCount;
        if (s_farJobBusy && !canAsync)
            FarJobWait();   // rare (knob, night, level change): the redraw below collects in place
        else if (s_farJobBusy) {
            if (s_farJobDone.load(std::memory_order_acquire)) { farFromJob = true; staticValid = false; }
            else { staticValid = true; ++s_farJobFrames; }
        }
        else if (!staticValid && canAsync) {
            EnsureCasterList();   // here, on the render thread; the job only reads it
            s_farJobCam = RV().vCameraPosition;
            s_farJobSun = sunDir;
            ShadowMap::BuildLightVP(sunDir, s_farJobCam, s_farJobView, s_farJobVP);
            s_farJobBusy   = true;
            s_farJobDone.store(false, std::memory_order_relaxed);
            s_farJobKickUs = CPU::QPC_us();
            s_farJobFrames = 0;
            s_FarJobQueue.SetSideEffects(false);
            FarWorker()->Run([] {
                s_FarJobQueue.Clear();
                const Fmatrix view = s_farJobView;
                Fmatrix identity; identity.identity();
                const auto visible = [&view](const Fsphere& s) { return ShadowMap::SphereVisibleIn(view, s.P, s.R); };
                for (const CasterEntry& e : s_Casters) {
                    if (e.bs.R > 0.f && !visible(e.bs)) continue;
                    SubmitCulled(e.rv, s_FarJobQueue, identity, visible);
                }
                s_FarJobQueue.SortByKey();
                s_farJobDone.store(true, std::memory_order_release);
            });
            staticValid = true;   // this frame keeps the old map
        }
    }

    // GPU-zone the far static-map REDRAW. Opened EVERY frame (even when the cache
    // holds → brackets no work → ~0 ms) so the profiler's open-order zone indices
    // never shift; a non-zero reading means the cache missed (camera/sun/level).
    NX_GPU_MARK(cmd, "sh.pre");   // [nx gpu slow]: everything of the frame before the far map
    const int zFar = VK::Prof::ZoneBegin(cmd, "Shadow/Far");
    // Why the cache missed, counted over 120 frames. 07-10 on the Switch the far map
    // was redrawn nearly every frame through a whole perf_route stop (Shadow/Far
    // 2.24 marks, ~3.7 ms) and nothing said which of the keys kept failing.
    {
        static u32 s_farFrames = 0, s_farN = 0, s_farVis = 0, s_farCam = 0, s_farSun = 0, s_farForce = 0;
        if (!staticValid) {
            ++s_farN;
            if (s_forceStaticRedraw || s_firstUse || !s_cacheValid) ++s_farForce;
            else {
                if (nVis != s_lastVisCount) ++s_farVis;
                if (RV().vCameraPosition.distance_to_sqr(s_lastCamPos) >= kRedrawDist * kRedrawDist) ++s_farCam;
                if (s_lastSunDir.dotproduct(sunDir) <= kSunDotRedraw) ++s_farSun;
            }
        }
        if (++s_farFrames >= 120) {
            if (s_farN > 2)
                Msg("[VK FarShadow] %u redraws / 120 frames: vis %u (now %u) cam %u sun %u force/first %u",
                    s_farN, s_farVis, u32(nVis), s_farCam, s_farSun, s_farForce);
            s_farFrames = s_farN = s_farVis = s_farCam = s_farSun = s_farForce = 0;
        }
    }
    s_farDrewThisFrame = !staticValid && !s_forceStaticRedraw && !s_firstUse && s_cacheValid && sunLit && !vsmActive && loaded;
    if (!staticValid)
    {
        if (farFromJob) {
            ShadowMap::SetLightVP(s_farJobView, s_farJobVP);   // the box the worker collected for
            s_lastCamPos = s_farJobCam;
            s_lastSunDir = s_farJobSun;
        } else {
            ShadowMap::ComputeLightVP(sunDir);
            s_lastCamPos = RV().vCameraPosition;
            s_lastSunDir = sunDir;
        }
        s_forceStaticRedraw = false;   // одноразовая: этот кадр и есть перерисовка
        // A night first-use pass leaves the cache INVALID: the map is cleared but
        // never drawn (sunLit gates below), so dawn must trigger a real redraw.
        s_cacheValid   = sunLit;
        s_lastVisCount = nVis;

        // Collect static casters (only when a level is loaded; otherwise we still
        // clear+transition so the map is a valid "fully lit" texture for receivers).
        // Visuals whose bounding sphere misses the light ortho box can't write any
        // shadow texel — cull them here instead of pushing the whole level.
        // night/VSM: draws below are skipped; gpuAT: the queue would only feed
        // FlushDepth(alphaTestedOnly) and the GPU path draws the AT casters now
        // → don't pay the visuals walk in either case.
        s_ShadowQueue.Clear();
        if (farFromJob)
            TakeFarJob(s_ShadowQueue);
        if (!farFromJob && loaded && sunLit && !vsmActive && !gpuAT) {
            VK_CPU_PROBE("ShadowQ/far");
            EnsureCasterList();
            Fmatrix identity; identity.identity();
            const auto visible = [](const Fsphere& s) { return ShadowMap::SphereVisible(s.P, s.R); };
            for (const CasterEntry& e : s_Casters) {
                if (e.bs.R > 0.f && !visible(e.bs)) continue;
                SubmitCulled(e.rv, s_ShadowQueue, identity, visible);
            }
            LoadProf::Scope _lpSort("sq.sort");
            s_ShadowQueue.SortByKey();
        }

        // GPU-driven opaque caster cull (compute) — MUST run before BeginRendering.
        if (gpuShadows && !vsmActive && sunLit) {
            if (clShadows) {
                const u32   tgt   = (u32)ShadowGPU::TGT_FAR;
                const float texel = orthoTexel(ShadowMap::GetLightVP(), ShadowMap::Size());
                WorldGPU::CullShadow(cmd, &tgt, &ShadowMap::GetLightVP(), &texel, 1);
            } else {
                Fvector4 planes[6];
                VK::ExtractFrustumPlanes(ShadowMap::GetLightVP(), planes);
                ShadowGPU::Target tgt = ShadowGPU::TGT_FAR;
                ShadowGPU::Cull(cmd, &tgt, planes, 1, RV().vCameraPosition, shadowLodDist);
            }
        }

        // Host rigid instances — an INDEPENDENT cull: ShadowGPU/WorldGPU never
        // build without a level, so this cannot ride their gpuShadows flag. Same
        // plane source, same "must precede BeginRendering" rule.
        if (instShadows && !vsmActive && sunLit) {
            Fvector4 planes[6];
            VK::ExtractFrustumPlanes(ShadowMap::GetLightVP(), planes);
            const InstanceGPU::Target tgt = InstanceGPU::TGT_FAR;
            InstanceGPU::CullShadow(cmd, &tgt, planes, 1);
        }

        // TREE casters, same rule and the same spot in the frame: a compute dispatch,
        // so it must precede BeginRendering. Gated on exactly the condition the
        // RenderDepth call below carries, or the slot would be filled for a map that
        // is not drawn (harmless but wasted) — or worse, left unfilled for one that is.
        if (ps_r_tree_shadow_gpu && ps_r_sun_far_trees && !vsmActive && sunLit
            && RImplementation.Trees && RImplementation.Trees->IsBuilt()) {
            VK::TreeShadowJob job{};
            job.target = VK::TREE_SH_FAR;
            job.vp     = ShadowMap::GetLightVP();
            if (ps_r_sun_far_tree_dist > 0.f) {   // 10-10: only the crowns this near the far map's centre
                job.maxDist = ps_r_sun_far_tree_dist;
                job.useEye  = true;
                job.eye     = s_lastCamPos;
            }
            job.lodScale = ps_r_tree_shadow_lod;
            RImplementation.Trees->DispatchShadowCull(cmd, &job, 1);
        }

        // TRANSFER_SRC (or UNDEFINED on first use) → DEPTH_ATTACHMENT for writing.
        const VkImageLayout oldLayout = s_firstUse ? VK_IMAGE_LAYOUT_UNDEFINED
                                                   : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        s_firstUse = false;
        ImageBarrier(cmd, ShadowMap::GetStaticImage(), oldLayout,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

        // BeginFlipped = the negative-height viewport (same X-Ray D3D→Vulkan Y-flip
        // as the scene passes, so the stored depth matches the camera-side
        // sampling convention) plus the full scissor.
        VK::RenderingBuilder(extShadow)
            .Depth(ShadowMap::GetStaticView(), VK_ATTACHMENT_LOAD_OP_CLEAR)
            .BeginFlipped(cmd);
        vkCmdSetDepthBias(cmd, kBiasConst, 0.0f, kBiasSlope);

        // Opaque statics via the GPU indirect path; the CPU queue draws only the
        // alpha-tested cutout casters (atOnly). Without the GPU path, full CPU flush.
        // Skipped under VSM (the map is left cleared/fully-lit — receivers use the mask).
        if (!vsmActive && sunLit) {
            if (!gpuAT)   // GPU AT: the cutouts ride the indirect draw below
                s_ShadowQueue.FlushDepth(cmd, ShadowMap::GetLightVP(), false, gpuShadows);
            if (gpuShadows) {
                if (clShadows) WorldGPU::DrawShadow(cmd, (u32)ShadowGPU::TGT_FAR, ShadowMap::GetLightVP());
                else           ShadowGPU::Draw(cmd, ShadowGPU::TGT_FAR, ShadowMap::GetLightVP());
            }
            NX_GPU_MARK(cmd, "sh.farStatics");   // (from here to sh.far = the trees)

            // Trees (GPU-driven elsewhere — not in the queue): alpha-tested leafy
            // crowns into the sun map, CPU-culled by the light box. Redraw-only cost. r_sun_far_trees 0: none (10-10).
            if (ps_r_sun_far_trees && RImplementation.Trees && RImplementation.Trees->IsBuilt())
                RImplementation.Trees->RenderDepth(cmd, ShadowMap::GetLightVP(), -1, nullptr,
                                                   0.0f, ps_r_sun_far_tree_dist > 0.f ? ps_r_sun_far_tree_dist : 1e9f, 0.0f, 0.0f, 0, false,
                                                   (s32)VK::TREE_SH_FAR, false, &s_lastCamPos,
                                                   VK::SunCfg::Get().cachedTreesRestPose ? 1 : 0);   // nfs47: cached -> rest pose
        }

        vkCmdEndRendering(cmd);

        // DEPTH_ATTACHMENT → TRANSFER_SRC: the static map is only ever copied from.
        ImageBarrier(cmd, ShadowMap::GetStaticImage(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    VK::Prof::ZoneEnd(cmd, zFar);
    NX_GPU_MARK(cmd, "sh.far");

    // ---- COMBINED map (sampled): every frame = static copy + dynamic casters. ----
    const int zComb = VK::Prof::ZoneBegin(cmd, "Shadow/Combined");
    // nfs43: profile without dynamic casters in the far map (r_sun_casc_lite: NPCs cast only into cascade 0) -> the combined
    // map changes only when the static one was redrawn: skip the 2048^2 copy (+ its two barriers) every other frame.
    const bool nxFarOverlay  = VK::SunCfg::Get().farDynamicCasters;
    const bool nxFarCombined = nxFarOverlay || !staticValid || s_combinedFirst;
    if (nxFarCombined) {
    ImageBarrier(cmd, ShadowMap::GetImage(),
                 s_combinedFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    s_combinedFirst = false;

    VkImageCopy copy{};
    copy.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
    copy.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
    copy.extent         = { sz, sz, 1 };
    if (!vsmActive && sunLit)   // under VSM/night nothing samples the far sun map → skip the 4096^2 copy
        vkCmdCopyImage(cmd, ShadowMap::GetStaticImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       ShadowMap::GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    ImageBarrier(cmd, ShadowMap::GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

    {
        // loadOp LOAD keeps the static copy. No copy (night / VSM / r_sun_shadows 0)
        // => CLEAR: the combined map is otherwise never written, and a boot straight
        // into r_sun_shadows 0 sampled UNDEFINED depth as "shadowed" (18-09-2026: a
        // ruler-straight lit/shadowed seam on the terrain along the cascade box edge).
        VK::RenderingBuilder(extShadow)
            .Depth(ShadowMap::GetView(), (!vsmActive && sunLit) ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                                : VK_ATTACHMENT_LOAD_OP_CLEAR)
            .BeginFlipped(cmd);
        vkCmdSetDepthBias(cmd, kBiasConst, 0.0f, kBiasSlope);

        if (!vsmActive && sunLit && nxFarOverlay)             // VSM live / night → no sun skinned overlay
        {
            if (!ps_r_shadow_lite) Skinned_RenderShadow(cmd, ShadowMap::GetLightVP());
            // Opaque rigid instances go GPU-driven when the set built; the CPU call
            // below then draws only the alpha-tested cutouts (it checks Ready()).
            InstanceGPU::DrawShadow(cmd, InstanceGPU::TGT_FAR, ShadowMap::GetLightVP());
            Rigid_RenderShadow(cmd, ShadowMap::GetLightVP());  // rigid dynamics — the editor's whole scene
        }

        vkCmdEndRendering(cmd);
    }

    // DEPTH_ATTACHMENT → SHADER_READ for the world/skinned receivers this frame.
    ImageBarrier(cmd, ShadowMap::GetImage(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }   // nfs43: nxFarCombined
    VK::Prof::ZoneEnd(cmd, zComb);
    NX_GPU_MARK(cmd, "sh.comb");

    // ---- RAIN occlusion map: top-down statics+trees depth, cached. Receivers
    // multiply wetness by it — geometry overhead (roof/tunnel) keeps a surface
    // dry. Rendered on demand: only while rain_density or wetness_factor is
    // non-zero (plus one initial clear so the bound image has a defined layout).
    // Zone also covers the per-frame water-sim dispatch below.
    const int zRain = VK::Prof::ZoneBegin(cmd, "Shadow/Rain");
    {
        float rainNeed = 0.f;
        if (g_pGamePersistent && ps_r_rain_enable) {
            const auto& env = ES();
            rainNeed = env.wetness_factor;
            if (env.CurrentEnv) rainNeed = _max(rainNeed, env.CurrentEnv->rain_density);
        }
        const bool wantRain   = rainNeed > 0.001f;
        // r_light_occ is the FOG's lamp occlusion now (vol_inject marches this map); the
        // surfaces leave buried lamps out by sector (vk_light.cpp), no map involved.
        const bool occWanted  = ps_r_light_occ != 0 && (ps_r_vol || ps_r_vol_debug);
        // Ground map (binding 13). Was water-sim only; the ripple POOL MASK needs
        // it too, and for a subtle reason: level water is a few huge flat sheets,
        // so "several puddles on a cellar floor" is one slab poking through uneven
        // ground. Only the floor height tells the puddles apart.
        const bool wantGround = ps_r_water_sim != 0
                             || (ps_r_wtr && ps_r_wtr_sim && ps_r_wtr_sim_pools && g_RenderQueue.HasWater());
        // The snow deform compute samples the RAIN map (binding 9 — the ground map came
        // out empty) for the snow-mesh base height AND the stamp ground-gate, so force
        // the rain map to render + stay fresh whenever deform is active.
        const bool wantDeform = ps_r_snow_deform_tex &&
            ((ps_r_snow_deform && ps_r_snow > 0.f) || ps_r_mud_deform > 0.f);   // mud prints need the field year-round
        // Water asks the same map whether it has sky overhead (r_wtr_shelter):
        // wind waves need wind, and a flooded cellar has none. Without this the
        // shelter query would read a stale map the moment someone turns
        // r_light_occ off, and the water would go back to surfing indoors.
        // The lean water FS (r_wtr_lean) has no shelter query, so it asks for nothing.
        const bool wantWater  = ps_r_wtr && !ps_r_wtr_lean && ps_r_wtr_shelter > 0.f && g_RenderQueue.HasWater();
        const bool wantMap    = wantRain || occWanted || wantGround || wantDeform || wantWater;
        // Occlusion-only (no rain) tolerates a much larger redraw step: the map covers
        // ±120 m and terrain is static, so redrawing every 48 m (vs 16 m for wetness)
        // cuts the always-on spike frequency 3× — fewer frame hitches that make the
        // sun cascades "tick".
        const float redrawDist = wantRain ? kRainRedrawDist : 48.0f;
        // Time floor on distance-triggered redraws: at walking speed 16 m ≈ every
        // 2-3 s (unchanged), but a fast demo flight crosses 16 m every 2-3 FRAMES —
        // each redraw a 30-50 ms caster re-collect for a ±75 m top-down map that
        // can't stay glued to a camera moving that fast anyway. ≥400 ms between
        // rebuilds bounds the flight cost to ~2 collects/s; invalid/level-change
        // rebuilds are exempt (correctness, not drift).
        static u32 s_rainLastMs = 0;
        const bool driftOk = RV().dwTimeGlobal - s_rainLastMs >= 400;
        const bool stale = !s_rainValid || nVis != s_rainVis
            || (driftOk && RV().vCameraPosition.distance_to_sqr(s_rainCamPos) > redrawDist * redrawDist);

        // Pool compaction: freed opaque statics only exist in the cluster pools —
        // draw them into the rain/ground maps via the kTargetRain cluster target
        // (the CPU queue keeps the alpha-tested cutouts, same split as the sun).
        const bool gpuRain = compacted && clShadows;

        if (s_rainFirst || (wantMap && stale))
        {
            ShadowMap::ComputeRainVP();

            s_RainQueue.Clear();
            if (loaded && wantMap) {
                VK_CPU_PROBE("ShadowQ/rain");
                // Occlusion-only (no rain) needs just the big blockers (terrain, walls,
                // floors) — skip small props with a much higher min radius so the
                // always-on redraw isn't the all-statics SPIKE that makes the frame
                // hitch ("sun ticks") every 16 m. Wetness still wants the small
                // occluders, so keep the tight cutoff while it's actually raining.
                const float minCasterR = wantRain ? kRainMinCasterR : 6.0f;
                if (gpuRain && gpuAT) {
                    // Fully GPU: opaque AND alpha-tested casters come from the
                    // kTargetRain cluster slice — no queue, no caster list.
                } else if (gpuRain) {
                    EnsureCasterList();
                    // GPU cluster path draws the opaque casters; the flushes below
                    // run alphaTestedOnly — so replay ONLY the cached AT items.
                    // The old walk re-Submitted whole MU hierarchies (188k items
                    // pushed, 8% drawn) + re-sorted them = the 48 ms rebuild spike,
                    // firing even in DRY weather via the occlusion/ground maps.
                    // The cache is pre-sorted; the filtered copy needs no sort.
                    for (const RainATItem& a : s_RainATItems) {
                        if (a.casterR > 0.f && a.casterR < minCasterR) continue;   // clutter trim (parent radius, as before)
                        if (a.bs.R > 0.f && !ShadowMap::RainSphereVisible(a.bs.P, a.bs.R)) continue;
                        s_RainQueue.PushPresorted(a.item);
                    }
                } else {
                    // CPU fallback draws opaque too — keep the full Submit walk.
                    EnsureCasterList();
                    Fmatrix identity; identity.identity();
                    for (const CasterEntry& e : s_Casters) {
                        const Fsphere& bs = e.bs;
                        if (bs.R > 0.f && bs.R < minCasterR) continue;   // skip clutter (spike trim)
                        if (bs.R > 0.f && !ShadowMap::RainSphereVisible(bs.P, bs.R)) continue;
                        e.rv->Submit(s_RainQueue, identity, 0.0f);
                    }
                    s_RainQueue.SortByKey();
                }
                static bool s_diagQ = false;
                if (!s_diagQ) { s_diagQ = true;
                    Msg("[VK Rain] rebuild: gpu=%d queue=%zu (AT cache %zu, casters %zu)",
                        (int)gpuRain, s_RainQueue.Size(), s_RainATItems.size(), s_Casters.size()); }
            }
            // Only mark the cache valid if we actually BUILT the queue (loaded). A
            // first render during level load (loaded=false → empty queue) must NOT
            // stick: otherwise, with nVis already full, the staleness check never
            // re-fires and the map stays empty until the camera moves 16 m — the
            // "occlusion does nothing" bug. Staying invalid re-renders next frame.
            s_rainValid  = wantMap && loaded;
            s_rainCamPos = RV().vCameraPosition;
            s_rainVis    = nVis;
            s_rainLastMs = RV().dwTimeGlobal;

            // Cluster cull for the rain VP (outside any render pass) — one cull
            // feeds both the rain map and the ground map below (same VP). The
            // draws below gate on the same (gpuRain && wantMap && loaded), so
            // the slice is never consumed stale (counts are zeroed at Build).
            if (gpuRain && wantMap && loaded) {
                const u32   tgt   = WorldGPU::kTargetRain;
                const float texel = orthoTexel(ShadowMap::GetRainVP(), ShadowMap::RainSize());
                WorldGPU::CullShadow(cmd, &tgt, &ShadowMap::GetRainVP(), &texel, 1);
            }

            const u32 rsz = ShadowMap::RainSize();
            const VkExtent2D extR{ rsz, rsz };

            // RAIN map (binding 9): wetness (statics+trees while raining) AND now the
            // dynamic-light occlusion (r_light_occ) — it samples THIS map (known-good
            // plumbing; the separate ground map came out empty). Trees only when
            // raining (a canopy must not occlude a lamp). Render when either wants it;
            // first frame clears once for a defined layout.
            // Tree cull for the rain map — before BeginRendering, and only when the
            // trees are actually drawn into it (wantRain: a canopy must not occlude a
            // lamp, so the map carries trees only while it is raining).
            // ⚠ THIS ALSO CORRECTS THE BOX. The CPU walk it replaces reached
            // RenderDepth with cascade -1 and no frustum, which selects
            // ShadowMap::SphereVisible — the FAR SUN box — while drawing through the
            // RAIN VP. The GPU path culls against the planes of the matrix it actually
            // draws with, so the set finally matches the map. That can only shrink the
            // set (anything it drops was outside the rain projection and was being
            // clipped by the rasterizer anyway), never add a missing caster.
            if (wantRain && ps_r_tree_shadow_gpu
                && RImplementation.Trees && RImplementation.Trees->IsBuilt()) {
                VK::TreeShadowJob job{};
                job.target = VK::TREE_SH_RAIN;
                job.vp     = ShadowMap::GetRainVP();
                RImplementation.Trees->DispatchShadowCull(cmd, &job, 1);
            }
            if (wantRain || occWanted || wantDeform || s_rainFirst) {
                ImageBarrier(cmd, ShadowMap::GetRainImage(),
                             s_rainFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

                VK::RenderingBuilder(extR)
                    .Depth(ShadowMap::GetRainView(), VK_ATTACHMENT_LOAD_OP_CLEAR)
                    .BeginFlipped(cmd);
                vkCmdSetDepthBias(cmd, kBiasConst, 0.0f, kBiasSlope);
                if (!(gpuRain && gpuAT))   // fully-GPU rain: AT rides the indirect draw
                    s_RainQueue.FlushDepth(cmd, ShadowMap::GetRainVP(), false, gpuRain);
                if (gpuRain && wantMap && loaded)
                    WorldGPU::DrawShadow(cmd, WorldGPU::kTargetRain, ShadowMap::GetRainVP());
                if (wantRain && RImplementation.Trees && RImplementation.Trees->IsBuilt())
                    RImplementation.Trees->RenderDepth(cmd, ShadowMap::GetRainVP(), -1, nullptr,
                                                       0.0f, 1e9f, 0.0f, 0.0f, 0, false,
                                                       (s32)VK::TREE_SH_RAIN);
                vkCmdEndRendering(cmd);
                ImageBarrier(cmd, ShadowMap::GetRainImage(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            }
            s_rainFirst = false;

            // Clean GROUND-height map (statics+terrain, NO trees): the dynamic-light
            // occlusion march (r_light_occ) AND the water flow sim sample it. Rendered
            // whenever either wants it (the leak fix needs it every frame), else cleared
            // once for a defined layout. Trees are EXCLUDED — a canopy must not block a lamp.
            static bool s_groundFirst = true;
            if (wantGround || s_groundFirst) {
                ImageBarrier(cmd, ShadowMap::GetGroundImage(),
                             s_groundFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
                s_groundFirst = false;
                { static bool s_gl = false; if (!s_gl && s_RainQueue.Size() > 0) { s_gl = true;
                    Msg("[VK Light] ground-height map rendered (occ=%d sim=%d, queue %u items)",
                        ps_r_light_occ, ps_r_water_sim, s_RainQueue.Size()); } }

                VK::RenderingBuilder(extR)
                    .Depth(ShadowMap::GetGroundView(), VK_ATTACHMENT_LOAD_OP_CLEAR)
                    .BeginFlipped(cmd);
                vkCmdSetDepthBias(cmd, kBiasConst, 0.0f, kBiasSlope);
                if (!(gpuRain && gpuAT))   // fully-GPU rain: AT rides the indirect draw
                    s_RainQueue.FlushDepth(cmd, ShadowMap::GetRainVP(), false, gpuRain);   // statics+terrain, NO trees
                if (gpuRain && wantMap && loaded)
                    WorldGPU::DrawShadow(cmd, WorldGPU::kTargetRain, ShadowMap::GetRainVP());
                vkCmdEndRendering(cmd);
                ImageBarrier(cmd, ShadowMap::GetGroundImage(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            }

            static bool s_diag = false;
            if (!s_diag && wantRain) { s_diag = true;
                Msg("[VK Rain] occlusion map drawn (%u items, need %.2f)", s_RainQueue.Size(), rainNeed); }
        }

        // ---- Water flow sim: advance the shallow-water field on the rain map
        // (top-down ground height = a height field). Drives geometric puddles +
        // the volumetric water render. Runs every frame while rain is enabled
        // (water keeps flowing and drains via evaporation after the rain stops).
        if (ps_r_rain_enable) {
            float rd = 0.f;
            if (g_pGamePersistent) {
                const auto& env = ES();
                if (env.CurrentEnv) rd = env.CurrentEnv->rain_density;
            }
            VK::WaterSim::Dispatch(cmd, rd);
        }

        // ---- Snow/mud deform texture: stamp foot contacts into the dense persistent
        // world-anchored press field (vk_deform). Read by the terrain shaders for
        // sharp, persistent, non-faceted footprints (snow prints + mud prints).
        // Own timed sub-zone (opened EVERY frame in fixed order — even when wantDeform is
        // false or the compute cache-gates — so it separates the deform cost from the rain
        // occlusion raster that shares this "Shadow/Rain" parent zone).
        const int zDef = VK::Prof::ZoneBegin(cmd, "Deform");
        if (wantDeform) {
            xr_vector<VK::Deform::Stamp> stamps;
            const Fvector& camp = RV().vCameraPosition;
            const float r = ps_r_snow_deform_radius;
            // PLAYER (first-person, no body skeleton): synthesize a STRIDE — one boot
            // print per ~0.68 m of horizontal travel, alternating left/right, oriented
            // along the movement. Standing still prints nothing (no trench, no pile-up).
            {
                static Fvector s_pCam{}; static bool s_pInit = false;
                static float s_stride = 0.f; static float s_side = 1.f;
                const float dx = camp.x - s_pCam.x, dz = camp.z - s_pCam.z;
                const float dmove = sqrtf(dx * dx + dz * dz);
                if (!s_pInit || dmove > 3.f) {                       // first frame / teleport
                    s_pInit = true; s_stride = 0.f;
                } else if (dmove > 1e-4f) {
                    s_stride += dmove;
                    if (s_stride >= 0.68f) {
                        s_stride = 0.f; s_side = -s_side;
                        const float inv = 1.f / dmove;
                        const float fx = dx * inv, fz = dz * inv;    // stride direction
                        const float px = -fz, pz = fx;               // perpendicular (foot offset)
                        // Foot ~at ground (≈1.65 m below the camera) so the compute's
                        // terrain ground-gate passes it.
                        stamps.push_back({ { camp.x + px * 0.13f * s_side, camp.y - 1.65f,
                                             camp.z + pz * 0.13f * s_side }, r, 1.f, fx, fz });
                    }
                }
                s_pCam = camp;
            }
            // Landed items/props: shallow ROUND print (~1/3 depth, smaller, no dir). The
            // COMPUTE ground-gates each stamp against the real terrain height, so flying
            // items don't stamp and it's robust on uneven terrain.
            xr_vector<Fvector> props; Skinned_CollectProps(props, 16);
            for (const Fvector& p : props) stamps.push_back({ p, r * 0.6f, 0.34f, 0.f, 0.f });
            // NPCs: DISCRETE footstep events (one per foot PLANT — separate boot prints,
            // not the per-frame trench), oriented by the body's facing.
            xr_vector<VK::Footstep> steps; Skinned_CollectFootsteps(steps, 48);
            for (const auto& f : steps) stamps.push_back({ f.pos, r, 1.f, f.dirX, f.dirZ });
            VK::Deform::Dispatch(cmd, stamps.data(), (u32)stamps.size());
        }
        VK::Prof::ZoneEnd(cmd, zDef);
    }
    VK::Prof::ZoneEnd(cmd, zRain);
    NX_GPU_MARK(cmd, "sh.rain");

    // ---- NEAR cascades (R4 port): static/combined split like the far map. The
    // statics-only depth is CACHED (re-rastered only when the camera/sun/level
    // moved past kCascRedrawDist — see Phase A); every frame the sampled combined
    // map = copy(static)+skinned overlay. Stability vs the sun creep still comes
    // from the world-anchored texel alignment inside ComputeCascadeVP.
    {
        if (!s_cascSunInit) {
            s_cascSunDir  = sunDir;
            s_cascSunInit = true;
        } else if (s_cascSunDir.dotproduct(sunDir) < kCascSnapDot) {
            s_cascSunDir = sunDir;                         // load / time-skip discontinuity → snap, don't slew
        } else {
            const float k = 1.f - expf(-RV().fTimeDelta / kCascSunTau);
            s_cascSunDir.lerp(s_cascSunDir, sunDir, k);
            if (s_cascSunDir.magnitude() > 1e-4f) s_cascSunDir.normalize();
            else                                  s_cascSunDir = sunDir;
        }

        // Phase A: pick the cascades whose STATIC map must be re-rastered this
        // frame (camera/sun/level moved past the cache thresholds), recompute ONLY
        // those VPs (the rest stay FROZEN so sampling matches the cached depth),
        // rebuild their caster queues, and batch-cull them in ONE compute pass
        // (so the depth raster never interleaves with compute).
        bool              redrawStatic[ShadowMap::kNumSunCascades] = {};
        ShadowGPU::Target cullTgts[ShadowMap::kNumSunCascades];
        Fvector4          cullPlanes[ShadowMap::kNumSunCascades * 6];
        // Same batch, own enum: InstanceGPU::Target mirrors ShadowGPU::Target's
        // values, but aliasing one array as the other would be a reinterpret_cast
        // across unrelated types — cheap to just fill both.
        InstanceGPU::Target instTgts[ShadowMap::kNumSunCascades];
        u32               clTgts[ShadowMap::kNumSunCascades];      // cluster path: target ids
        Fmatrix           clVPs[ShadowMap::kNumSunCascades];       // + per-target VP
        float             clTexel[ShadowMap::kNumSunCascades];     // + per-target texel (m)
        u32 nCull = 0;
        // Sun-rotation redraw threshold: a frozen cascade re-rasters once the sun
        // creeps past r_shadow_casc_sun degrees so shadows keep moving with the sun.
        // Measured as distance-of-unit-vectors² (≈ θ²) — far better float precision
        // near 0 than a cos dot-product (cos(0.05°) is below float32 epsilon at 1.0).
        // Cheap amortized: ~one 3 ms redraw per second of sun motion. 0 master-toggle
        // (r_shadow_casc_cache) forces a full per-frame raster (old smooth behaviour).
        const float sunEps   = deg2rad(ps_r_shadow_casc_sun);
        const float sunEpsSq = sunEps * sunEps;
        for (u32 ci = 0; ci < ShadowMap::kNumSunCascades; ++ci)
        {
            // Night (== !sunLit): no redraw at all past the first-use init — the
            // cascades are unread. Validity is NOT stamped, so dawn re-rasters.
            // cascRaster (== sunLit && cascades actually sampled) instead of plain
            // sunLit: while VSM owns the sun AND the froxel fog reads the VSM atlas,
            // the cascades are pure fallback — before this gate every 2-4 m of
            // camera drift still re-walked the caster list + rebuilt both cascade
            // queues for maps whose draws cascRaster then skipped anyway (a steady
            // slice of the fast-flight CPU wall). Night semantics unchanged.
            CascStaticCache& sc = s_cascStatic[ci];
            // nfs47: the same test split by reason, for the [nx shadow] counter.
            const bool rFirst = s_cascStaticFirst || (cascRaster && (!ps_r_shadow_casc_cache || !sc.valid));
            const bool rVis   = cascRaster && nVis != sc.vis;
            const bool rDist  = cascRaster && RV().vCameraPosition.distance_to_sqr(sc.camPos) > kCascRedrawDist[ci] * kCascRedrawDist[ci];
            const bool rSun   = cascRaster && sc.sunDir.distance_to_sqr(sunDir) > sunEpsSq;
            redrawStatic[ci] = rFirst || rVis || rDist || rSun;
            // nfs48: one cache re-raster per frame -- the spike is per cascade, not the sum. The later cascade waits;
            // its trigger still holds next frame. Cached trees are in the rest pose, so a frame of delay shows nothing.
            if (VK::SunCfg::Get().staggerStaticRedraw && ci > 0 && redrawStatic[ci] && redrawStatic[0] && !rFirst) {
                redrawStatic[ci] = false;
                ++s_nxDeferred;
            }
            // 10-10: nor in the frame the far map was re-rastered (s_farDrewThisFrame)
            if (VK::SunCfg::Get().staggerStaticRedraw && redrawStatic[ci] && s_farDrewThisFrame && !rFirst) {
                redrawStatic[ci] = false;
                ++s_nxDeferred;
            }
#ifdef __SWITCH__
            if (redrawStatic[ci]) {
                const u32 why = rFirst ? 0u : rVis ? 3u : rDist ? 1u : 2u;
                ++s_nxRedrawWhy[ci][why];
            }
#endif
            if (!redrawStatic[ci])
                continue;   // static cached → VP frozen, no queue/cull/raster; only the combined copy+skinned runs in Phase B

            sc.valid  = sunLit;
            sc.camPos = RV().vCameraPosition;
            sc.sunDir = sunDir;
            sc.vis    = nVis;
            ShadowMap::ComputeCascadeVP(ci, s_cascSunDir);

            // Cached caster queue: the visuals walk is the expensive part —
            // only redo it when the camera/sun drifted past the slack the
            // queue was collected with (kCascQueueInflate).
            const bool qValid = s_cascQValid[ci] && nVis == s_cascQVis[ci]
                && RV().vCameraPosition.distance_to_sqr(s_cascQCamPos[ci]) < kCascQueueDist * kCascQueueDist
                && s_cascQSunDir[ci].dotproduct(sunDir) > kCascQueueSunDot;
            if (!qValid && sunLit && !gpuAT)   // night/gpuAT: the queue isn't flushed → skip the walk too
            {
                s_cascQValid[ci]  = true;
                s_cascQCamPos[ci] = RV().vCameraPosition;
                s_cascQSunDir[ci] = sunDir;
                s_cascQVis[ci]    = nVis;
                s_CascQueue[ci].Clear();
                if (loaded) {
                    VK_CPU_PROBE("ShadowQ/casc");
                    EnsureCasterList();
                    Fmatrix identity; identity.identity();
                    const auto visible = [ci](const Fsphere& s) { return ShadowMap::CascadeSphereVisible(ci, s.P, s.R, kCascQueueInflate); };
                    for (const CasterEntry& e : s_Casters) {
                        if (e.bs.R > 0.f && !visible(e.bs)) continue;
                        SubmitCulled(e.rv, s_CascQueue[ci], identity, visible);
                    }
                    LoadProf::Scope _lpSort("sq.sort"); // [rt prof]: the queue walk above vs this sort
                    s_CascQueue[ci].SortByKey();
                }
            }

            if (gpuShadows || instShadows) {
                VK::ExtractFrustumPlanes(ShadowMap::GetCascadeCullVP(ci), &cullPlanes[nCull * 6]);   // nfs47: short column
                cullTgts[nCull] = (ShadowGPU::Target)(ShadowGPU::TGT_CASCADE0 + ci);
                instTgts[nCull] = (InstanceGPU::Target)(InstanceGPU::TGT_CASCADE0 + ci);
                clTgts[nCull]   = (u32)(ShadowGPU::TGT_CASCADE0 + ci);
                clVPs[nCull]    = ShadowMap::GetCascadeCullVP(ci);   // nfs47: planes only; x/y (texel) identical
                clTexel[nCull]  = orthoTexel(ShadowMap::GetCascadeVP(ci), ShadowMap::CascadeSize(ci));
                ++nCull;
            }
        }
        // nfs48: which live layers rebuild this frame. nxTrees = the trees/grass (+ rigid on unsplit cascades) layer,
        // nxComb = the sampled map. A cascade in SunCfg::kNpcLayerMask has a MID image between them: its combined map is
        // copy(mid) + NPCs/rigid at r_sun_cad_npc, the mid copy(static) + trees at r_sun_cad_c0. Cascade 1 rebuilds one
        // live layer at r_sun_cad_c1. A cache redraw or first use forces every layer of that cascade.
        bool nxComb[ShadowMap::kNumSunCascades], nxTrees[ShadowMap::kNumSunCascades];
        {
            // 10-09: 0 = the live layer rebuilds only together with its static cache (camera/sun moved past the cache
            // thresholds) -- the whole cascade is a cache; trees cast from the rest pose, moved props from where they were.
            s_cadNpc = (u32)_max(0, ps_r_sun_cad_npc);
            s_cadC0  = (u32)_max(0, ps_r_sun_cad_c0);
            s_cadC1  = (u32)_max(0, ps_r_sun_cad_c1);
            const u32 f = RV().frameIndex;
            auto due = [](u32 cad, u32 phase) { return cad == 1u || (cad > 1u && (phase % cad) == 0u); };
            for (u32 ci = 0; ci < ShadowMap::kNumSunCascades; ++ci) {
                const bool split = ((VK::SunCfg::Get().npcLayerMask >> ci) & 1u) != 0u;
                const u32  cadT  = (ci == 0) ? s_cadC0 : s_cadC1;
                const bool force = redrawStatic[ci] || s_cascFirst || (split && s_cascMidFirst);
                nxTrees[ci] = force || due(cadT, f + ci);
                nxComb[ci]  = split ? (nxTrees[ci] || due(s_cadNpc, f)) : nxTrees[ci];
            }
#ifdef __SWITCH__
            {   // nfs45/47/48 instrument: rebuilds per 300 frames, why the caches redrew, how far the camera went.
                static u32 s_frames = 0, s_reb[ShadowMap::kNumSunCascades] = {}, s_tre[ShadowMap::kNumSunCascades] = {}, s_static[ShadowMap::kNumSunCascades] = {};
                static float s_walk = 0.f; static Fvector s_prevCam = RV().vCameraPosition;
                s_walk += _min(RV().vCameraPosition.distance_to(s_prevCam), 50.f);   // teleports clipped
                s_prevCam = RV().vCameraPosition;
                for (u32 ci = 0; ci < ShadowMap::kNumSunCascades; ++ci) {
                    s_reb[ci] += nxComb[ci] ? 1u : 0u; s_tre[ci] += nxTrees[ci] ? 1u : 0u; s_static[ci] += redrawStatic[ci] ? 1u : 0u;
                }
                if (++s_frames >= 300u) {
                    Msg("[nx shadow] per 300 frames: c0 npc %u trees %u | c1 %u | static redraws c0 %u c1 %u (c1 deferred %u)"
                        " | why c0 first %u dist %u sun %u vis %u | c1 first %u dist %u sun %u vis %u"
                        " | walked %.1f m | cad npc %u c0 %u c1 %u | wind_dist %.0f lod %.2f (frame %u)",
                        s_reb[0], s_tre[0], s_tre[1], s_static[0], s_static[1], s_nxDeferred,
                        s_nxRedrawWhy[0][0], s_nxRedrawWhy[0][1], s_nxRedrawWhy[0][2], s_nxRedrawWhy[0][3],
                        s_nxRedrawWhy[1][0], s_nxRedrawWhy[1][1], s_nxRedrawWhy[1][2], s_nxRedrawWhy[1][3],
                        s_walk, s_cadNpc, s_cadC0, s_cadC1, ps_r_wind_shadow_dist, ps_r_tree_shadow_lod, RV().frameIndex);
                    s_frames = 0; s_walk = 0.f; s_nxDeferred = 0;
                    for (u32 ci = 0; ci < ShadowMap::kNumSunCascades; ++ci) {
                        s_reb[ci] = 0; s_tre[ci] = 0; s_static[ci] = 0;
                        for (u32 w = 0; w < 4; ++w) s_nxRedrawWhy[ci][w] = 0;
                    }
                }
            }
#endif
        }
        const int zCull = VK::Prof::ZoneBegin(cmd, "Shadow/CascCull");
        if (gpuShadows && nCull && cascRaster) {
            LoadProf::Scope _lp("sh.casc_cull"); // [rt prof]
            if (clShadows) WorldGPU::CullShadow(cmd, clTgts, clVPs, clTexel, nCull);
            else           ShadowGPU::Cull(cmd, cullTgts, cullPlanes, nCull, RV().vCameraPosition, shadowLodDist);
        }
        // Host rigid instances: one batched cull for every cascade, same as above.
        if (instShadows && nCull && cascRaster)
            InstanceGPU::CullShadow(cmd, instTgts, cullPlanes, nCull);

        // TREE casters for both cascades and BOTH LAYERS, in one batch (r_tree_shadow_gpu).
        // This is where the five CPU walks went: Phase B draws each layer inside a render
        // pass, so every dispatch that feeds it has to be recorded here, before the loop.
        //
        // ⚠ FOUR SLOTS, NOT TWO. Per cascade the CACHED static layer (far trees, redrawn
        // only on a cache miss) and the PER-FRAME dynamic layer (near crowns at this
        // frame's wind pose) hold disjoint sets and land in different images. They are
        // complementary halves of one camera-distance split, so both bands are pushed
        // from the same ps_r_wind_shadow_dist — a tree leaving one must enter the other
        // on the same comparison or its shadow blinks out for a frame.
        // ⚠ The STATIC layer is queued only when its cache actually misses. Culling it
        // every frame would be ~a third of the work back for a map nobody redraws.
        // ⚠ The VP for a cache-HIT cascade is frozen, which is exactly what the dyn
        // layer wants: it draws into a map built with that same frozen VP.
        if (ps_r_tree_shadow_gpu && cascRaster
            && RImplementation.Trees && RImplementation.Trees->IsBuilt())
        {
            // The slot ids are laid out as [casc0 static, casc1 static, casc0 dyn, casc1
            // dyn], so `BASE + ci` addressing is only valid while there are exactly two
            // cascades. Adding a third without extending TreeShadowTarget would silently
            // alias cascade 2's static slot onto cascade 0's dynamic one.
            static_assert(ShadowMap::kNumSunCascades == 2, "TreeShadowTarget assumes 2 sun cascades");
            VK::TreeShadowJob tjobs[ShadowMap::kNumSunCascades * 2];
            u32 nT = 0;
            for (u32 ci = 0; ci < ShadowMap::kNumSunCascades; ++ci) {
                // nfs47: both bands cut around the static cache's camera (s_cascStatic[ci].camPos, just updated if this
                // frame redraws) -- complementary by construction; planes from the short cull column.
                if (redrawStatic[ci]) {
                    VK::TreeShadowJob& j = tjobs[nT++];
                    j.target  = VK::TREE_SH_CASC0_STATIC + ci;
                    j.lodScale = ps_r_tree_shadow_lod;   // nfs48: sliding-window LOD for the cached crowns
                    j.vp      = ShadowMap::GetCascadeCullVP(ci);
                    j.minDist = ps_r_wind_shadow_dist;   // 0 => the whole forest is cached
                    j.maxDist = 1e9f;
                    j.useEye  = true; j.eye = s_cascStatic[ci].camPos;
                }
                if (ps_r_wind_shadow_dist > 0.f && nxTrees[ci]) {   // nfs48: live trees only on their rebuild frames
                    VK::TreeShadowJob& j = tjobs[nT++];
                    j.target  = VK::TREE_SH_CASC0_DYN + ci;
                    j.vp      = ShadowMap::GetCascadeCullVP(ci);
                    j.minDist = 0.0f;
                    j.maxDist = ps_r_wind_shadow_dist;
                    j.useEye  = true; j.eye = s_cascStatic[ci].camPos;
                }
            }
            if (nT) { LoadProf::Scope _lp("sh.tree_cull"); RImplementation.Trees->DispatchShadowCull(cmd, tjobs, nT); }
        }
        VK::Prof::ZoneEnd(cmd, zCull);
        NX_GPU_MARK(cmd, "sh.cascCull");

        // Phase B: per cascade — (re-)raster the STATIC map only on a cache miss,
        // then EVERY frame build the sampled COMBINED map = copy(static) + skinned
        // dynamics on top (NPCs move, so they can't be cached). The per-cascade
        // zone is opened EVERY frame regardless (profiler matches by open index).
        for (u32 ci = 0; ci < ShadowMap::kNumSunCascades; ++ci)
        {
            const int zCasc = VK::Prof::ZoneBegin(cmd, ci == 0 ? "Shadow/Casc0" : "Shadow/Casc1");
            const u32 nsz = ShadowMap::CascadeSize(ci);
            const VkExtent2D extC{ nsz, nsz };

            // --- STATIC map (cached): statics + opaque casters + trees. Only on a miss.
            if (redrawStatic[ci])
            {
                // [rt prof] 📏08-10-2026 Switch v237: a sun-step redraw of both cascades = SunShadow 16-22 ms of the record, twice
                LoadProf::Scope _lp(ci == 0 ? "sh.casc0_static" : "sh.casc1_static");
                ImageBarrier(cmd, ShadowMap::GetCascadeStaticImage(ci),
                             s_cascStaticFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

                VK::RenderingBuilder(extC)
                    .Depth(ShadowMap::GetCascadeStaticView(ci), VK_ATTACHMENT_LOAD_OP_CLEAR)
                    .BeginFlipped(cmd);
                vkCmdSetDepthBias(cmd, kBiasConst, 0.0f, kBiasSlope);

                if (cascRaster) {   // VSM live → normally cleared/fully-lit, but kept rendered for r_vol froxel occlusion
                    if (!gpuAT)   // GPU AT: the cutouts ride the indirect draw below
                        s_CascQueue[ci].FlushDepth(cmd, ShadowMap::GetCascadeVP(ci), false, gpuShadows);
                    if (gpuShadows) {
                        if (clShadows) WorldGPU::DrawShadow(cmd, (u32)(ShadowGPU::TGT_CASCADE0 + ci), ShadowMap::GetCascadeVP(ci));
                        else           ShadowGPU::Draw(cmd, (ShadowGPU::Target)(ShadowGPU::TGT_CASCADE0 + ci), ShadowMap::GetCascadeVP(ci));
                    }
                    if (RImplementation.Trees && RImplementation.Trees->IsBuilt()) {
                        // Static (cached) layer = FAR trees only; near trees sway in
                        // the per-frame dynamic overlay below (r_wind_shadow_dist;
                        // 0 → minDist 0 → all trees cached, original behaviour).
                        // Own zone: this is the CACHE-REFILL cost (only on redraw), a
                        // different beast from the per-frame dynamic layer below, and
                        // Shadow/CascN lumps the two together.
                        const int zTC = VK::Prof::ZoneBegin(cmd, ci == 0 ? "Sh/TreeCache0" : "Sh/TreeCache1");
                        RImplementation.Trees->RenderDepth(cmd, ShadowMap::GetCascadeVP(ci), (s32)ci,
                                                           nullptr, ps_r_wind_shadow_dist, 1e9f,
                                                           0.0f, 0.0f, 0, false,
                                                           (s32)(VK::TREE_SH_CASC0_STATIC + ci), false,
                                                           &s_cascStatic[ci].camPos,
                                                           VK::SunCfg::Get().cachedTreesRestPose ? 1 : 0);   // nfs47
                        VK::Prof::ZoneEnd(cmd, zTC);
                    }
                }

                vkCmdEndRendering(cmd);

                ImageBarrier(cmd, ShadowMap::GetCascadeStaticImage(ci), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            }

            // nfs48: the live layers. Split cascade (SunCfg::kNpcLayerMask): MID = copy(static) + trees/grass at r_sun_cad_c0,
            // COMBINED = copy(mid) + NPCs/rigid at r_sun_cad_npc. Otherwise COMBINED = copy(static) + everything. A layer not
            // due keeps its last contents (MID in TRANSFER_SRC, COMBINED in SHADER_READ).
            const bool split = ((VK::SunCfg::Get().npcLayerMask >> ci) & 1u) != 0u;
            VkImageCopy copy{};
            copy.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
            copy.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
            copy.extent         = { nsz, nsz, 1 };
            // NEAR trees at their wind pose (the far forest sits in the cache) + grass casters. Flutter only on a layer
            // rebuilt at >= 30 Hz: slower than that the leaves visibly step (nfs45).
            auto drawTreeLayer = [&]()
            {
                if (ps_r_wind_shadow_dist > 0.f && RImplementation.Trees && RImplementation.Trees->IsBuilt()) {
                    const u32 cadT = (ci == 0) ? s_cadC0 : s_cadC1;
                    const int zTD = VK::Prof::ZoneBegin(cmd, ci == 0 ? "Sh/TreeDyn0" : "Sh/TreeDyn1");
                    RImplementation.Trees->RenderDepth(cmd, ShadowMap::GetCascadeVP(ci), (s32)ci,
                                                       nullptr, 0.0f, ps_r_wind_shadow_dist,
                                                       0.0f, 0.0f, 0, false,
                                                       (s32)(VK::TREE_SH_CASC0_DYN + ci), false,
                                                       &s_cascStatic[ci].camPos,
                                                       (cadT == 0u || cadT > 2u) ? 2 : 0);   // 0 = with the cache only: no flutter
                    VK::Prof::ZoneEnd(cmd, zTD);
                }
                // GRASS casters (r_sun_grass): skip casc1 when the reach fits inside casc0's half-box.
                if (ps_r_sun_grass && (ci == 0 || ps_r_sun_grass_dist > 12.f))
                    DrawGrassSpotCasters(cmd, ShadowMap::GetCascadeVP(ci),
                                         RV().vCameraPosition, ps_r_sun_grass_dist);
            };

            if (split && nxTrees[ci]) {
                VkImage mid = ShadowMap::GetCascadeMidImage(ci);
                // Same first-use rule as the combined map (s_cascFirst is what the shadow toggles / level reset raise).
                ImageBarrier(cmd, mid, (s_cascMidFirst || s_cascFirst) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
                if (cascRaster)
                    vkCmdCopyImage(cmd, ShadowMap::GetCascadeStaticImage(ci), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   mid, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                ImageBarrier(cmd, mid, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
                VK::RenderingBuilder(extC)
                    .Depth(ShadowMap::GetCascadeMidView(ci), cascRaster ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                                        : VK_ATTACHMENT_LOAD_OP_CLEAR)
                    .BeginFlipped(cmd);
                vkCmdSetDepthBias(cmd, kBiasConst, 0.0f, kBiasSlope);
                if (cascRaster) drawTreeLayer();
                vkCmdEndRendering(cmd);
                ImageBarrier(cmd, mid, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
                s_cascMidFirst = false;
            }

            if (nxComb[ci]) {
            ImageBarrier(cmd, ShadowMap::GetCascadeImage(ci),
                         s_cascFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            if (cascRaster)   // copy mid (split) or cached static -> sampled combined (skipped only when truly VSM-only)
                vkCmdCopyImage(cmd, split ? ShadowMap::GetCascadeMidImage(ci) : ShadowMap::GetCascadeStaticImage(ci),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               ShadowMap::GetCascadeImage(ci), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            ImageBarrier(cmd, ShadowMap::GetCascadeImage(ci), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

            // loadOp LOAD keeps the copy; no copy => CLEAR (see the far map above).
            VK::RenderingBuilder(extC)
                .Depth(ShadowMap::GetCascadeView(ci), cascRaster ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                                 : VK_ATTACHMENT_LOAD_OP_CLEAR)
                .BeginFlipped(cmd);
            vkCmdSetDepthBias(cmd, kBiasConst, 0.0f, kBiasSlope);
            if (cascRaster) {                                // skinned (NPC) overlay → they also cast volumetric shadows
                if (!ps_r_shadow_lite && (VK::SunCfg::Get().skinnedCascadeMask & (1u << ci)))   // nfs47 cfg (Switch: cascade 0 only)
                    Skinned_RenderShadow(cmd, ShadowMap::GetCascadeVP(ci));
                // Opaque rigid instances GPU-driven; the CPU call keeps the cutouts. Rigid dynamics include the weapons in
                // the NPCs' hands, so they ride the NPC rate.
                InstanceGPU::DrawShadow(cmd, (InstanceGPU::Target)(InstanceGPU::TGT_CASCADE0 + ci),
                                        ShadowMap::GetCascadeVP(ci));
                Rigid_RenderShadow(cmd, ShadowMap::GetCascadeVP(ci), (s32)ci);  // rigid dynamics
                if (!split) drawTreeLayer();
            }
            vkCmdEndRendering(cmd);

            ImageBarrier(cmd, ShadowMap::GetCascadeImage(ci), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            }   // nfs48: nxComb
            VK::Prof::ZoneEnd(cmd, zCasc);
            NX_GPU_MARK(cmd, "sh.casc");
        }
        s_cascStaticFirst = false;
        s_cascFirst = false;
    }

    // (The dedicated per-frame fog sun-shadow map lived here — REMOVED 12-08-2026.
    // It was built to fix "trembling shafts", but the real bug turned out to be the
    // temporal reprojection, fixed in vol_inject; the map has been dead code behind
    // r_vol_shadow 0 ever since. Removed together with the vk_shadow GetFogShadow*/
    // ComputeFogShadowVP API, the vk_volumetrics binding 10 + fog_shadow_vp UBO field,
    // vol_inject's sampleFogShadow, and the gridParams.w == 2 mode. The froxel pass
    // takes its sun occlusion from the VSM atlas, falling back to the cascades.)

    // ===== Dynamic light shadows (STEP 3b): spot POOL + 1 point cube per frame =====
    // EnvLight::Update (Pass_World, later this frame) reads the same CollectFrame
    // result, so the gpu[] indices below match what the receivers see.
    const auto& FL = Lights::CollectFrame(RV().vCameraPosition);
    const bool haveSpots  = FL.poolCount  > 0;
    const bool havePoints = FL.poolCountP > 0;

    // Level unload: tile/cube owners are dangling identities of freed lights and
    // the cached queues hold dead visuals — reset the whole pool.
    if (!loaded) {
        FarJobWait();
        s_castersBuilt = false;
        s_Casters.clear();          // drop the dangling visual pointers with the level
        s_RainATItems.clear();
        for (auto& T : s_spotTiles) {
            T.owner = nullptr; T.staticValid = false; T.contentValid = false;
            T.hadSkinned = false; T.qValid = false; T.queue.Clear();
            T.lastSeen = 0; T.lastDyn = 0;
        }
        for (auto& C : s_pointCubes) {
            C.owner = nullptr; C.contentValid = false; C.hadSkinned = false;
            C.qValid = false; C.queue.Clear(); C.lastSeen = 0; C.lastDyn = 0;
        }
    }

    // Spot pool + point (campfire) dynamic shadow maps. Zone opened here and
    // closed on BOTH exits below so the per-frame zone count is constant even
    // when the early-out fires (profiler matches zones by open-order index).
    const int zDyn = VK::Prof::ZoneBegin(cmd, "Shadow/Dyn");

    // Pool compaction: freed opaque statics only exist in the cluster pools —
    // spot tiles / point-cube faces draw them via the shared kTargetDyn cluster
    // target (cull→draw pairs serialize in the frame cmd); the cached CPU queue
    // keeps the alpha-tested cutouts + anything outside the GPU set.
    const bool gpuDynStatics = compacted && clShadows;
    // GPU AT for the dyn maps too: the kTargetDyn slice covers the cutouts →
    // the per-light static queues aren't rebuilt or flushed at all.
    const bool gpuDynAT = gpuDynStatics && WorldGPU::ShadowATActive();

    // Nothing selected and the targets already initialized → zero cost.
    if (!haveSpots && !havePoints && !s_spotAtlasFirst && !s_pointFirst)
    {
        VK::Prof::ZoneEnd(cmd, zDyn);
        return;
    }

    // Rebuild a cached static-caster queue if the light sphere changed (full
    // visuals walk — the expensive part we must NOT do per frame). Oversized
    // visuals (terrain chunks) are excluded — see kSpotCasterMaxR.
    auto refreshStaticQueue = [&](RenderQueue& q, bool& valid, Fvector& qPos, float& qRange,
                                  size_t& qVis, const Fvector& p, float range, float maxCasterR) -> bool {
        if (valid && qVis == nVis && qRange == range
            && qPos.distance_to_sqr(p) < 1.0f)   // < 1m drift keeps the cache
            return false;
        q.Clear();
        if (loaded) {
            EnsureCasterList();
            Fmatrix identity; identity.identity();
            for (const CasterEntry& e : s_Casters) {
                const Fsphere& bs = e.bs;
                if (bs.R > maxCasterR) continue;
                if (bs.R > 0.f) {
                    // +1 m: qPos may drift up to 1 m before the next re-collect
                    // (the cache-keep test above) — without the margin a walking
                    // light misses casters entering the sphere in that window.
                    const float rr = range + bs.R + 1.0f;
                    if (p.distance_to_sqr(bs.P) > rr * rr) continue;
                }
                e.rv->Submit(q, identity, 0.0f);
            }
            q.SortByKey();
        }
        valid = true; qPos = p; qRange = range; qVis = nVis;
        return true;
    };

    // One depth render into a POINT-cube face. STATIC/DYNAMIC split: the static
    // pass (statics=&queue, skinned/grass off, clear=true) fills the cached STATIC
    // cube; the dynamic pass (statics=null, skinned/grass on, clear=false) overlays
    // NPC + culled grass on TOP of a copy of the static face. This is what stopped
    // an NPC/grass refresh from re-rastering all 6 static faces (the ~2.5ms DynPoint).
    const u32 kPointSz = ShadowMap::PointSize();
    auto renderPointFace = [&](VkImageView view, const Fmatrix& vp, RenderQueue* statics,
                               const Fvector& lightPos, float lightRange,
                               bool clear, bool doSkinned, bool drawGrass, int grassCullSlot) {
        VK::RenderingBuilder(kPointSz, kPointSz)
            .Depth(view, clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD)
            .BeginFlipped(cmd);
        vkCmdSetDepthBias(cmd, kBiasConst, 0.0f, kBiasSlope);
        if (statics) {
            // gpuDynStatics: opaque statics via the kTargetDyn cluster slice the
            // caller culled for this face; the CPU queue keeps the AT cutouts —
            // unless gpuDynAT covers those through the same slice too.
            if (!gpuDynAT) {
                // Per-face frustum for the statics too: the queue is a sphere collect,
                // each face sees ~1/6 of it (same lever as the skinned cull below).
                CFrustum fr; Fmatrix vpCopy = vp; fr.CreateFromMatrix(vpCopy, FRUSTUM_P_ALL);
                statics->FlushDepth(cmd, vp, false, gpuDynStatics, false, &fr);
            }
            if (gpuDynStatics)
                WorldGPU::DrawShadow(cmd, WorldGPU::kTargetDyn, vp, /*skipTerrain*/true);
        }
        if (doSkinned) {
            // Per-face frustum: an NPC by the fire sits in 1-2 of the 6 cube faces,
            // but the sphere cull alone re-drew it into all six (the ~2 ms DynPoint
            // remainder after the static/dynamic split). Cull it to the faces it's in.
            CFrustum fr; Fmatrix vpCopy = vp; fr.CreateFromMatrix(vpCopy, FRUSTUM_P_ALL);
            Skinned_RenderShadow(cmd, vp, &lightPos, lightRange, &fr);
        }
        if (drawGrass) {
            if (grassCullSlot >= 0)
                GrassCull::Draw(cmd, vp, lightPos, lightRange, u32(grassCullSlot));
            else
                DrawGrassSpotCasters(cmd, vp, lightPos, lightRange);
        }
        vkCmdEndRendering(cmd);
    };

    // --- SPOT POOL: assign atlas tiles to this frame's pooled lights (ownership
    // persists across frames → cached tiles survive), then re-render only the
    // tiles that actually changed. A static lamp costs its tile ONCE; the
    // flashlight (moves every frame) re-renders as before.
    // Sub-zone so a fat Shadow/Dyn in the [VK Perf] log attributes itself:
    // spot tiles (lamps/beams/flashlight) vs point cubes (campfires) below.
    const int zDynSpot = VK::Prof::ZoneBegin(cmd, "Shadow/DynSpot");
    {
        for (auto& t : s_giTile) t = -1;
        const u32 frame = RV().frameIndex;
        // r_spot_scale: the maps are rendered into the top-left corner of each tile and
        // the receivers scale their atlas UV by the same ratio (spot_flash.w). Froxel fog
        // and the beam cones read the atlas with their own full-tile math -> whole tiles
        // while they are on. A new size re-renders every tile.
        const u32   tilePx    = ShadowMap::SpotSize();
        const float wantScale = (ps_r_vol || ps_r_light_cones) ? 1.f : std::clamp(ps_r_spot_scale, 0.25f, 1.f);
        const u32   rendPx    = std::clamp(u32(float(tilePx) * wantScale + 0.5f) & ~7u, 64u, tilePx);
        if (rendPx != s_spotRendPx) {
            for (SpotTile& T : s_spotTiles) { T.staticValid = false; T.contentValid = false; }
            s_spotRendPx = rendPx;
        }
        ShadowMap::SetSpotTileScale(float(rendPx) / float(tilePx));
        // Only grass casters, the beam cones and froxel fog read the BEAM atlas (the
        // receivers blend it in with a zero weight without grass) -> no clean->beam copy
        // per frame without them.
        const bool beamRead = ps_r_spot_grass || ps_r_light_cones || ps_r_vol;
        int assign[Lights::kMaxShadowSpots];
        // Pass 1: keep tiles already owned by a pooled light.
        for (u32 k = 0; k < FL.poolCount; ++k) {
            assign[k] = -1;
            const void* owner = FL.src[FL.poolGi[k]];
            for (u32 t = 0; t < Lights::kMaxShadowSpots; ++t)
                if (s_spotTiles[t].owner == owner && s_spotTiles[t].lastSeen != frame) {
                    assign[k] = int(t); s_spotTiles[t].lastSeen = frame; break;
                }
        }
        // Pass 2: newcomers get the least-recently-used remaining tiles.
        for (u32 k = 0; k < FL.poolCount; ++k) {
            if (assign[k] >= 0) continue;
            int best = -1; u32 bestSeen = 0xFFFFFFFFu;
            for (u32 t = 0; t < Lights::kMaxShadowSpots; ++t) {
                if (s_spotTiles[t].lastSeen == frame) continue;   // taken this frame
                if (s_spotTiles[t].lastSeen < bestSeen) { bestSeen = s_spotTiles[t].lastSeen; best = int(t); }
            }
            if (best < 0) break;
            SpotTile& T = s_spotTiles[best];
            T.owner = nullptr; T.staticValid = false; T.contentValid = false;
            T.qValid = false;   // evicted — everything stale
            T.lastSeen = frame;
            assign[k] = best;
        }

        // Camera frustum for the dynamic-LOD visibility gate: a light whose cone
        // volume is entirely off-screen freezes its NPC/grass overlays (the
        // cached static tile keeps shadowing anything that IS in view of it).
        CFrustum camFrustum;
        { Fmatrix mvp = RV().mFullTransform;   // CreateFromMatrix takes a non-const ref
          camFrustum.CreateFromMatrix(mvp, FRUSTUM_P_LRTB | FRUSTUM_P_FAR); }

        // Per-tile work decision + VP refresh. THREE layers (VSM-style split):
        //   STATIC = statics+trees (cached until the light MOVES)
        //   CLEAN  = static copy + NPC overlay (recomposited on an LOD cadence)
        //   BEAM   = clean copy + grass (near, in-view, for wind animation)
        bool needStatic[Lights::kMaxShadowSpots] = {};
        bool needClean[Lights::kMaxShadowSpots]  = {};
        bool needBeam[Lights::kMaxShadowSpots]   = {};
        float npcRange[Lights::kMaxShadowSpots]  = {};   // NPC caster reach per tile (r_spot_npc_dist for flashlights)
        bool anyStatic = false, anyClean = false, anyBeam = false;
        const Fvector eyePos = RV().vCameraPosition;
        for (u32 k = 0; k < FL.poolCount; ++k) {
            if (assign[k] < 0) continue;
            const u32 t  = u32(assign[k]);
            SpotTile&  T = s_spotTiles[t];
            const int gi = FL.poolGi[k];
            if (gi >= 0 && gi < int(Lights::kMaxLights)) s_giTile[gi] = int(t);
            const void* l = FL.src[gi];   // identity key (light uid) -- the light itself is never read here
            const Fvector lpos = FL.srcPos[gi];
            Fvector ldir = FL.srcDir[gi];
            if (ldir.magnitude() < 1e-5f) ldir.set(0.f, 0.f, 1.f);
            ldir.normalize();

            // STATIC layer: only when the light itself moved / changed owner.
            // Thresholds are FLOAT-NOISE guards, not motion deadbands: a truly
            // static lamp repeats bit-identical pos/dir so the cache holds, while
            // anything genuinely animated (NPC headlamp swaying with the head
            // bone) re-renders every frame like the player flashlight. The old
            // 2 cm / ~0.6° band left slow head-sway INSIDE it → stale map.
            const bool newOwner = (T.owner != (const void*)l);
            const bool moved = newOwner
                || T.pos.distance_to_sqr(lpos) > 4e-6f        // 2 mm
                || T.dir.dotproduct(ldir) < 0.999999f         // ~0.08°
                || _abs(T.range - FL.srcRange[gi]) > 0.01f
                || _abs(T.cone  - FL.srcCone[gi])  > 0.001f;

            // Visibility + distance LOD. The cone bound is a sphere at the light
            // of radius = range (conservative). Off-screen → freeze; near → every
            // frame; mid/far → staggered cadence.
            const bool  visible = camFrustum.testSphere_dirty(lpos, FL.srcRange[gi]);
            const float dist    = eyePos.distance_to(lpos);
            u32 cadence = (dist < 30.f) ? 1u : (dist < 70.f ? 4u : 12u);
            const bool dynDue = ((frame + t) % cadence) == 0u;

            // Moving-light LOD: a near light that moved re-renders its STATIC
            // layer every frame (the honest headlamp cost); farther out the
            // refresh rides the same dyn cadence, and off-screen it freezes.
            // Safe against wobble because VP + content always update TOGETHER.
            needStatic[t] = !T.staticValid || newOwner || (moved && visible && dynDue);

            // The sampling VP must stay glued to the depth CONTENT: refresh it
            // only when the tile re-renders. Setting it every frame while the
            // tile cached made receivers project with a matrix the map was never
            // rendered with — a headlamp drifting inside the deadband wobbled its
            // whole shadow (drift + snap-back on every threshold trip).
            if (needStatic[t])
                ShadowMap::SetSpotTileVP(t, ShadowMap::ComputeSpotVPFor(lpos, ldir, FL.srcRange[gi], FL.srcCone[gi]));
            if (gi == FL.spotIdx) ShadowMap::SetSpotVP(ShadowMap::GetSpotTileVP(t));   // cookie projection matrix

            // r_spot_npc_dist: a flashlight takes NPC casters only this near (its windowed falloff leaves far ones a
            // dim speck, and each NPC is a full skin pass into the tile). 0 = the whole range, < 0 = no NPCs at all.
            const bool noNpc = FL.flashFlag[gi] && ps_r_spot_npc_dist < 0.f;
            npcRange[t] = (FL.flashFlag[gi] && ps_r_spot_npc_dist > 0.f) ? _min(FL.srcRange[gi], ps_r_spot_npc_dist) : FL.srcRange[gi];
            const bool skinnedNow = visible && !noNpc && Skinned_AnyCasterInSphere(lpos, npcRange[t]);
            // Recomposite CLEAN when the static layer changed, or (visible) an NPC
            // is due for a refresh, or an NPC that WAS there has left (clear it).
            needClean[t] = needStatic[t]
                || (!T.contentValid)
                || (visible && dynDue && (skinnedNow || T.hadSkinned));
            // BEAM: whenever clean recomposited, plus in-view lights on a distance-
            // tiered cadence so the SSFX wind keeps animating the grass cutouts.
            // NOT every frame: a beam refresh = tile copy + a full-grass VS pass,
            // and with several evening lamps pooled this was the single biggest
            // Shadow/Dyn cost (~4.4ms, 2026-07-04 profile). Wind sway at 20 m+
            // reads through coarse tile texels + penumbra — half/quarter rate is
            // invisible there; only near beams keep the every-frame animation.
            const u32 beamCad = (dist < 20.f) ? 1u : (dist < 35.f ? 2u : 4u);
            needBeam[t] = beamRead && (needClean[t]
                || (ps_r_spot_grass && visible && dist < 50.f && ((frame + t) % beamCad) == 0u));

            anyStatic |= needStatic[t];
            anyClean  |= needClean[t];
            anyBeam   |= needBeam[t];
            if (needStatic[t]) {
                T.owner = l; T.pos = lpos; T.dir = ldir;
                T.range = FL.srcRange[gi]; T.cone = FL.srcCone[gi];
                T.staticValid = true;
            }
            if (needClean[t]) {
                T.hadSkinned   = skinnedNow;
                T.contentValid = true;
                T.lastDyn      = frame;
            }
        }

        const u32 ax     = ShadowMap::SpotAtlasX();
        // The tile's rendered corner (r_spot_scale): raster, clears and copies stay inside it.
        auto tileRect = [&](u32 t) {
            return VkRect2D{ { s32((t % ax) * tilePx), s32((t / ax) * tilePx) }, { rendPx, rendPx } };
        };
        const VkExtent2D atlasExt{ tilePx * ax, tilePx * ShadowMap::SpotAtlasY() };
        auto setTileViewport = [&](const VkRect2D& r) {
            VkViewport v{ float(r.offset.x), float(r.offset.y) + float(rendPx),
                          float(rendPx), -float(rendPx), 0.f, 1.f };
            vkCmdSetViewport(cmd, 0, 1, &v);
        };

        // 1) STATIC atlas: re-raster only the tiles whose light moved (statics +
        // trees, NO NPCs). This is the layer NPC movement no longer disturbs.
        // gpuDynStatics: the opaque statics come from the cluster kTargetDyn
        // target, whose cull is a compute dispatch that can't live inside a
        // render pass — so each tile gets its own cull → begin/load → draw →
        // end sequence (rare: tiles are cached; only the flashlight re-renders
        // per frame). Without it the original single-pass path is kept intact.
        if (anyStatic || s_spotAtlasFirst)
        {
            ImageBarrier(cmd, ShadowMap::GetSpotStaticImage(),
                         s_spotAtlasFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            // loadOp LOAD keeps cached tiles (CLEAR only on the atlas's first use).
            VK::RenderingBuilder rbStatic(atlasExt);
            rbStatic.Depth(ShadowMap::GetSpotStaticView(),
                           s_spotAtlasFirst ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                            : VK_ATTACHMENT_LOAD_OP_LOAD);
            // WAW guard between the per-tile depth passes (disjoint tiles, but
            // same subresource): previous pass's depth writes → next pass.
            auto depthPassBarrier = [&]() {
                VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                     0, 1, &mb, 0, nullptr, 0, nullptr);
            };
            if (gpuDynStatics && s_spotAtlasFirst) {
                // First use under per-tile rendering: one clear-only pass so the
                // per-tile LOAD ops below always see defined content.
                rbStatic.Begin(cmd);
                vkCmdEndRendering(cmd);
                rbStatic.SetDepthLoad(VK_ATTACHMENT_LOAD_OP_LOAD);
            }
            if (!gpuDynStatics)
                rbStatic.Begin(cmd);
            bool firstTilePass = true;
            for (u32 k = 0; k < FL.poolCount; ++k) {
                if (assign[k] < 0) continue;
                const u32 t = u32(assign[k]);
                if (!needStatic[t]) continue;
                SpotTile& T = s_spotTiles[t];
                const VkRect2D r = tileRect(t);
                const Fmatrix& vp = ShadowMap::GetSpotTileVP(t);
                if (!gpuDynAT)   // fully-GPU statics: the tile flush below is skipped
                    refreshStaticQueue(T.queue, T.qValid, T.qPos, T.qRange, T.qVis,
                                       T.pos, T.range, kSpotCasterMaxR);
                if (gpuDynStatics) {
                    // Cluster cull for this tile's VP (outside the render pass);
                    // error budget = the tile's texel size at the cone's far end.
                    const u32   tgt   = WorldGPU::kTargetDyn;
                    const float texel = 2.f * T.range * tanf(T.cone * 0.5f) / _max(1.f, (float)rendPx);
                    WorldGPU::CullShadow(cmd, &tgt, &vp, &texel, 1);
                    if (!firstTilePass || s_spotAtlasFirst) depthPassBarrier();
                    rbStatic.Begin(cmd);
                }
                firstTilePass = false;
                if (!s_spotAtlasFirst) {   // whole atlas cleared on first use
                    VkClearAttachment ca{};
                    ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    ca.clearValue.depthStencil = { 1.0f, 0 };
                    VkClearRect cr{ r, 0, 1 };
                    vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
                }
                setTileViewport(r);
                vkCmdSetScissor(cmd, 0, 1, &r);
                vkCmdSetDepthBias(cmd, kBiasConst, 0.0f, kBiasSlope);
                // Cone frustum: the cached queue is a range-SPHERE collect, but
                // the spot cone is a small slice of that sphere — cull statics
                // (and trees) to it. Same lever as the point-cube per-face cull.
                CFrustum fr;
                { Fmatrix vpCopy = vp; fr.CreateFromMatrix(vpCopy, FRUSTUM_P_ALL); }
                if (!gpuDynAT)   // GPU AT: the cutouts ride the indirect draw below
                    T.queue.FlushDepth(cmd, vp, false, gpuDynStatics, false, &fr);
                if (gpuDynStatics)
                    WorldGPU::DrawShadow(cmd, WorldGPU::kTargetDyn, vp, /*skipTerrain*/true);
                if (RImplementation.Trees && RImplementation.Trees->IsReady())
                    RImplementation.Trees->RenderDepth(cmd, vp, -1, &fr);
                if (gpuDynStatics)
                    vkCmdEndRendering(cmd);
            }
            if (!gpuDynStatics)
                vkCmdEndRendering(cmd);
            ImageBarrier(cmd, ShadowMap::GetSpotStaticImage(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        }

        // Region copy helper (static→clean or clean→beam) for the refreshed tiles.
        auto copyTiles = [&](VkImage src, VkImage dst, const bool* mask) {
            VkImageCopy regions[Lights::kMaxShadowSpots];
            u32 nRegions = 0;
            if (s_spotAtlasFirst) {
                VkImageCopy& rg = regions[nRegions++]; rg = {};
                rg.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
                rg.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
                rg.extent         = { atlasExt.width, atlasExt.height, 1 };
            } else {
                for (u32 k = 0; k < FL.poolCount; ++k) {
                    if (assign[k] < 0 || !mask[u32(assign[k])]) continue;
                    const VkRect2D r = tileRect(u32(assign[k]));
                    VkImageCopy& rg = regions[nRegions++]; rg = {};
                    rg.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
                    rg.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
                    rg.srcOffset      = { r.offset.x, r.offset.y, 0 };
                    rg.dstOffset      = { r.offset.x, r.offset.y, 0 };
                    rg.extent         = { r.extent.width, r.extent.height, 1 };
                }
            }
            if (nRegions)
                vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nRegions, regions);
        };

        NX_GPU_MARK(cmd, "sh.spotStatic");   // [nx gpu slow]: the static layer (statics + trees)

        // 2) CLEAN atlas: copy the (cached) static tile, then draw the NPC overlay
        // on top. Static rasterization is NOT repeated — that's the split's win.
        if (anyClean || s_spotAtlasFirst)
        {
            ImageBarrier(cmd, ShadowMap::GetSpotImage(),
                         s_spotAtlasFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            copyTiles(ShadowMap::GetSpotStaticImage(), ShadowMap::GetSpotImage(), needClean);
            ImageBarrier(cmd, ShadowMap::GetSpotImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            // loadOp LOAD — NPC ON TOP of the static copy.
            VK::RenderingBuilder(atlasExt).Depth(ShadowMap::GetSpotView()).Begin(cmd);
            for (u32 k = 0; k < FL.poolCount; ++k) {
                if (assign[k] < 0) continue;
                const u32 t = u32(assign[k]);
                if (!needClean[t]) continue;
                SpotTile& T = s_spotTiles[t];
                if (!T.hadSkinned) continue;   // clean tile = pure static copy, no NPC to draw
                const VkRect2D r = tileRect(t);
                setTileViewport(r);
                vkCmdSetScissor(cmd, 0, 1, &r);
                vkCmdSetDepthBias(cmd, kBiasConst, 0.0f, kBiasSlope);
                // Cone cull (mirrors the point-face fix): an NPC beside/behind
                // the lamp sits inside the range sphere but outside the cone.
                CFrustum fr;
                { Fmatrix vpc = ShadowMap::GetSpotTileVP(t); fr.CreateFromMatrix(vpc, FRUSTUM_P_ALL); }
                Skinned_RenderShadow(cmd, ShadowMap::GetSpotTileVP(t), &T.pos, npcRange[t], &fr);
            }
            vkCmdEndRendering(cmd);
            ImageBarrier(cmd, ShadowMap::GetSpotImage(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         (anyBeam || s_spotAtlasFirst) ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                                       : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_ASPECT_DEPTH_BIT);
        }

        NX_GPU_MARK(cmd, "sh.spotClean");    // the static->clean copy + the NPC overlay

        // GPU grass cull (Phase 1): compact the beam tiles' blades ONCE, up front
        // (compute, before the render pass), so each per-tile beam draw becomes a
        // few-hundred-instance indirect instead of a full grass VS pass. Falls back
        // to brute force when r_grass_cull is off or the cull failed to init.
        int  beamSlot[Lights::kMaxShadowSpots]; for (auto& s : beamSlot) s = -1;
        bool beamCulled = false;
        if (ps_r_spot_grass && (anyBeam || s_spotAtlasFirst) && GrassCull::Ensure()) {
            Fvector4 lights[GrassCull::kMaxLights]; u32 nL = 0;
            for (u32 k = 0; k < FL.poolCount; ++k) {
                if (assign[k] < 0) continue;
                const u32 t = u32(assign[k]);
                if (!needBeam[t] || nL >= GrassCull::kMaxLights) continue;
                const SpotTile& T = s_spotTiles[t];
                lights[nL].set(T.pos.x, T.pos.y, T.pos.z, T.range + 2.0f);   // +2 m = the grass VS cull margin
                beamSlot[t] = int(nL);
                ++nL;
            }
            if (nL > 0) { GrassCull::CullLights(cmd, lights, nL); beamCulled = true; }
        }

        // 3) BEAM atlas: copy the clean tile + grass on top (in-view near lights).
        if (anyBeam || s_spotAtlasFirst)
        {
            if (!anyClean && !s_spotAtlasFirst)
                ImageBarrier(cmd, ShadowMap::GetSpotImage(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            ImageBarrier(cmd, ShadowMap::GetSpotBeamImage(),
                         s_spotAtlasFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            copyTiles(ShadowMap::GetSpotImage(), ShadowMap::GetSpotBeamImage(), needBeam);
            ImageBarrier(cmd, ShadowMap::GetSpotImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            ImageBarrier(cmd, ShadowMap::GetSpotBeamImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            // loadOp LOAD — grass ON TOP of the copy.
            VK::RenderingBuilder(atlasExt).Depth(ShadowMap::GetSpotBeamView()).Begin(cmd);
            for (u32 k = 0; k < FL.poolCount; ++k) {
                if (assign[k] < 0) continue;
                const u32 t = u32(assign[k]);
                if (!needBeam[t]) continue;
                SpotTile& T = s_spotTiles[t];
                const VkRect2D r = tileRect(t);
                setTileViewport(r);
                vkCmdSetScissor(cmd, 0, 1, &r);
                vkCmdSetDepthBias(cmd, kBiasConst, 0.0f, kBiasSlope);
                if (ps_r_spot_grass) {
                    if (beamCulled && beamSlot[t] >= 0)
                        GrassCull::Draw(cmd, ShadowMap::GetSpotTileVP(t), T.pos, T.range, u32(beamSlot[t]));
                    else
                        DrawGrassSpotCasters(cmd, ShadowMap::GetSpotTileVP(t), T.pos, T.range);
                }
            }
            vkCmdEndRendering(cmd);
            ImageBarrier(cmd, ShadowMap::GetSpotBeamImage(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        }
        s_spotAtlasFirst = false;
    }
    VK::Prof::ZoneEnd(cmd, zDynSpot);
    NX_GPU_MARK(cmd, "sh.spot");

    // --- POINT (campfire / lamp) cube: STATIC occluders + skinned casters. The
    // statics (floors, walls, AND terrain — no size cap here) are what stop the
    // light + the NPC shadow from leaking through the ground onto geometry above
    // (a basement lamp lighting the earth + a fence overhead, with the NPC's
    // shadow cast onto it). Terrain is the blocker for a small-range buried lamp,
    // unlike the wide flashlight where it's a receiver. Re-render while an NPC is
    // (or was) in radius OR when the static set changed (light picked / moved
    // >1 m); otherwise the cached cube keeps occluding for free.
    // POINT POOL: a cube per pooled campfire/lamp (cube ARRAY), same LRU cache +
    // distance/visibility LOD as the spot pool. A static fire with no NPCs costs
    // its cube ONCE; a fire with NPCs re-renders on a distance cadence; an
    // off-screen fire freezes. (Phase A: whole-cube re-render, no static/dynamic
    // split — cube copies ×6 faces aren't worth it until profiling says so.)
    const int zDynPoint = VK::Prof::ZoneBegin(cmd, "Shadow/DynPoint");
    {
        const u32 nLayers = 6u * Lights::kMaxShadowPoints;
        for (auto& g : s_giCube) g = -1;
        const u32 frame = RV().frameIndex;

        // Tile assignment (2-pass LRU, mirrors the spot pool).
        int assignP[Lights::kMaxShadowPoints];
        for (u32 k = 0; k < FL.poolCountP; ++k) {
            assignP[k] = -1;
            const void* owner = FL.src[FL.poolGiP[k]];
            for (u32 t = 0; t < Lights::kMaxShadowPoints; ++t)
                if (s_pointCubes[t].owner == owner && s_pointCubes[t].lastSeen != frame) {
                    assignP[k] = int(t); s_pointCubes[t].lastSeen = frame; break;
                }
        }
        for (u32 k = 0; k < FL.poolCountP; ++k) {
            if (assignP[k] >= 0) continue;
            int best = -1; u32 bestSeen = 0xFFFFFFFFu;
            for (u32 t = 0; t < Lights::kMaxShadowPoints; ++t) {
                if (s_pointCubes[t].lastSeen == frame) continue;
                if (s_pointCubes[t].lastSeen < bestSeen) { bestSeen = s_pointCubes[t].lastSeen; best = int(t); }
            }
            if (best < 0) break;
            PointCube& C = s_pointCubes[best];
            C.owner = nullptr; C.staticValid = false; C.contentValid = false; C.qValid = false;
            C.lastSeen = frame;
            assignP[k] = best;
        }

        CFrustum camFrustumP;
        { Fmatrix mvp = RV().mFullTransform;
          camFrustumP.CreateFromMatrix(mvp, FRUSTUM_P_LRTB | FRUSTUM_P_FAR); }

        // Static/dynamic cube split (mirrors the spot pool + sun cascades). A near
        // fire's ~2.5 ms DynPoint was 6 STATIC faces re-rastered EVERY time an NPC
        // moved or grass swayed (A/B confirmed grass itself was noise). Now the
        // statics are cached in a STATIC cube; an NPC/grass refresh copies that +
        // draws only the dynamic overlay (skinned + culled grass).
        bool needStatic[Lights::kMaxShadowPoints] = {};   // re-raster the 6 static faces (fire/casters moved)
        bool needDyn[Lights::kMaxShadowPoints]    = {};    // recomposite combined = static copy + NPC/grass
        bool grassCube[Lights::kMaxShadowPoints]  = {};
        bool anyStatic = false, anyDyn = false;
        const Fvector eyeP = RV().vCameraPosition;
        for (u32 k = 0; k < FL.poolCountP; ++k) {
            if (assignP[k] < 0) continue;
            const u32 t  = u32(assignP[k]);
            PointCube& C = s_pointCubes[t];
            const int gi = FL.poolGiP[k];
            if (gi >= 0 && gi < int(Lights::kMaxLights)) s_giCube[gi] = int(t);
            const void* l = FL.src[gi];   // identity key (light uid)
            const Fvector lpos = FL.srcPos[gi];
            // EFFECTIVE range = the boosted gpu[] range (r_point_range) so the cube's
            // far plane matches what the receivers light with — a bigger fire lights
            // AND shadows farther in one consistent radius.
            const float lrange = FL.gpu[gi].pos[3];

            const bool newOwner = (C.owner != (const void*)l);
            const bool moved = newOwner || C.pos.distance_to_sqr(lpos) > 0.0004f
                            || _abs(C.range - lrange) > 0.01f;
            // Static-caster queue for this cube (statics/walls, NOT terrain — a lamp
            // under the ground is kept off the terrain by sector, vk_light.cpp).
            // gpuDynAT: no CPU queue, but keep the visuals-count stamp so level
            // streaming still re-renders the cube when new geometry loads in.
            bool staticChanged;
            if (gpuDynAT) {
                staticChanged = C.qVis != nVis;
                C.qVis = nVis; C.qValid = false;   // flip to CPU later → full rebuild
            } else {
                staticChanged = refreshStaticQueue(C.queue, C.qValid, C.qPos, C.qRange, C.qVis,
                                                   lpos, lrange, kSpotCasterMaxR);
            }
            // STATIC layer: only when the fire itself moved or its casters changed.
            needStatic[t] = !C.staticValid || moved || staticChanged;

            const bool visible = camFrustumP.testSphere_dirty(lpos, lrange);
            const float dist   = eyeP.distance_to(lpos);
            u32 cadence = (dist < 20.f) ? 1u : (dist < 50.f ? 4u : 12u);
            const bool dynDue = ((frame + t) % cadence) == 0u;
            const bool skinnedNow = visible && Skinned_AnyCasterInSphere(lpos, lrange);
            // Grass dapples (r_point_grass): near in-view fires get the detail
            // manager's blades into their cube — swaying grass around a campfire
            // cuts its light like the spot BEAM atlas. (Grass instances only exist
            // near the camera, hence the 40 m gate.) Grass runs on its OWN cadence
            // at HALF the NPC rate; firelight flicker masks the half-rate sway.
            grassCube[t] = ps_r_point_grass && visible && dist < 40.f;
            const bool grassDue = ((frame + t) % (cadence * 2u)) == 0u;

            // DYNAMIC recomposite: static changed, or (visible) an NPC due, or an
            // NPC that WAS there left, or a grass sway is due. This copies the cached
            // static + overlays the dynamics — it NO LONGER re-rasters the statics.
            needDyn[t] = needStatic[t] || !C.contentValid
                      || (visible && dynDue && (skinnedNow || C.hadSkinned))
                      || (grassCube[t] && grassDue);
            anyStatic |= needStatic[t];
            anyDyn    |= needDyn[t];
            if (needStatic[t]) { C.owner = l; C.pos = lpos; C.range = lrange; C.staticValid = true; }
            if (needDyn[t])    { C.hadSkinned = skinnedNow; C.contentValid = true; C.lastDyn = frame; }
            // Diagnostic (r_light_debug): why a fire near the player does/doesn't
            // cast — is it pooled, visible, does the pass see an NPC in range?
            if (ps_r_light_debug && dist < 25.f) {
                static u32 s_pdLog = 0;
                if (RV().dwTimeGlobal - s_pdLog > 2000) { s_pdLog = RV().dwTimeGlobal;
                    Msg("[VK PointPool] cube=%u gi=%d pos=(%.0f,%.0f,%.0f) range=%.1f dist=%.1f vis=%d npc=%d static=%d dyn=%d",
                        t, gi, lpos.x, lpos.y, lpos.z, lrange, dist, visible ? 1 : 0,
                        skinnedNow ? 1 : 0, needStatic[t] ? 1 : 0, needDyn[t] ? 1 : 0);
                }
            }
        }

        // GPU grass cull (Phase 1): one sphere cull per campfire covers ALL 6 of
        // its cube faces — instead of 6 full grass VS passes per fire we compact
        // once and draw a few-hundred-instance indirect ×6. Compute, before the
        // render pass. Falls back to brute force when off / not ready.
        int  cubeSlot[Lights::kMaxShadowPoints]; for (auto& s : cubeSlot) s = -1;
        bool cubeCulled = false;
        if (ps_r_point_grass && (anyDyn || s_pointFirst) && GrassCull::Ensure()) {
            Fvector4 lights[GrassCull::kMaxLights]; u32 nL = 0;
            for (u32 k = 0; k < FL.poolCountP; ++k) {
                if (assignP[k] < 0) continue;
                const u32 t = u32(assignP[k]);
                if (!needDyn[t] || !grassCube[t] || nL >= GrassCull::kMaxLights) continue;
                const PointCube& C = s_pointCubes[t];
                lights[nL].set(C.pos.x, C.pos.y, C.pos.z, C.range + 2.0f);   // +2 m = the grass VS cull margin
                cubeSlot[t] = int(nL);
                ++nL;
            }
            if (nL > 0) { GrassCull::CullLights(cmd, lights, nL); cubeCulled = true; }
        }

        // 1) STATIC cube faces: re-raster only cubes whose fire/casters moved
        //    (statics only, no NPC/grass) — the cached copy source.
        if (anyStatic || s_pointStaticFirst)
        {
            ImageBarrier(cmd, ShadowMap::GetPointStaticImage(),
                         s_pointStaticFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT, nLayers);
            s_pointStaticFirst = false;
            for (u32 k = 0; k < FL.poolCountP; ++k) {
                if (assignP[k] < 0) continue;
                const u32 t = u32(assignP[k]);
                if (!needStatic[t]) continue;
                PointCube& C = s_pointCubes[t];
                for (u32 f = 0; f < 6; ++f) {
                    const Fmatrix faceVP = ShadowMap::ComputePointFaceVP(C.pos, C.range, f);
                    if (gpuDynStatics) {
                        // Shared kTargetDyn slot, culled per face right before its
                        // draw (the cull's own barriers order it after the previous
                        // face's indirect draw). 90° face: texel = 2·range / size.
                        const u32   tgt   = WorldGPU::kTargetDyn;
                        const float texel = 2.f * C.range / _max(1.f, (float)kPointSz);
                        WorldGPU::CullShadow(cmd, &tgt, &faceVP, &texel, 1);
                    }
                    renderPointFace(ShadowMap::GetPointStaticFaceView(t, f), faceVP, &C.queue,
                                    C.pos, C.range, /*clear*/true, /*skinned*/false, /*grass*/false, -1);
                }
            }
            ImageBarrier(cmd, ShadowMap::GetPointStaticImage(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT, nLayers);
        }

        // 2) COMBINED cube: copy the cached static faces, then overlay the dynamics
        //    (skinned NPC + culled grass) — NO static re-raster on an NPC/grass tick.
        if (anyDyn || s_pointFirst)
        {
            ImageBarrier(cmd, ShadowMap::GetPointImage(),
                         s_pointFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT, nLayers);
            for (u32 k = 0; k < FL.poolCountP; ++k) {
                if (assignP[k] < 0) continue;
                const u32 t = u32(assignP[k]);
                if (!needDyn[t]) continue;
                VkImageCopy rg{};
                rg.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, t * 6u, 6u };   // this cube's 6 faces
                rg.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, t * 6u, 6u };
                rg.extent         = { kPointSz, kPointSz, 1 };
                vkCmdCopyImage(cmd, ShadowMap::GetPointStaticImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               ShadowMap::GetPointImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
            }
            ImageBarrier(cmd, ShadowMap::GetPointImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT, nLayers);
            s_pointFirst = false;
            for (u32 k = 0; k < FL.poolCountP; ++k) {
                if (assignP[k] < 0) continue;
                const u32 t = u32(assignP[k]);
                if (!needDyn[t]) continue;
                PointCube& C = s_pointCubes[t];
                for (u32 f = 0; f < 6; ++f) {
                    const Fmatrix faceVP = ShadowMap::ComputePointFaceVP(C.pos, C.range, f);
                    renderPointFace(ShadowMap::GetPointFaceView(t, f), faceVP, nullptr,
                                    C.pos, C.range, /*clear*/false, /*skinned*/true, grassCube[t],
                                    cubeCulled ? cubeSlot[t] : -1);
                }
            }
            ImageBarrier(cmd, ShadowMap::GetPointImage(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT, nLayers);
        }
    }
    VK::Prof::ZoneEnd(cmd, zDynPoint);
    NX_GPU_MARK(cmd, "sh.point");
    VK::Prof::ZoneEnd(cmd, zDyn);
}

// Spot-pool tile of a light this frame (-1 = not pooled / not rendered yet).
// Identity comparison only — callers pass the vkLight* from FrameLights::src.
int SpotShadow_TileOfLight(const void* light)
{
    if (!light) return -1;
    for (u32 t = 0; t < Lights::kMaxShadowSpots; ++t)
        if (s_spotTiles[t].owner == light && s_spotTiles[t].lastSeen == RV().frameIndex
            && s_spotTiles[t].contentValid)
            return int(t);
    return -1;
}

// Point-pool cube of a light this frame (-1 = not pooled / not rendered yet).
int PointShadow_CubeOfLight(const void* light)
{
    if (!light) return -1;
    for (u32 t = 0; t < Lights::kMaxShadowPoints; ++t)
        if (s_pointCubes[t].owner == light && s_pointCubes[t].lastSeen == RV().frameIndex
            && s_pointCubes[t].contentValid)
            return int(t);
    return -1;
}

// Grass-spot caster pipeline teardown (device shutdown). Survives level swaps:
// the vertex stride is sizeof(CDetail::Vertex) (compile-time) and the next
// level's diffuse set layout is created from the same bindings (compatible).
void SunShadow_Destroy()
{
    if (s_grassSpotPipe   != VK_NULL_HANDLE) { vkDestroyPipeline(VulkanHW.m_Device, s_grassSpotPipe, nullptr);        s_grassSpotPipe   = VK_NULL_HANDLE; }
    if (s_grassSpotLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_grassSpotLayout, nullptr); s_grassSpotLayout = VK_NULL_HANDLE; }
    // Shader modules are owned by g_ShaderManager (shared cache) — not ours.
    s_grassSpotVS = s_grassSpotFS = VK_NULL_HANDLE;
    s_grassSpotTried = false;
    GrassCull::Destroy();
}

}  // namespace VK

// ============================================================================================================================
// nfs46 (r_sun_mask, Switch preset): the half-res SUN MASK. One fullscreen pass right after the depth prepass resolves the cascade/far lookup
// once per 2x2 pixels into RGBA16F (shadow, cover, depth); the R1 terrain/statics read it with a depth-aware 2x2 pick
// (nx_sun_shadow.glsl NX_SUNMASK_READ) instead of doing the lookup per pixel.
// ============================================================================================================================
#include "vk_fullscreen.h"     // Fullscreen::CreatePipeline / OpaqueAttachment
#include "vk_env_light.h"      // EnvLight::GetSetLayout / GetCurrentSet
#include "vk_image.h"          // CreateImageView
namespace VK { namespace SunMask {
namespace {
    VkImage               s_img    = VK_NULL_HANDLE;
    VmaAllocation         s_alloc  = VK_NULL_HANDLE;
    VkImageView           s_view   = VK_NULL_HANDLE;
    VkSampler             s_depthSampler = VK_NULL_HANDLE;
    VkSampler             s_maskSampler  = VK_NULL_HANDLE;
    VkDescriptorSetLayout s_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool      s_pool   = VK_NULL_HANDLE;
    VkDescriptorSet       s_set    = VK_NULL_HANDLE;
    VkPipelineLayout      s_layout = VK_NULL_HANDLE;
    VkPipeline            s_pipe   = VK_NULL_HANDLE;
    VkExtent2D            s_full{}, s_half{};
    VkImageView           s_boundDepth = VK_NULL_HANDLE;
    bool                  s_first = true, s_failed = false;
    struct Push { Fmatrix invViewProj; float p[4]; };

    bool InitPipeline()
    {
        if (s_pipe) return true;
        if (s_failed || !g_ShaderManager || EnvLight::GetSetLayout() == VK_NULL_HANDLE) return false;
        VkShaderModule vs = g_ShaderManager->Load("tonemap.vert.spv");
        VkShaderModule fs = g_ShaderManager->Load("nx_sunmask.frag.spv");   // nfs47 content (+ cascade z cut)
        if (!vs || !fs) { s_failed = true; Msg("![nx sunmask] nx_sunmask.frag.spv missing -> per-pixel receive"); return false; }
        s_setLayout = MakeSetLayout({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER }, VK_SHADER_STAGE_FRAGMENT_BIT, "SunMask");
        s_pool      = MakeDescriptorPool({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER }, 1, "SunMask");
        if (!s_setLayout || !s_pool || !AllocSets(s_pool, s_setLayout, 1, &s_set, "SunMask")) { s_failed = true; return false; }
        VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
        VkDescriptorSetLayout sl[2] = { s_setLayout, EnvLight::GetSetLayout() };
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 2; plci.pSetLayouts = sl; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_layout) != VK_SUCCESS) { s_failed = true; return false; }
        s_pipe = Fullscreen::CreatePipeline(vs, fs, VK_FORMAT_R16G16B16A16_SFLOAT, s_layout, Fullscreen::OpaqueAttachment(), "SunMask");
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_depthSampler);
        vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_maskSampler);
        if (!s_pipe || !s_depthSampler || !s_maskSampler) { s_failed = true; return false; }
        Msg("[nx sunmask] pipeline OK");
        return true;
    }
}

bool Active()
{
    // Only the R1 receivers (r_world_flat) read the mask: without them the resolve would be pure cost.
    return ps_r_sun_mask && ps_r_world_flat && ps_r_sun_shadows != 0 && !VSM::Wanted() && !VSM::MaskReady()
        && RImplementation.b_loaded && InitPipeline();
}

bool Ensure(VkExtent2D screen)
{
    if (!InitPipeline() || screen.width == 0 || screen.height == 0) return false;
    if (s_img && screen.width == s_full.width && screen.height == s_full.height) return true;
    if (s_img) {
        VK_DeviceWaitIdleLocked();   // a render-size change, never per frame
        vkDestroyImageView(VulkanHW.m_Device, s_view, nullptr);
        vmaDestroyImage(VulkanHW.m_Allocator, s_img, s_alloc);
        s_img = VK_NULL_HANDLE; s_view = VK_NULL_HANDLE; s_alloc = VK_NULL_HANDLE;
    }
    s_full = screen;
    s_half = { (screen.width + 1) / 2, (screen.height + 1) / 2 };
    VkImageCreateInfo ic{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ic.imageType = VK_IMAGE_TYPE_2D; ic.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ic.extent = { s_half.width, s_half.height, 1 }; ic.mipLevels = 1; ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo ac{}; ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(VulkanHW.m_Allocator, &ic, &ac, &s_img, &s_alloc, nullptr) != VK_SUCCESS) {
        Msg("![nx sunmask] image create failed"); s_img = VK_NULL_HANDLE; s_failed = true; return false;
    }
    s_view = CreateImageView(s_img, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
    s_first = true;
    Msg("[nx sunmask] target %ux%u (render %ux%u)", s_half.width, s_half.height, screen.width, screen.height);
    return s_view != VK_NULL_HANDLE;
}

VkImageView View()    { return s_view; }
VkSampler   Sampler() { return s_maskSampler; }

void Resolve(VkCommandBuffer cmd, VkImageView sceneDepth, VkExtent2D screen, const Fmatrix& viewProj)
{
    if (!Ensure(screen) || sceneDepth == VK_NULL_HANDLE) return;
    VkDescriptorSet env = EnvLight::GetCurrentSet();
    if (env == VK_NULL_HANDLE) return;
    if (sceneDepth != s_boundDepth) {   // the depth view changes only with the swapchain (device idle)
        VkDescriptorImageInfo di{ s_depthSampler, sceneDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        DescriptorWriter(s_set).Image(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, di).Flush();
        s_boundDepth = sceneDepth;
    }
    ImageBarrier(cmd, s_img, s_first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    s_first = false;
    RenderingBuilder(s_half).Color(s_view, VK_ATTACHMENT_LOAD_OP_DONT_CARE).BeginPlain(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 1, &s_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 1, 1, &env, 0, nullptr);
    Push pc{};
    pc.invViewProj.invert_44(viewProj);
    pc.p[0] = 1.0f / float(screen.width); pc.p[1] = 1.0f / float(screen.height);
    vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    ImageBarrier(cmd, s_img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_ASPECT_COLOR_BIT);
}
}}   // namespace VK::SunMask
